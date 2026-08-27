#include <cassert>
#include <chrono>
#include <sstream>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include "zed/ui/markdown.hpp"
#include "zed/ui/terminal.hpp"

namespace {

std::string render(ftxui::Element element) {
  const auto width = ftxui::Dimension::Fit(element);
  const auto height = ftxui::Dimension::Fit(element);
  auto screen = ftxui::Screen::Create(width, height);
  ftxui::Render(screen, element);
  return screen.ToString();
}

std::string render_in_box(ftxui::Element element, int width, int height) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  return screen.ToString();
}

int rendered_content_rows(ftxui::Element element, int width, int height) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  int rows = 0;
  for (int y = 0; y < screen.dimy(); ++y) {
    bool has_content = false;
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &character = screen.CellAt(x, y).character;
      if (!character.empty() && character != " ") {
        has_content = true;
        break;
      }
    }
    if (has_content) {
      ++rows;
    }
  }
  return rows;
}

bool rendered_glyph_has_style(ftxui::Element element, std::string_view glyph,
                              bool bold, bool inverted) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(element),
                                      ftxui::Dimension::Fit(element));
  ftxui::Render(screen, element);
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &cell = screen.CellAt(x, y);
      if (cell.character == glyph && cell.bold == bold &&
          cell.inverted == inverted) {
        return true;
      }
    }
  }
  return false;
}

std::size_t count_occurrences(const std::string &value,
                              const std::string &needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = value.find(needle, offset)) != std::string::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

} // namespace

int main() {
  ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);

  const auto help_command = zed::ui::parse_terminal_command("/help\n");
  assert(help_command.name == "help");
  assert(help_command.arguments.empty());
  const auto reasoning_command =
      zed::ui::parse_terminal_command("  /reasoning\t high\r\n");
  assert(reasoning_command.name == "reasoning");
  assert(reasoning_command.arguments == "high");

  const std::vector<zed::ui::TerminalCommandHint> command_hints{
      {"help", "Show help."},
      {"skills", "List skills."},
      {"skill",
       "Activate a skill.",
       {{"review", "Review code."}, {"test", "Run tests."}}},
      {"theme",
       "Change theme.",
       {{"light", "Light theme."}, {"monaka", "Dark theme."}}},
      {"exit", "Quit."},
  };
  assert(zed::ui::terminal_command_suggestions("hello", command_hints).empty());
  assert(zed::ui::terminal_command_suggestions("/unknown ", command_hints)
             .empty());
  const auto all_command_suggestions =
      zed::ui::terminal_command_suggestions("/", command_hints);
  assert(all_command_suggestions.size() == command_hints.size());
  assert(all_command_suggestions[0].value == "/help");
  const auto theme_suggestions =
      zed::ui::terminal_command_suggestions("/th", command_hints);
  assert(theme_suggestions.size() == 1);
  assert(theme_suggestions[0].value == "/theme");
  assert(!theme_suggestions[0].option);
  assert(zed::ui::terminal_command_help("/th", command_hints) == nullptr);
  const auto *theme_help =
      zed::ui::terminal_command_help("/theme", command_hints);
  assert(theme_help != nullptr);
  assert(theme_help->description == "Change theme.");
  assert(theme_help->options.size() == 2);
  const auto theme_options =
      zed::ui::terminal_command_suggestions("/theme", command_hints);
  assert(theme_options.size() == 2);
  assert(theme_options[0].value == "light");
  assert(theme_options[0].option);
  assert(
      zed::ui::terminal_command_suggestions("/theme ", command_hints).size() ==
      2);
  const auto monaka_options =
      zed::ui::terminal_command_suggestions("/theme m", command_hints);
  assert(monaka_options.size() == 1);
  assert(monaka_options[0].value == "monaka");
  assert(zed::ui::terminal_command_suggestions("/theme monaka ", command_hints)
             .empty());
  assert(zed::ui::terminal_command_help("/theme monaka ", command_hints) ==
         theme_help);
  const auto skill_suggestions =
      zed::ui::terminal_command_suggestions("/ski", command_hints);
  assert(skill_suggestions.size() == 2);
  assert(zed::ui::complete_terminal_command("/th", command_hints) == "/theme ");
  assert(zed::ui::complete_terminal_command("/theme", command_hints) ==
         "/theme light");
  assert(zed::ui::complete_terminal_command("/theme m", command_hints) ==
         "/theme monaka");
  assert(zed::ui::complete_terminal_command("/ski", command_hints, 1) ==
         "/skill ");
  assert(zed::ui::complete_terminal_command("ordinary", command_hints) ==
         "ordinary");

  assert(zed::ui::terminal_activity_label(zed::ui::TerminalActivity::thinking,
                                          zed::core::ReasoningEffort::low) ==
         "low thinking");
  assert(zed::ui::terminal_activity_label(zed::ui::TerminalActivity::thinking,
                                          zed::core::ReasoningEffort::high) ==
         "high thinking");
  assert(zed::ui::terminal_activity_label(zed::ui::TerminalActivity::action,
                                          zed::core::ReasoningEffort::low) ==
         "action");

  zed::ui::TerminalScrollState scroll_state;
  assert(scroll_state.follows_latest());
  assert(scroll_state.offset_rows(100, 20) == 80);
  assert(scroll_state.focus_row(100, 20) == 89);
  scroll_state.scroll_up(100, 20);
  assert(!scroll_state.follows_latest());
  assert(scroll_state.offset_rows(100, 20) == 77);
  assert(scroll_state.focus_row(100, 20) == 86);
  scroll_state.scroll_up(100, 20);
  assert(scroll_state.offset_rows(100, 20) == 74);
  scroll_state.scroll_down(100, 20);
  assert(scroll_state.offset_rows(100, 20) == 77);
  assert(!scroll_state.follows_latest());
  scroll_state.scroll_down(100, 20);
  assert(scroll_state.follows_latest());
  assert(scroll_state.offset_rows(100, 20) == 80);
  scroll_state.scroll_up(10, 20);
  assert(!scroll_state.follows_latest());
  assert(scroll_state.offset_rows(10, 20) == 0);
  scroll_state.scroll_down(10, 20);
  assert(scroll_state.follows_latest());

  zed::ui::TerminalScrollState filtered_scroll_state;
  zed::ui::TerminalWheelScrollFilter wheel_scroll_filter;
  const auto first_wheel_event =
      zed::ui::TerminalWheelScrollFilter::Clock::time_point{};
  const auto apply_wheel_event = [&](zed::ui::TerminalScrollDirection direction,
                                     std::chrono::milliseconds delay) {
    if (!wheel_scroll_filter.accept(direction, first_wheel_event + delay))
      return;
    if (direction == zed::ui::TerminalScrollDirection::up) {
      filtered_scroll_state.scroll_up(100, 20);
    } else {
      filtered_scroll_state.scroll_down(100, 20);
    }
  };
  apply_wheel_event(zed::ui::TerminalScrollDirection::up,
                    std::chrono::milliseconds(0));
  apply_wheel_event(zed::ui::TerminalScrollDirection::up,
                    std::chrono::milliseconds(20));
  apply_wheel_event(zed::ui::TerminalScrollDirection::up,
                    std::chrono::milliseconds(100));
  assert(filtered_scroll_state.offset_rows(100, 20) == 77);
  apply_wheel_event(zed::ui::TerminalScrollDirection::up,
                    std::chrono::milliseconds(220));
  assert(filtered_scroll_state.offset_rows(100, 20) == 74);
  apply_wheel_event(zed::ui::TerminalScrollDirection::down,
                    std::chrono::milliseconds(225));
  assert(filtered_scroll_state.offset_rows(100, 20) == 77);

  ftxui::Elements scroll_fixture_rows;
  for (int row = 0; row < 30; ++row) {
    const auto label =
        row < 10 ? "row 0" + std::to_string(row) : "row " + std::to_string(row);
    scroll_fixture_rows.push_back(ftxui::text(label));
  }
  const auto scroll_fixture = ftxui::vbox(std::move(scroll_fixture_rows));
  const auto latest_view = render_in_box(
      scroll_fixture | ftxui::focusPositionRelative(0.0F, 1.0F) | ftxui::yframe,
      10, 10);
  assert(latest_view.starts_with("row 20"));
  zed::ui::TerminalScrollState viewport_scroll_state;
  viewport_scroll_state.scroll_up(30, 10);
  const auto three_rows_up_view = render_in_box(
      scroll_fixture |
          ftxui::focusPosition(0, viewport_scroll_state.focus_row(30, 10)) |
          ftxui::yframe,
      10, 10);
  assert(three_rows_up_view.starts_with("row 17"));

  const auto welcome = render(zed::ui::render_terminal_welcome(
      "/tmp/workspace", "fixture-model", "0.1.0", "session-fixture",
      zed::core::ReasoningEffort::low, true, zed::ui::ThemeKind::light));
  assert(welcome.find("zeda 0.1.0") != std::string::npos);
  assert(welcome.find("fixture-model") != std::string::npos);
  assert(welcome.find("low") != std::string::npos);
  assert(welcome.find("session-fixture") != std::string::npos);
  assert(welcome.find("quick bash:") != std::string::npos);
  assert(welcome.find("theme:") != std::string::npos);
  assert(welcome.find("mouse wheel scrolls") != std::string::npos);

  assert(zed::ui::theme_kind_from_name("light") == zed::ui::ThemeKind::light);
  assert(zed::ui::theme_kind_from_name("monaka") == zed::ui::ThemeKind::monaka);
  assert(!zed::ui::theme_kind_from_name("monokai").has_value());
  assert(!zed::ui::theme_kind_from_name("custom").has_value());
  const auto &light_theme = zed::ui::terminal_theme(zed::ui::ThemeKind::light);
  assert(light_theme.input_background.Print(true) !=
         light_theme.background.Print(true));
  assert(light_theme.markdown_heading.Print(false) !=
         light_theme.markdown_link.Print(false));
  assert(light_theme.markdown_link.Print(false) !=
         light_theme.markdown_code.Print(false));
  assert(light_theme.markdown_code.Print(false) !=
         light_theme.markdown_strong.Print(false));
  assert(light_theme.markdown_heading.Print(false) !=
         light_theme.markdown_strong.Print(false));

  const auto markdown =
      "# Report\n\n"
      "Text with **bold**, `code`, and [a link](https://example.com).\n\n"
      "| Name | Score | Note |\n"
      "| :--- | ---: | :---: |\n"
      "| **Alice** | 10 | *good* |\n"
      "| Bob | 20 | uses an escaped \\| pipe |\n\n"
      "- first\n"
      "- second\n";

  const auto output = render(zed::ui::render_markdown(markdown));

  assert(output.find("Report") != std::string::npos);
  assert(output.find("Name") != std::string::npos);
  assert(output.find("Score") != std::string::npos);
  assert(output.find("Alice") != std::string::npos);
  assert(output.find("**Alice**") == std::string::npos);
  assert(output.find("*good*") == std::string::npos);
  assert(output.find("uses an escaped | pipe") != std::string::npos);
  assert(output.find("first") != std::string::npos);
  assert(output.find("┌") != std::string::npos);
  assert(output.find("╔") == std::string::npos);
  assert(rendered_glyph_has_style(zed::ui::render_markdown(markdown), "A", true,
                                  false));

  const auto soft_wrapped = render_in_box(
      zed::ui::render_markdown("first source line\nsecond **bold** source line",
                               light_theme),
      80, 4);
  assert(rendered_content_rows(
             zed::ui::render_markdown(
                 "first source line\nsecond **bold** source line", light_theme),
             80, 4) == 1);
  assert(soft_wrapped.find("first source line second") != std::string::npos);

  const auto cjk_wrapped = render_in_box(
      zed::ui::render_markdown("这是一段需要在终端宽度内自然换行的中文内容，中"
                               "间还有**加粗文字**以及后续内容。",
                               light_theme),
      20, 10);
  assert(
      rendered_content_rows(
          zed::ui::render_markdown(
              "这是一段需要在终端宽度内自然换行的中文内容，中间还有**加粗文字**"
              "以及后续内容。",
              light_theme),
          20, 10) >= 3);
  assert(cjk_wrapped.find("加粗文字") != std::string::npos);

  const std::vector<zed::ui::TerminalMessage> wrapped_messages{{
      zed::ui::TerminalMessageKind::assistant,
      {},
      "这是一段需要在终端宽度内自然换行的中文内容，中间还有**加粗文字**"
      "以及后续内容。",
  }};
  assert(rendered_content_rows(
             zed::ui::render_terminal_messages(wrapped_messages, light_theme),
             20, 10) >= 3);
  auto viewport_content = ftxui::vbox({
      zed::ui::render_terminal_welcome(
          "/tmp/workspace", "fixture-model", "0.1.0", "session-fixture",
          zed::core::ReasoningEffort::low, false, zed::ui::ThemeKind::light),
      ftxui::separatorEmpty(),
      zed::ui::render_terminal_messages(wrapped_messages, light_theme),
  });
  auto viewport =
      ftxui::vbox({viewport_content | ftxui::focusPositionRelative(0.0F, 1.0F) |
                       ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex,
                   ftxui::text("› "), ftxui::text("idle")});
  const auto viewport_output = render_in_box(viewport, 50, 24);
  assert(viewport_output.find("后续内容") != std::string::npos);

  zed::ui::TerminalTranscript transcript;
  assert(transcript.activity() == zed::ui::TerminalActivity::idle);
  transcript.begin_request(
      "This is user text, not Markdown:\n| input | only |\n| --- | --- |");
  assert(transcript.activity() == zed::ui::TerminalActivity::thinking);
  transcript.append_event({
      zed::core::AgentEventType::assistant_delta,
      "| Name | Value |\n| --- | ---: |\n| alpha |",
      {},
      {},
  });
  assert(transcript.activity() == zed::ui::TerminalActivity::stream);
  transcript.append_event({
      zed::core::AgentEventType::assistant_delta,
      " 10 |",
      {},
      {},
  });
  transcript.append_event({
      zed::core::AgentEventType::assistant_message,
      "| Name | Value |\n| --- | ---: |\n| alpha | 10 |",
      {},
      {},
  });
  transcript.append_event({
      zed::core::AgentEventType::agent_end,
      "| Name | Value |\n| --- | ---: |\n| alpha | 10 |",
      {},
      {},
  });
  transcript.complete_request(zed::core::Result<std::string>::success(
      "| Name | Value |\n| --- | ---: |\n| alpha | 10 |"));
  assert(transcript.activity() == zed::ui::TerminalActivity::idle);

  zed::ui::TerminalTranscript activity_transcript;
  activity_transcript.begin_request("use a tool");
  activity_transcript.append_event({
      zed::core::AgentEventType::tool_start,
      "read",
      {},
      {},
  });
  assert(activity_transcript.activity() == zed::ui::TerminalActivity::action);
  activity_transcript.append_event({
      zed::core::AgentEventType::tool_result,
      "done",
      {},
      {},
  });
  assert(activity_transcript.activity() == zed::ui::TerminalActivity::thinking);
  activity_transcript.cancel_request();
  assert(activity_transcript.activity() ==
         zed::ui::TerminalActivity::cancelling);

  zed::ui::TerminalTranscript private_bash_transcript;
  private_bash_transcript.begin_request("run a check");
  private_bash_transcript.append_event({
      zed::core::AgentEventType::tool_start,
      "Check the build status",
      zed::core::ToolCall{
          "bash-private", "bash",
          R"({"purpose":"Check the build status","command":"private-command --secret"})"},
      {},
  });
  private_bash_transcript.append_event({
      zed::core::AgentEventType::tool_result,
      "private output",
      {},
      zed::core::ToolResult{"bash-private", "private output", false},
  });
  auto private_bash_output = render(zed::ui::render_terminal_messages(
      private_bash_transcript.messages(), light_theme));
  assert(private_bash_output.find("Check the build status") !=
         std::string::npos);
  assert(private_bash_output.find("private-command") == std::string::npos);
  assert(private_bash_output.find("private output") == std::string::npos);
  assert(private_bash_output.find("click to expand") != std::string::npos);
  assert(private_bash_transcript.toggle_message_expansion(1));
  private_bash_output = render(zed::ui::render_terminal_messages(
      private_bash_transcript.messages(), light_theme));
  assert(private_bash_output.find("private output") != std::string::npos);
  assert(private_bash_output.find("private-command") == std::string::npos);

  zed::ui::TerminalTranscript failed_tool_transcript;
  failed_tool_transcript.begin_request("inspect a missing file");
  failed_tool_transcript.append_event({
      zed::core::AgentEventType::tool_start,
      "Inspect the requested file",
      {},
      {},
  });
  failed_tool_transcript.append_event({
      zed::core::AgentEventType::tool_result,
      "file not found",
      {},
      zed::core::ToolResult{"read-failure", "file not found", true},
  });
  const auto failed_tool_output = render(zed::ui::render_terminal_messages(
      failed_tool_transcript.messages(), light_theme));
  assert(failed_tool_output.find("failed") != std::string::npos);
  assert(failed_tool_output.find("file not found") == std::string::npos);

  zed::ui::TerminalTranscript command_transcript;
  command_transcript.append_command(
      "/theme monaka",
      zed::core::Result<std::string>::success("theme: monaka"));
  assert(command_transcript.messages().size() == 1);
  auto collapsed_command_output = render(zed::ui::render_terminal_messages(
      command_transcript.messages(), light_theme));
  assert(collapsed_command_output.find("/theme monaka") != std::string::npos);
  assert(collapsed_command_output.find("done") != std::string::npos);
  assert(collapsed_command_output.find("theme: monaka") == std::string::npos);
  assert(command_transcript.toggle_message_expansion(0));
  const auto expanded_command_output = render(zed::ui::render_terminal_messages(
      command_transcript.messages(), light_theme));
  assert(expanded_command_output.find("/theme monaka") != std::string::npos);
  assert(expanded_command_output.find("theme: monaka") != std::string::npos);
  assert(!command_transcript.toggle_message_expansion(4));

  zed::ui::TerminalTranscript failed_command_transcript;
  failed_command_transcript.append_command(
      "/theme ultraviolet",
      zed::core::Result<std::string>::failure(
          {zed::core::ErrorCode::invalid_argument, "unsupported theme"}));
  const auto failed_command_output = render(zed::ui::render_terminal_messages(
      failed_command_transcript.messages(), light_theme));
  assert(failed_command_output.find("/theme ultraviolet") != std::string::npos);
  assert(failed_command_output.find("failed") != std::string::npos);
  assert(failed_command_output.find("unsupported theme") == std::string::npos);

  zed::ui::TerminalPromptHistory prompt_history;
  assert(!prompt_history.previous("draft").has_value());
  prompt_history.remember("first prompt");
  prompt_history.remember("second prompt");
  assert(prompt_history.previous("unfinished draft") == "second prompt");
  assert(prompt_history.previous("second prompt") == "first prompt");
  assert(!prompt_history.previous("first prompt").has_value());
  assert(prompt_history.next() == "second prompt");
  assert(prompt_history.next() == "unfinished draft");
  assert(!prompt_history.next().has_value());
  assert(prompt_history.previous("new draft") == "second prompt");
  prompt_history.reset_navigation();
  assert(prompt_history.previous("changed draft") == "second prompt");
  assert(prompt_history.next() == "changed draft");

  zed::ui::TerminalTranscript usage_transcript;
  usage_transcript.append_event({
      zed::core::AgentEventType::assistant_message,
      "first",
      {},
      {},
      zed::core::ModelUsage{100, 25, 10},
  });
  usage_transcript.append_event({
      zed::core::AgentEventType::assistant_message,
      "second",
      {},
      {},
      zed::core::ModelUsage{60, 0, 5},
  });
  const auto token_metrics = usage_transcript.token_metrics();
  assert(token_metrics.context_tokens == 60);
  assert(token_metrics.input_tokens == 160);
  assert(token_metrics.output_tokens == 15);
  assert(token_metrics.all_tokens() == 175);
  assert(zed::ui::terminal_token_summary(token_metrics) ==
         "ctx 60 · ↑ 160 · ↓ 15 → 175");
  assert(zed::ui::terminal_token_summary({1'000, 1'234, 18'000}) ==
         "ctx 1k · ↑ 1.2k · ↓ 18k → 19.2k");
  assert(zed::ui::terminal_token_summary({2'450'000, 3'000'000, 1'500'000}) ==
         "ctx 2.4m · ↑ 3m · ↓ 1.5m → 4.5m");

  zed::ui::TerminalTranscript quick_bash_transcript;
  quick_bash_transcript.begin_request("pwd", zed::ui::TerminalActivity::action);
  assert(quick_bash_transcript.activity() == zed::ui::TerminalActivity::action);
  assert(quick_bash_transcript.messages().size() == 1);
  assert(quick_bash_transcript.messages()[0].kind ==
         zed::ui::TerminalMessageKind::command);
  quick_bash_transcript.append_event({
      zed::core::AgentEventType::tool_start,
      "quick bash",
      {},
      {},
  });
  assert(quick_bash_transcript.activity() == zed::ui::TerminalActivity::action);
  quick_bash_transcript.append_event({
      zed::core::AgentEventType::tool_result,
      "/tmp/workspace",
      {},
      zed::core::ToolResult{"quick-bash-fixture", "/tmp/workspace", false},
  });
  quick_bash_transcript.append_event({
      zed::core::AgentEventType::agent_end,
      {},
      {},
      {},
  });
  quick_bash_transcript.complete_request(
      zed::core::Result<std::string>::success("/tmp/workspace"));
  assert(quick_bash_transcript.activity() == zed::ui::TerminalActivity::idle);
  assert(quick_bash_transcript.token_metrics().all_tokens() == 0);
  auto collapsed_quick_bash = render(zed::ui::render_terminal_messages(
      quick_bash_transcript.messages(), light_theme));
  assert(collapsed_quick_bash.find("pwd") != std::string::npos);
  assert(collapsed_quick_bash.find("/tmp/workspace") == std::string::npos);
  assert(quick_bash_transcript.toggle_message_expansion(0));
  const auto expanded_quick_bash = render(zed::ui::render_terminal_messages(
      quick_bash_transcript.messages(), light_theme));
  assert(expanded_quick_bash.find("pwd") != std::string::npos);
  assert(expanded_quick_bash.find("/tmp/workspace") != std::string::npos);

  assert(transcript.messages().size() == 2);
  assert(transcript.messages()[0].kind == zed::ui::TerminalMessageKind::user);
  assert(transcript.messages()[1].kind ==
         zed::ui::TerminalMessageKind::assistant);
  assert(count_occurrences(transcript.messages()[1].content, "alpha") == 1);

  const auto transcript_output =
      render(zed::ui::render_terminal_messages(transcript.messages()));
  assert(count_occurrences(transcript_output, "┌") == 1);
  assert(count_occurrences(transcript_output, "alpha") == 1);

  std::stringstream fallback_output;
  zed::ui::TerminalRenderer fallback_renderer(fallback_output, {false});
  fallback_renderer.render({
      zed::core::AgentEventType::assistant_delta,
      "streamed ",
      {},
      {},
  });
  assert(fallback_output.str() == "streamed ");
  fallback_renderer.render({
      zed::core::AgentEventType::assistant_delta,
      "once",
      {},
      {},
  });
  assert(fallback_output.str() == "streamed once");
  fallback_renderer.render({
      zed::core::AgentEventType::assistant_message,
      "streamed once",
      {},
      {},
  });
  fallback_renderer.render({
      zed::core::AgentEventType::agent_end,
      "streamed once",
      {},
      {},
  });
  assert(count_occurrences(fallback_output.str(), "streamed once") == 1);
  fallback_renderer.render({
      zed::core::AgentEventType::tool_start,
      "Inspect dependencies",
      zed::core::ToolCall{
          "bash-fallback", "bash",
          R"({"purpose":"Inspect dependencies","command":"hidden-command --token"})"},
      {},
  });
  assert(fallback_output.str().find("Inspect dependencies") !=
         std::string::npos);
  assert(fallback_output.str().find("hidden-command") == std::string::npos);

  std::stringstream non_streaming_output;
  zed::ui::TerminalRenderer non_streaming_renderer(non_streaming_output,
                                                   {false});
  non_streaming_renderer.render({
      zed::core::AgentEventType::assistant_message,
      "**complete** response",
      {},
      {},
  });
  assert(non_streaming_output.str().find("complete") != std::string::npos);
  assert(non_streaming_output.str().find("response") != std::string::npos);
  return 0;
}
