#pragma once

#include "zed/app/config.hpp"
#include "zed/app/configure_web.hpp"
#include "zed/core/agent_loop.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/extensions/quick_bash_input.hpp"
#include "zed/plugins/plugin_manager.hpp"
#include "zed/providers/opencode_go_model.hpp"
#include "zed/session/jsonl_session_store.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/tools/subagent_tool.hpp"
#include "zed/ui/theme.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace zed::app {

using SessionMetadataFactory = std::function<session::SessionMetadata(
    const std::filesystem::path &, std::string)>;

class BuiltinCommandRegistrar {
public:
  BuiltinCommandRegistrar(
      extensions::ExtensionRegistry &extensions,
      const RuntimeConfig &runtime_config,
      std::vector<providers::OpenCodeGoModelInfo> &model_catalog,
      std::vector<subagents::AgentDefinition> &built_in_agents,
      tools::SubagentTool *subagent_tool_handle, core::ToolRegistry &tools,
      ConfigureWebServer &configure_web, skills::SkillRegistry &skills,
      std::string &active_skill, core::ModelRef &active_model,
      core::ReasoningEffort &active_reasoning_effort,
      core::ContextLimits &active_context_limits, ui::ThemeKind &active_theme,
      core::AgentLoop &loop, providers::OpenCodeGoModel &model,
      extensions::QuickBashInput &quick_bash,
      const std::filesystem::path &session_directory,
      session::JsonlSessionStore &session,
      const SessionMetadataFactory &session_metadata,
      plugins::PluginManager &plugins);

  bool register_commands();

private:
  extensions::Command
  create_model_command(std::vector<extensions::CommandOption> options);
  extensions::Command
  create_session_command(std::vector<extensions::CommandOption> options);

  extensions::ExtensionRegistry &extensions;
  const RuntimeConfig &runtime_config;
  std::vector<providers::OpenCodeGoModelInfo> &model_catalog;
  std::vector<subagents::AgentDefinition> &built_in_agents;
  tools::SubagentTool *subagent_tool_handle;
  core::ToolRegistry &tools;
  ConfigureWebServer &configure_web;
  skills::SkillRegistry &skills;
  std::string &active_skill;
  core::ModelRef &active_model;
  core::ReasoningEffort &active_reasoning_effort;
  core::ContextLimits &active_context_limits;
  ui::ThemeKind &active_theme;
  core::AgentLoop &loop;
  providers::OpenCodeGoModel &model;
  extensions::QuickBashInput &quick_bash;
  const std::filesystem::path &session_directory;
  session::JsonlSessionStore &session;
  const SessionMetadataFactory &session_metadata;
  plugins::PluginManager &plugins;
};

} // namespace zed::app
