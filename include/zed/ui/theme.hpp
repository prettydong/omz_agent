#pragma once

#include <optional>
#include <string_view>

#include <ftxui/screen/color.hpp>

namespace zed::ui {

enum class ThemeKind {
  light,
  monaka,
};

struct TerminalTheme {
  ftxui::Color primary;
  ftxui::Color secondary;
  ftxui::Color accent;
  ftxui::Color error;
  ftxui::Color warning;
  ftxui::Color success;
  ftxui::Color info;
  ftxui::Color text;
  ftxui::Color text_muted;
  ftxui::Color background;
  ftxui::Color background_panel;
  ftxui::Color background_element;
  ftxui::Color input_background;
  ftxui::Color border;
  ftxui::Color border_active;
  ftxui::Color markdown_text;
  ftxui::Color markdown_heading;
  ftxui::Color markdown_link;
  ftxui::Color markdown_code;
  ftxui::Color markdown_quote;
  ftxui::Color markdown_emphasis;
  ftxui::Color markdown_strong;
  ftxui::Color markdown_rule;
  ftxui::Color markdown_list;
  ftxui::Color markdown_code_block;
};

[[nodiscard]] constexpr std::string_view theme_name(ThemeKind kind) {
  switch (kind) {
  case ThemeKind::light:
    return "light";
  case ThemeKind::monaka:
    return "monaka";
  }
  return "light";
}

[[nodiscard]] constexpr std::optional<ThemeKind>
theme_kind_from_name(std::string_view name) {
  if (name == "light")
    return ThemeKind::light;
  if (name == "monaka")
    return ThemeKind::monaka;
  return std::nullopt;
}

[[nodiscard]] const TerminalTheme &terminal_theme(ThemeKind kind);

} // namespace zed::ui
