#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zed/app/config.hpp"
#include "zed/providers/opencode_go_catalog.hpp"
#include "zed/session/session_catalog.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/ui/terminal.hpp"

namespace zed::app {

inline ui::TerminalCommandOption
completion_option(std::string value, std::string description,
                  std::vector<ui::TerminalCommandOption> children = {},
                  bool accepts_argument = false) {
  ui::TerminalCommandOption option;
  option.value = std::move(value);
  option.description = std::move(description);
  option.children = std::move(children);
  option.accepts_argument = accepts_argument;
  return option;
}

inline std::vector<ui::TerminalCommandOption> model_id_options(
    const std::vector<providers::OpenCodeGoModelInfo> &model_catalog) {
  std::vector<ui::TerminalCommandOption> options;
  options.reserve(model_catalog.size());
  for (const auto &model : model_catalog) {
    options.push_back(completion_option(model.id, model.name));
  }
  return options;
}

inline std::vector<ui::TerminalCommandOption> reasoning_effort_options(
    const std::vector<providers::OpenCodeGoModelInfo> &model_catalog,
    std::string_view model_id) {
  std::vector<ui::TerminalCommandOption> options;
  options.push_back(completion_option("auto", "Use the model default."));
  const auto *model =
      providers::find_opencode_go_model(model_catalog, model_id);
  if (model == nullptr)
    return options;
  options.reserve(model->reasoning_efforts.size() + 1);
  for (const auto effort : model->reasoning_efforts) {
    const auto name = std::string(core::reasoning_effort_name(effort));
    if (name != "auto")
      options.push_back(completion_option(name, "Supported by this model."));
  }
  return options;
}

inline std::vector<ui::TerminalCommandOption> agent_set_field_options(
    const std::vector<providers::OpenCodeGoModelInfo> &model_catalog,
    std::string_view active_model) {
  return {
      completion_option("model", "Set the profile model.",
                        model_id_options(model_catalog)),
      completion_option("reasoning", "Set the profile reasoning effort.",
                        reasoning_effort_options(model_catalog, active_model)),
      completion_option("max-turns", "Set the turn limit.", {}, true),
      completion_option("max-output-tokens", "Set the output token limit.", {},
                        true),
      completion_option("temperature", "Set temperature from 0 to 2.", {},
                        true),
      completion_option("compaction", "Enable or disable compaction.",
                        {completion_option("on", "Enable compaction."),
                         completion_option("off", "Disable compaction.")}),
      completion_option("trigger", "Set the compaction trigger tokens.", {},
                        true),
      completion_option("tools", "Set comma-separated tool names.", {}, true),
      completion_option("system-prompt", "Set the system prompt.", {}, true),
  };
}

inline std::vector<ui::TerminalCommandOption> subagent_set_field_options(
    const std::vector<providers::OpenCodeGoModelInfo> &model_catalog,
    std::string_view model_id) {
  return {
      completion_option("enabled", "Enable or disable this Sub Agent.",
                        {completion_option("on", "Enable this Sub Agent."),
                         completion_option("off", "Disable this Sub Agent.")}),
      completion_option("model", "Set the Sub Agent model.",
                        model_id_options(model_catalog)),
      completion_option("reasoning", "Set the Sub Agent reasoning effort.",
                        reasoning_effort_options(model_catalog, model_id)),
      completion_option("max-turns", "Set the turn limit.", {}, true),
      completion_option("max-output-tokens", "Set the output token limit.", {},
                        true),
      completion_option("system-prompt", "Set the system prompt.", {}, true),
  };
}

inline std::vector<ui::TerminalCommandOption> context_set_field_options(
    const std::vector<providers::OpenCodeGoModelInfo> &model_catalog) {
  return {
      completion_option("model", "Set the context model.",
                        model_id_options(model_catalog)),
      completion_option("max-tokens", "Set maximum context tokens.", {}, true),
      completion_option("reserved-output-tokens", "Reserve output tokens.", {},
                        true),
      completion_option("trigger", "Set the compaction trigger tokens.", {},
                        true),
      completion_option("max-output-tokens", "Set context output tokens.", {},
                        true),
      completion_option("max-concurrency", "Set Sub Agent concurrency.", {},
                        true),
      completion_option("timeout-ms", "Set Sub Agent timeout in ms.", {}, true),
      completion_option("max-output-bytes", "Set Sub Agent output bytes.", {},
                        true),
      completion_option("system-prompt", "Set the context system prompt.", {},
                        true),
  };
}

inline std::vector<ui::TerminalCommandOption> configure_completion_options(
    const std::filesystem::path &workspace,
    const std::vector<providers::OpenCodeGoModelInfo> &model_catalog) {
  auto agent_description = std::string("Manage saved Agent profiles.");
  auto subagent_description = std::string("Inspect or edit Sub Agents.");
  auto skill_description = std::string("Manage workspace Skills.");
  std::vector<ui::TerminalCommandOption> profile_ids;
  std::vector<ui::TerminalCommandOption> subagent_ids;
  std::string active_model;

  const auto workspace_config = load_workspace_config(workspace);
  if (!workspace_config) {
    const auto failure = " Saved profile suggestions unavailable; run "
                         "/configure show to diagnose.";
    agent_description += failure;
    subagent_description += failure;
  } else {
    const auto prompts = load_workspace_prompts(workspace);
    if (!prompts) {
      const auto failure = " Saved profile suggestions unavailable; run "
                           "/configure show to diagnose.";
      agent_description += failure;
      subagent_description += failure;
    } else {
      const auto management = load_agent_management(
          workspace, workspace_config.value(), prompts.value());
      if (!management) {
        const auto failure = " Saved profile suggestions unavailable; run "
                             "/configure show to diagnose.";
        agent_description += failure;
        subagent_description += failure;
      } else {
        profile_ids.reserve(management.value().agents.size());
        for (const auto &profile : management.value().agents) {
          profile_ids.push_back(completion_option(profile.id, profile.name));
          if (profile.id == management.value().active_agent)
            active_model = profile.config.model.model;
        }
        subagent_ids.reserve(management.value().subagents.size() + 1);
        subagent_ids.push_back(completion_option(
            "explorer", "Built-in Explorer Sub Agent.",
            subagent_set_field_options(
                model_catalog, workspace_config.value().explorer.model.model)));
        for (const auto &subagent : management.value().subagents) {
          subagent_ids.push_back(completion_option(
              subagent.name, subagent.description,
              subagent_set_field_options(model_catalog, subagent.model.model)));
        }
      }
    }
  }

  const auto loaded_skills = skills::load_workspace_skills(workspace);
  std::vector<ui::TerminalCommandOption> skill_ids;
  if (!loaded_skills) {
    skill_description += " Managed Skill suggestions unavailable; run "
                         "/configure skill list to diagnose.";
  } else {
    skill_ids.reserve(loaded_skills.value().size());
    for (const auto &skill : loaded_skills.value()) {
      skill_ids.push_back(completion_option(skill.id, skill.name));
    }
  }

  if (active_model.empty() && workspace_config)
    active_model = workspace_config.value().agent.model.model;
  const auto agent_fields =
      agent_set_field_options(model_catalog, active_model);
  const bool needs_profile_id = profile_ids.empty();
  const bool needs_skill_id = skill_ids.empty();

  return {
      completion_option("show", "Show saved workspace configuration."),
      completion_option("model", "Show or persist the startup model.",
                        model_id_options(model_catalog)),
      completion_option("reasoning", "Set the startup reasoning effort.",
                        reasoning_effort_options(model_catalog, active_model)),
      completion_option(
          "agent", std::move(agent_description),
          {completion_option("list", "List saved Agent profiles."),
           completion_option("select", "Select an Agent profile.",
                             std::move(profile_ids), needs_profile_id),
           completion_option("set", "Set a field on the active profile.",
                             agent_fields)}),
      completion_option(
          "subagent", std::move(subagent_description),
          {completion_option("list", "List configured Sub Agents."),
           completion_option("set", "Set a Sub Agent field.",
                             std::move(subagent_ids))}),
      completion_option(
          "skill", std::move(skill_description),
          {completion_option("list", "List managed workspace Skills."),
           completion_option("enable", "Enable a managed Skill.", skill_ids,
                             needs_skill_id),
           completion_option("disable", "Disable a managed Skill.",
                             std::move(skill_ids), needs_skill_id)}),
      completion_option(
          "context", "Edit context and execution limits.",
          {completion_option("set", "Set a context field.",
                             context_set_field_options(model_catalog))}),
  };
}

inline std::vector<ui::TerminalCommandOption>
session_completion_options(const std::filesystem::path &session_directory) {
  std::vector<ui::TerminalCommandOption> session_ids;
  auto open_description = std::string("Open a saved Session.");
  const auto sessions = session::list_sessions(session_directory);
  if (!sessions) {
    open_description +=
        " Saved Session IDs unavailable; run /session list to diagnose.";
  } else {
    session_ids.reserve(sessions.value().size());
    for (const auto &session : sessions.value()) {
      if (session.valid) {
        session_ids.push_back(
            completion_option(session.name, "Open Session: " + session.title));
      }
    }
  }
  const bool needs_session_id = session_ids.empty();
  std::vector<ui::TerminalCommandOption> options{
      completion_option("list", "List Session v2 files."),
      completion_option("new", "Create and open a new Session.", {}, true),
      completion_option("open", std::move(open_description), session_ids,
                        needs_session_id),
      completion_option("rename", "Rename the active Session.", {}, true),
      completion_option("fork", "Fork the active Session.", {}, true),
  };
  options.insert(options.end(), session_ids.begin(), session_ids.end());
  return options;
}

} // namespace zed::app
