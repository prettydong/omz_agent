#include "zed/ui/terminal.hpp"

#include "zed/core/utf8.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zed/ui/markdown.hpp"

#include <nlohmann/json.hpp>

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace zed::ui {

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaximumDocumentViewBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumDocumentPages = 128;
constexpr std::size_t kMaximumDocumentPageBytes = 2 * 1024 * 1024;

class LayoutBoxCapture final : public ftxui::Node {
public:
  LayoutBoxCapture(ftxui::Element child, ftxui::Box &captured_box)
      : Node(ftxui::unpack(std::move(child))), captured_box_(captured_box) {}

  void SetBox(ftxui::Box box) override {
    captured_box_ = box;
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }

private:
  ftxui::Box &captured_box_;
};

class BlockCursorOverride final : public ftxui::Node {
public:
  explicit BlockCursorOverride(ftxui::Element child)
      : Node(ftxui::unpack(std::move(child))) {}

  void ComputeRequirement() override {
    Node::ComputeRequirement();
    if (requirement_.focused.enabled) {
      requirement_.focused.cursor_shape = ftxui::Screen::Cursor::BlockBlinking;
    }
  }

  void SetBox(ftxui::Box box) override {
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }
};

class FullWidthBoundaryGuard final : public ftxui::Node {
public:
  FullWidthBoundaryGuard(ftxui::Element child, ftxui::Color background_color)
      : Node(ftxui::unpack(std::move(child))),
        background_color_(background_color) {}

  void SetBox(ftxui::Box box) override {
    Node::SetBox(box);
    children_[0]->SetBox(box);
  }

  void Render(ftxui::Screen &screen) override {
    const int preceding_column = box_.x_min - 1;
    if (preceding_column >= 0 && preceding_column < screen.dimx()) {
      const int first_row = std::max(0, box_.y_min);
      const int last_row = std::min(screen.dimy() - 1, box_.y_max);
      for (int row = first_row; row <= last_row; ++row) {
        auto &cell = screen.CellAt(preceding_column, row);
        cell = ftxui::Cell{};
        cell.character = " ";
        cell.background_color = background_color_;
      }
    }
    Node::Render(screen);
  }

private:
  ftxui::Color background_color_;
};

ftxui::Element guard_full_width_boundary(ftxui::Element child,
                                         ftxui::Color background_color) {
  return std::make_shared<FullWidthBoundaryGuard>(std::move(child),
                                                  background_color);
}

std::string render_element(ftxui::Element element) {
  const auto width = ftxui::Dimension::Fit(element);
  const auto height = ftxui::Dimension::Fit(element);
  auto screen = ftxui::Screen::Create(width, height);
  ftxui::Render(screen, std::move(element));
  return screen.ToString();
}

bool is_command_whitespace(char character) {
  return character == ' ' || character == '\t' || character == '\r' ||
         character == '\n';
}

bool valid_document_label(std::string_view value, std::size_t maximum,
                          bool allow_empty = false) {
  if ((!allow_empty && value.empty()) || value.size() > maximum ||
      !core::is_valid_utf8(value)) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

std::string_view
page_markdown_without_repeated_title(const TerminalDocumentPage &page) {
  if (!page.markdown.starts_with("# "))
    return page.markdown;
  const auto newline = page.markdown.find('\n');
  const auto heading = page.markdown.substr(
      2, newline == std::string::npos ? std::string::npos : newline - 2);
  if (heading != page.title || newline == std::string::npos)
    return page.markdown;
  std::string_view remaining(page.markdown);
  remaining.remove_prefix(newline + 1);
  while (!remaining.empty() &&
         (remaining.front() == '\r' || remaining.front() == '\n')) {
    remaining.remove_prefix(1);
  }
  return remaining;
}

std::string encode_base64(std::string_view input) {
  constexpr std::array<char, 64> kAlphabet{
      'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
      'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
      'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
      'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/',
  };
  const auto alphabet_at = [&](std::uint32_t index) {
    return kAlphabet[static_cast<std::size_t>(index)];
  };

  std::string encoded;
  encoded.reserve(((input.size() + 2) / 3) * 4);
  std::size_t index = 0;
  while (index + 3 <= input.size()) {
    const auto value =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(input[index]))
         << 16U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(input[index + 1]))
         << 8U) |
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(input[index + 2]));
    encoded.push_back(alphabet_at((value >> 18U) & 0x3FU));
    encoded.push_back(alphabet_at((value >> 12U) & 0x3FU));
    encoded.push_back(alphabet_at((value >> 6U) & 0x3FU));
    encoded.push_back(alphabet_at(value & 0x3FU));
    index += 3;
  }

  const auto remaining = input.size() - index;
  if (remaining == 1) {
    const auto value =
        static_cast<std::uint32_t>(static_cast<unsigned char>(input[index]))
        << 16U;
    encoded.push_back(alphabet_at((value >> 18U) & 0x3FU));
    encoded.push_back(alphabet_at((value >> 12U) & 0x3FU));
    encoded += "==";
  } else if (remaining == 2) {
    const auto value =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(input[index]))
         << 16U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(input[index + 1]))
         << 8U);
    encoded.push_back(alphabet_at((value >> 18U) & 0x3FU));
    encoded.push_back(alphabet_at((value >> 12U) & 0x3FU));
    encoded.push_back(alphabet_at((value >> 6U) & 0x3FU));
    encoded.push_back('=');
  }
  return encoded;
}

std::string compact_token_count(core::TokenCount count) {
  core::TokenCount divisor = 1;
  std::string_view suffix;
  if (count >= 1'000'000) {
    divisor = 1'000'000;
    suffix = "m";
  } else if (count >= 1'000) {
    divisor = 1'000;
    suffix = "k";
  } else {
    return std::to_string(count);
  }

  const auto whole = count / divisor;
  const auto decimal = ((count % divisor) * 10) / divisor;
  auto formatted = std::to_string(whole);
  if (decimal != 0)
    formatted += "." + std::to_string(decimal);
  formatted += suffix;
  return formatted;
}

std::string context_percentage(core::TokenCount context_tokens,
                               core::TokenCount max_context_tokens) {
  const auto percentage = static_cast<double>(context_tokens) /
                          static_cast<double>(max_context_tokens) * 100.0;
  std::ostringstream formatted;
  formatted << std::fixed << std::setprecision(0) << percentage << '%';
  return formatted.str();
}

std::string token_rate(double output_tokens_per_second) {
  std::ostringstream formatted;
  formatted << std::fixed << std::setprecision(1) << output_tokens_per_second
            << " tok/s";
  return formatted.str();
}

std::string context_share(core::TokenCount tokens,
                          core::TokenCount context_tokens) {
  if (context_tokens == 0)
    return "0.0%";
  const auto percentage =
      static_cast<double>(tokens) / static_cast<double>(context_tokens) * 100.0;
  std::ostringstream formatted;
  formatted << std::fixed << std::setprecision(1) << percentage << '%';
  return formatted.str();
}

ftxui::Element context_breakdown_row(std::string label, core::TokenCount tokens,
                                     core::TokenCount context_tokens,
                                     const TerminalTheme &theme) {
  const auto ratio =
      context_tokens == 0
          ? 0.0F
          : static_cast<float>(tokens) / static_cast<float>(context_tokens);
  return ftxui::hbox({
      ftxui::text(std::move(label)) | ftxui::bold | ftxui::color(theme.text) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 19),
      ftxui::gauge(ratio) | ftxui::color(theme.secondary) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 14),
      ftxui::text("  " + compact_token_count(tokens)) | ftxui::bold |
          ftxui::color(theme.primary) |
          ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 9),
      ftxui::text(context_share(tokens, context_tokens)) | ftxui::bold |
          ftxui::color(theme.secondary),
  });
}

std::size_t box_rows(const ftxui::Box &box) {
  if (box.y_max < box.y_min)
    return 0;
  return static_cast<std::size_t>(box.y_max - box.y_min) + 1;
}

std::size_t layout_box_rows(const ftxui::Box &box) {
  if (box.y_max <= box.y_min)
    return 0;
  return static_cast<std::size_t>(box.y_max - box.y_min);
}

std::size_t maximum_scroll_offset(std::size_t content_rows,
                                  std::size_t viewport_rows) {
  return content_rows > viewport_rows ? content_rows - viewport_rows : 0;
}

std::string_view trim_command_whitespace(std::string_view value) {
  while (!value.empty() && is_command_whitespace(value.front()))
    value.remove_prefix(1);
  while (!value.empty() && is_command_whitespace(value.back()))
    value.remove_suffix(1);
  return value;
}

ftxui::Element render_three_line_user_surface(ftxui::Element content,
                                              const TerminalTheme &theme) {
  return ftxui::vbox({
             ftxui::separatorEmpty(),
             std::move(content),
             ftxui::separatorEmpty(),
         }) |
         ftxui::bgcolor(theme.input_background);
}

ftxui::Element render_message(const TerminalMessage &message,
                              const TerminalTheme &theme) {
  switch (message.kind) {
  case TerminalMessageKind::user: {
    auto content = ftxui::hbox({
        ftxui::text("› ") | ftxui::bold | ftxui::color(theme.primary),
        ftxui::paragraph(message.content) | ftxui::bold |
            ftxui::color(theme.text) | ftxui::xflex,
    });
    return render_three_line_user_surface(std::move(content), theme);
  }
  case TerminalMessageKind::assistant:
    return ftxui::hbox({
        ftxui::text("• ") | ftxui::bold | ftxui::color(theme.success),
        render_markdown(message.content, theme) | ftxui::xflex,
    });
  case TerminalMessageKind::tool: {
    const auto color = message.is_error ? theme.error : theme.warning;
    const auto state = message.is_error                ? "failed"
                       : message.content == "running…" ? "running"
                                                       : "done";
    auto header = ftxui::hbox({
        ftxui::text(message.expanded ? "▾ " : "▸ ") | ftxui::color(color),
        ftxui::text(message.label) | ftxui::bold | ftxui::color(theme.text),
        ftxui::text(" · " + std::string(state) +
                    (message.expanded ? " · click to collapse"
                                      : " · click to expand")) |
            ftxui::color(theme.text_muted),
    });
    if (!message.expanded)
      return header;
    return ftxui::vbox({
        std::move(header),
        ftxui::hbox({
            ftxui::text("  "),
            ftxui::paragraph(message.content) |
                ftxui::color(message.is_error ? theme.error
                                              : theme.text_muted) |
                ftxui::xflex,
        }),
    });
  }
  case TerminalMessageKind::command: {
    const auto color = message.is_error ? theme.error : theme.primary;
    const bool running = message.content == "running…";
    const auto state = message.is_error ? "failed"
                       : running        ? "running"
                                        : "done";
    if (!message.expanded) {
      return ftxui::hbox({
          ftxui::text(message.is_error ? "⚠ " : "▸ ") | ftxui::color(color),
          ftxui::paragraph(message.label) | ftxui::bold | ftxui::color(color) |
              ftxui::xflex,
          ftxui::text(" · " + std::string(state) + " · click to expand") |
              ftxui::color(theme.text_muted),
      });
    }
    return ftxui::vbox({
        ftxui::hbox({
            ftxui::text("▾ ") | ftxui::color(color),
            ftxui::paragraph(message.label) | ftxui::bold |
                ftxui::color(color) | ftxui::xflex,
            ftxui::text(" · " + std::string(state) + " · click to collapse") |
                ftxui::color(theme.text_muted),
        }),
        ftxui::hbox({
            ftxui::text("  "),
            ftxui::paragraph(message.content) | ftxui::color(color) |
                ftxui::xflex,
        }),
    });
  }
  case TerminalMessageKind::error:
    return ftxui::hbox({
        ftxui::text("⚠ ") | ftxui::bold | ftxui::color(theme.error),
        ftxui::paragraph(message.content) | ftxui::color(theme.error) |
            ftxui::xflex,
    });
  }
  return ftxui::text("");
}

ftxui::Element
render_messages(const std::vector<TerminalMessage> &messages,
                const TerminalTheme &theme,
                std::vector<ftxui::Box> *message_boxes = nullptr) {
  if (message_boxes != nullptr)
    message_boxes->assign(messages.size(), {});
  ftxui::Elements elements;
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (!elements.empty())
      elements.push_back(ftxui::separatorEmpty());
    auto element = render_message(messages[index], theme);
    if (message_boxes != nullptr && messages[index].collapsible)
      element |= ftxui::reflect((*message_boxes)[index]);
    elements.push_back(std::move(element));
  }
  if (elements.empty())
    return ftxui::text("");
  return ftxui::vbox(std::move(elements));
}

struct CommandCompletionInput {
  bool command{false};
  std::string_view name;
  bool has_arguments{false};
  std::string_view option_prefix;
  bool has_additional_arguments{false};
};

CommandCompletionInput parse_command_completion_input(std::string_view input) {
  if (!input.starts_with('/'))
    return {};
  input.remove_prefix(1);
  const auto separator =
      std::find_if(input.begin(), input.end(), is_command_whitespace);
  const auto name_length =
      static_cast<std::size_t>(std::distance(input.begin(), separator));
  CommandCompletionInput parsed;
  parsed.command = true;
  parsed.name = input.substr(0, name_length);
  if (separator == input.end())
    return parsed;

  parsed.has_arguments = true;
  auto arguments = input.substr(name_length);
  while (!arguments.empty() && is_command_whitespace(arguments.front()))
    arguments.remove_prefix(1);
  const auto argument_separator =
      std::find_if(arguments.begin(), arguments.end(), is_command_whitespace);
  const auto argument_length = static_cast<std::size_t>(
      std::distance(arguments.begin(), argument_separator));
  parsed.option_prefix = arguments.substr(0, argument_length);
  parsed.has_additional_arguments = argument_separator != arguments.end();
  return parsed;
}

ftxui::Element render_activity(TerminalActivity activity,
                               core::ReasoningEffort reasoning_effort,
                               std::size_t frame, const TerminalTheme &theme) {
  ftxui::Color color;
  switch (activity) {
  case TerminalActivity::idle:
    color = theme.text_muted;
    break;
  case TerminalActivity::thinking:
    color = theme.warning;
    break;
  case TerminalActivity::action:
    color = theme.info;
    break;
  case TerminalActivity::stream:
    color = theme.success;
    break;
  case TerminalActivity::cancelling:
    color = theme.error;
    break;
  }
  const auto label = terminal_activity_label(activity, reasoning_effort);
  if (activity == TerminalActivity::idle) {
    return ftxui::text("○ " + label) | ftxui::bold | ftxui::color(color);
  }
  return ftxui::hbox({
      ftxui::spinner(12, frame) | ftxui::bold | ftxui::color(color),
      ftxui::text(" " + label) | ftxui::bold | ftxui::color(color),
  });
}

} // namespace

ftxui::Decorator capture_layout_box(ftxui::Box &box) {
  return [&box](ftxui::Element child) {
    return std::make_shared<LayoutBoxCapture>(std::move(child), box);
  };
}

TerminalCommand parse_terminal_command(std::string_view line) {
  line = trim_command_whitespace(line);
  if (!line.empty() && line.front() == '/')
    line.remove_prefix(1);
  line = trim_command_whitespace(line);

  const auto separator =
      std::find_if(line.begin(), line.end(), is_command_whitespace);
  const auto name_length =
      static_cast<std::size_t>(std::distance(line.begin(), separator));
  const auto arguments = trim_command_whitespace(line.substr(name_length));
  return {std::string(line.substr(0, name_length)), std::string(arguments)};
}

bool terminal_command_reloads_session(std::string_view name,
                                      std::string_view arguments) {
  if (name != "session")
    return false;
  arguments = trim_command_whitespace(arguments);
  if (arguments.empty())
    return false;
  const auto separator =
      std::find_if(arguments.begin(), arguments.end(), is_command_whitespace);
  const auto subcommand = arguments.substr(
      0, static_cast<std::size_t>(std::distance(arguments.begin(), separator)));
  return subcommand != "list" && subcommand != "rename";
}

std::vector<TerminalCommandSuggestion> terminal_command_suggestions(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands) {
  const auto parsed = parse_command_completion_input(input);
  if (!parsed.command)
    return {};

  const auto exact_command =
      std::find_if(available_commands.begin(), available_commands.end(),
                   [&](const TerminalCommandHint &command) {
                     return command.name == parsed.name;
                   });
  std::vector<TerminalCommandSuggestion> suggestions;
  if (exact_command != available_commands.end()) {
    if (parsed.has_additional_arguments)
      return suggestions;
    for (const auto &option : exact_command->options) {
      if (option.value.starts_with(parsed.option_prefix)) {
        suggestions.push_back({option.value, option.description,
                               "/" + exact_command->name + " " + option.value,
                               true});
      }
    }
    return suggestions;
  }
  if (parsed.has_arguments)
    return suggestions;

  for (const auto &command : available_commands) {
    if (command.name.starts_with(parsed.name)) {
      suggestions.push_back({"/" + command.name, command.description,
                             "/" + command.name + " ", false});
    }
  }
  return suggestions;
}

const TerminalCommandHint *terminal_command_help(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands) {
  const auto parsed = parse_command_completion_input(input);
  if (!parsed.command)
    return nullptr;
  const auto command =
      std::find_if(available_commands.begin(), available_commands.end(),
                   [&](const TerminalCommandHint &candidate) {
                     return candidate.name == parsed.name;
                   });
  return command == available_commands.end() ? nullptr : &*command;
}

std::string complete_terminal_command(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands,
    std::size_t selected_suggestion) {
  const auto suggestions =
      terminal_command_suggestions(input, available_commands);
  if (suggestions.empty())
    return std::string(input);
  const auto index = std::min(selected_suggestion, suggestions.size() - 1);
  return suggestions[index].completion;
}

ftxui::Element render_terminal_command_guide(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands,
    std::size_t selected_suggestion, const TerminalTheme &theme,
    std::size_t max_visible_suggestions) {
  const auto suggestions =
      terminal_command_suggestions(input, available_commands);
  const auto *command_help = terminal_command_help(input, available_commands);
  if (selected_suggestion >= suggestions.size())
    selected_suggestion = 0;
  const auto first_visible =
      max_visible_suggestions == 0 ||
              selected_suggestion < max_visible_suggestions
          ? 0
          : selected_suggestion - max_visible_suggestions + 1;
  const auto visible = max_visible_suggestions == 0
                           ? std::size_t{}
                           : std::min(suggestions.size() - first_visible,
                                      max_visible_suggestions);

  ftxui::Elements rows;
  rows.reserve(visible + 2);
  if (command_help != nullptr) {
    rows.push_back(ftxui::hbox({
        ftxui::text("/" + command_help->name) | ftxui::bold |
            ftxui::color(theme.primary),
        ftxui::text("  " + command_help->description) |
            ftxui::color(theme.text_muted),
    }));
  }
  if (visible > 0) {
    rows.push_back(
        ftxui::text(
            suggestions.front().option
                ? "options · ↑/↓ select · enter run/complete · tab complete"
                : "commands · ↑/↓ select · enter run/complete · tab complete") |
        ftxui::color(theme.text_muted));
  }
  for (std::size_t offset = 0; offset < visible; ++offset) {
    const auto index = first_visible + offset;
    const auto &suggestion = suggestions[index];
    const bool selected = index == selected_suggestion;
    auto row = ftxui::hbox({
        ftxui::text(selected ? "› " : "  ") | ftxui::color(theme.secondary),
        ftxui::text(suggestion.value) | ftxui::bold |
            ftxui::color(selected ? theme.secondary : theme.primary),
        ftxui::text("  " + suggestion.description) |
            ftxui::color(theme.text_muted),
    });
    if (selected)
      row |= ftxui::bgcolor(theme.input_background);
    rows.push_back(std::move(row));
  }
  return ftxui::vbox(std::move(rows));
}

bool is_terminal_command_completion_event(const ftxui::Event &event) {
  return event == ftxui::Event::Return || event == ftxui::Event::Tab;
}

bool terminal_command_opens_document_view(
    std::string_view name, std::string_view arguments,
    const std::vector<TerminalCommandHint> &available_commands) {
  const auto command =
      std::find_if(available_commands.begin(), available_commands.end(),
                   [&](const TerminalCommandHint &candidate) {
                     return candidate.name == name;
                   });
  if (command == available_commands.end())
    return false;
  const auto option =
      std::find_if(command->options.begin(), command->options.end(),
                   [&](const TerminalCommandOption &candidate) {
                     return candidate.value == arguments;
                   });
  return option != command->options.end() && option->opens_document_view;
}

core::Result<TerminalDocumentView>
parse_terminal_document_view(std::string_view json) {
  if (json.empty() || json.size() > kMaximumDocumentViewBytes) {
    return core::Result<TerminalDocumentView>::failure({
        core::ErrorCode::invalid_argument,
        "terminal document view must contain at most 16 MiB of JSON",
    });
  }
  const auto document = Json::parse(json, nullptr, false);
  if (!document.is_object() || !document.contains("schema_version") ||
      !document.at("schema_version").is_number_unsigned() ||
      document.at("schema_version").get<std::uint64_t>() != 1 ||
      !document.contains("title") || !document.at("title").is_string() ||
      !document.contains("pages") || !document.at("pages").is_array()) {
    return core::Result<TerminalDocumentView>::failure({
        core::ErrorCode::invalid_argument,
        "terminal document view has an invalid schema",
    });
  }

  TerminalDocumentView result;
  result.title = document.at("title").get<std::string>();
  if (document.contains("subtitle")) {
    if (!document.at("subtitle").is_string()) {
      return core::Result<TerminalDocumentView>::failure({
          core::ErrorCode::invalid_argument,
          "terminal document view subtitle must be text",
      });
    }
    result.subtitle = document.at("subtitle").get<std::string>();
  }
  if (!valid_document_label(result.title, 256) ||
      !valid_document_label(result.subtitle, 2'048, true)) {
    return core::Result<TerminalDocumentView>::failure({
        core::ErrorCode::invalid_argument,
        "terminal document view contains invalid metadata",
    });
  }

  const auto &pages = document.at("pages");
  if (pages.empty() || pages.size() > kMaximumDocumentPages) {
    return core::Result<TerminalDocumentView>::failure({
        core::ErrorCode::invalid_argument,
        "terminal document view requires between 1 and 128 pages",
    });
  }
  result.pages.reserve(pages.size());
  for (const auto &page : pages) {
    if (!page.is_object() || !page.contains("id") ||
        !page.at("id").is_string() || !page.contains("title") ||
        !page.at("title").is_string() || !page.contains("markdown") ||
        !page.at("markdown").is_string()) {
      return core::Result<TerminalDocumentView>::failure({
          core::ErrorCode::invalid_argument,
          "terminal document view contains an invalid page",
      });
    }
    TerminalDocumentPage parsed;
    parsed.id = page.at("id").get<std::string>();
    parsed.title = page.at("title").get<std::string>();
    parsed.markdown = page.at("markdown").get<std::string>();
    if (page.contains("description")) {
      if (!page.at("description").is_string()) {
        return core::Result<TerminalDocumentView>::failure({
            core::ErrorCode::invalid_argument,
            "terminal document page description must be text",
        });
      }
      parsed.description = page.at("description").get<std::string>();
    }
    if (page.contains("badge")) {
      if (!page.at("badge").is_string()) {
        return core::Result<TerminalDocumentView>::failure({
            core::ErrorCode::invalid_argument,
            "terminal document page badge must be text",
        });
      }
      parsed.badge = page.at("badge").get<std::string>();
    }
    if (!valid_document_label(parsed.id, 128) ||
        !valid_document_label(parsed.title, 256) ||
        !valid_document_label(parsed.description, 2'048, true) ||
        !valid_document_label(parsed.badge, 64, true) ||
        parsed.markdown.empty() ||
        parsed.markdown.size() > kMaximumDocumentPageBytes ||
        parsed.markdown.find('\0') != std::string::npos ||
        !core::is_valid_utf8(parsed.markdown)) {
      return core::Result<TerminalDocumentView>::failure({
          core::ErrorCode::invalid_argument,
          "terminal document view page exceeds its content limits",
      });
    }
    const auto duplicate =
        std::find_if(result.pages.begin(), result.pages.end(),
                     [&](const TerminalDocumentPage &existing) {
                       return existing.id == parsed.id;
                     });
    if (duplicate != result.pages.end()) {
      return core::Result<TerminalDocumentView>::failure({
          core::ErrorCode::conflict,
          "terminal document view contains duplicate page ids",
      });
    }
    result.pages.push_back(std::move(parsed));
  }
  return core::Result<TerminalDocumentView>::success(std::move(result));
}

std::string terminal_activity_label(TerminalActivity activity,
                                    core::ReasoningEffort reasoning_effort) {
  switch (activity) {
  case TerminalActivity::idle:
    return "idle";
  case TerminalActivity::thinking:
    return std::string(core::reasoning_effort_name(reasoning_effort)) +
           " think";
  case TerminalActivity::action:
    return "tool";
  case TerminalActivity::stream:
    return "reply";
  case TerminalActivity::cancelling:
    return "cancel";
  }
  return "idle";
}

std::string terminal_token_summary(const TerminalTokenMetrics &metrics,
                                   core::TokenCount max_context_tokens) {
  auto summary = terminal_context_summary(metrics, max_context_tokens);
  summary += " ↑" + compact_token_count(metrics.input_tokens) + " ↓" +
             compact_token_count(metrics.output_tokens) + " Σ" +
             compact_token_count(metrics.all_tokens());
  if (metrics.output_tokens_per_second > 0.0 &&
      std::isfinite(metrics.output_tokens_per_second)) {
    summary += " " + token_rate(metrics.output_tokens_per_second);
  }
  return summary;
}

std::string terminal_context_summary(const TerminalTokenMetrics &metrics,
                                     core::TokenCount max_context_tokens) {
  auto summary = "ctx:" + compact_token_count(metrics.context_tokens);
  if (max_context_tokens > 0) {
    summary += " (" +
               context_percentage(metrics.context_tokens, max_context_tokens) +
               ")";
  }
  return summary;
}

ftxui::Element
render_terminal_context_analysis(const TerminalTokenMetrics &metrics,
                                 core::TokenCount max_context_tokens,
                                 const TerminalTheme &theme) {
  ftxui::Elements rows{
      ftxui::hbox({
          ftxui::text("Context analysis") | ftxui::bold |
              ftxui::color(theme.secondary),
          ftxui::filler(),
          ftxui::text("LATEST REQUEST") | ftxui::bold |
              ftxui::color(theme.primary),
      }),
      ftxui::separator(),
      ftxui::hbox({
          ftxui::text("Used  ") | ftxui::bold | ftxui::color(theme.primary),
          ftxui::text(compact_token_count(metrics.context_tokens)) |
              ftxui::bold | ftxui::color(theme.secondary),
          max_context_tokens > 0
              ? ftxui::text(" / " + compact_token_count(max_context_tokens) +
                            "  (" +
                            context_percentage(metrics.context_tokens,
                                               max_context_tokens) +
                            ")")
              : ftxui::text("") | ftxui::color(theme.text),
      }),
  };
  if (max_context_tokens > 0) {
    const auto remaining = max_context_tokens > metrics.context_tokens
                               ? max_context_tokens - metrics.context_tokens
                               : core::TokenCount{};
    rows.push_back(ftxui::hbox({
        ftxui::text("Free  ") | ftxui::bold | ftxui::color(theme.primary),
        ftxui::text(compact_token_count(remaining)) | ftxui::bold |
            ftxui::color(theme.success),
    }));
  }
  if (metrics.cached_context_tokens > 0) {
    rows.push_back(ftxui::hbox({
        ftxui::text("Cached  ") | ftxui::bold | ftxui::color(theme.primary),
        ftxui::text(compact_token_count(metrics.cached_context_tokens)) |
            ftxui::bold | ftxui::color(theme.accent),
        ftxui::text("  (" +
                    context_share(metrics.cached_context_tokens,
                                  metrics.context_tokens) +
                    ")") |
            ftxui::bold | ftxui::color(theme.accent),
    }));
  }
  rows.push_back(ftxui::separatorEmpty());
  rows.push_back(ftxui::text("Estimated composition") | ftxui::bold |
                 ftxui::color(theme.primary));
  if (!metrics.context_breakdown.has_value()) {
    rows.push_back(ftxui::text("Send a message to collect context details.") |
                   ftxui::color(theme.text_muted));
  } else {
    const auto &breakdown = *metrics.context_breakdown;
    rows.push_back(context_breakdown_row("System instructions",
                                         breakdown.system_tokens,
                                         metrics.context_tokens, theme));
    rows.push_back(context_breakdown_row("User messages", breakdown.user_tokens,
                                         metrics.context_tokens, theme));
    rows.push_back(context_breakdown_row("Assistant messages",
                                         breakdown.assistant_tokens,
                                         metrics.context_tokens, theme));
    rows.push_back(context_breakdown_row("Tool calls / results",
                                         breakdown.tool_tokens,
                                         metrics.context_tokens, theme));
    rows.push_back(context_breakdown_row("Tool definitions",
                                         breakdown.tool_definition_tokens,
                                         metrics.context_tokens, theme));
    rows.push_back(context_breakdown_row("Other / protocol",
                                         breakdown.other_tokens,
                                         metrics.context_tokens, theme));
  }
  rows.push_back(ftxui::separatorEmpty());
  rows.push_back(
      ftxui::text("Total is exact; categories are estimated. · Esc closes") |
      ftxui::color(theme.text_muted));
  return ftxui::vbox(std::move(rows)) |
         ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 58) |
         ftxui::color(theme.text) | ftxui::bgcolor(theme.input_background) |
         ftxui::borderStyled(ftxui::ROUNDED, theme.secondary) |
         ftxui::selectionStyleReset | ftxui::clear_under;
}

ftxui::Element render_terminal_context_overlay(
    ftxui::Element page, const TerminalTokenMetrics &metrics,
    core::TokenCount max_context_tokens, const TerminalTheme &theme) {
  auto analysis = guard_full_width_boundary(
      render_terminal_context_analysis(metrics, max_context_tokens, theme),
      theme.background);
  return ftxui::dbox({
      std::move(page) | ftxui::dim,
      std::move(analysis) | ftxui::center,
  });
}

std::string terminal_clipboard_sequence(std::string_view text) {
  if (text.empty())
    return {};
  return "\x1b]52;c;" + encode_base64(text) + "\x07";
}

void TerminalScrollState::scroll_up(std::size_t content_rows,
                                    std::size_t viewport_rows) {
  constexpr std::size_t kWheelScrollRows = 3;
  const auto maximum_offset =
      maximum_scroll_offset(content_rows, viewport_rows);
  const auto current_offset =
      follows_latest_ ? maximum_offset : std::min(offset_rows_, maximum_offset);
  offset_rows_ =
      current_offset > kWheelScrollRows ? current_offset - kWheelScrollRows : 0;
  follows_latest_ = false;
}

void TerminalScrollState::scroll_down(std::size_t content_rows,
                                      std::size_t viewport_rows) {
  constexpr std::size_t kWheelScrollRows = 3;
  const auto maximum_offset =
      maximum_scroll_offset(content_rows, viewport_rows);
  const auto current_offset =
      follows_latest_ ? maximum_offset : std::min(offset_rows_, maximum_offset);
  offset_rows_ = std::min(maximum_offset, current_offset + kWheelScrollRows);
  follows_latest_ = offset_rows_ >= maximum_offset;
}

void TerminalScrollState::follow_latest() {
  offset_rows_ = 0;
  follows_latest_ = true;
}

void TerminalScrollState::reset_to_top() {
  offset_rows_ = 0;
  follows_latest_ = false;
}

std::size_t TerminalScrollState::offset_rows(std::size_t content_rows,
                                             std::size_t viewport_rows) const {
  const auto maximum_offset =
      maximum_scroll_offset(content_rows, viewport_rows);
  return follows_latest_ ? maximum_offset
                         : std::min(offset_rows_, maximum_offset);
}

int TerminalScrollState::focus_row(std::size_t content_rows,
                                   std::size_t viewport_rows) const {
  const auto offset = offset_rows(content_rows, viewport_rows);
  const auto viewport_center = viewport_rows == 0 ? 0 : (viewport_rows - 1) / 2;
  const auto maximum_int =
      static_cast<std::size_t>(std::numeric_limits<int>::max());
  const auto row = offset > maximum_int - std::min(viewport_center, maximum_int)
                       ? maximum_int
                       : offset + viewport_center;
  return static_cast<int>(std::min(row, maximum_int));
}

bool TerminalScrollState::follows_latest() const { return follows_latest_; }

void TerminalPromptHistory::remember(std::string prompt) {
  if (!prompt.empty())
    prompts_.push_back(std::move(prompt));
  reset_navigation();
}

void TerminalPromptHistory::reset_navigation() {
  position_.reset();
  draft_.clear();
}

std::optional<std::string>
TerminalPromptHistory::previous(std::string_view current_input) {
  if (prompts_.empty())
    return std::nullopt;
  if (!position_.has_value()) {
    draft_ = current_input;
    position_ = prompts_.size();
  }
  if (*position_ == 0)
    return std::nullopt;
  --*position_;
  return prompts_[*position_];
}

std::optional<std::string> TerminalPromptHistory::next() {
  if (!position_.has_value())
    return std::nullopt;
  if (*position_ + 1 < prompts_.size()) {
    ++*position_;
    return prompts_[*position_];
  }
  const auto draft = std::move(draft_);
  reset_navigation();
  return draft;
}

void TerminalTranscript::begin_request(std::string user_input,
                                       TerminalActivity initial_activity) {
  active_assistant_.reset();
  active_tool_.reset();
  active_direct_command_.reset();
  request_ended_ = false;
  request_error_seen_ = false;
  activity_ = initial_activity;
  if (initial_activity == TerminalActivity::action) {
    active_direct_command_ =
        append_message({TerminalMessageKind::command, std::move(user_input),
                        "running…", true, true, false});
  } else {
    append_message({TerminalMessageKind::user, {}, std::move(user_input)});
  }
}

void TerminalTranscript::cancel_request() {
  activity_ = TerminalActivity::cancelling;
}

void TerminalTranscript::append_event(const core::AgentEvent &event) {
  using core::AgentEventType;
  switch (event.type) {
  case AgentEventType::agent_start:
    activity_ = TerminalActivity::thinking;
    break;
  case AgentEventType::assistant_delta:
    activity_ = TerminalActivity::stream;
    if (!active_assistant_.has_value()) {
      active_assistant_ =
          append_message({TerminalMessageKind::assistant, "zeda", {}});
    }
    messages_[*active_assistant_].content += event.text;
    break;
  case AgentEventType::assistant_message:
    activity_ = TerminalActivity::stream;
    if (event.model_usage.has_value()) {
      token_metrics_.context_tokens = event.model_usage->input_tokens;
      token_metrics_.input_tokens += event.model_usage->input_tokens;
      token_metrics_.output_tokens += event.model_usage->output_tokens;
      token_metrics_.cached_context_tokens =
          event.model_usage->cached_input_tokens;
      token_metrics_.context_breakdown = event.model_usage->context_breakdown;
      if (event.model_usage->output_tokens_per_second > 0.0) {
        token_metrics_.output_tokens_per_second =
            event.model_usage->output_tokens_per_second;
      }
    }
    if (active_assistant_.has_value()) {
      if (!event.text.empty()) {
        messages_[*active_assistant_].content = event.text;
      }
    } else if (!event.text.empty()) {
      append_message({TerminalMessageKind::assistant, "zeda", event.text});
    }
    active_assistant_.reset();
    break;
  case AgentEventType::tool_start:
    activity_ = TerminalActivity::action;
    active_assistant_.reset();
    if (!active_direct_command_.has_value()) {
      active_tool_ = append_message({TerminalMessageKind::tool, event.text,
                                     "running…", true, false, false});
    }
    break;
  case AgentEventType::tool_update:
    activity_ = TerminalActivity::action;
    if (active_tool_.has_value())
      messages_[*active_tool_].content = event.text;
    break;
  case AgentEventType::tool_result:
    activity_ = TerminalActivity::thinking;
    if (event.model_usage.has_value()) {
      token_metrics_.input_tokens += event.model_usage->input_tokens;
      token_metrics_.output_tokens += event.model_usage->output_tokens;
      token_metrics_.cached_context_tokens +=
          event.model_usage->cached_input_tokens;
    }
    if (active_direct_command_.has_value()) {
      auto &message = messages_[*active_direct_command_];
      message.content = event.text;
      message.is_error =
          event.tool_result.has_value() && event.tool_result->is_error;
    } else if (active_tool_.has_value()) {
      auto &message = messages_[*active_tool_];
      message.content = event.text;
      message.is_error =
          event.tool_result.has_value() && event.tool_result->is_error;
    } else {
      const bool is_error =
          event.tool_result.has_value() && event.tool_result->is_error;
      append_message({TerminalMessageKind::tool, "tool", event.text, true,
                      false, is_error});
    }
    active_tool_.reset();
    break;
  case AgentEventType::agent_end:
    if (active_assistant_.has_value() &&
        messages_[*active_assistant_].content.empty() && !event.text.empty()) {
      messages_[*active_assistant_].content = event.text;
    }
    active_assistant_.reset();
    active_tool_.reset();
    active_direct_command_.reset();
    request_ended_ = true;
    activity_ = TerminalActivity::idle;
    break;
  case AgentEventType::error:
    if (active_direct_command_.has_value()) {
      auto &message = messages_[*active_direct_command_];
      message.content = event.text;
      message.is_error = true;
    } else {
      append_message({TerminalMessageKind::error, {}, event.text});
    }
    active_assistant_.reset();
    active_tool_.reset();
    active_direct_command_.reset();
    request_error_seen_ = true;
    activity_ = TerminalActivity::idle;
    break;
  default:
    break;
  }
}

void TerminalTranscript::complete_request(
    const core::Result<std::string> &result) {
  if (active_direct_command_.has_value()) {
    auto &message = messages_[*active_direct_command_];
    if (result) {
      if (!result.value().empty() || message.content == "running…")
        message.content = result.value();
    } else {
      message.content = result.error().message;
      message.is_error = true;
    }
  } else if (result) {
    if (!request_ended_ && !result.value().empty()) {
      append_message({TerminalMessageKind::assistant, "zeda", result.value()});
    }
  } else if (!request_error_seen_) {
    append_message({TerminalMessageKind::error, {}, result.error().message});
  }
  active_assistant_.reset();
  active_tool_.reset();
  active_direct_command_.reset();
  activity_ = TerminalActivity::idle;
}

void TerminalTranscript::append_command(
    std::string command, const core::Result<std::string> &result) {
  if (result) {
    append_message({TerminalMessageKind::command, std::move(command),
                    result.value(), true, true, false});
  } else {
    append_message({TerminalMessageKind::command, std::move(command),
                    result.error().message, true, true, true});
  }
}

void TerminalTranscript::restore(const std::vector<core::Message> &history) {
  messages_.clear();
  active_assistant_.reset();
  active_tool_.reset();
  active_direct_command_.reset();
  request_ended_ = true;
  request_error_seen_ = false;
  activity_ = TerminalActivity::idle;
  token_metrics_ = {};

  std::unordered_map<core::ToolCallId, std::string> tool_names;
  for (const auto &message : history) {
    switch (message.role) {
    case core::Role::system:
      break;
    case core::Role::user:
      append_message(
          {TerminalMessageKind::user, {}, message.content, false, true, false});
      break;
    case core::Role::assistant:
      if (!message.content.empty()) {
        append_message({TerminalMessageKind::assistant, "zeda", message.content,
                        false, true, false});
      }
      for (const auto &call : message.tool_calls)
        tool_names[call.id] = call.name;
      break;
    case core::Role::tool: {
      std::string label = "tool";
      if (message.tool_call_id.has_value()) {
        const auto name = tool_names.find(*message.tool_call_id);
        if (name != tool_names.end())
          label = name->second;
      }
      append_message({TerminalMessageKind::tool, std::move(label),
                      message.content, true, false, message.is_error});
      break;
    }
    }
  }
}

const std::vector<TerminalMessage> &TerminalTranscript::messages() const {
  return messages_;
}

TerminalActivity TerminalTranscript::activity() const { return activity_; }

TerminalTokenMetrics TerminalTranscript::token_metrics() const {
  return token_metrics_;
}

bool TerminalTranscript::toggle_message_expansion(std::size_t index) {
  if (index >= messages_.size() || !messages_[index].collapsible)
    return false;
  messages_[index].expanded = !messages_[index].expanded;
  return true;
}

std::string terminal_startup_summary(const TerminalStartupTiming &timing) {
  constexpr auto kVisibleThreshold = std::chrono::milliseconds(1);
  const auto total = timing.config + timing.core + timing.session +
                     timing.setup + timing.plugins + timing.ui;
  const auto milliseconds = [](std::chrono::microseconds duration) {
    std::ostringstream output;
    output << std::fixed << std::setprecision(3)
           << static_cast<double>(duration.count()) / 1000.0;
    return output.str();
  };
  const std::array phases{
      std::pair<std::string_view, std::chrono::microseconds>{"config",
                                                             timing.config},
      std::pair<std::string_view, std::chrono::microseconds>{"core",
                                                             timing.core},
      std::pair<std::string_view, std::chrono::microseconds>{"session",
                                                             timing.session},
      std::pair<std::string_view, std::chrono::microseconds>{"setup",
                                                             timing.setup},
      std::pair<std::string_view, std::chrono::microseconds>{"plugins",
                                                             timing.plugins},
      std::pair<std::string_view, std::chrono::microseconds>{"ui", timing.ui},
  };

  std::vector<std::string> components;
  std::chrono::microseconds other{};
  for (const auto &[name, duration] : phases) {
    if (duration > kVisibleThreshold) {
      components.push_back(std::string(name) + " " + milliseconds(duration));
    } else {
      other += duration;
    }
  }
  if (other.count() > 0 || components.empty())
    components.push_back("other " + milliseconds(other));

  std::string result = "startup: " + milliseconds(total) + " ms = ";
  for (std::size_t index = 0; index < components.size(); ++index) {
    if (index > 0)
      result += " + ";
    result += components[index];
  }
  return result;
}

std::size_t TerminalTranscript::append_message(TerminalMessage message) {
  messages_.push_back(std::move(message));
  return messages_.size() - 1;
}

ftxui::Element
render_terminal_messages(const std::vector<TerminalMessage> &messages) {
  return render_terminal_messages(messages, terminal_theme(ThemeKind::light));
}

ftxui::Element
render_terminal_messages(const std::vector<TerminalMessage> &messages,
                         const TerminalTheme &theme) {
  return render_messages(messages, theme);
}

ftxui::Element render_terminal_input(ftxui::InputState state,
                                     const TerminalTheme &theme) {
  state.element |=
      ftxui::color(state.is_placeholder ? theme.text_muted : theme.text);
  state.element |= ftxui::bgcolor(theme.input_background);
  if (state.focused) {
    state.element =
        std::make_shared<BlockCursorOverride>(std::move(state.element));
  }
  return state.element;
}

ftxui::Element render_terminal_welcome(std::string_view workspace,
                                       std::string_view model,
                                       std::string_view version,
                                       const TerminalStartupTiming &startup,
                                       core::ReasoningEffort reasoning_effort,
                                       ThemeKind theme_kind) {
  const auto &theme = terminal_theme(theme_kind);
  const auto startup_summary = terminal_startup_summary(startup);
  ftxui::Elements elements{
      ftxui::hbox({
          ftxui::text(">_ ") | ftxui::color(theme.text_muted),
          ftxui::text("zeda " + std::string(version)) | ftxui::bold |
              ftxui::color(theme.primary),
      }),
      ftxui::hbox({
          ftxui::text("model: ") | ftxui::color(theme.text_muted),
          ftxui::text(std::string(model)) | ftxui::color(theme.text),
          ftxui::text(" · reasoning: ") | ftxui::color(theme.text_muted),
          ftxui::text(
              std::string(core::reasoning_effort_name(reasoning_effort))) |
              ftxui::color(theme.text),
      }),
      ftxui::hbox({
          ftxui::text("workspace: ") | ftxui::color(theme.text_muted),
          ftxui::text(std::string(workspace)) | ftxui::color(theme.text),
      }),
      ftxui::text(startup_summary) | ftxui::color(theme.text_muted),
  };
  return ftxui::vbox(std::move(elements)) |
         ftxui::bgcolor(theme.background_panel) |
         ftxui::borderStyled(ftxui::ROUNDED, theme.border);
}

TerminalRenderer::TerminalRenderer(std::ostream &output,
                                   TerminalOptions options)
    : output_(output), options_(options) {}

void TerminalRenderer::banner(std::string_view workspace,
                              std::string_view model, std::string_view version,
                              const TerminalStartupTiming &startup,
                              core::ReasoningEffort reasoning_effort) {
  const auto startup_summary = terminal_startup_summary(startup);
  output_ << render_text("zeda " + std::string(version), Tone::title) << "\n"
          << render_text(
                 "model: " + std::string(model) + " · reasoning: " +
                 std::string(core::reasoning_effort_name(reasoning_effort)))
          << "\n"
          << render_text("workspace: " + std::string(workspace)) << "\n"
          << startup_summary << "\n";
}

void TerminalRenderer::prompt() {
  output_ << render_text("› ", Tone::prompt) << std::flush;
}

void TerminalRenderer::render(const core::AgentEvent &event) {
  using core::AgentEventType;
  switch (event.type) {
  case AgentEventType::assistant_delta:
    if (!event.text.empty()) {
      output_ << event.text << std::flush;
      assistant_stream_open_ = true;
    }
    break;
  case AgentEventType::assistant_message:
    if (assistant_stream_open_) {
      output_ << "\n" << std::flush;
      assistant_stream_open_ = false;
    } else if (!event.text.empty()) {
      output_ << render_element(render_markdown(event.text,
                                                terminal_theme(options_.theme)))
              << "\n"
              << std::flush;
    }
    break;
  case AgentEventType::tool_start:
    if (assistant_stream_open_) {
      output_ << "\n";
      assistant_stream_open_ = false;
    }
    output_ << "\n"
            << render_text("[tool: " + event.text + "]", Tone::tool) << "\n"
            << std::flush;
    last_tool_update_.clear();
    break;
  case AgentEventType::tool_update:
    if (!event.text.empty() && event.text != last_tool_update_) {
      output_ << render_text("[tool update: " + event.text + "]", Tone::tool)
              << "\n"
              << std::flush;
      last_tool_update_ = event.text;
    }
    break;
  case AgentEventType::tool_result:
    output_ << render_element(ftxui::paragraph(event.text) |
                              ftxui::color(terminal_theme(options_.theme).text))
            << "\n"
            << std::flush;
    last_tool_update_.clear();
    break;
  case AgentEventType::agent_end:
    if (assistant_stream_open_) {
      output_ << "\n" << std::flush;
      assistant_stream_open_ = false;
    }
    break;
  case AgentEventType::error:
    assistant_stream_open_ = false;
    error(event.text);
    break;
  default:
    break;
  }
}

void TerminalRenderer::error(std::string_view message) {
  output_ << "\n"
          << render_text("[error] " + std::string(message), Tone::error) << "\n"
          << std::flush;
}

void TerminalRenderer::set_theme(ThemeKind theme) { options_.theme = theme; }

std::string TerminalRenderer::render_text(std::string_view text,
                                          Tone tone) const {
  auto element = ftxui::text(std::string(text));
  if (options_.color) {
    const auto &theme = terminal_theme(options_.theme);
    switch (tone) {
    case Tone::title:
      element = element | ftxui::bold | ftxui::color(theme.primary);
      break;
    case Tone::prompt:
      element = element | ftxui::bold | ftxui::color(theme.success);
      break;
    case Tone::tool:
      element = element | ftxui::color(theme.warning);
      break;
    case Tone::error:
      element = element | ftxui::bold | ftxui::color(theme.error);
      break;
    case Tone::normal:
      element = element | ftxui::color(theme.text);
      break;
    }
  }
  return render_element(std::move(element));
}

TerminalInput::TerminalInput(std::istream &input) : input_(input) {}

core::Result<std::string> TerminalInput::read_line() {
  std::string line;
  if (!std::getline(input_, line)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::cancelled,
        "input stream closed",
    });
  }
  return core::Result<std::string>::success(std::move(line));
}

TerminalApplication::TerminalApplication(
    std::string workspace, std::string &model, std::string version,
    TerminalStartupTiming startup, core::TokenCount &max_context_tokens,
    core::ReasoningEffort &reasoning_effort, ThemeKind &theme_kind,
    QuickBashState quick_bash_enabled, SessionNameState session_name,
    SessionLoader session_loader, InitialActivity initial_activity,
    std::vector<TerminalCommandHint> command_hints, SubmitHandler submit,
    CommandHandler command)
    : workspace_(std::move(workspace)), model_state_(model),
      version_(std::move(version)), startup_(startup),
      max_context_tokens_state_(max_context_tokens),
      reasoning_effort_state_(reasoning_effort), theme_kind_state_(theme_kind),
      displayed_model_(model),
      displayed_max_context_tokens_(max_context_tokens),
      displayed_reasoning_effort_(reasoning_effort),
      displayed_theme_kind_(theme_kind),
      quick_bash_enabled_(std::move(quick_bash_enabled)),
      session_name_(std::move(session_name)),
      session_loader_(std::move(session_loader)),
      initial_activity_(std::move(initial_activity)),
      command_hints_(std::move(command_hints)), submit_(std::move(submit)),
      command_(std::move(command)) {}

TerminalApplication::~TerminalApplication() {
  if (active_cancellation_ != nullptr)
    active_cancellation_->cancel();
  if (worker_.joinable())
    worker_.join();
}

core::Result<void> TerminalApplication::run() {
  if (!submit_ || !command_ || !quick_bash_enabled_ || !session_name_ ||
      !session_loader_ || !initial_activity_) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "terminal application handlers are not configured",
    });
  }
  const auto restored = reload_session();
  if (!restored)
    return restored;

  configure_app();
  auto root =
      ftxui::Renderer(input_component_, [this] { return render_page(); });
  root |= ftxui::CatchEvent(
      [this](ftxui::Event event) { return handle_event(std::move(event)); });

  app_->Loop(root);
  if (active_cancellation_ != nullptr)
    active_cancellation_->cancel();
  if (worker_.joinable())
    worker_.join();
  input_component_.reset();
  app_.reset();
  return core::Result<void>::success();
}

void TerminalApplication::configure_app() {
  app_ = std::make_unique<ftxui::App>(ftxui::App::FullscreenAlternateScreen());
  app_->ForceHandleCtrlC(false);
  app_->SelectionChange([this] {
    const auto selection = app_->GetSelection();
    if (!selection.empty())
      selected_text_ = selection;
    if (!selection_copy_pending_ || selected_text_.empty())
      return;
    selection_copy_pending_ = false;
    copy_selection(selected_text_);
  });

  ftxui::InputOption input_options;
  input_options.placeholder = "Ask zeda to do anything";
  input_options.multiline = false;
  input_options.cursor_position = &input_cursor_position_;
  input_options.on_change = [this] {
    selected_command_suggestion_ = 0;
    prompt_history_.reset_navigation();
  };
  input_options.on_enter = [this] { submit_line(); };
  input_options.transform = [this](ftxui::InputState state) {
    return render_terminal_input(std::move(state),
                                 terminal_theme(displayed_theme_kind_));
  };
  input_component_ = ftxui::Input(&input_, input_options);
}

ftxui::Element TerminalApplication::render_page() {
  if (document_view_.has_value())
    return render_document_view();
  const auto &theme = terminal_theme(displayed_theme_kind_);
  const auto activity = transcript_.activity();
  const auto now = std::chrono::steady_clock::now();
  const bool copy_status_visible = !copy_status_.empty();
  if (activity != TerminalActivity::idle)
    ftxui::animation::RequestAnimationFrame();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  const auto spinner_frame = static_cast<std::size_t>(elapsed.count() / 80);
  auto content = ftxui::vbox({
      render_terminal_welcome(workspace_, displayed_model_, version_, startup_,
                              displayed_reasoning_effort_,
                              displayed_theme_kind_),
      ftxui::separatorEmpty(),
      render_messages(transcript_.messages(), theme, &message_boxes_),
  });
  content |= capture_layout_box(scroll_content_box_);
  if (scroll_state_.follows_latest()) {
    content |= ftxui::focusPositionRelative(0.0F, 1.0F);
  } else {
    content |= ftxui::focusPosition(
        0, scroll_state_.focus_row(layout_box_rows(scroll_content_box_),
                                   box_rows(scroll_viewport_box_)));
  }
  auto body = content | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex |
              ftxui::reflect(scroll_viewport_box_);
  auto composer = render_three_line_user_surface(
      ftxui::hbox({
          ftxui::text("› ") | ftxui::bold | ftxui::color(theme.primary),
          input_component_->Render() | ftxui::flex,
      }),
      theme);
  const auto command_suggestions =
      terminal_command_suggestions(input_, command_hints_);
  const auto *command_help = terminal_command_help(input_, command_hints_);
  if (selected_command_suggestion_ >= command_suggestions.size())
    selected_command_suggestion_ = 0;
  auto suggestions = render_terminal_command_guide(
      input_, command_hints_, selected_command_suggestion_, theme);
  const bool command_guide_visible =
      command_help != nullptr || !command_suggestions.empty();
  ftxui::Elements footer_elements{
      render_activity(activity, displayed_reasoning_effort_, spinner_frame,
                      theme),
      ftxui::filler(),
  };
  if (copy_status_visible) {
    footer_elements.push_back(
        ftxui::text(copy_status_) |
        ftxui::color(copy_status_error_ ? theme.error : theme.success));
    footer_elements.push_back(ftxui::text("  ") |
                              ftxui::color(theme.text_muted));
  }
  const auto token_metrics = transcript_.token_metrics();
  const auto context_summary =
      terminal_context_summary(token_metrics, displayed_max_context_tokens_);
  auto token_summary =
      terminal_token_summary(token_metrics, displayed_max_context_tokens_);
  token_summary.erase(0, context_summary.size());
  footer_elements.push_back(ftxui::text(context_summary) | ftxui::bold |
                            ftxui::color(theme.secondary) |
                            ftxui::bgcolor(theme.input_background) |
                            ftxui::reflect(context_footer_box_));
  footer_elements.push_back(ftxui::text(std::move(token_summary)) |
                            ftxui::color(theme.text_muted));
  if (!command_suggestions.empty()) {
    footer_elements.push_back(ftxui::text("  enter/tab complete") |
                              ftxui::color(theme.text_muted));
  }
  auto footer = ftxui::hbox(std::move(footer_elements));
  ftxui::Elements layout{body, ftxui::separatorEmpty()};
  if (command_guide_visible)
    layout.push_back(std::move(suggestions));
  layout.push_back(std::move(composer));
  layout.push_back(std::move(footer));
  auto page = ftxui::vbox(std::move(layout)) | ftxui::color(theme.text) |
              ftxui::bgcolor(theme.background) |
              ftxui::selectionForegroundColor(theme.text) |
              ftxui::selectionBackgroundColor(theme.background_element);
  if (!context_analysis_visible_)
    return page;
  return render_terminal_context_overlay(std::move(page), token_metrics,
                                         displayed_max_context_tokens_, theme);
}

ftxui::Element TerminalApplication::render_document_view() {
  const auto &theme = terminal_theme(displayed_theme_kind_);
  const auto &document = *document_view_;
  if (selected_document_page_ >= document.pages.size())
    selected_document_page_ = 0;
  document_page_boxes_.assign(document.pages.size(), {});

  ftxui::Elements page_rows;
  page_rows.reserve(document.pages.size() + 1);
  page_rows.push_back(ftxui::text(" PAGES") | ftxui::bold |
                      ftxui::color(theme.text_muted));
  for (std::size_t index = 0; index < document.pages.size(); ++index) {
    const auto &page = document.pages[index];
    const bool selected = index == selected_document_page_;
    auto title = ftxui::hbox({
        ftxui::text(selected ? " › " : "   ") |
            ftxui::color(selected ? theme.secondary : theme.text_muted),
        ftxui::text(page.title) | ftxui::bold |
            ftxui::color(selected ? theme.primary : theme.text),
        ftxui::filler(),
    });
    if (!page.badge.empty()) {
      title = ftxui::hbox({
          std::move(title) | ftxui::flex,
          ftxui::text(" " + page.badge + " ") | ftxui::bold |
              ftxui::color(theme.warning),
      });
    }
    ftxui::Elements row_elements{std::move(title)};
    if (!page.description.empty()) {
      row_elements.push_back(ftxui::hbox({
          ftxui::text("   "),
          ftxui::paragraph(page.description) | ftxui::color(theme.text_muted) |
              ftxui::xflex,
      }));
    }
    auto row = ftxui::vbox(std::move(row_elements)) |
               ftxui::reflect(document_page_boxes_[index]);
    if (selected)
      row |= ftxui::bgcolor(theme.background_element);
    page_rows.push_back(std::move(row));
  }
  auto sidebar_content = ftxui::vbox(std::move(page_rows));
  const auto relative_selection =
      document.pages.size() <= 1
          ? 0.0F
          : static_cast<float>(selected_document_page_) /
                static_cast<float>(document.pages.size() - 1);
  sidebar_content |= ftxui::focusPositionRelative(0.0F, relative_selection);
  auto sidebar = std::move(sidebar_content) | ftxui::vscroll_indicator |
                 ftxui::yframe | ftxui::flex |
                 ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 34) |
                 ftxui::bgcolor(theme.background_panel) |
                 ftxui::borderStyled(ftxui::ROUNDED, document_sidebar_focused_
                                                         ? theme.border_active
                                                         : theme.border);

  const auto &page = document.pages[selected_document_page_];
  ftxui::Elements article_rows{
      ftxui::hbox({
          ftxui::text(page.title) | ftxui::bold |
              ftxui::color(theme.markdown_heading),
          ftxui::filler(),
          page.badge.empty() ? ftxui::text("")
                             : ftxui::text(" " + page.badge + " ") |
                                   ftxui::bold | ftxui::color(theme.warning),
      }),
  };
  if (!page.description.empty()) {
    article_rows.push_back(ftxui::paragraph(page.description) |
                           ftxui::color(theme.text_muted));
  }
  article_rows.push_back(ftxui::separatorEmpty());
  article_rows.push_back(
      render_markdown(page_markdown_without_repeated_title(page), theme));
  auto article = ftxui::vbox(std::move(article_rows)) |
                 capture_layout_box(document_content_box_);
  if (document_scroll_state_.follows_latest()) {
    article |= ftxui::focusPositionRelative(0.0F, 1.0F);
  } else {
    article |= ftxui::focusPosition(
        0,
        document_scroll_state_.focus_row(layout_box_rows(document_content_box_),
                                         box_rows(document_viewport_box_)));
  }
  auto body = std::move(article) | ftxui::vscroll_indicator | ftxui::yframe |
              ftxui::flex | ftxui::reflect(document_viewport_box_) |
              ftxui::borderStyled(ftxui::ROUNDED, document_sidebar_focused_
                                                      ? theme.border
                                                      : theme.border_active);

  auto header = ftxui::hbox({
      ftxui::text(" ◫ " + document.title + " ") | ftxui::bold |
          ftxui::color(theme.primary),
      ftxui::text(document.subtitle.empty() ? "" : "  " + document.subtitle) |
          ftxui::color(theme.text_muted),
      ftxui::filler(),
      ftxui::text("Esc/q back ") | ftxui::color(theme.text_muted),
  });
  auto footer = ftxui::hbox({
      ftxui::text(document_sidebar_focused_ ? "TOC" : "PAGE") | ftxui::bold |
          ftxui::color(theme.secondary),
      ftxui::text(
          "  Tab/←/→ focus · ↑/↓ or j/k navigate · mouse wheel scroll") |
          ftxui::color(theme.text_muted),
      ftxui::filler(),
      ftxui::text(std::to_string(selected_document_page_ + 1) + "/" +
                  std::to_string(document.pages.size()) + " ") |
          ftxui::bold | ftxui::color(theme.primary),
  });
  return ftxui::vbox({
             std::move(header),
             ftxui::separator(),
             ftxui::hbox({std::move(sidebar), ftxui::separator(),
                          std::move(body) | ftxui::flex}) |
                 ftxui::flex,
             ftxui::separator(),
             std::move(footer),
         }) |
         ftxui::color(theme.text) | ftxui::bgcolor(theme.background) |
         ftxui::selectionForegroundColor(theme.text) |
         ftxui::selectionBackgroundColor(theme.background_element);
}

void TerminalApplication::select_document_page(std::size_t index) {
  if (!document_view_.has_value() || document_view_->pages.empty())
    return;
  selected_document_page_ = std::min(index, document_view_->pages.size() - 1);
  document_scroll_state_.reset_to_top();
}

bool TerminalApplication::handle_document_view_event(ftxui::Event event) {
  if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlC ||
      event == ftxui::Event::Character('q')) {
    document_view_.reset();
    document_page_boxes_.clear();
    selected_document_page_ = 0;
    document_sidebar_focused_ = true;
    document_scroll_state_.reset_to_top();
    return true;
  }
  if (event == ftxui::Event::Tab || event == ftxui::Event::ArrowLeft ||
      event == ftxui::Event::ArrowRight) {
    document_sidebar_focused_ = !document_sidebar_focused_;
    return true;
  }
  if (!document_view_.has_value() || document_view_->pages.empty())
    return event != ftxui::Event::Custom;

  if (event.is_mouse() && event.mouse().button == ftxui::Mouse::Left &&
      event.mouse().motion == ftxui::Mouse::Pressed) {
    for (std::size_t index = 0; index < document_page_boxes_.size(); ++index) {
      if (document_page_boxes_[index].Contain(event.mouse().x,
                                              event.mouse().y)) {
        document_sidebar_focused_ = true;
        select_document_page(index);
        return true;
      }
    }
    if (document_viewport_box_.Contain(event.mouse().x, event.mouse().y)) {
      document_sidebar_focused_ = false;
      return true;
    }
  }

  const bool previous =
      event == ftxui::Event::ArrowUp || event == ftxui::Event::Character('k');
  const bool next =
      event == ftxui::Event::ArrowDown || event == ftxui::Event::Character('j');
  if (document_sidebar_focused_) {
    if (previous) {
      if (selected_document_page_ > 0)
        select_document_page(selected_document_page_ - 1);
      return true;
    }
    if (next) {
      select_document_page(selected_document_page_ + 1);
      return true;
    }
    if (event == ftxui::Event::Home) {
      select_document_page(0);
      return true;
    }
    if (event == ftxui::Event::End) {
      select_document_page(document_view_->pages.size() - 1);
      return true;
    }
  } else {
    if (previous || event == ftxui::Event::PageUp) {
      const int repetitions = event == ftxui::Event::PageUp ? 5 : 1;
      for (int index = 0; index < repetitions; ++index) {
        document_scroll_state_.scroll_up(layout_box_rows(document_content_box_),
                                         box_rows(document_viewport_box_));
      }
      return true;
    }
    if (next || event == ftxui::Event::PageDown) {
      const int repetitions = event == ftxui::Event::PageDown ? 5 : 1;
      for (int index = 0; index < repetitions; ++index) {
        document_scroll_state_.scroll_down(
            layout_box_rows(document_content_box_),
            box_rows(document_viewport_box_));
      }
      return true;
    }
    if (event == ftxui::Event::Home) {
      document_scroll_state_.reset_to_top();
      return true;
    }
    if (event == ftxui::Event::End) {
      document_scroll_state_.follow_latest();
      return true;
    }
  }

  if (event.is_mouse() && event.mouse().motion == ftxui::Mouse::Pressed &&
      (event.mouse().button == ftxui::Mouse::WheelUp ||
       event.mouse().button == ftxui::Mouse::WheelDown)) {
    if (document_sidebar_focused_) {
      if (event.mouse().button == ftxui::Mouse::WheelUp &&
          selected_document_page_ > 0) {
        select_document_page(selected_document_page_ - 1);
      } else if (event.mouse().button == ftxui::Mouse::WheelDown) {
        select_document_page(selected_document_page_ + 1);
      }
    } else if (event.mouse().button == ftxui::Mouse::WheelUp) {
      document_scroll_state_.scroll_up(layout_box_rows(document_content_box_),
                                       box_rows(document_viewport_box_));
    } else {
      document_scroll_state_.scroll_down(layout_box_rows(document_content_box_),
                                         box_rows(document_viewport_box_));
    }
    return true;
  }
  return event != ftxui::Event::Custom;
}

bool TerminalApplication::handle_event(ftxui::Event event) {
  if (event != ftxui::Event::Custom && !copy_status_.empty()) {
    copy_status_.clear();
    copy_status_error_ = false;
  }
  if (document_view_.has_value())
    return handle_document_view_event(std::move(event));
  if (event.is_mouse() && event.mouse().button == ftxui::Mouse::Left) {
    const auto &mouse = event.mouse();
    if (mouse.motion == ftxui::Mouse::Pressed &&
        context_footer_box_.Contain(mouse.x, mouse.y)) {
      context_footer_pressed_ = true;
      return true;
    }
    if (mouse.motion == ftxui::Mouse::Released &&
        std::exchange(context_footer_pressed_, false)) {
      if (context_footer_box_.Contain(mouse.x, mouse.y))
        context_analysis_visible_ = !context_analysis_visible_;
      return true;
    }
  }
  if (context_analysis_visible_) {
    if (event == ftxui::Event::Escape)
      context_analysis_visible_ = false;
    return event != ftxui::Event::Custom;
  }
  const auto command_suggestions =
      terminal_command_suggestions(input_, command_hints_);
  if (!busy_ && !command_suggestions.empty()) {
    if (event == ftxui::Event::ArrowDown) {
      selected_command_suggestion_ =
          (selected_command_suggestion_ + 1) % command_suggestions.size();
      return true;
    }
    if (event == ftxui::Event::ArrowUp) {
      selected_command_suggestion_ =
          (selected_command_suggestion_ + command_suggestions.size() - 1) %
          command_suggestions.size();
      return true;
    }
    if (is_terminal_command_completion_event(event)) {
      auto completed = complete_terminal_command(input_, command_hints_,
                                                 selected_command_suggestion_);
      if (event == ftxui::Event::Return && completed == input_)
        return false;
      input_ = std::move(completed);
      input_cursor_position_ = static_cast<int>(input_.size());
      selected_command_suggestion_ = 0;
      return true;
    }
  }
  if (!busy_ && command_suggestions.empty() &&
      (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown)) {
    const auto recalled = event == ftxui::Event::ArrowUp
                              ? prompt_history_.previous(input_)
                              : prompt_history_.next();
    if (recalled.has_value()) {
      input_ = *recalled;
      input_cursor_position_ = static_cast<int>(input_.size());
    }
    return true;
  }
  if (event.is_mouse() && event.mouse().button == ftxui::Mouse::Left) {
    const auto &mouse = event.mouse();
    if (mouse.motion == ftxui::Mouse::Pressed) {
      mouse_selection_pressed_ = scroll_viewport_box_.Contain(mouse.x, mouse.y);
      mouse_selection_dragged_ = false;
      mouse_press_x_ = mouse.x;
      mouse_press_y_ = mouse.y;
      selection_copy_pending_ = false;
      selected_text_.clear();
      pressed_collapsible_message_.reset();
      if (mouse_selection_pressed_) {
        const auto &messages = transcript_.messages();
        const auto box_count = std::min(messages.size(), message_boxes_.size());
        for (std::size_t index = 0; index < box_count; ++index) {
          if (messages[index].collapsible &&
              message_boxes_[index].Contain(mouse.x, mouse.y)) {
            pressed_collapsible_message_ = index;
            break;
          }
        }
      }
    } else if (mouse.motion == ftxui::Mouse::Moved &&
               mouse_selection_pressed_) {
      mouse_selection_dragged_ = mouse_selection_dragged_ ||
                                 mouse.x != mouse_press_x_ ||
                                 mouse.y != mouse_press_y_;
    } else if (mouse.motion == ftxui::Mouse::Released &&
               mouse_selection_pressed_) {
      mouse_selection_pressed_ = false;
      const bool dragged = std::exchange(mouse_selection_dragged_, false);
      const auto clicked_message =
          std::exchange(pressed_collapsible_message_, std::nullopt);
      if (dragged) {
        if (selected_text_.empty())
          selection_copy_pending_ = true;
        else
          copy_selection(selected_text_);
        return false;
      }
      if (clicked_message.has_value() &&
          *clicked_message < message_boxes_.size() &&
          message_boxes_[*clicked_message].Contain(mouse.x, mouse.y)) {
        return transcript_.toggle_message_expansion(*clicked_message);
      }
    }
  }
  if (event.is_mouse() && event.mouse().motion == ftxui::Mouse::Pressed &&
      (event.mouse().button == ftxui::Mouse::WheelUp ||
       event.mouse().button == ftxui::Mouse::WheelDown)) {
    if (event.mouse().button == ftxui::Mouse::WheelUp) {
      scroll_state_.scroll_up(layout_box_rows(scroll_content_box_),
                              box_rows(scroll_viewport_box_));
    } else {
      scroll_state_.scroll_down(layout_box_rows(scroll_content_box_),
                                box_rows(scroll_viewport_box_));
    }
    return true;
  }
  if (event == ftxui::Event::CtrlC || event == ftxui::Event::Escape) {
    if (active_cancellation_ != nullptr) {
      active_cancellation_->cancel();
      transcript_.cancel_request();
    } else if (event == ftxui::Event::CtrlC) {
      app_->Exit();
    }
    return true;
  }
  return false;
}

core::Result<void> TerminalApplication::reload_session() {
  const auto history = session_loader_();
  if (!history) {
    return core::Result<void>::failure({
        core::ErrorCode::session_error,
        "cannot restore terminal session: " + history.error().message,
    });
  }
  transcript_.restore(history.value());
  return core::Result<void>::success();
}

void TerminalApplication::copy_selection(std::string_view selection) {
  std::cout << terminal_clipboard_sequence(selection) << std::flush;
  copy_status_error_ = !std::cout.good();
  copy_status_ = copy_status_error_ ? "copy failed" : "selection copied";
}

void TerminalApplication::submit_line() {
  if (input_.empty() || busy_)
    return;
  const std::string line = std::exchange(input_, {});
  const auto trimmed_line = trim_command_whitespace(line);
  if (trimmed_line.empty())
    return;
  if (trimmed_line.starts_with('/')) {
    const auto command = parse_terminal_command(trimmed_line);
    if (command.name == "exit") {
      app_->Exit();
      return;
    }
    if (worker_.joinable())
      worker_.join();
    busy_ = true;
    scroll_state_.follow_latest();
    const std::string display =
        "/" + command.name +
        (command.arguments.empty() ? std::string{} : " " + command.arguments);
    const bool opens_document_view = terminal_command_opens_document_view(
        command.name, command.arguments, command_hints_);
    transcript_.begin_request(display, TerminalActivity::action);
    active_cancellation_ = std::make_shared<core::CancellationSource>();
    const auto cancellation = active_cancellation_;
    worker_ = std::thread(
        [this, command, cancellation, display, opens_document_view] {
          const auto post_event = [this](core::AgentEvent event) {
            app_->Post([this, event = std::move(event)] {
              transcript_.append_event(event);
            });
          };
          auto result = command_(command.name, command.arguments,
                                 cancellation->token(), post_event);
          std::optional<TerminalDocumentView> document_view;
          if (opens_document_view && result) {
            auto parsed = parse_terminal_document_view(result.value());
            if (!parsed) {
              result = core::Result<std::string>::failure(parsed.error());
            } else {
              document_view = std::move(parsed.value());
            }
          }
          // Command handlers own the mutable runtime state. Snapshot it on the
          // worker after the handler returns, then publish only value copies to
          // the UI thread so rendering never races with command-side mutations.
          auto displayed_model = model_state_;
          const auto displayed_max_context_tokens = max_context_tokens_state_;
          const auto displayed_reasoning_effort = reasoning_effort_state_;
          const auto displayed_theme_kind = theme_kind_state_;
          app_->Post([this, command, display, result = std::move(result),
                      document_view = std::move(document_view),
                      displayed_model = std::move(displayed_model),
                      displayed_max_context_tokens, displayed_reasoning_effort,
                      displayed_theme_kind]() mutable {
            displayed_model_ = std::move(displayed_model);
            displayed_max_context_tokens_ = displayed_max_context_tokens;
            displayed_reasoning_effort_ = displayed_reasoning_effort;
            displayed_theme_kind_ = displayed_theme_kind;
            bool appended_after_restore = false;
            if (document_view.has_value()) {
              auto opened = core::Result<std::string>::success(
                  "Opened " + document_view->title + " interactive view.");
              transcript_.complete_request(opened);
              document_view_ = std::move(document_view);
              selected_document_page_ = 0;
              document_sidebar_focused_ = true;
              document_scroll_state_.reset_to_top();
            } else if (result && terminal_command_reloads_session(
                                     command.name, command.arguments)) {
              const auto restored = reload_session();
              if (!restored) {
                result = core::Result<std::string>::failure(restored.error());
              } else {
                transcript_.append_command(display, result);
                appended_after_restore = true;
              }
            }
            if (!document_view_.has_value() && !appended_after_restore)
              transcript_.complete_request(result);
            busy_ = false;
            active_cancellation_.reset();
          });
        });
    return;
  }

  if (worker_.joinable())
    worker_.join();
  busy_ = true;
  scroll_state_.follow_latest();
  const auto initial_activity = initial_activity_(trimmed_line);
  if (initial_activity != TerminalActivity::action)
    prompt_history_.remember(std::string(trimmed_line));
  transcript_.begin_request(std::string(trimmed_line), initial_activity);
  active_cancellation_ = std::make_shared<core::CancellationSource>();
  const auto cancellation = active_cancellation_;
  worker_ = std::thread([this, line = std::string(trimmed_line), cancellation] {
    constexpr auto kFrameInterval = std::chrono::milliseconds(33);
    std::string pending_delta;
    auto last_delta_post = std::chrono::steady_clock::time_point{};
    bool delta_posted = false;

    const auto post_event = [this](core::AgentEvent event) {
      app_->Post([this, event = std::move(event)] {
        transcript_.append_event(event);
      });
    };
    const auto flush_delta = [&] {
      if (pending_delta.empty())
        return;
      post_event({core::AgentEventType::assistant_delta,
                  std::exchange(pending_delta, {}), std::nullopt,
                  std::nullopt});
      last_delta_post = std::chrono::steady_clock::now();
      delta_posted = true;
    };

    const auto result = submit_(
        line, cancellation->token(), [&](const core::AgentEvent &event) {
          if (event.type == core::AgentEventType::assistant_delta) {
            pending_delta += event.text;
            const auto now = std::chrono::steady_clock::now();
            if (!delta_posted || now - last_delta_post >= kFrameInterval)
              flush_delta();
            return;
          }
          flush_delta();
          post_event(event);
        });
    flush_delta();
    app_->Post([this, result = std::move(result)]() mutable {
      transcript_.complete_request(result);
      busy_ = false;
      active_cancellation_.reset();
    });
  });
}

} // namespace zed::ui
