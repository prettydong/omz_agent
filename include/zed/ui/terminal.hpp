#pragma once

#include <chrono>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "zed/core/agent_event.hpp"
#include "zed/core/cancellation.hpp"
#include "zed/core/model.hpp"
#include "zed/core/result.hpp"
#include "zed/ui/theme.hpp"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component_base.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace zed::ui {

struct TerminalOptions {
  bool color{true};
  ThemeKind theme{ThemeKind::light};
};

enum class TerminalActivity {
  idle,
  thinking,
  action,
  stream,
  cancelling,
};

[[nodiscard]] std::string
terminal_activity_label(TerminalActivity activity,
                        core::ReasoningEffort reasoning_effort);

struct TerminalTokenMetrics {
  core::TokenCount context_tokens{};
  core::TokenCount input_tokens{};
  core::TokenCount output_tokens{};

  [[nodiscard]] core::TokenCount all_tokens() const {
    return input_tokens + output_tokens;
  }
};

[[nodiscard]] std::string
terminal_token_summary(const TerminalTokenMetrics &metrics);

enum class TerminalMessageKind {
  user,
  assistant,
  tool,
  command,
  error,
};

struct TerminalMessage {
  TerminalMessageKind kind{TerminalMessageKind::assistant};
  std::string label;
  std::string content;
  bool collapsible{false};
  bool expanded{true};
  bool is_error{false};
};

struct TerminalCommand {
  std::string name;
  std::string arguments;
};

[[nodiscard]] TerminalCommand parse_terminal_command(std::string_view line);

struct TerminalCommandOption {
  std::string value;
  std::string description;
};

struct TerminalCommandHint {
  std::string name;
  std::string description;
  std::vector<TerminalCommandOption> options;
};

struct TerminalCommandSuggestion {
  std::string value;
  std::string description;
  std::string completion;
  bool option{false};
};

[[nodiscard]] std::vector<TerminalCommandSuggestion>
terminal_command_suggestions(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands);

[[nodiscard]] const TerminalCommandHint *terminal_command_help(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands);

[[nodiscard]] std::string complete_terminal_command(
    std::string_view input,
    const std::vector<TerminalCommandHint> &available_commands,
    std::size_t selected_suggestion = 0);

class TerminalTranscript {
public:
  void
  begin_request(std::string user_input,
                TerminalActivity initial_activity = TerminalActivity::thinking);
  void cancel_request();
  void append_event(const core::AgentEvent &event);
  void complete_request(const core::Result<std::string> &result);
  void append_command(std::string command,
                      const core::Result<std::string> &result);

  [[nodiscard]] const std::vector<TerminalMessage> &messages() const;
  [[nodiscard]] TerminalActivity activity() const;
  [[nodiscard]] TerminalTokenMetrics token_metrics() const;
  bool toggle_message_expansion(std::size_t index);

private:
  std::size_t append_message(TerminalMessage message);

  std::vector<TerminalMessage> messages_;
  std::optional<std::size_t> active_assistant_;
  std::optional<std::size_t> active_tool_;
  std::optional<std::size_t> active_direct_command_;
  bool request_ended_{false};
  bool request_error_seen_{false};
  TerminalActivity activity_{TerminalActivity::idle};
  TerminalTokenMetrics token_metrics_;
};

ftxui::Element
render_terminal_messages(const std::vector<TerminalMessage> &messages);
ftxui::Element
render_terminal_messages(const std::vector<TerminalMessage> &messages,
                         const TerminalTheme &theme);

ftxui::Element
render_terminal_welcome(std::string_view workspace, std::string_view model,
                        std::string_view version, std::string_view session,
                        core::ReasoningEffort reasoning_effort,
                        bool quick_bash_enabled, ThemeKind theme_kind);

class TerminalScrollState {
public:
  void scroll_up(std::size_t content_rows, std::size_t viewport_rows);
  void scroll_down(std::size_t content_rows, std::size_t viewport_rows);
  void follow_latest();

  [[nodiscard]] std::size_t offset_rows(std::size_t content_rows,
                                        std::size_t viewport_rows) const;
  [[nodiscard]] int focus_row(std::size_t content_rows,
                              std::size_t viewport_rows) const;
  [[nodiscard]] bool follows_latest() const;

private:
  std::size_t offset_rows_{0};
  bool follows_latest_{true};
};

enum class TerminalScrollDirection {
  up,
  down,
};

class TerminalWheelScrollFilter {
public:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] bool accept(TerminalScrollDirection direction,
                            Clock::time_point event_time);

private:
  std::optional<TerminalScrollDirection> last_direction_;
  std::optional<Clock::time_point> last_event_time_;
};

class TerminalPromptHistory {
public:
  void remember(std::string prompt);
  void reset_navigation();

  [[nodiscard]] std::optional<std::string>
  previous(std::string_view current_input);
  [[nodiscard]] std::optional<std::string> next();

private:
  std::vector<std::string> prompts_;
  std::optional<std::size_t> position_;
  std::string draft_;
};

class TerminalRenderer {
public:
  explicit TerminalRenderer(std::ostream &output, TerminalOptions options = {});

  void banner(std::string_view workspace, std::string_view model,
              std::string_view version, std::string_view session,
              bool quick_bash_enabled = false);
  void prompt();
  void render(const core::AgentEvent &event);
  void error(std::string_view message);
  void set_theme(ThemeKind theme);

private:
  enum class Tone {
    normal,
    title,
    prompt,
    tool,
    error,
  };

  std::string render_text(std::string_view text,
                          Tone tone = Tone::normal) const;

  std::ostream &output_;
  TerminalOptions options_;
  bool assistant_stream_open_{false};
};

class TerminalInput {
public:
  explicit TerminalInput(std::istream &input);

  core::Result<std::string> read_line();

private:
  std::istream &input_;
};

class TerminalApplication {
public:
  using SubmitHandler = std::function<core::Result<std::string>(
      std::string, core::CancellationToken, core::AgentEventCallback)>;
  using CommandHandler = std::function<core::Result<std::string>(
      std::string_view, std::string_view)>;
  using QuickBashState = std::function<bool()>;
  using InitialActivity = std::function<TerminalActivity(std::string_view)>;

  TerminalApplication(std::string workspace, std::string model,
                      std::string version,
                      core::ReasoningEffort &reasoning_effort,
                      ThemeKind &theme_kind, QuickBashState quick_bash_enabled,
                      InitialActivity initial_activity,
                      std::vector<TerminalCommandHint> command_hints,
                      std::string session, SubmitHandler submit,
                      CommandHandler command);
  ~TerminalApplication();

  core::Result<void> run();

private:
  void submit_line();

  std::string workspace_;
  std::string model_;
  std::string version_;
  core::ReasoningEffort &reasoning_effort_;
  ThemeKind &theme_kind_;
  QuickBashState quick_bash_enabled_;
  InitialActivity initial_activity_;
  std::vector<TerminalCommandHint> command_hints_;
  std::string session_;
  SubmitHandler submit_;
  CommandHandler command_;
  std::unique_ptr<ftxui::App> app_;
  ftxui::Component input_component_;
  std::string input_;
  int input_cursor_position_{0};
  std::size_t selected_command_suggestion_{0};
  TerminalTranscript transcript_;
  std::vector<ftxui::Box> message_boxes_;
  ftxui::Box scroll_content_box_;
  ftxui::Box scroll_viewport_box_;
  TerminalScrollState scroll_state_;
  TerminalWheelScrollFilter wheel_scroll_filter_;
  TerminalPromptHistory prompt_history_;
  bool busy_{false};
  std::shared_ptr<core::CancellationSource> active_cancellation_;
  std::thread worker_;
};

} // namespace zed::ui
