#include "zed/ui/theme.hpp"

#include <cstdint>

namespace zed::ui {

namespace {

ftxui::Color rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
  return ftxui::Color::RGB(red, green, blue);
}

TerminalTheme make_light_theme() {
  return {
      rgb(59, 125, 216),  // primary
      rgb(123, 91, 182),  // secondary
      rgb(214, 140, 39),  // accent
      rgb(209, 56, 61),   // error
      rgb(214, 140, 39),  // warning
      rgb(61, 154, 87),   // success
      rgb(49, 135, 149),  // info
      rgb(26, 26, 26),    // text
      rgb(138, 138, 138), // text muted
      rgb(255, 255, 255), // background
      rgb(250, 250, 250), // background panel
      rgb(245, 245, 245), // background element
      rgb(248, 244, 255), // input background
      rgb(184, 184, 184), // border
      rgb(160, 160, 160), // border active
      rgb(26, 26, 26),    // markdown text
      rgb(59, 125, 216),  // markdown heading
      rgb(49, 135, 149),  // markdown link
      rgb(61, 154, 87),   // markdown code
      rgb(176, 133, 31),  // markdown quote
      rgb(176, 133, 31),  // markdown emphasis
      rgb(123, 91, 182),  // markdown strong
      rgb(138, 138, 138), // markdown rule
      rgb(214, 140, 39),  // markdown list
      rgb(26, 26, 26),    // markdown code block
  };
}

TerminalTheme make_monaka_theme() {
  return {
      rgb(102, 217, 239), // primary
      rgb(174, 129, 255), // secondary
      rgb(166, 226, 46),  // accent
      rgb(249, 38, 114),  // error
      rgb(230, 219, 116), // warning
      rgb(166, 226, 46),  // success
      rgb(253, 151, 31),  // info
      rgb(248, 248, 242), // text
      rgb(117, 113, 94),  // text muted
      rgb(39, 40, 34),    // background
      rgb(30, 31, 28),    // background panel
      rgb(62, 61, 50),    // background element
      rgb(48, 43, 55),    // input background
      rgb(62, 61, 50),    // border
      rgb(102, 217, 239), // border active
      rgb(248, 248, 242), // markdown text
      rgb(249, 38, 114),  // markdown heading
      rgb(174, 129, 255), // markdown link
      rgb(166, 226, 46),  // markdown code
      rgb(117, 113, 94),  // markdown quote
      rgb(230, 219, 116), // markdown emphasis
      rgb(253, 151, 31),  // markdown strong
      rgb(117, 113, 94),  // markdown rule
      rgb(102, 217, 239), // markdown list
      rgb(248, 248, 242), // markdown code block
  };
}

} // namespace

const TerminalTheme &terminal_theme(ThemeKind kind) {
  static const TerminalTheme light = make_light_theme();
  static const TerminalTheme monaka = make_monaka_theme();
  return kind == ThemeKind::monaka ? monaka : light;
}

} // namespace zed::ui
