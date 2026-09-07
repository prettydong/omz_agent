#pragma once

#include <chrono>

#include "zed/context/context_archive.hpp"
#include "zed/core/tool.hpp"
#include "zed/core/tool_registry.hpp"

namespace zed::tools {

class ContextNotesTool final : public core::Tool {
public:
  explicit ContextNotesTool(
      context::ContextArchive &archive,
      std::chrono::milliseconds execution_budget = std::chrono::seconds(5))
      : archive_(archive), execution_budget_(execution_budget) {}
  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

private:
  context::ContextArchive &archive_;
  std::chrono::milliseconds execution_budget_;
};

class ContextHistoryTool final : public core::Tool {
public:
  explicit ContextHistoryTool(
      context::ContextArchive &archive,
      std::chrono::milliseconds execution_budget = std::chrono::seconds(5))
      : archive_(archive), execution_budget_(execution_budget) {}
  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

private:
  context::ContextArchive &archive_;
  std::chrono::milliseconds execution_budget_;
};

class NewContextTool final : public core::Tool {
public:
  explicit NewContextTool(
      context::ContextArchive &archive,
      std::chrono::milliseconds execution_budget = std::chrono::seconds(5))
      : archive_(archive), execution_budget_(execution_budget) {}
  [[nodiscard]] const core::ToolDefinition &definition() const override;
  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

private:
  context::ContextArchive &archive_;
  std::chrono::milliseconds execution_budget_;
};

core::Result<void> register_context_tools(core::ToolRegistry &registry,
                                          context::ContextArchive &archive);

} // namespace zed::tools
