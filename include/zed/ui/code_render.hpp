#pragma once

#include <string_view>

#include <ftxui/dom/elements.hpp>

#include "zed/ui/theme.hpp"

namespace zed::ui {

// Render fenced source while retaining whitespace and using a small, bounded
// lexer for common programming languages.
ftxui::Element render_code_block(std::string_view code,
                                 std::string_view language,
                                 const TerminalTheme &theme);

// Render a unified diff with file headers and old/new line numbers.
ftxui::Element render_diff(std::string_view diff, const TerminalTheme &theme);

// Returns true only for a complete unified/git diff, never for a lone +++ line.
[[nodiscard]] bool looks_like_diff(std::string_view text);

// Preserve ordinary tool output; render an embedded complete unified diff when
// one is present.
ftxui::Element render_tool_output(std::string_view text,
                                  const TerminalTheme &theme);

} // namespace zed::ui
