#include <cassert>
#include <chrono>
#include <sstream>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/selection.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include "zed/ui/markdown.hpp"
#include "zed/ui/terminal.hpp"

namespace {

zed::ui::TerminalStartupTiming startup_timing() {
  using namespace std::chrono_literals;
  return {2ms, 1ms, 3ms, 5ms, 10ms, 2ms};
}

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

int rendered_rows(ftxui::Element element) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(element),
                                      ftxui::Dimension::Fit(element));
  ftxui::Render(screen, element);
  return screen.dimy();
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

bool rendered_glyph_has_foreground(ftxui::Element element,
                                   std::string_view glyph,
                                   const ftxui::Color &color, bool bold) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(element),
                                      ftxui::Dimension::Fit(element));
  ftxui::Render(screen, element);
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &cell = screen.CellAt(x, y);
      if (cell.character == glyph && cell.foreground_color == color &&
          cell.bold == bold) {
        return true;
      }
    }
  }
  return false;
}

bool rendered_glyph_has_dim(ftxui::Element element, std::string_view glyph,
                            bool dim) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fit(element),
                                      ftxui::Dimension::Fit(element));
  ftxui::Render(screen, element);
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &cell = screen.CellAt(x, y);
      if (cell.character == glyph && cell.dim == dim)
        return true;
    }
  }
  return false;
}

bool rendered_row_has_background(ftxui::Element element, int width, int row,
                                 const ftxui::Color &color) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(3));
  ftxui::Render(screen, element);
  if (row < 0 || row >= screen.dimy())
    return false;
  for (int column = 0; column < screen.dimx(); ++column) {
    if (screen.CellAt(column, row).background_color != color)
      return false;
  }
  return true;
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
  assert(zed::ui::is_terminal_command_completion_event(ftxui::Event::Return));
  assert(zed::ui::is_terminal_command_completion_event(ftxui::Event::Tab));
  assert(!zed::ui::is_terminal_command_completion_event(
      ftxui::Event::Character('x')));

  const auto command_guide = render(zed::ui::render_terminal_command_guide(
      "/theme", command_hints, 0,
      zed::ui::terminal_theme(zed::ui::ThemeKind::light)));
  assert(command_guide.find("/theme") != std::string::npos);
  assert(command_guide.find("Change theme.") != std::string::npos);
  assert(command_guide.find("options ·") != std::string::npos);
  assert(command_guide.find("light") != std::string::npos);
  assert(command_guide.find("Light theme.") != std::string::npos);
  assert(command_guide.find("monaka") != std::string::npos);
  assert(command_guide.find("Dark theme.") != std::string::npos);
  assert(command_guide.find("secondary options") == std::string::npos);
  assert(count_occurrences(command_guide, "light") == 1);
  assert(count_occurrences(command_guide, "monaka") == 1);

  assert(zed::ui::terminal_activity_label(zed::ui::TerminalActivity::thinking,
                                          zed::core::ReasoningEffort::low) ==
         "low think");
  assert(zed::ui::terminal_activity_label(zed::ui::TerminalActivity::thinking,
                                          zed::core::ReasoningEffort::high) ==
         "high think");
  assert(zed::ui::terminal_activity_label(zed::ui::TerminalActivity::action,
                                          zed::core::ReasoningEffort::low) ==
         "tool");

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

  zed::ui::TerminalScrollState repeated_scroll_state;
  repeated_scroll_state.scroll_up(100, 20);
  repeated_scroll_state.scroll_up(100, 20);
  repeated_scroll_state.scroll_up(100, 20);
  assert(repeated_scroll_state.offset_rows(100, 20) == 71);
  repeated_scroll_state.scroll_down(100, 20);
  assert(repeated_scroll_state.offset_rows(100, 20) == 74);

  ftxui::Elements scroll_fixture_rows;
  for (int row = 0; row < 30; ++row) {
    const auto label =
        row < 10 ? "row 0" + std::to_string(row) : "row " + std::to_string(row);
    scroll_fixture_rows.push_back(ftxui::text(label));
  }
  const auto scroll_fixture = ftxui::vbox(std::move(scroll_fixture_rows));
  ftxui::Box captured_content_box;
  ftxui::Box captured_viewport_box;
  const auto latest_view = render_in_box(
      scroll_fixture | zed::ui::capture_layout_box(captured_content_box) |
          ftxui::focusPositionRelative(0.0F, 1.0F) | ftxui::vscroll_indicator |
          ftxui::yframe | ftxui::reflect(captured_viewport_box),
      10, 10);
  assert(latest_view.starts_with("row 20"));
  assert(captured_content_box.y_max - captured_content_box.y_min == 30);
  assert(captured_viewport_box.y_max - captured_viewport_box.y_min + 1 == 10);
  const auto captured_content_rows = static_cast<std::size_t>(
      captured_content_box.y_max - captured_content_box.y_min);
  const auto captured_viewport_rows = static_cast<std::size_t>(
      captured_viewport_box.y_max - captured_viewport_box.y_min + 1);
  zed::ui::TerminalScrollState viewport_scroll_state;
  viewport_scroll_state.scroll_up(captured_content_rows,
                                  captured_viewport_rows);
  const auto three_rows_up_view = render_in_box(
      scroll_fixture | zed::ui::capture_layout_box(captured_content_box) |
          ftxui::focusPosition(
              0, viewport_scroll_state.focus_row(captured_content_rows,
                                                 captured_viewport_rows)) |
          ftxui::vscroll_indicator | ftxui::yframe |
          ftxui::reflect(captured_viewport_box),
      10, 10);
  assert(three_rows_up_view.starts_with("row 17"));

  const auto welcome = render(zed::ui::render_terminal_welcome(
      "/tmp/workspace", "fixture-model", "0.1.0", startup_timing(),
      zed::core::ReasoningEffort::low, zed::ui::ThemeKind::light));
  const auto startup_summary =
      zed::ui::terminal_startup_summary(startup_timing());
  assert(startup_summary ==
         "startup: 23.000 ms = config 2.000 + session 3.000 + setup 5.000 + "
         "plugins 10.000 + ui 2.000 + other 1.000");
  assert(startup_summary.find('\n') == std::string::npos);
  assert(startup_summary.find("core") == std::string::npos);
  assert(startup_summary.find("other 1.000") != std::string::npos);
  assert(welcome.find("fixture-model") != std::string::npos);
  assert(welcome.find("low") != std::string::npos);
  assert(welcome.find("/tmp/workspace") != std::string::npos);
  assert(welcome.find("zeda 0.1.0") != std::string::npos);
  assert(welcome.find("startup:") != std::string::npos);
  assert(welcome.find("23.000 ms") != std::string::npos);
  assert(welcome.find("session") != std::string::npos);
  assert(welcome.find("plugins") != std::string::npos);
  assert(welcome.find("10.000") != std::string::npos);
  assert(welcome.find("other") != std::string::npos);
  assert(welcome.find("quick bash") == std::string::npos);
  assert(welcome.find("theme") == std::string::npos);
  assert(rendered_rows(zed::ui::render_terminal_welcome(
             "/tmp/workspace", "fixture-model", "0.1.0", startup_timing(),
             zed::core::ReasoningEffort::low, zed::ui::ThemeKind::light)) == 6);

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

  const auto separated_paragraphs =
      render(zed::ui::render_markdown("first paragraph\n\nsecond paragraph"));
  assert(separated_paragraphs.find("first paragraph") != std::string::npos);
  assert(separated_paragraphs.find("second paragraph") != std::string::npos);
  assert(rendered_rows(zed::ui::render_markdown(
             "first paragraph\n\nsecond paragraph")) == 3);
  const auto collapsed_paragraph_spacing =
      render(zed::ui::render_markdown("first paragraph\n\n\nsecond paragraph"));
  assert(collapsed_paragraph_spacing == separated_paragraphs);

  const auto unseparated_source_lines =
      render(zed::ui::render_markdown("first source line\nsecond source line"));
  assert(unseparated_source_lines.find(
             "first source line second source line") != std::string::npos);
  assert(rendered_rows(zed::ui::render_markdown(
             "first source line\nsecond source line")) == 1);

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

  const std::vector<zed::ui::TerminalMessage> user_messages{{
      zed::ui::TerminalMessageKind::user,
      {},
      "user request",
  }};
  const auto light_user_message =
      zed::ui::render_terminal_messages(user_messages, light_theme);
  assert(rendered_rows(light_user_message) == 3);
  assert(rendered_row_has_background(light_user_message, 40, 0,
                                     light_theme.input_background));
  assert(rendered_row_has_background(light_user_message, 40, 1,
                                     light_theme.input_background));
  assert(rendered_row_has_background(light_user_message, 40, 2,
                                     light_theme.input_background));
  const auto &monaka_theme =
      zed::ui::terminal_theme(zed::ui::ThemeKind::monaka);
  const auto monaka_user_message =
      zed::ui::render_terminal_messages(user_messages, monaka_theme);
  assert(rendered_rows(monaka_user_message) == 3);
  assert(rendered_row_has_background(monaka_user_message, 40, 0,
                                     monaka_theme.input_background));
  assert(rendered_row_has_background(monaka_user_message, 40, 1,
                                     monaka_theme.input_background));
  assert(rendered_row_has_background(monaka_user_message, 40, 2,
                                     monaka_theme.input_background));

  auto input_cursor_screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(8),
                                                   ftxui::Dimension::Fixed(1));
  auto input_with_bar_cursor = ftxui::hbox({
      ftxui::text("abc"),
      ftxui::text("x") | ftxui::focusCursorBarBlinking,
  });
  auto input_with_block_cursor = zed::ui::render_terminal_input(
      {std::move(input_with_bar_cursor), false, true, false}, light_theme);
  ftxui::Render(input_cursor_screen, input_with_block_cursor);
  assert(input_cursor_screen.cursor().x == 3);
  assert(input_cursor_screen.cursor().shape ==
         ftxui::Screen::Cursor::BlockBlinking);

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
          "/tmp/workspace", "fixture-model", "0.1.0", startup_timing(),
          zed::core::ReasoningEffort::low, zed::ui::ThemeKind::light),
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
  const auto activity_message_count = activity_transcript.messages().size();
  activity_transcript.append_event({
      zed::core::AgentEventType::tool_update,
      "[explorer 1/1] running — read: inspect fixture",
      {},
      {},
  });
  assert(activity_transcript.messages().size() == activity_message_count);
  assert(activity_transcript.messages().back().content.find("explorer") !=
         std::string::npos);
  activity_transcript.append_event({
      zed::core::AgentEventType::tool_result,
      "done",
      {},
      zed::core::ToolResult{"subagent-fixture", "done", false,
                            zed::core::ModelUsage{7, 2, 3}},
      zed::core::ModelUsage{7, 2, 3},
  });
  assert(activity_transcript.activity() == zed::ui::TerminalActivity::thinking);
  assert(activity_transcript.token_metrics().input_tokens == 7);
  assert(activity_transcript.token_metrics().output_tokens == 3);
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
  const auto expanded_command_output = render(zed::ui::render_terminal_messages(
      command_transcript.messages(), light_theme));
  assert(expanded_command_output.find("/theme monaka") != std::string::npos);
  assert(expanded_command_output.find("theme: monaka") != std::string::npos);
  assert(command_transcript.toggle_message_expansion(0));
  const auto collapsed_command_output =
      render(zed::ui::render_terminal_messages(command_transcript.messages(),
                                               light_theme));
  assert(collapsed_command_output.find("/theme monaka") != std::string::npos);
  assert(collapsed_command_output.find("done") != std::string::npos);
  assert(collapsed_command_output.find("theme: monaka") == std::string::npos);
  assert(!command_transcript.toggle_message_expansion(4));

  zed::ui::TerminalTranscript progress_command_transcript;
  progress_command_transcript.begin_request("/deepwiki update",
                                            zed::ui::TerminalActivity::action);
  progress_command_transcript.append_event({
      zed::core::AgentEventType::tool_result,
      "正在索引 10/10",
      {},
      {},
  });
  progress_command_transcript.complete_request(
      zed::core::Result<std::string>::success(
          "DeepWiki 已是最新状态；未调用模型。"));
  assert(progress_command_transcript.messages().size() == 1);
  assert(progress_command_transcript.messages()[0].content.find("未调用模型") !=
         std::string::npos);

  zed::ui::TerminalTranscript failed_command_transcript;
  failed_command_transcript.append_command(
      "/theme ultraviolet",
      zed::core::Result<std::string>::failure(
          {zed::core::ErrorCode::invalid_argument, "unsupported theme"}));
  const auto failed_command_output = render(zed::ui::render_terminal_messages(
      failed_command_transcript.messages(), light_theme));
  assert(failed_command_output.find("/theme ultraviolet") != std::string::npos);
  assert(failed_command_output.find("failed") != std::string::npos);
  assert(failed_command_output.find("unsupported theme") != std::string::npos);

  zed::ui::TerminalTranscript restored_transcript;
  const std::vector<zed::core::Message> saved_history{
      {"system-1", zed::core::Role::system, "private system prompt", {}, {}},
      {"user-1", zed::core::Role::user, "resume this task", {}, {}},
      {"assistant-1",
       zed::core::Role::assistant,
       "I will inspect it.",
       {{"call-1", "read", R"({"path":"README.md"})"}},
       {}},
      {"tool-1",
       zed::core::Role::tool,
       "restored tool output",
       {},
       "call-1",
       true},
      {"assistant-2",
       zed::core::Role::assistant,
       "Inspection complete.",
       {},
       {}},
  };
  restored_transcript.restore(saved_history);
  assert(restored_transcript.messages().size() == 4);
  assert(restored_transcript.messages()[0].kind ==
         zed::ui::TerminalMessageKind::user);
  assert(!restored_transcript.messages()[0].collapsible);
  assert(restored_transcript.messages()[0].expanded);
  assert(restored_transcript.messages()[2].kind ==
         zed::ui::TerminalMessageKind::tool);
  assert(restored_transcript.messages()[2].label == "read");
  assert(!restored_transcript.messages()[2].expanded);
  assert(restored_transcript.messages()[2].is_error);
  const auto restored_output = render(zed::ui::render_terminal_messages(
      restored_transcript.messages(), light_theme));
  assert(restored_output.find("resume this task") != std::string::npos);
  assert(restored_output.find("Inspection complete.") != std::string::npos);
  assert(restored_output.find("private system prompt") == std::string::npos);
  assert(restored_output.find("restored tool output") == std::string::npos);

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
      zed::core::ModelUsage{100, 25, 10, 20.0},
  });
  usage_transcript.append_event({
      zed::core::AgentEventType::assistant_message,
      "second",
      {},
      {},
      zed::core::ModelUsage{
          60,
          30,
          5,
          42.34,
          zed::core::ContextTokenBreakdown{12, 8, 10, 6, 18, 6},
      },
  });
  const auto token_metrics = usage_transcript.token_metrics();
  assert(token_metrics.context_tokens == 60);
  assert(token_metrics.input_tokens == 160);
  assert(token_metrics.output_tokens == 15);
  assert(token_metrics.output_tokens_per_second == 42.34);
  assert(token_metrics.cached_context_tokens == 30);
  assert(token_metrics.context_breakdown.has_value());
  assert(token_metrics.context_breakdown->total_tokens() == 60);
  assert(token_metrics.all_tokens() == 175);
  assert(zed::ui::terminal_context_summary(token_metrics, 1'000) ==
         "ctx:60 (6%)");
  assert(zed::ui::terminal_token_summary(token_metrics, 1'000) ==
         "ctx:60 (6%) ↑160 ↓15 Σ175 42.3 tok/s");
  assert(zed::ui::terminal_token_summary({1'000, 1'234, 18'000}) ==
         "ctx:1k ↑1.2k ↓18k Σ19.2k");
  assert(zed::ui::terminal_token_summary({2'450'000, 3'000'000, 1'500'000}) ==
         "ctx:2.4m ↑3m ↓1.5m Σ4.5m");
  const auto context_analysis =
      render(zed::ui::render_terminal_context_analysis(token_metrics, 1'000,
                                                       light_theme));
  assert(context_analysis.find("Context analysis") != std::string::npos);
  assert(context_analysis.find("System instructions") != std::string::npos);
  assert(context_analysis.find("Tool definitions") != std::string::npos);
  assert(context_analysis.find("Other / protocol") != std::string::npos);
  assert(context_analysis.find("Cached") != std::string::npos);
  assert(context_analysis.find("categories are estimated") !=
         std::string::npos);
  assert(
      rendered_glyph_has_foreground(zed::ui::render_terminal_context_analysis(
                                        token_metrics, 1'000, light_theme),
                                    "C", light_theme.secondary, true));
  ftxui::Elements underlay_rows;
  for (int row = 0; row < 17; ++row)
    underlay_rows.push_back(ftxui::text(std::string(60, 'X')));
  auto opaque_analysis = ftxui::dbox({
      ftxui::vbox(std::move(underlay_rows)),
      zed::ui::render_terminal_context_analysis(token_metrics, 1'000,
                                                light_theme),
  });
  auto opaque_screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60),
                                             ftxui::Dimension::Fixed(17));
  ftxui::Render(opaque_screen, opaque_analysis);
  assert(opaque_screen.CellAt(30, 1).character != "X");
  ftxui::Selection analysis_selection(1, 7, 30, 12);
  ftxui::Render(opaque_screen, opaque_analysis.get(), analysis_selection);
  for (int row = 7; row <= 12; ++row) {
    for (int column = 1; column <= 19; ++column) {
      assert(!opaque_screen.CellAt(column, row).inverted);
      assert(opaque_screen.CellAt(column, row).background_color ==
             light_theme.input_background);
    }
  }
  ftxui::Elements background_rows;
  for (int row = 0; row < 20; ++row)
    background_rows.push_back(ftxui::text(std::string(80, 'B')));
  auto context_overlay = zed::ui::render_terminal_context_overlay(
      ftxui::vbox(std::move(background_rows)), token_metrics, 1'000,
      light_theme);
  assert(rendered_glyph_has_dim(context_overlay, "B", true));
  assert(rendered_glyph_has_dim(
      zed::ui::render_terminal_context_overlay(
          ftxui::vbox({ftxui::text(std::string(80, 'B')) |
                       ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, 20)}),
          token_metrics, 1'000, light_theme),
      "C", false));
  std::string wide_background_line;
  for (int column = 0; column < 41; ++column)
    wide_background_line += "汉";
  ftxui::Elements wide_background_rows;
  for (int row = 0; row < 17; ++row)
    wide_background_rows.push_back(ftxui::text(wide_background_line));
  auto wide_context_overlay = zed::ui::render_terminal_context_overlay(
      ftxui::vbox(std::move(wide_background_rows)), token_metrics, 1'000,
      light_theme);
  auto wide_overlay_screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(82),
                                                   ftxui::Dimension::Fixed(17));
  ftxui::Render(wide_overlay_screen, wide_context_overlay);
  assert(wide_overlay_screen.CellAt(10, 1).character == " ");
  assert(wide_overlay_screen.CellAt(11, 1).character == "│");

  assert(zed::ui::terminal_clipboard_sequence("").empty());
  assert(zed::ui::terminal_clipboard_sequence("A") == "\x1b]52;c;QQ==\x07");
  assert(zed::ui::terminal_clipboard_sequence("hello") ==
         "\x1b]52;c;aGVsbG8=\x07");
  assert(zed::ui::terminal_clipboard_sequence("line one\nline two") ==
         "\x1b]52;c;bGluZSBvbmUKbGluZSB0d28=\x07");

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
  const auto expanded_quick_bash = render(zed::ui::render_terminal_messages(
      quick_bash_transcript.messages(), light_theme));
  assert(expanded_quick_bash.find("pwd") != std::string::npos);
  assert(expanded_quick_bash.find("/tmp/workspace") != std::string::npos);
  assert(quick_bash_transcript.toggle_message_expansion(0));
  const auto collapsed_quick_bash = render(zed::ui::render_terminal_messages(
      quick_bash_transcript.messages(), light_theme));
  assert(collapsed_quick_bash.find("pwd") != std::string::npos);
  assert(collapsed_quick_bash.find("/tmp/workspace") == std::string::npos);

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
