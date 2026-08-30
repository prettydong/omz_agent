#pragma once

#include "zed/lsp/clangd_client.hpp"
#include "zed/tools/basic_tools.hpp"

namespace zed::tools {

class ClangdTool final : public core::Tool, public WorkspaceToolBase {
public:
  ClangdTool(std::filesystem::path workspace_root, lsp::ClangdClient &client,
             ToolLimits limits = {});

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

private:
  lsp::ClangdClient &client_;
};

} // namespace zed::tools
