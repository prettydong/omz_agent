#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/context.hpp"
#include "zed/core/model.hpp"
#include "zed/core/types.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/tools/basic_tools.hpp"

namespace zed::app {

inline constexpr std::size_t kWorkspaceConfigVersion = 1;
inline constexpr std::size_t kMaximumPromptBytes = 1024 * 1024;
inline constexpr std::size_t kAgentManagementVersion = 2;

struct MainAgentConfig {
  core::ModelRef model{"opencode-go", "muse-spark-1.2-contributor"};
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::size_t max_turns{32};
  std::size_t max_output_tokens{};
  double temperature{0.0};
};

struct ContextConfig {
  core::ModelRef model{"opencode-go", "muse-spark-1.2-contributor"};
  core::ContextLimits limits{1'000'000, 4'096, 800'000};
  std::size_t max_output_tokens{1'024};
};

struct SubagentExecutionConfig {
  std::size_t max_concurrency{4};
  std::size_t total_timeout_ms{600'000};
  std::size_t max_aggregate_output_bytes{256 * 1024};
};

struct WorkspaceConfig {
  std::size_t version{kWorkspaceConfigVersion};
  MainAgentConfig agent;
  subagents::ExplorerAgentConfig explorer;
  SubagentExecutionConfig subagent_execution;
  ContextConfig context;
};

struct WorkspacePrompts {
  std::string agent;
  std::string explorer;
  std::string context;
};

struct AgentProfile {
  std::string id{"default"};
  std::string name{"Default"};
  std::string description{"Primary Agent"};
  MainAgentConfig config;
  bool automatic_context_compaction{true};
  std::size_t compaction_trigger_tokens{800'000};
  std::vector<std::string> tools{"*"};
  std::string system_prompt;
};

struct AgentManagementConfig {
  std::size_t version{kAgentManagementVersion};
  std::string active_agent{"default"};
  std::vector<AgentProfile> agents;
  std::vector<subagents::ExplorerAgentConfig> subagents;
};

struct RuntimeConfig {
  std::string opencode_go_api_key;
  std::string opencode_endpoint;
  std::string opencode_path{"opencode"};
  std::string clangd_path{"clangd"};
  std::size_t opencode_request_timeout_ms{120'000};
  std::filesystem::path workspace;
  std::filesystem::path session_path;
  std::filesystem::path system_prompt_path;
  std::filesystem::path explorer_system_prompt_path;
  std::filesystem::path context_system_prompt_path;
  std::string system_prompt;
  std::string context_system_prompt;
  core::ModelRef main_model;
  core::ModelRef context_model;
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::string terminal_theme{"light"};
  bool quick_bash_enabled{true};
  core::ContextLimits context_limits;
  tools::ToolLimits tool_limits;
  std::size_t max_turns{32};
  std::size_t main_max_output_tokens{};
  double main_temperature{0.0};
  subagents::ExplorerAgentConfig explorer;
  std::vector<subagents::ExplorerAgentConfig> subagents;
  SubagentExecutionConfig subagent_execution;
  std::size_t context_max_output_tokens{1'024};
  std::vector<std::string> agent_tools{"*"};
  std::filesystem::path workspace_config_path;
};

struct RuntimeConfigLoadOptions {
  bool load_workspace_system_prompt{true};
};

core::Result<RuntimeConfig>
load_runtime_config(RuntimeConfigLoadOptions options = {});

[[nodiscard]] std::filesystem::path
workspace_config_path(const std::filesystem::path &workspace);

[[nodiscard]] WorkspaceConfig default_workspace_config();

[[nodiscard]] core::Result<WorkspaceConfig>
parse_workspace_config(std::string_view json_text);

[[nodiscard]] std::string
serialize_workspace_config(const WorkspaceConfig &config);

[[nodiscard]] core::Result<WorkspaceConfig>
load_workspace_config(const std::filesystem::path &workspace);

[[nodiscard]] core::Result<void>
save_workspace_config(const std::filesystem::path &workspace,
                      const WorkspaceConfig &config);

[[nodiscard]] core::Result<WorkspacePrompts>
load_workspace_prompts(const std::filesystem::path &workspace,
                       bool load_agent_prompt = true);

[[nodiscard]] core::Result<void>
validate_workspace_prompts(const WorkspacePrompts &prompts);

[[nodiscard]] core::Result<void>
save_workspace_prompts(const std::filesystem::path &workspace,
                       const WorkspacePrompts &prompts);

[[nodiscard]] std::filesystem::path
agent_management_path(const std::filesystem::path &workspace);

[[nodiscard]] core::Result<AgentManagementConfig>
load_agent_management(const std::filesystem::path &workspace,
                      const WorkspaceConfig &workspace_config,
                      const WorkspacePrompts &prompts);

[[nodiscard]] core::Result<AgentManagementConfig>
parse_agent_management_config(std::string_view json_text);

[[nodiscard]] std::string
serialize_agent_management_config(const AgentManagementConfig &management);

[[nodiscard]] core::Result<void>
validate_agent_management(const AgentManagementConfig &management);

[[nodiscard]] core::Result<void>
save_agent_management(const std::filesystem::path &workspace,
                      const AgentManagementConfig &management);

} // namespace zed::app
