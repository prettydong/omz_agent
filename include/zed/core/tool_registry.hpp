#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zed/core/tool.hpp"

namespace zed::core {

[[nodiscard]] Result<std::string> tool_call_purpose(const ToolCall &call);

class ToolRegistry {
public:
  Result<void> register_tool(std::unique_ptr<Tool> tool);

  [[nodiscard]] std::vector<ToolDefinition> definitions() const;

  Result<ToolResult> execute(const ToolCall &call,
                             CancellationToken cancellation,
                             const ToolProgressCallback &on_progress = {});

private:
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Tool>> tools_;
};

} // namespace zed::core
