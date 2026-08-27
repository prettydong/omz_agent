#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "zed/core/cancellation.hpp"
#include "zed/core/result.hpp"
#include "zed/core/tool.hpp"
#include "zed/core/tool_registry.hpp"

namespace zed::extensions {

[[nodiscard]] constexpr std::string_view quick_bash_purpose() {
  return "Run the Quick Bash input";
}

class QuickBashInput {
public:
  explicit QuickBashInput(core::ToolRegistry &tools, bool enabled = true);

  [[nodiscard]] bool enabled() const;
  void set_enabled(bool enabled);

  [[nodiscard]] core::Result<std::optional<std::string>>
  classify(std::string_view input) const;

  core::Result<core::ToolResult>
  execute(std::string_view command, core::CancellationToken cancellation = {});

private:
  core::ToolRegistry &tools_;
  bool enabled_{true};
};

} // namespace zed::extensions
