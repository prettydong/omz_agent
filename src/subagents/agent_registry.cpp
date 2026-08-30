#include "zed/subagents/agent_registry.hpp"

#include <algorithm>
#include <utility>

namespace zed::subagents {

namespace {

constexpr std::string_view kExplorerModel = "muse-spark-1.2-contributor";

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
built_in_agents(const std::vector<providers::OpenCodeGoModelInfo> &models) {
  const auto *model = providers::find_opencode_go_model(models, kExplorerModel);
  const bool supports_low =
      model != nullptr &&
      providers::supports_reasoning_effort(*model, core::ReasoningEffort::low);
  AgentDefinition explorer{
      "explorer",
      "Read-only workspace exploration and evidence gathering.",
      {"opencode-go", std::string(kExplorerModel)},
      core::ReasoningEffort::low,
      std::string(kExplorerSystemPrompt),
      {"read", "grep", "find", "ls", "lsp"},
      model != nullptr && supports_low,
      {},
  };
  if (model == nullptr) {
    explorer.unavailable_reason =
        "fixed model is not present in the OpenCode Go catalog";
  } else if (!supports_low) {
    explorer.unavailable_reason = "fixed model does not support low reasoning";
  }
  return {std::move(explorer)};
}

const AgentDefinition *find_agent(const std::vector<AgentDefinition> &agents,
                                  std::string_view name) {
  const auto iterator =
      std::find_if(agents.begin(), agents.end(),
                   [&](const auto &agent) { return agent.name == name; });
  return iterator == agents.end() ? nullptr : &*iterator;
}

std::string format_agents(const std::vector<AgentDefinition> &agents) {
  std::string result = "built-in agents:\n";
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
    result += "\n    status: ";
    result += agent.available
                  ? "available\n"
                  : "unavailable — " + agent.unavailable_reason + "\n";
  }
  return result;
}

} // namespace zed::subagents
