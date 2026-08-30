#include "zed/subagents/agent_registry.hpp"

#include <algorithm>
#include <utility>

namespace zed::subagents {

namespace {

constexpr std::string_view kExplorerSystemPrompt =
    "You are zeda's Explorer sub-agent. Investigate the requested question "
    "inside the current workspace and return a concise, evidence-based "
    "summary to the parent agent. You are strictly read-only: never claim to "
    "edit files, run shell commands, load plugins or skills, or delegate to "
    "another agent. Use only read, grep, find, ls, and lsp. Include relevant "
    "workspace-relative paths and line numbers when possible. Do not stop at "
    "a progress update; finish the investigation or state the exact blocker.";

} // namespace

std::vector<AgentDefinition>
built_in_agents(const std::vector<providers::OpenCodeGoModelInfo> &models,
                const ExplorerAgentConfig &explorer_config) {
  return configured_agents(models, {explorer_config});
}

std::vector<AgentDefinition>
configured_agents(const std::vector<providers::OpenCodeGoModelInfo> &models,
                  const std::vector<ExplorerAgentConfig> &agent_configs) {
  std::vector<AgentDefinition> agents;
  agents.reserve(agent_configs.size());
  for (const auto &config : agent_configs) {
    const auto *model =
        providers::find_opencode_go_model(models, config.model.model);
    const bool supports_reasoning =
        model != nullptr &&
        providers::supports_reasoning_effort(*model, config.reasoning_effort);
    AgentDefinition agent{
        config.name,
        config.description,
        config.model,
        config.reasoning_effort,
        config.system_prompt.empty() ? std::string(kExplorerSystemPrompt)
                                     : config.system_prompt,
        {"read", "grep", "find", "ls", "lsp"},
        config.max_turns,
        config.max_output_tokens,
        config.enabled && model != nullptr && supports_reasoning,
        {},
    };
    if (!config.enabled) {
      agent.unavailable_reason = "disabled by workspace configuration";
    } else if (model == nullptr) {
      agent.unavailable_reason =
          "configured model is not present in the OpenCode Go catalog";
    } else if (!supports_reasoning) {
      agent.unavailable_reason =
          "configured model does not support the selected reasoning effort";
    }
    agents.push_back(std::move(agent));
  }
  return agents;
}

std::string_view default_explorer_system_prompt() {
  return kExplorerSystemPrompt;
}

const AgentDefinition *find_agent(const std::vector<AgentDefinition> &agents,
                                  std::string_view name) {
  const auto iterator =
      std::find_if(agents.begin(), agents.end(),
                   [&](const auto &agent) { return agent.name == name; });
  return iterator == agents.end() ? nullptr : &*iterator;
}

std::string format_agents(const std::vector<AgentDefinition> &agents) {
  std::string result = "configured subagents:\n";
  for (const auto &agent : agents) {
    result += "  " + agent.name + " — " + agent.description + "\n";
    result += "    model: " + agent.model.provider + "/" + agent.model.model +
              " · reasoning: " +
              std::string(core::reasoning_effort_name(agent.reasoning_effort)) +
              "\n";
    result += "    tools: ";
    for (std::size_t index = 0; index < agent.tools.size(); ++index) {
      if (index > 0)
        result += ", ";
      result += agent.tools[index];
    }
    result += "\n    limits: " + std::to_string(agent.max_turns) + " turns · " +
              std::to_string(agent.max_output_tokens) +
              " output tokens\n    status: ";
    result += agent.available
                  ? "available\n"
                  : "unavailable — " + agent.unavailable_reason + "\n";
  }
  return result;
}

} // namespace zed::subagents
