#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "zed/core/tool.hpp"

namespace zed::lsp {
class ClangdClient;
}

namespace zed::tools {

struct ToolLimits {
  std::size_t max_read_bytes{256 * 1024};
  std::size_t max_write_bytes{256 * 1024};
  std::size_t max_command_output_bytes{256 * 1024};
  std::size_t command_timeout_ms{30'000};
};

class WorkspaceToolBase {
public:
  WorkspaceToolBase(std::filesystem::path workspace_root,
                    ToolLimits limits = {});

protected:
  core::Result<std::filesystem::path> resolve_path(std::string_view path) const;

  [[nodiscard]] const std::filesystem::path &workspace_root() const {
    return workspace_root_;
  }
  [[nodiscard]] const ToolLimits &limits() const { return limits_; }

private:
  std::filesystem::path workspace_root_;
  ToolLimits limits_;
};

class ReadFileTool final : public core::Tool, public WorkspaceToolBase {
public:
  using WorkspaceToolBase::WorkspaceToolBase;

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;
};

class WriteFileTool final : public core::Tool, public WorkspaceToolBase {
public:
  WriteFileTool(std::filesystem::path workspace_root, ToolLimits limits = {},
                lsp::ClangdClient *clangd = nullptr);

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

private:
  lsp::ClangdClient *clangd_;
};

class BashTool final : public core::Tool, public WorkspaceToolBase {
public:
  using WorkspaceToolBase::WorkspaceToolBase;

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;
};

class GrepTool final : public core::Tool, public WorkspaceToolBase {
public:
  using WorkspaceToolBase::WorkspaceToolBase;

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;
};

class FindFilesTool final : public core::Tool, public WorkspaceToolBase {
public:
  using WorkspaceToolBase::WorkspaceToolBase;

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;
};

class ListDirectoryTool final : public core::Tool, public WorkspaceToolBase {
public:
  using WorkspaceToolBase::WorkspaceToolBase;

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;
};

class EditFileTool final : public core::Tool, public WorkspaceToolBase {
public:
  EditFileTool(std::filesystem::path workspace_root, ToolLimits limits = {},
               lsp::ClangdClient *clangd = nullptr);

  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

private:
  lsp::ClangdClient *clangd_;
};

} // namespace zed::tools
