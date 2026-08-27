#include "zed/ui/terminal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "zed/ui/markdown.hpp"

#include <ftxui/component/animation.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

namespace zed::ui {

namespace {

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

std::size_t box_rows(const ftxui::Box &box) {
  if (box.y_max < box.y_min)
    return 0;
  return static_cast<std::size_t>(box.y_max - box.y_min) + 1;
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

ftxui::Element render_message(const TerminalMessage &message,
                              const TerminalTheme &theme) {
  switch (message.kind) {
  case TerminalMessageKind::user:
    return ftxui::hbox({
        ftxui::text("› ") | ftxui::bold | ftxui::color(theme.primary),
        ftxui::paragraph(message.content) | ftxui::bold |
            ftxui::color(theme.text) | ftxui::xflex,
    });
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
            ftxui::text(" · click to collapse") |
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

std::string terminal_activity_label(TerminalActivity activity,
                                    core::ReasoningEffort reasoning_effort) {
  switch (activity) {
  case TerminalActivity::idle:
    return "idle";
  case TerminalActivity::thinking:
    return std::string(core::reasoning_effort_name(reasoning_effort)) +
           " thinking";
  case TerminalActivity::action:
    return "action";
  case TerminalActivity::stream:
    return "stream";
  case TerminalActivity::cancelling:
    return "cancelling";
  }
  return "idle";
}

std::string terminal_token_summary(const TerminalTokenMetrics &metrics) {
  return "ctx " + compact_token_count(metrics.context_tokens) + " · ↑ " +
         compact_token_count(metrics.input_tokens) + " · ↓ " +
         compact_token_count(metrics.output_tokens) + " → " +
         compact_token_count(metrics.all_tokens());
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

bool TerminalWheelScrollFilter::accept(TerminalScrollDirection direction,
                                       Clock::time_point event_time) {
  constexpr auto kWheelBurstIdleTime = std::chrono::milliseconds(120);
  const bool direction_changed =
      last_direction_.has_value() && *last_direction_ != direction;
  const bool burst_ended =
      !last_event_time_.has_value() || event_time < *last_event_time_ ||
      event_time - *last_event_time_ >= kWheelBurstIdleTime;
  last_direction_ = direction;
  last_event_time_ = event_time;
  return direction_changed || burst_ended;
}

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
                        "running…", true, false, false});
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
  case AgentEventType::tool_result:
    activity_ = TerminalActivity::thinking;
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
      if (message.content == "running…")
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
                    result.value(), true, false, false});
  } else {
    append_message({TerminalMessageKind::command, std::move(command),
                    result.error().message, true, false, true});
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

ftxui::Element
render_terminal_welcome(std::string_view workspace, std::string_view model,
                        std::string_view version, std::string_view session,
                        core::ReasoningEffort reasoning_effort,
                        bool quick_bash_enabled, ThemeKind theme_kind) {
  const auto &theme = terminal_theme(theme_kind);
  const auto metadata = [&](std::string label, std::string value) {
    return ftxui::hbox({
        ftxui::text(std::move(label)) | ftxui::color(theme.text_muted),
        ftxui::text(std::move(value)) | ftxui::color(theme.text),
    });
  };
  return ftxui::vbox({
             ftxui::hbox({
                 ftxui::text(">_ ") | ftxui::color(theme.text_muted),
                 ftxui::text("zeda " + std::string(version)) | ftxui::bold |
                     ftxui::color(theme.primary),
             }),
             ftxui::text("A predictable C++ coding agent for this workspace.") |
                 ftxui::color(theme.text_muted),
             ftxui::separatorEmpty(),
             metadata("model:      ", std::string(model)),
             metadata("reasoning:  ", std::string(core::reasoning_effort_name(
                                          reasoning_effort))),
             metadata("quick bash: ", quick_bash_enabled ? "on" : "off"),
             metadata("theme:      ", std::string(theme_name(theme_kind))),
             metadata("directory:  ", std::string(workspace)),
             metadata("session:    ", std::string(session)),
             metadata("tools:      ", "read · write · edit · grep · bash"),
             ftxui::separatorEmpty(),
             ftxui::text("Enter sends · Esc interrupts · mouse wheel scrolls") |
                 ftxui::color(theme.text_muted),
             ftxui::text("Commands: /help · /theme <light|monaka> · "
                         "/quick-bash <on|off> · /reasoning <level>") |
                 ftxui::color(theme.text_muted),
         }) |
         ftxui::bgcolor(theme.background_panel) |
         ftxui::borderStyled(ftxui::ROUNDED, theme.border);
}

TerminalRenderer::TerminalRenderer(std::ostream &output,
                                   TerminalOptions options)
    : output_(output), options_(options) {}

void TerminalRenderer::banner(std::string_view workspace,
                              std::string_view model, std::string_view version,
                              std::string_view session,
                              bool quick_bash_enabled) {
  output_ << render_text("zeda " + std::string(version), Tone::title) << "\n"
          << render_text("workspace: " + std::string(workspace)) << "\n"
          << render_text("model: " + std::string(model)) << "\n"
          << render_text("session: " + std::string(session)) << "\n"
          << render_text(std::string("quick bash: ") +
                         (quick_bash_enabled ? "on" : "off"))
          << "\n"
          << render_text("theme: " + std::string(theme_name(options_.theme)))
          << "\n"
          << render_text("commands: /help /theme <light|monaka> /reasoning "
                         "<level> /session /skills /quick-bash <on|off> "
                         "/skill <name> /exit")
          << "\n\n";
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
    break;
  case AgentEventType::tool_result:
    output_ << render_element(ftxui::paragraph(event.text) |
                              ftxui::color(terminal_theme(options_.theme).text))
            << "\n"
            << std::flush;
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
    std::string workspace, std::string model, std::string version,
    core::ReasoningEffort &reasoning_effort, ThemeKind &theme_kind,
    QuickBashState quick_bash_enabled, InitialActivity initial_activity,
    std::vector<TerminalCommandHint> command_hints, std::string session,
    SubmitHandler submit, CommandHandler command)
    : workspace_(std::move(workspace)), model_(std::move(model)),
      version_(std::move(version)), reasoning_effort_(reasoning_effort),
      theme_kind_(theme_kind),
      quick_bash_enabled_(std::move(quick_bash_enabled)),
      initial_activity_(std::move(initial_activity)),
      command_hints_(std::move(command_hints)), session_(std::move(session)),
      submit_(std::move(submit)), command_(std::move(command)) {}

TerminalApplication::~TerminalApplication() {
  if (active_cancellation_ != nullptr)
    active_cancellation_->cancel();
  if (worker_.joinable())
    worker_.join();
}

core::Result<void> TerminalApplication::run() {
  if (!submit_ || !command_ || !quick_bash_enabled_ || !initial_activity_) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "terminal application handlers are not configured",
    });
  }

  app_ = std::make_unique<ftxui::App>(ftxui::App::FullscreenAlternateScreen());
  app_->ForceHandleCtrlC(false);

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
    const auto &theme = terminal_theme(theme_kind_);
    state.element |=
        ftxui::color(state.is_placeholder ? theme.text_muted : theme.text);
    state.element |= ftxui::bgcolor(theme.input_background);
    if (state.hovered)
      state.element |= ftxui::underlined;
    return state.element;
  };
  input_component_ = ftxui::Input(&input_, input_options);

  auto root = ftxui::Renderer(input_component_, [this] {
    const auto &theme = terminal_theme(theme_kind_);
    const auto activity = transcript_.activity();
    if (activity != TerminalActivity::idle) {
      ftxui::animation::RequestAnimationFrame();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    const auto spinner_frame = static_cast<std::size_t>(elapsed.count() / 80);
    auto content = ftxui::vbox({
        render_terminal_welcome(workspace_, model_, version_, session_,
                                reasoning_effort_, quick_bash_enabled_(),
                                theme_kind_),
        ftxui::separatorEmpty(),
        render_messages(transcript_.messages(), theme, &message_boxes_),
    });
    content |= ftxui::reflect(scroll_content_box_);
    if (scroll_state_.follows_latest()) {
      content |= ftxui::focusPositionRelative(0.0F, 1.0F);
    } else {
      content |= ftxui::focusPosition(
          0, scroll_state_.focus_row(box_rows(scroll_content_box_),
                                     box_rows(scroll_viewport_box_)));
    }
    auto body = content | ftxui::vscroll_indicator | ftxui::yframe |
                ftxui::flex | ftxui::reflect(scroll_viewport_box_);
    auto composer =
        ftxui::hbox({
            ftxui::text("› ") | ftxui::bold | ftxui::color(theme.primary),
            input_component_->Render() | ftxui::flex,
        }) |
        ftxui::bgcolor(theme.input_background);
    const auto command_suggestions =
        terminal_command_suggestions(input_, command_hints_);
    const auto *command_help = terminal_command_help(input_, command_hints_);
    if (selected_command_suggestion_ >= command_suggestions.size()) {
      selected_command_suggestion_ = 0;
    }
    ftxui::Elements suggestion_rows;
    constexpr std::size_t kMaxVisibleSuggestions = 6;
    const auto first_visible_suggestion =
        selected_command_suggestion_ < kMaxVisibleSuggestions
            ? 0
            : selected_command_suggestion_ - kMaxVisibleSuggestions + 1;
    const auto visible_suggestions =
        std::min(command_suggestions.size() - first_visible_suggestion,
                 kMaxVisibleSuggestions);
    suggestion_rows.reserve(visible_suggestions + 3);
    if (command_help != nullptr) {
      suggestion_rows.push_back(ftxui::hbox({
          ftxui::text("/" + command_help->name) | ftxui::bold |
              ftxui::color(theme.primary),
          ftxui::text("  " + command_help->description) |
              ftxui::color(theme.text_muted),
      }));
      std::string options = "secondary options: ";
      if (command_help->options.empty()) {
        options += "none";
      } else {
        for (std::size_t index = 0; index < command_help->options.size();
             ++index) {
          if (index > 0)
            options += " · ";
          options += command_help->options[index].value;
        }
      }
      suggestion_rows.push_back(ftxui::paragraph(std::move(options)) |
                                ftxui::color(theme.text_muted));
    }
    if (visible_suggestions > 0) {
      suggestion_rows.push_back(
          ftxui::text(command_suggestions.front().option
                          ? "options · ↑/↓ select · tab complete"
                          : "commands · ↑/↓ select · tab complete") |
          ftxui::color(theme.text_muted));
    }
    for (std::size_t offset = 0; offset < visible_suggestions; ++offset) {
      const auto index = first_visible_suggestion + offset;
      const auto &suggestion = command_suggestions[index];
      const bool selected = index == selected_command_suggestion_;
      auto row = ftxui::hbox({
          ftxui::text(selected ? "› " : "  ") | ftxui::color(theme.secondary),
          ftxui::text(suggestion.value) | ftxui::bold |
              ftxui::color(selected ? theme.secondary : theme.primary),
          ftxui::text("  " + suggestion.description) |
              ftxui::color(theme.text_muted),
      });
      if (selected)
        row |= ftxui::bgcolor(theme.input_background);
      suggestion_rows.push_back(std::move(row));
    }
    auto suggestions = ftxui::vbox(std::move(suggestion_rows));
    const bool command_guide_visible =
        command_help != nullptr || !command_suggestions.empty();
    auto footer = ftxui::hbox({
        render_activity(activity, reasoning_effort_, spinner_frame, theme),
        ftxui::filler(),
        ftxui::text(terminal_token_summary(transcript_.token_metrics())) |
            ftxui::color(theme.text_muted),
        ftxui::text(command_suggestions.empty() ? " · ? for shortcuts"
                                                : " · tab completes") |
            ftxui::color(theme.text_muted),
    });
    ftxui::Elements layout{body, ftxui::separatorEmpty()};
    if (command_guide_visible) {
      layout.push_back(std::move(suggestions));
    }
    layout.push_back(std::move(composer));
    layout.push_back(std::move(footer));
    return ftxui::vbox(std::move(layout)) | ftxui::color(theme.text) |
           ftxui::bgcolor(theme.background) |
           ftxui::selectionForegroundColor(theme.text) |
           ftxui::selectionBackgroundColor(theme.background_element);
  });
  root |= ftxui::CatchEvent([this](ftxui::Event event) {
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
      if (event == ftxui::Event::Tab) {
        input_ = complete_terminal_command(input_, command_hints_,
                                           selected_command_suggestion_);
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
    if (event.is_mouse() && event.mouse().button == ftxui::Mouse::Left &&
        event.mouse().motion == ftxui::Mouse::Pressed) {
      const auto &messages = transcript_.messages();
      const auto box_count = std::min(messages.size(), message_boxes_.size());
      for (std::size_t index = 0; index < box_count; ++index) {
        if (messages[index].collapsible &&
            message_boxes_[index].Contain(event.mouse().x, event.mouse().y)) {
          return transcript_.toggle_message_expansion(index);
        }
      }
    }
    if (event.is_mouse() && event.mouse().motion == ftxui::Mouse::Pressed &&
        (event.mouse().button == ftxui::Mouse::WheelUp ||
         event.mouse().button == ftxui::Mouse::WheelDown)) {
      const auto direction = event.mouse().button == ftxui::Mouse::WheelUp
                                 ? TerminalScrollDirection::up
                                 : TerminalScrollDirection::down;
      if (!wheel_scroll_filter_.accept(direction,
                                       std::chrono::steady_clock::now())) {
        return true;
      }
      if (direction == TerminalScrollDirection::up) {
        scroll_state_.scroll_up(box_rows(scroll_content_box_),
                                box_rows(scroll_viewport_box_));
      } else {
        scroll_state_.scroll_down(box_rows(scroll_content_box_),
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
  });

  app_->Loop(root);
  if (active_cancellation_ != nullptr)
    active_cancellation_->cancel();
  if (worker_.joinable())
    worker_.join();
  input_component_.reset();
  app_.reset();
  return core::Result<void>::success();
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
    const auto result = command_(command.name, command.arguments);
    scroll_state_.follow_latest();
    const std::string display =
        "/" + command.name +
        (command.arguments.empty() ? std::string{} : " " + command.arguments);
    transcript_.append_command(display, result);
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
