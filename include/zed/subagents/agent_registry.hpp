#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zed/core/model.hpp"
#include "zed/providers/opencode_go_catalog.hpp"

namespace zed::subagents {

struct AgentDefinition {
  std::string name;
  std::string description;
  core::ModelRef model;
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::string system_prompt;
  std::vector<std::string> tools;
  bool available{false};
  std::string unavailable_reason;
};

[[nodiscard]] std::vector<AgentDefinition>
built_in_agents(const std::vector<providers::OpenCodeGoModelInfo> &models);

[[nodiscard]] const AgentDefinition *
find_agent(const std::vector<AgentDefinition> &agents, std::string_view name);

[[nodiscard]] std::string
format_agents(const std::vector<AgentDefinition> &agents);

} // namespace zed::subagents
