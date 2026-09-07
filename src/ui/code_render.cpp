#include "zed/ui/code_render.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>

namespace zed::ui {
namespace {

constexpr std::size_t kMaximumLexedLineBytes = 8 * 1024;
constexpr std::size_t kMaximumTokensPerLine = 384;
constexpr std::size_t kMaximumHighlightedBlockBytes = 512 * 1024;
constexpr std::size_t kMaximumHighlightedLines = 10 * 1024;

enum class TokenTone { text, keyword, string, number, comment, type, header };

struct LexState {
  bool block_comment{false};
  char quote{0};
};

struct Token {
  std::string text;
  TokenTone tone{TokenTone::text};
};

struct HunkState {
  int old_line{0};
  int new_line{0};
  int old_remaining{0};
  int new_remaining{0};
};

using Lines = std::vector<std::string>;

Lines split_lines(std::string_view text) {
  Lines lines;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const auto end = text.find('\n', begin);
    const auto line_end = end == std::string_view::npos ? text.size() : end;
    std::string line(text.substr(begin, line_end - begin));
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return lines;
}

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return result;
}

enum class Language { plain, c_like, python, json, shell, rust, go };

Language language_from_name(std::string_view name) {
  const auto value = lower(name.substr(0, name.find_first_of(" \t,")));
  if (value == "c" || value == "cc" || value == "cpp" || value == "c++" ||
      value == "h" || value == "hpp" || value == "js" ||
      value == "javascript" || value == "ts" || value == "typescript" ||
      value == "java" || value == "cs")
    return Language::c_like;
  if (value == "py" || value == "python")
    return Language::python;
  if (value == "json" || value == "jsonc")
    return Language::json;
  if (value == "sh" || value == "bash" || value == "zsh" || value == "shell")
    return Language::shell;
  if (value == "rs" || value == "rust")
    return Language::rust;
  if (value == "go" || value == "golang")
    return Language::go;
  return Language::plain;
}

bool is_word_start(unsigned char character) {
  return std::isalpha(character) != 0 || character == '_';
}

bool is_word_continue(unsigned char character) {
  return std::isalnum(character) != 0 || character == '_';
}

std::size_t utf8_sequence_length(unsigned char character) {
  if ((character & 0xE0U) == 0xC0U)
    return 2;
  if ((character & 0xF0U) == 0xE0U)
    return 3;
  if ((character & 0xF8U) == 0xF0U)
    return 4;
  return 1;
}

bool is_keyword(std::string_view word, Language language) {
  static const std::unordered_set<std::string> kCommon{
      "if",        "else",     "for",     "while",   "switch",    "case",
      "break",     "continue", "return",  "class",   "struct",    "enum",
      "namespace", "using",    "public",  "private", "protected", "const",
      "static",    "void",     "auto",    "new",     "delete",    "try",
      "catch",     "throw",    "import",  "export",  "from",      "function",
      "async",     "await",    "let",     "var",     "def",       "lambda",
      "with",      "as",       "in",      "match",   "fn",        "impl",
      "trait",     "pub",      "package", "range",   "defer",     "select",
      "interface", "type",
  };
  static const std::unordered_set<std::string> kLiterals{
      "true", "false", "null", "nil", "none", "this", "self"};
  if (kCommon.contains(std::string(word)) ||
      kLiterals.contains(std::string(word)))
    return true;
  return language == Language::json &&
         (word == "true" || word == "false" || word == "null");
}

bool is_type(std::string_view word) {
  static const std::unordered_set<std::string> kTypes{
      "int",     "long",     "short",   "char",     "double",   "float",
      "bool",    "string",   "size_t",  "uint32_t", "uint64_t", "usize",
      "i32",     "i64",      "u32",     "u64",      "str",      "String",
      "vector",  "optional", "Result",  "Error",    "any",      "number",
      "boolean", "object",   "unknown", "never",    "byte",     "rune",
  };
  return kTypes.contains(std::string(word));
}

void append_token(std::vector<Token> &tokens, std::string_view text,
                  TokenTone tone) {
  if (text.empty())
    return;
  if (!tokens.empty() && tokens.back().tone == tone &&
      tokens.back().text.size() + text.size() <= 256) {
    tokens.back().text.append(text);
    return;
  }
  tokens.push_back({std::string(text), tone});
}

std::vector<Token> lex_line(std::string_view line, Language language,
                            LexState &state) {
  if (language == Language::plain || line.size() > kMaximumLexedLineBytes) {
    state = {};
    return {{std::string(line), TokenTone::text}};
  }

  std::vector<Token> tokens;
  std::size_t index = 0;
  const bool hash_comment =
      language == Language::python || language == Language::shell;
  const bool slash_comment =
      language == Language::c_like || language == Language::rust ||
      language == Language::go || language == Language::json;
  const bool block_comments =
      language == Language::c_like || language == Language::rust ||
      language == Language::go || language == Language::json;
  while (index < line.size() && tokens.size() < kMaximumTokensPerLine) {
    if (state.block_comment) {
      const auto end = line.find("*/", index);
      if (end == std::string_view::npos) {
        append_token(tokens, line.substr(index), TokenTone::comment);
        return tokens;
      }
      append_token(tokens, line.substr(index, end + 2 - index),
                   TokenTone::comment);
      index = end + 2;
      state.block_comment = false;
      continue;
    }
    if (state.quote != 0) {
      const std::size_t begin = index;
      while (index < line.size()) {
        if (line[index] == '\\' && index + 1 < line.size()) {
          index += 2;
          continue;
        }
        if (line[index++] == state.quote) {
          state.quote = 0;
          break;
        }
      }
      append_token(tokens, line.substr(begin, index - begin),
                   TokenTone::string);
      continue;
    }
    if (block_comments && index + 1 < line.size() && line[index] == '/' &&
        line[index + 1] == '*') {
      const auto end = line.find("*/", index + 2);
      if (end == std::string_view::npos) {
        append_token(tokens, line.substr(index), TokenTone::comment);
        state.block_comment = true;
        break;
      }
      append_token(tokens, line.substr(index, end + 2 - index),
                   TokenTone::comment);
      index = end + 2;
      continue;
    }
    if ((slash_comment && index + 1 < line.size() && line[index] == '/' &&
         line[index + 1] == '/') ||
        (hash_comment && line[index] == '#')) {
      append_token(tokens, line.substr(index), TokenTone::comment);
      index = line.size();
      break;
    }
    if (line[index] == '\'' || line[index] == '"' ||
        (language != Language::json && line[index] == '`')) {
      state.quote = line[index];
      append_token(tokens, line.substr(index, 1), TokenTone::string);
      ++index;
      continue;
    }
    const auto character = static_cast<unsigned char>(line[index]);
    if (character >= 0x80U) {
      const auto length =
          std::min(utf8_sequence_length(character), line.size() - index);
      append_token(tokens, line.substr(index, length), TokenTone::text);
      index += length;
      continue;
    }
    if (std::isdigit(character) != 0) {
      const std::size_t begin = index++;
      while (index < line.size() &&
             (std::isalnum(static_cast<unsigned char>(line[index])) != 0 ||
              line[index] == '.' || line[index] == '_'))
        ++index;
      append_token(tokens, line.substr(begin, index - begin),
                   TokenTone::number);
      continue;
    }
    if (is_word_start(character)) {
      const std::size_t begin = index++;
      while (index < line.size() &&
             is_word_continue(static_cast<unsigned char>(line[index])))
        ++index;
      const auto word = line.substr(begin, index - begin);
      append_token(tokens, word,
                   is_keyword(word, language) ? TokenTone::keyword
                   : is_type(word)            ? TokenTone::type
                                              : TokenTone::text);
      continue;
    }
    append_token(tokens, line.substr(index, 1), TokenTone::text);
    ++index;
  }
  if (index < line.size())
    return {{std::string(line), TokenTone::text}};
  return tokens;
}

ftxui::Color token_color(TokenTone tone, const TerminalTheme &theme) {
  switch (tone) {
  case TokenTone::keyword:
    return theme.code_keyword;
  case TokenTone::string:
    return theme.code_string;
  case TokenTone::number:
    return theme.code_number;
  case TokenTone::comment:
    return theme.code_comment;
  case TokenTone::type:
    return theme.code_type;
  case TokenTone::header:
    return theme.diff_header;
  case TokenTone::text:
    return theme.code_text;
  }
  return theme.code_text;
}

class WrappedTokensNode final : public ftxui::Node {
public:
  WrappedTokensNode(std::vector<Token> tokens, const TerminalTheme &theme)
      : tokens_(std::move(tokens)), theme_(theme) {
    natural_width_ = 1;
    int column = 0;
    for (const auto &token : tokens_) {
      for (const auto &glyph : glyphs_with_tabs(token.text)) {
        if (glyph == "\t")
          column += 4 - (column % 4);
        else
          column += ftxui::string_width(glyph);
      }
    }
    natural_width_ = std::max(1, column);
    layout_width_ = natural_width_;
    rebuild(layout_width_);
  }

  void ComputeRequirement() override {
    int height = 0;
    for (const auto &child : children_) {
      child->ComputeRequirement();
      height += std::max(1, child->requirement().min_y);
    }
    requirement_.min_x = natural_width_;
    requirement_.min_y = std::max(1, height);
  }

  void SetBox(ftxui::Box box) override {
    box_ = box;
    const int width = std::max(1, box.x_max - box.x_min + 1);
    if (width != layout_width_)
      requested_width_ = width;
    int y = box.y_min;
    for (const auto &child : children_) {
      const int height = std::max(1, child->requirement().min_y);
      child->SetBox({box.x_min, box.x_max, y, y + height - 1});
      y += height;
    }
  }

  void Render(ftxui::Screen &screen) override {
    for (const auto &child : children_)
      child->Render(screen);
  }

  void Check(Status *status) override {
    ftxui::Node::Check(status);
    if (requested_width_) {
      layout_width_ = *requested_width_;
      requested_width_.reset();
      rebuild(layout_width_);
      status->need_iteration = true;
    }
  }

private:
  static std::vector<std::string> glyphs_with_tabs(std::string_view text) {
    std::vector<std::string> glyphs;
    std::size_t begin = 0;
    while (begin <= text.size()) {
      const auto tab = text.find('\t', begin);
      const auto end = tab == std::string_view::npos ? text.size() : tab;
      const auto ordinary =
          ftxui::Utf8ToGlyphs(text.substr(begin, end - begin));
      glyphs.insert(glyphs.end(), ordinary.begin(), ordinary.end());
      if (tab == std::string_view::npos)
        break;
      glyphs.emplace_back("\t");
      begin = tab + 1;
    }
    return glyphs;
  }

  void append_fragment(ftxui::Elements &row, std::string &fragment,
                       TokenTone tone) const {
    if (fragment.empty())
      return;
    row.push_back(ftxui::text(std::move(fragment)) |
                  ftxui::color(token_color(tone, theme_)));
    fragment.clear();
  }

  void rebuild(int width) {
    children_.clear();
    ftxui::Elements row;
    std::string fragment;
    TokenTone fragment_tone = TokenTone::text;
    bool has_fragment = false;
    int column = 0;
    const auto finish_row = [&] {
      if (has_fragment)
        append_fragment(row, fragment, fragment_tone);
      if (row.empty())
        row.push_back(ftxui::text(" ") | ftxui::color(theme_.code_text));
      children_.push_back(ftxui::hbox(std::move(row)));
      row.clear();
      column = 0;
      has_fragment = false;
    };
    for (const auto &token : tokens_) {
      for (const auto &glyph : glyphs_with_tabs(token.text)) {
        const int glyph_width =
            glyph == "\t" ? 4 - (column % 4) : ftxui::string_width(glyph);
        if (glyph_width == 0) {
          if (!has_fragment || fragment_tone != token.tone) {
            if (has_fragment)
              append_fragment(row, fragment, fragment_tone);
            fragment_tone = token.tone;
            has_fragment = true;
          }
          fragment += glyph;
          continue;
        }
        if (column > 0 && column + glyph_width > width)
          finish_row();
        if (!has_fragment || fragment_tone != token.tone) {
          if (has_fragment)
            append_fragment(row, fragment, fragment_tone);
          fragment_tone = token.tone;
          has_fragment = true;
        }
        if (glyph == "\t")
          fragment.append(static_cast<std::size_t>(glyph_width), ' ');
        else
          fragment += glyph;
        column += glyph_width;
      }
    }
    finish_row();
  }

  std::vector<Token> tokens_;
  TerminalTheme theme_;
  int natural_width_{1};
  int layout_width_{1};
  std::optional<int> requested_width_;
};

ftxui::Element render_tokens(std::string_view line, Language language,
                             LexState &state, const TerminalTheme &theme) {
  const auto tokens = lex_line(line, language, state);
  return std::make_shared<WrappedTokensNode>(tokens, theme);
}

ftxui::Element render_wrapped_text(std::string_view text, TokenTone tone,
                                   const TerminalTheme &theme) {
  return std::make_shared<WrappedTokensNode>(
      std::vector<Token>{{std::string(text), tone}}, theme);
}

std::string line_number(std::optional<int> value) {
  if (!value)
    return "     ";
  const auto string = std::to_string(*value);
  return std::string(string.size() < 4 ? 4 - string.size() : 0, ' ') + string +
         " ";
}

bool is_file_marker(std::string_view line) {
  return line.starts_with("--- ") || line.starts_with("+++ ");
}

bool is_diff_start(const Lines &lines, std::size_t index) {
  if (lines[index].starts_with("diff --git "))
    return true;
  return lines[index].starts_with("--- ") && index + 1 < lines.size() &&
         lines[index + 1].starts_with("+++ ");
}

std::optional<HunkState> parse_hunk(std::string_view line) {
  if (!line.starts_with("@@ -"))
    return std::nullopt;
  const auto read_range =
      [](std::string_view value,
         std::size_t &cursor) -> std::optional<std::pair<int, int>> {
    const std::size_t begin = cursor;
    while (cursor < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[cursor])) != 0)
      ++cursor;
    if (cursor == begin)
      return std::nullopt;
    int start = 0;
    const auto [start_end, start_error] =
        std::from_chars(value.data() + begin, value.data() + cursor, start);
    if (start_error != std::errc{} || start_end != value.data() + cursor)
      return std::nullopt;
    int count = 1;
    if (cursor < value.size() && value[cursor] == ',') {
      const std::size_t count_begin = ++cursor;
      while (cursor < value.size() &&
             std::isdigit(static_cast<unsigned char>(value[cursor])) != 0)
        ++cursor;
      if (cursor == count_begin)
        return std::nullopt;
      const auto [count_end, count_error] = std::from_chars(
          value.data() + count_begin, value.data() + cursor, count);
      if (count_error != std::errc{} || count_end != value.data() + cursor)
        return std::nullopt;
    }
    if (start < 0 || count < 0 ||
        start > std::numeric_limits<int>::max() - count)
      return std::nullopt;
    return std::pair{start, count};
  };
  std::size_t cursor = 4;
  const auto old_range = read_range(line, cursor);
  if (!old_range || !line.substr(cursor).starts_with(" +"))
    return std::nullopt;
  cursor += 2;
  const auto new_range = read_range(line, cursor);
  if (!new_range || !line.substr(cursor).starts_with(" @@"))
    return std::nullopt;
  return HunkState{old_range->first, new_range->first, old_range->second,
                   new_range->second};
}

Language language_from_diff_path(std::string_view line) {
  auto path = line.substr(4);
  if (const auto tab = path.find('\t'); tab != std::string_view::npos)
    path = path.substr(0, tab);
  if (!path.empty() && path.back() == '"')
    path.remove_suffix(1);
  const auto dot = path.rfind('.');
  return dot == std::string_view::npos
             ? Language::plain
             : language_from_name(path.substr(dot + 1));
}

ftxui::Element render_plain_lines(const Lines &lines,
                                  const TerminalTheme &theme) {
  ftxui::Elements output;
  for (const auto &line : lines)
    output.push_back(
        render_wrapped_text(line.empty() ? " " : line, TokenTone::text, theme));
  return output.empty() ? ftxui::text("") : ftxui::vbox(std::move(output));
}

} // namespace

bool looks_like_diff(std::string_view text) {
  const auto lines = split_lines(text);
  bool has_headers = false;
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (lines[index].starts_with("--- ") && index + 1 < lines.size() &&
        lines[index + 1].starts_with("+++ ")) {
      has_headers = true;
      continue;
    }
    if (has_headers && parse_hunk(lines[index]))
      return true;
  }
  return false;
}

ftxui::Element render_code_block(std::string_view code,
                                 std::string_view language,
                                 const TerminalTheme &theme) {
  const auto lines = split_lines(code);
  if (code.size() > kMaximumHighlightedBlockBytes ||
      lines.size() > kMaximumHighlightedLines) {
    LexState plain_state;
    ftxui::Elements plain_rows;
    plain_rows.reserve(lines.size());
    for (const auto &line : lines)
      plain_rows.push_back(
          render_tokens(line, Language::plain, plain_state, theme));
    return ftxui::vbox(std::move(plain_rows)) |
           ftxui::bgcolor(theme.background_panel) |
           ftxui::borderStyled(ftxui::LIGHT, theme.border);
  }
  const auto parsed_language = language_from_name(language);
  LexState state;
  ftxui::Elements rows;
  rows.reserve(lines.size());
  for (std::size_t index = 0; index < lines.size(); ++index) {
    auto content = render_tokens(lines[index], parsed_language, state, theme);
    rows.push_back(ftxui::hbox({
                       ftxui::text(line_number(static_cast<int>(index + 1))) |
                           ftxui::color(theme.code_line_number),
                       std::move(content) | ftxui::flex,
                   }) |
                   ftxui::bgcolor(theme.background_panel));
  }
  return ftxui::vbox(std::move(rows)) |
         ftxui::borderStyled(ftxui::LIGHT, theme.border);
}

ftxui::Element render_diff(std::string_view diff, const TerminalTheme &theme) {
  const auto lines = split_lines(diff);
  ftxui::Elements rows;
  enum class Region { metadata, body };
  Region region = Region::metadata;
  HunkState hunk{};
  Language language = Language::plain;
  LexState old_state;
  LexState new_state;
  for (const auto &line : lines) {
    if (const auto parsed_hunk = parse_hunk(line)) {
      hunk = *parsed_hunk;
      region = Region::body;
      old_state = {};
      new_state = {};
      rows.push_back(render_wrapped_text(line, TokenTone::header, theme) |
                     ftxui::bold | ftxui::color(theme.diff_header) |
                     ftxui::bgcolor(theme.background_element));
      continue;
    }
    if (region == Region::metadata &&
        (line.starts_with("diff --git ") || line.starts_with("index ") ||
         is_file_marker(line) || line.starts_with("new file") ||
         line.starts_with("deleted file"))) {
      if (line.starts_with("+++ "))
        language = language_from_diff_path(line);
      old_state = {};
      new_state = {};
      rows.push_back(render_wrapped_text(line, TokenTone::header, theme) |
                     ftxui::bold | ftxui::color(theme.diff_header) |
                     ftxui::bgcolor(theme.background_element));
      continue;
    }
    if (region == Region::metadata) {
      rows.push_back(ftxui::paragraph(line.empty() ? " " : line) |
                     ftxui::color(theme.text));
      continue;
    }
    char marker = line.empty() ? ' ' : line.front();
    std::optional<int> old_number;
    std::optional<int> new_number;
    ftxui::Color foreground = theme.code_text;
    ftxui::Color background = theme.background_panel;
    if (marker == '+' && hunk.new_remaining > 0) {
      new_number = hunk.new_line++;
      --hunk.new_remaining;
      foreground = theme.diff_added;
      background = theme.diff_added_background;
    } else if (marker == '-' && hunk.old_remaining > 0) {
      old_number = hunk.old_line++;
      --hunk.old_remaining;
      foreground = theme.diff_removed;
      background = theme.diff_removed_background;
    } else if (!line.empty() && marker == ' ' && hunk.old_remaining > 0 &&
               hunk.new_remaining > 0) {
      old_number = hunk.old_line++;
      new_number = hunk.new_line++;
      --hunk.old_remaining;
      --hunk.new_remaining;
    } else {
      rows.push_back(ftxui::paragraph(line.empty() ? " " : line) |
                     ftxui::color(theme.text));
      continue;
    }
    const auto body = line.substr(1);
    ftxui::Element syntax;
    if (marker == '-') {
      syntax = render_tokens(body, language, old_state, theme);
    } else if (marker == '+') {
      syntax = render_tokens(body, language, new_state, theme);
    } else {
      (void)lex_line(body, language, old_state);
      syntax = render_tokens(body, language, new_state, theme);
    }
    rows.push_back(ftxui::hbox({
                       ftxui::text(line_number(old_number)) |
                           ftxui::color(theme.code_line_number),
                       ftxui::text(line_number(new_number)) |
                           ftxui::color(theme.code_line_number),
                       ftxui::text(std::string(1, marker)) | ftxui::bold |
                           ftxui::color(foreground),
                       std::move(syntax) | ftxui::flex,
                   }) |
                   ftxui::bgcolor(background));
    if (hunk.old_remaining == 0 && hunk.new_remaining == 0) {
      region = Region::metadata;
      old_state = {};
      new_state = {};
    }
  }
  return rows.empty() ? ftxui::text("")
                      : ftxui::vbox(std::move(rows)) |
                            ftxui::borderStyled(ftxui::LIGHT, theme.border);
}

ftxui::Element render_tool_output(std::string_view text,
                                  const TerminalTheme &theme) {
  if (looks_like_diff(text))
    return render_diff(text, theme);
  const auto lines = split_lines(text);
  for (std::size_t index = 0; index < lines.size(); ++index) {
    if (!is_diff_start(lines, index))
      continue;
    std::size_t end = index + 1;
    while (end < lines.size() && !is_diff_start(lines, end))
      ++end;
    std::string candidate;
    for (std::size_t row = index; row < end; ++row) {
      if (row != index)
        candidate += '\n';
      candidate += lines[row];
    }
    if (!looks_like_diff(candidate))
      continue;
    Lines before(lines.begin(),
                 lines.begin() + static_cast<std::ptrdiff_t>(index));
    Lines after(lines.begin() + static_cast<std::ptrdiff_t>(end), lines.end());
    ftxui::Elements blocks;
    if (!before.empty())
      blocks.push_back(render_plain_lines(before, theme));
    blocks.push_back(render_diff(candidate, theme));
    if (!after.empty())
      blocks.push_back(render_plain_lines(after, theme));
    return ftxui::vbox(std::move(blocks));
  }
  return render_plain_lines(lines, theme);
}

} // namespace zed::ui
