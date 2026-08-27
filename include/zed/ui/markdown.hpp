#pragma once

#include <string_view>

#include <ftxui/dom/elements.hpp>

#include "zed/ui/theme.hpp"

namespace zed::ui {

// 将模型返回的 Markdown 转成可供终端渲染的 FTXUI DOM。
// 表格会被识别为 Markdown table，并使用 FTXUI Table 布局。
ftxui::Element render_markdown(std::string_view markdown,
                               const TerminalTheme &theme);
ftxui::Element render_markdown(std::string_view markdown);

} // namespace zed::ui
