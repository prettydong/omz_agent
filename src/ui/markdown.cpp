#include "zed/ui/markdown.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/string.hpp>

namespace zed::ui {

namespace {

using Lines = std::vector<std::string>;

enum class TableAlignment {
  left,
  center,
  right,
};

enum class InlineTone {
  normal,
  bold,
  italic,
  link,
  code,
};

std::string trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

Lines split_lines(std::string_view markdown) {
  Lines lines;
  std::size_t begin = 0;
  while (begin <= markdown.size()) {
    const auto end = markdown.find('\n', begin);
    const auto line_end = end == std::string_view::npos ? markdown.size() : end;
    std::string line(markdown.substr(begin, line_end - begin));
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return lines;
}

bool is_blank(std::string_view line) { return trim(line).empty(); }

std::vector<std::string> split_table_row(std::string_view line) {
  auto value = trim(line);
  if (!value.empty() && value.front() == '|')
    value.erase(0, 1);
  if (!value.empty() && value.back() == '|')
    value.pop_back();

  std::vector<std::string> cells;
  std::string cell;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '|') {
      cell.push_back('|');
      ++i;
    } else if (value[i] == '|') {
      cells.push_back(trim(cell));
      cell.clear();
    } else {
      cell.push_back(value[i]);
    }
  }
  cells.push_back(trim(cell));
  return cells;
}

bool is_table_separator(std::string_view line) {
  const auto cells = split_table_row(line);
  if (cells.empty())
    return false;
  for (const auto &raw_cell : cells) {
    auto cell = trim(raw_cell);
    if (!cell.empty() && cell.front() == ':')
      cell.erase(0, 1);
    if (!cell.empty() && cell.back() == ':')
      cell.pop_back();
    if (cell.size() < 3 ||
        !std::all_of(cell.begin(), cell.end(),
                     [](char character) { return character == '-'; })) {
      return false;
    }
  }
  return true;
}

bool is_table_at(const Lines &lines, std::size_t index) {
  if (index + 1 >= lines.size())
    return false;
  if (lines[index].find('|') == std::string::npos)
    return false;
  if (!is_table_separator(lines[index + 1]))
    return false;
  const auto header = split_table_row(lines[index]);
  const auto separator = split_table_row(lines[index + 1]);
  return !header.empty() && header.size() == separator.size();
}

bool is_fence(std::string_view line, char &marker, std::size_t &length) {
  const auto value = trim(line);
  if (value.size() < 3 || (value.front() != '`' && value.front() != '~')) {
    return false;
  }
  marker = value.front();
  length = 0;
  while (length < value.size() && value[length] == marker)
    ++length;
  return length >= 3;
}

bool is_heading(std::string_view line) {
  const auto value = trim(line);
  std::size_t hashes = 0;
  while (hashes < value.size() && value[hashes] == '#')
    ++hashes;
  return hashes > 0 && hashes <= 6 && hashes < value.size() &&
         value[hashes] == ' ';
}

bool is_horizontal_rule(std::string_view line) {
  const auto value = trim(line);
  if (value.size() < 3)
    return false;
  char marker = value.front();
  if (marker != '-' && marker != '*' && marker != '_')
    return false;
  for (const auto character : value) {
    if (character != marker && character != ' ')
      return false;
  }
  return true;
}

bool is_list_item(std::string_view line) {
  const auto value = trim(line);
  if (value.size() >= 2 &&
      (value[0] == '-' || value[0] == '*' || value[0] == '+') &&
      value[1] == ' ') {
    return true;
  }
  std::size_t digits = 0;
  while (digits < value.size() &&
         std::isdigit(static_cast<unsigned char>(value[digits])) != 0) {
    ++digits;
  }
  return digits > 0 && digits + 1 < value.size() && value[digits] == '.' &&
         value[digits + 1] == ' ';
}

std::string list_item_text(std::string_view line) {
  auto value = trim(line);
  if (!value.empty() &&
      (value.front() == '-' || value.front() == '*' || value.front() == '+')) {
    return trim(value.substr(1));
  }
  const auto dot = value.find('.');
  return dot == std::string::npos ? value : trim(value.substr(dot + 1));
}

bool is_quote(std::string_view line) {
  const auto value = trim(line);
  return !value.empty() && value.front() == '>';
}

std::string quote_text(std::string_view line) {
  const auto value = trim(line);
  return trim(value.substr(1));
}

std::size_t find_closing(std::string_view value, std::size_t begin,
                         std::string_view marker) {
  return value.find(marker, begin);
}

ftxui::Element style_inline(std::string value, InlineTone tone,
                            const TerminalTheme &theme) {
  auto element = ftxui::text(std::move(value));
  switch (tone) {
  case InlineTone::bold:
    return element | ftxui::bold | ftxui::color(theme.markdown_strong);
  case InlineTone::italic:
    return element | ftxui::italic | ftxui::color(theme.markdown_emphasis);
  case InlineTone::link:
    return element | ftxui::underlined | ftxui::color(theme.markdown_link);
  case InlineTone::code:
    return element | ftxui::bold | ftxui::color(theme.markdown_code);
  case InlineTone::normal:
    return element | ftxui::color(theme.markdown_text);
  }
  return element;
}

void append_inline_text(ftxui::Elements &elements, std::string_view value,
                        InlineTone tone, const TerminalTheme &theme) {
  std::string ascii_word;
  const auto flush_ascii = [&] {
    if (ascii_word.empty())
      return;
    elements.push_back(style_inline(std::move(ascii_word), tone, theme));
    ascii_word.clear();
  };

  for (const auto &glyph : ftxui::Utf8ToGlyphs(value)) {
    if (glyph.empty())
      continue;
    const bool ascii =
        glyph.size() == 1 && static_cast<unsigned char>(glyph.front()) < 0x80U;
    if (ascii && glyph.front() != ' ' && glyph.front() != '\t') {
      ascii_word += glyph;
      if (ascii_word.size() >= 24)
        flush_ascii();
      continue;
    }
    flush_ascii();
    elements.push_back(style_inline(glyph, tone, theme));
  }
  flush_ascii();
}

ftxui::Element render_inline(std::string_view value,
                             const TerminalTheme &theme) {
  ftxui::Elements elements;
  std::string plain;
  const auto flush_plain = [&] {
    if (!plain.empty()) {
      append_inline_text(elements, plain, InlineTone::normal, theme);
      plain.clear();
    }
  };

  for (std::size_t i = 0; i < value.size();) {
    if (value[i] == '`') {
      const auto end = find_closing(value, i + 1, "`");
      if (end != std::string_view::npos) {
        flush_plain();
        append_inline_text(elements, value.substr(i + 1, end - i - 1),
                           InlineTone::code, theme);
        i = end + 1;
        continue;
      }
    }

    const std::string_view emphasis =
        value.substr(i, std::min<std::size_t>(2, value.size() - i));
    if (emphasis == "**" || emphasis == "__") {
      const auto end = find_closing(value, i + 2, emphasis);
      if (end != std::string_view::npos) {
        flush_plain();
        append_inline_text(elements, value.substr(i + 2, end - i - 2),
                           InlineTone::bold, theme);
        i = end + 2;
        continue;
      }
    }

    if (value[i] == '*' && (i + 1 >= value.size() || value[i + 1] != '*')) {
      const auto end = find_closing(value, i + 1, "*");
      if (end != std::string_view::npos && end > i + 1) {
        flush_plain();
        append_inline_text(elements, value.substr(i + 1, end - i - 1),
                           InlineTone::italic, theme);
        i = end + 1;
        continue;
      }
    }

    if (value[i] == '[') {
      const auto label_end = value.find(']', i + 1);
      if (label_end != std::string_view::npos && label_end + 1 < value.size() &&
          value[label_end + 1] == '(') {
        const auto url_end = value.find(')', label_end + 2);
        if (url_end != std::string_view::npos) {
          flush_plain();
          append_inline_text(elements, value.substr(i + 1, label_end - i - 1),
                             InlineTone::link, theme);
          i = url_end + 1;
          continue;
        }
      }
    }

    plain.push_back(value[i]);
    ++i;
  }
  flush_plain();

  if (elements.empty())
    return ftxui::text("");
  if (elements.size() == 1)
    return std::move(elements.front());
  return ftxui::hflow(std::move(elements));
}

ftxui::Element render_text_block(const Lines &lines, std::size_t begin,
                                 std::size_t end, const TerminalTheme &theme) {
  std::string paragraph;
  for (std::size_t i = begin; i < end; ++i) {
    if (!paragraph.empty())
      paragraph += ' ';
    paragraph += trim(lines[i]);
  }
  return render_inline(paragraph, theme);
}

ftxui::Element render_table(const Lines &lines, std::size_t &index,
                            const TerminalTheme &theme) {
  auto header = split_table_row(lines[index]);
  const auto separator_cells = split_table_row(lines[index + 1]);
  const auto column_count = separator_cells.size();

  std::vector<TableAlignment> alignments;
  alignments.reserve(column_count);
  for (const auto &separator : separator_cells) {
    const auto value = trim(separator);
    const bool left_colon = !value.empty() && value.front() == ':';
    const bool right_colon = !value.empty() && value.back() == ':';
    if (left_colon && right_colon) {
      alignments.push_back(TableAlignment::center);
    } else if (right_colon) {
      alignments.push_back(TableAlignment::right);
    } else {
      alignments.push_back(TableAlignment::left);
    }
  }

  std::vector<std::vector<std::string>> rows;
  rows.push_back(std::move(header));
  index += 2;
  while (index < lines.size() && !is_blank(lines[index]) &&
         lines[index].find('|') != std::string::npos &&
         !is_table_separator(lines[index])) {
    auto row = split_table_row(lines[index]);
    row.resize(column_count);
    rows.push_back(std::move(row));
    ++index;
  }

  std::vector<std::vector<ftxui::Element>> rendered_rows;
  rendered_rows.reserve(rows.size());
  for (const auto &row : rows) {
    std::vector<ftxui::Element> rendered_row;
    rendered_row.reserve(row.size());
    for (std::size_t column = 0; column < row.size(); ++column) {
      auto cell = render_inline(row[column], theme);
      switch (alignments[column]) {
      case TableAlignment::left:
        rendered_row.push_back(std::move(cell));
        break;
      case TableAlignment::center:
        rendered_row.push_back(ftxui::hcenter(std::move(cell)));
        break;
      case TableAlignment::right:
        rendered_row.push_back(ftxui::align_right(std::move(cell)));
        break;
      }
    }
    rendered_rows.push_back(std::move(rendered_row));
  }

  ftxui::Table table(std::move(rendered_rows));
  table.SelectAll().Border(ftxui::LIGHT, ftxui::color(theme.border));
  table.SelectAll().Separator(ftxui::LIGHT, ftxui::color(theme.border));
  table.SelectRow(0).DecorateCells(ftxui::bold);
  return table.Render();
}

} // namespace

ftxui::Element render_markdown(std::string_view markdown,
                               const TerminalTheme &theme) {
  const auto lines = split_lines(markdown);
  ftxui::Elements blocks;

  for (std::size_t index = 0; index < lines.size();) {
    if (is_blank(lines[index])) {
      ++index;
      continue;
    }

    char fence_marker = 0;
    std::size_t fence_length = 0;
    if (is_fence(lines[index], fence_marker, fence_length)) {
      ++index;
      std::string code;
      while (index < lines.size()) {
        char closing_marker = 0;
        std::size_t closing_length = 0;
        if (is_fence(lines[index], closing_marker, closing_length) &&
            closing_marker == fence_marker && closing_length >= fence_length) {
          ++index;
          break;
        }
        if (!code.empty())
          code += '\n';
        code += lines[index++];
      }
      blocks.push_back(ftxui::paragraph(std::move(code)) |
                       ftxui::color(theme.markdown_code_block) |
                       ftxui::bgcolor(theme.background_panel) |
                       ftxui::borderStyled(ftxui::LIGHT, theme.border));
      continue;
    }

    if (is_table_at(lines, index)) {
      blocks.push_back(render_table(lines, index, theme));
      continue;
    }

    const auto value = trim(lines[index]);
    if (is_heading(lines[index])) {
      const auto first_space = value.find(' ');
      blocks.push_back(render_inline(value.substr(first_space + 1), theme) |
                       ftxui::bold | ftxui::color(theme.markdown_heading));
      ++index;
      continue;
    }

    if (is_horizontal_rule(lines[index])) {
      blocks.push_back(ftxui::separator() | ftxui::color(theme.markdown_rule));
      ++index;
      continue;
    }

    if (is_list_item(lines[index])) {
      ftxui::Elements items;
      while (index < lines.size() && is_list_item(lines[index])) {
        const auto item = list_item_text(lines[index++]);
        items.push_back(ftxui::hbox({
            ftxui::text("• ") | ftxui::color(theme.markdown_list),
            render_inline(item, theme),
        }));
      }
      blocks.push_back(ftxui::vbox(std::move(items)));
      continue;
    }

    if (is_quote(lines[index])) {
      ftxui::Elements quotes;
      while (index < lines.size() && is_quote(lines[index])) {
        quotes.push_back(ftxui::hbox({
            ftxui::text("│ ") | ftxui::color(theme.markdown_quote),
            render_inline(quote_text(lines[index++]), theme),
        }));
      }
      blocks.push_back(ftxui::vbox(std::move(quotes)) |
                       ftxui::color(theme.markdown_quote));
      continue;
    }

    const auto begin = index;
    while (index < lines.size() && !is_blank(lines[index]) &&
           !is_heading(lines[index]) && !is_horizontal_rule(lines[index]) &&
           !is_list_item(lines[index]) && !is_quote(lines[index]) &&
           !is_table_at(lines, index)) {
      char ignored_marker = 0;
      std::size_t ignored_length = 0;
      if (is_fence(lines[index], ignored_marker, ignored_length))
        break;
      ++index;
    }
    blocks.push_back(render_text_block(lines, begin, index, theme));
  }

  if (blocks.empty())
    return ftxui::text("");
  return ftxui::vbox(std::move(blocks));
}

ftxui::Element render_markdown(std::string_view markdown) {
  return render_markdown(markdown, terminal_theme(ThemeKind::light));
}

} // namespace zed::ui
