#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zed/core/model.hpp"
#include "zed/providers/opencode_go_catalog.hpp"

namespace zed::subagents {

struct ExplorerAgentConfig {
  std::string name{"explorer"};
  std::string description{
      "Read-only workspace exploration and evidence gathering."};
  bool enabled{true};
  core::ModelRef model{"opencode-go", "muse-spark-1.2-contributor"};
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::size_t max_turns{12};
  std::size_t max_output_tokens{8'192};
  std::string system_prompt;
};

struct AgentDefinition {
  std::string name;
  std::string description;
  core::ModelRef model;
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::string system_prompt;
  std::vector<std::string> tools;
  std::size_t max_turns{12};
  std::size_t max_output_tokens{8'192};
  bool available{false};
  std::string unavailable_reason;
};

[[nodiscard]] std::vector<AgentDefinition>
built_in_agents(const std::vector<providers::OpenCodeGoModelInfo> &models,
                const ExplorerAgentConfig &explorer_config = {});

[[nodiscard]] std::vector<AgentDefinition>
configured_agents(const std::vector<providers::OpenCodeGoModelInfo> &models,
                  const std::vector<ExplorerAgentConfig> &agent_configs);

[[nodiscard]] std::string_view default_explorer_system_prompt();

[[nodiscard]] const AgentDefinition *
find_agent(const std::vector<AgentDefinition> &agents, std::string_view name);

[[nodiscard]] std::string
format_agents(const std::vector<AgentDefinition> &agents);

} // namespace zed::subagents
