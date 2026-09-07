#include <cassert>
#include <cctype>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include "zed/ui/code_render.hpp"

namespace {

ftxui::Screen render(ftxui::Element element, int width = 100, int height = 40) {
  auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(width),
                                      ftxui::Dimension::Fixed(height));
  ftxui::Render(screen, element);
  return screen;
}

bool has_text(const ftxui::Screen &screen, std::string_view text) {
  for (int y = 0; y < screen.dimy(); ++y) {
    std::string row;
    for (int x = 0; x < screen.dimx(); ++x)
      row += screen.CellAt(x, y).character;
    if (row.find(text) != std::string::npos)
      return true;
  }
  return false;
}

std::string ascii_letters(const ftxui::Screen &screen) {
  std::string result;
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &glyph = screen.CellAt(x, y).character;
      if (glyph.size() == 1 &&
          std::isalpha(static_cast<unsigned char>(glyph[0])) != 0)
        result += glyph;
    }
  }
  return result;
}

bool has_foreground(const ftxui::Screen &screen, std::string_view glyph,
                    const ftxui::Color &color) {
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      const auto &cell = screen.CellAt(x, y);
      if (cell.character == glyph && cell.foreground_color == color)
        return true;
    }
  }
  return false;
}

bool has_background(const ftxui::Screen &screen, const ftxui::Color &color) {
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      if (screen.CellAt(x, y).background_color == color)
        return true;
    }
  }
  return false;
}

} // namespace

int main() {
  ftxui::Terminal::SetColorSupport(ftxui::Terminal::TrueColor);
  for (const auto kind :
       {zed::ui::ThemeKind::light, zed::ui::ThemeKind::monaka}) {
    const auto &theme = zed::ui::terminal_theme(kind);
    const auto code = zed::ui::render_code_block(
        "int main() {\n  // 注释\n\n  return 42;\n}", "cpp", theme);
    const auto code_screen = render(code);
    assert(has_foreground(code_screen, "r", theme.code_keyword));
    assert(has_text(code_screen, "   3 "));
    assert(has_foreground(code_screen, "4", theme.code_number));
    assert(has_foreground(code_screen, "/", theme.code_comment));
    const auto multiline_comment = render(zed::ui::render_code_block(
        "/* first\nsecond */\nint value;", "cpp", theme));
    assert(has_foreground(multiline_comment, "s", theme.code_comment));

    const auto narrow =
        render(zed::ui::render_code_block("const extraordinarilylongidentifier "
                                          "= \"unicode 内容 wraps here\";",
                                          "ts", theme),
               18, 12);
    assert(ascii_letters(narrow).find(
               "constextraordinarilylongidentifierunicodewrapshere") !=
           std::string::npos);
    const auto tabs = render(
        zed::ui::render_code_block("a\tb", "unlisted-language", theme), 18, 8);
    assert(has_text(tabs, "a   b"));
    const auto plain = render(
        zed::ui::render_code_block("return 42;", "unlisted-language", theme));
    assert(has_foreground(plain, "r", theme.code_text));

    const std::string diff = "diff --git a/demo.cpp b/demo.cpp\n"
                             "--- a/demo.cpp\n"
                             "+++ b/demo.cpp\n"
                             "@@ -2,2 +2,2 @@\n"
                             "-int old_value = 1;\n"
                             "+int new_value = 42;\n"
                             " context();";
    assert(zed::ui::looks_like_diff(diff));
    assert(
        !zed::ui::looks_like_diff("+++ actual program output\nint added = 1;"));
    const auto diff_screen = render(zed::ui::render_diff(diff, theme));
    assert(has_text(diff_screen, "demo.cpp"));
    assert(has_text(diff_screen, "new_value"));
    assert(has_background(diff_screen, theme.diff_added_background));
    assert(has_background(diff_screen, theme.diff_removed_background));

    const auto plus_code = render(zed::ui::render_diff(
        "--- a/demo.py\n+++ b/demo.py\n@@ -1 +1 @@\n+++value = 1\n", theme));
    assert(has_text(plus_code, "+++value = 1"));
    assert(has_background(plus_code, theme.diff_added_background));

    const auto narrow_diff = render(
        zed::ui::render_diff("--- a/a.cpp\n+++ b/a.cpp\n@@ -1 +1 @@\n"
                             "+const extraordinarilylongidentifier = 1;\n",
                             theme),
        20, 20);
    assert(
        ascii_letters(narrow_diff).find("constextraordinarilylongidentifier") !=
        std::string::npos);
    assert(zed::ui::looks_like_diff(
        "--- a/a.cpp\n+++ b/a.cpp\n@@ -0,0 +1 @@\n+++ foo"));

    const auto separate_states = render(zed::ui::render_diff(
        "--- a/a.cpp\n+++ b/a.cpp\n@@ -1 +1 @@\n-/* old\n+int value = 1;",
        theme));
    assert(has_foreground(separate_states, "i", theme.code_type));

    const auto truncated_hunk = render(zed::ui::render_diff(
        "--- a/a.cpp\n+++ b/a.cpp\n@@ -1,2 +1,2 @@\n old\n", theme));
    assert(has_text(truncated_hunk, "old"));

    const auto tool_screen = render(zed::ui::render_tool_output(
        "tool summary\n" + diff + "\nclang: warning: retained", theme));
    assert(has_text(tool_screen, "tool"));
    assert(has_text(tool_screen, "summary"));
    assert(has_text(tool_screen, "clang:"));
    assert(has_text(tool_screen, "warning:"));
  }
}
