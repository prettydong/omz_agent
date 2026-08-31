#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "zed/core/tool.hpp"

namespace zed::core {

[[nodiscard]] Result<std::string> tool_call_purpose(const ToolCall &call);

class ToolRegistry {
public:
  ToolRegistry() = default;
  explicit ToolRegistry(std::vector<std::string> allowed_tools);

  [[nodiscard]] Result<void>
  validate_tool(const ToolDefinition &definition) const;
  Result<void> register_tool(std::unique_ptr<Tool> tool);
  bool unregister_tool(std::string_view name);

  [[nodiscard]] std::vector<ToolDefinition> definitions() const;
  [[nodiscard]] std::vector<ToolDefinition> registered_definitions() const;

  Result<ToolResult> execute(const ToolCall &call,
                             CancellationToken cancellation,
                             const ToolProgressCallback &on_progress = {});

private:
  [[nodiscard]] bool allowed(std::string_view name) const;

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Tool>> tools_;
  std::vector<ToolDefinition> prepared_definitions_;
  std::optional<std::unordered_set<std::string>> allowed_tools_;
};

} // namespace zed::core
