#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

#include "zed/core/tool.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/subagents/subagent_runner.hpp"
#include "zed/subagents/worker_protocol.hpp"

namespace zed::tools {

struct SubagentToolConfig {
  std::size_t max_tasks{8};
  std::size_t max_concurrency{4};
  std::size_t max_task_bytes{subagents::kMaximumTaskBytes};
  std::size_t max_aggregate_output_bytes{256 * 1024};
  std::chrono::milliseconds total_timeout{600'000};
};

class SubagentTool final : public core::Tool {
public:
  SubagentTool(subagents::SubagentRunner &runner,
               std::vector<subagents::AgentDefinition> agents,
               SubagentToolConfig config = {});

  [[nodiscard]] const core::ToolDefinition &definition() const override;

  void set_agents(std::vector<subagents::AgentDefinition> agents);

  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override;

  core::Result<core::ToolResult>
  execute_with_progress(const core::ToolCall &call,
                        core::CancellationToken cancellation,
                        const core::ToolProgressCallback &on_progress) override;

private:
  subagents::SubagentRunner &runner_;
  std::vector<subagents::AgentDefinition> agents_;
  SubagentToolConfig config_;
  core::ToolDefinition definition_;
};

} // namespace zed::tools
