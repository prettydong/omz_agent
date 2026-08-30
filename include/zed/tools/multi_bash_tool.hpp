#pragma once

#include "zed/tools/basic_tools.hpp"

namespace zed::tools {

class MultiBashTool final : public core::Tool, public WorkspaceToolBase {
public:
  using WorkspaceToolBase::WorkspaceToolBase;

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;
};

} // namespace zed::tools
