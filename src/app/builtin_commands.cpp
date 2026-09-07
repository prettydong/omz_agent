#include "builtin_commands.hpp"

#include "zed/session/session_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string_view>
#include <utility>

namespace {

std::string trim_ascii_whitespace(std::string_view value) {
  const auto visible = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  const auto begin = std::find_if(value.begin(), value.end(), visible);
  const auto end = std::find_if(value.rbegin(), value.rend(), visible).base();
  if (begin >= end)
    return {};
  return std::string(begin, end);
}

std::pair<std::string, std::string>
split_first_argument(std::string_view arguments) {
  const auto trimmed = trim_ascii_whitespace(arguments);
  const auto separator = trimmed.find_first_of(" \t\r\n");
  if (separator == std::string::npos)
    return {trimmed, {}};
  return {trimmed.substr(0, separator),
          trim_ascii_whitespace(trimmed.substr(separator + 1))};
}

zed::core::Result<std::size_t> parse_size(std::string_view text,
                                          std::string_view field) {
  std::size_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    return zed::core::Result<std::size_t>::failure({
        zed::core::ErrorCode::invalid_argument,
        std::string(field) + " must be an integer",
    });
  }
  return zed::core::Result<std::size_t>::success(value);
}

zed::core::Result<double> parse_temperature(std::string_view text) {
  double value = 0.0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    return zed::core::Result<double>::failure({
        zed::core::ErrorCode::invalid_argument,
        "temperature must be a number between 0 and 2",
    });
  }
  return zed::core::Result<double>::success(value);
}

zed::core::Result<bool> parse_on_off(std::string_view text,
                                     std::string_view field) {
  if (text == "on")
    return zed::core::Result<bool>::success(true);
  if (text == "off")
    return zed::core::Result<bool>::success(false);
  return zed::core::Result<bool>::failure({
      zed::core::ErrorCode::invalid_argument,
      std::string(field) + " must be on or off",
  });
}

std::vector<std::string> split_csv(std::string_view text) {
  std::vector<std::string> values;
  while (true) {
    const auto separator = text.find(',');
    values.push_back(trim_ascii_whitespace(text.substr(0, separator)));
    if (separator == std::string_view::npos)
      return values;
    text.remove_prefix(separator + 1);
  }
}

} // namespace

namespace zed::app {

BuiltinCommandRegistrar::BuiltinCommandRegistrar(
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
    plugins::PluginManager &plugins, context::ContextArchive *context_archive)
    : extensions(extensions), runtime_config(runtime_config),
      model_catalog(model_catalog), built_in_agents(built_in_agents),
      subagent_tool_handle(subagent_tool_handle), tools(tools),
      configure_web(configure_web), skills(skills), active_skill(active_skill),
      active_model(active_model),
      active_reasoning_effort(active_reasoning_effort),
      active_context_limits(active_context_limits), active_theme(active_theme),
      loop(loop), model(model), quick_bash(quick_bash),
      session_directory(session_directory), session(session),
      session_metadata(session_metadata), plugins(plugins),
      context_archive(context_archive) {}

extensions::Command BuiltinCommandRegistrar::create_model_command(
    std::vector<extensions::CommandOption> options) {
  return extensions::Command{
      .name = "model",
      .description = "Show, set, or refresh OpenCode Go models: /model "
                     "<id|list|refresh>.",
      .execute = {},
      .options = std::move(options),
      .execute_with_events =
          [this](std::string_view arguments,
                 core::CancellationToken cancellation,
                 core::AgentEventCallback) {
            const auto describe = [&](const auto &model_info) {
              std::string result =
                  model_info.id + " — " + model_info.name + " [" +
                  std::string(
                      providers::open_code_protocol_name(model_info.protocol)) +
                  "]";
              if (!model_info.reasoning_efforts.empty()) {
                result += " — reasoning: ";
                for (std::size_t index = 0;
                     index < model_info.reasoning_efforts.size(); ++index) {
                  if (index > 0)
                    result += ",";
                  result += core::reasoning_effort_name(
                      model_info.reasoning_efforts[index]);
                }
              }
              return result;
            };

            std::string requested = trim_ascii_whitespace(arguments);
            if (requested == "refresh") {
              const auto discovered = providers::discover_opencode_go_models(
                  runtime_config.opencode_path, 5'000, cancellation);
              if (!discovered)
                return core::Result<std::string>::failure(discovered.error());
              model_catalog = discovered.value();
              model.set_models(model_catalog);
              built_in_agents = subagents::configured_agents(
                  model_catalog, runtime_config.subagents);
              subagent_tool_handle->set_agents(built_in_agents);

              std::string result = "refreshed OpenCode Go models: " +
                                   std::to_string(model_catalog.size()) + "\n";
              const auto *active_info = providers::find_opencode_go_model(
                  model_catalog, active_model.model);
              if (active_info == nullptr) {
                result += "warning: active model is no longer available: " +
                          active_model.model + "\n";
              } else {
                active_context_limits =
                    core::cap_context_limits(runtime_config.context_limits,
                                             active_info->max_context_tokens);
                loop.set_context_limits(active_context_limits);
                if (!providers::supports_reasoning_effort(
                        *active_info, active_reasoning_effort)) {
                  active_reasoning_effort = core::ReasoningEffort::automatic;
                  loop.set_reasoning_effort(active_reasoning_effort);
                  result += "reasoning reset to auto\n";
                }
              }
              if (providers::find_opencode_go_model(
                      model_catalog, runtime_config.context_model.model) ==
                  nullptr) {
                result += "warning: context model is no longer available: " +
                          runtime_config.context_model.model + "\n";
              }
              result += "use /model list to inspect the refreshed catalog\n";
              return core::Result<std::string>::success(std::move(result));
            }
            if (requested == "list") {
              std::string result;
              for (const auto &model_info : model_catalog) {
                result += (model_info.id == active_model.model ? "* " : "  ") +
                          describe(model_info) + "\n";
              }
              return core::Result<std::string>::success(std::move(result));
            }
            if (requested.empty()) {
              const auto *model_info = providers::find_opencode_go_model(
                  model_catalog, active_model.model);
              std::string result = "main: " + active_model.model + "\n";
              if (model_info != nullptr)
                result += describe(*model_info) + "\n";
              result += "context: " + runtime_config.context_model.model +
                        "\nusage: /model <id|list|refresh>\n";
              return core::Result<std::string>::success(std::move(result));
            }
            constexpr std::string_view kProviderPrefix = "opencode-go/";
            if (requested.starts_with(kProviderPrefix))
              requested.erase(0, kProviderPrefix.size());
            const auto *model_info =
                providers::find_opencode_go_model(model_catalog, requested);
            if (model_info == nullptr) {
              return core::Result<std::string>::failure({
                  core::ErrorCode::not_found,
                  "OpenCode Go model not found: " + requested +
                      "; use /model list or /model refresh",
              });
            }

            active_model.model = requested;
            loop.set_model(active_model);
            active_context_limits = core::cap_context_limits(
                runtime_config.context_limits, model_info->max_context_tokens);
            loop.set_context_limits(active_context_limits);
            if (!providers::supports_reasoning_effort(
                    *model_info, active_reasoning_effort)) {
              active_reasoning_effort = core::ReasoningEffort::automatic;
              loop.set_reasoning_effort(active_reasoning_effort);
            }
            return core::Result<std::string>::success(
                "model: " + describe(*model_info) + "\nreasoning: " +
                std::string(
                    core::reasoning_effort_name(active_reasoning_effort)) +
                "\n");
          },
  };
}

extensions::Command BuiltinCommandRegistrar::create_session_command(
    std::vector<extensions::CommandOption> options) {
  return {
      "session",
      "Manage Session v2: /session [list|new|open|rename|fork].",
      [this](std::string_view arguments) {
        const auto [action, remainder] = split_first_argument(arguments);
        const auto open_session = [&](std::string_view identifier) {
          if (identifier.empty()) {
            return zed::core::Result<std::string>::failure({
                zed::core::ErrorCode::invalid_argument,
                "usage: /session open <id-or-title>",
            });
          }
          const auto selected =
              zed::session::find_session(session_directory, identifier);
          if (!selected)
            return zed::core::Result<std::string>::failure(selected.error());
          const auto switched = session.switch_to(selected.value().path);
          if (!switched)
            return zed::core::Result<std::string>::failure(switched.error());
          std::string result = "opened session: " + selected.value().title +
                               " [" + selected.value().name + "]\n";
          if (switched.value().recovered) {
            result +=
                "recovered interrupted turn: " + switched.value().turn_id +
                "\n";
          }
          return zed::core::Result<std::string>::success(std::move(result));
        };

        if (action == "new") {
          const auto path = zed::session::new_session_path(session_directory);
          {
            zed::session::JsonlSessionStore created(path);
            const auto initialized =
                created.initialize(session_metadata(path, remainder));
            if (!initialized) {
              return zed::core::Result<std::string>::failure(
                  initialized.error());
            }
          }
          const auto switched = session.switch_to(path);
          if (!switched)
            return zed::core::Result<std::string>::failure(switched.error());
          const auto info = session.inspect();
          if (!info)
            return zed::core::Result<std::string>::failure(info.error());
          return zed::core::Result<std::string>::success(
              "created session: " + info.value().metadata.title + " [" +
              info.value().metadata.id + "]\n");
        }
        if (action == "rename") {
          if (remainder.empty()) {
            return zed::core::Result<std::string>::failure({
                zed::core::ErrorCode::invalid_argument,
                "usage: /session rename <title>",
            });
          }
          const auto renamed = session.set_title(remainder);
          if (!renamed)
            return zed::core::Result<std::string>::failure(renamed.error());
          return zed::core::Result<std::string>::success(
              "renamed session: " + remainder + "\n");
        }
        if (action == "fork") {
          const auto path = zed::session::new_session_path(session_directory);
          const auto forked = session.fork_to(path, remainder);
          if (!forked)
            return zed::core::Result<std::string>::failure(forked.error());
          if (context_archive != nullptr) {
            const auto copied = context_archive->fork_to(
                std::filesystem::path(path.string() + ".context"), {});
            if (!copied)
              return core::Result<std::string>::failure(
                  {copied.error().code,
                   "session transcript fork was created but context state "
                   "could not be copied; current session unchanged: " +
                       copied.error().message});
          }
          const auto switched = session.switch_to(path);
          if (!switched)
            return zed::core::Result<std::string>::failure(switched.error());
          const auto info = session.inspect();
          if (!info)
            return zed::core::Result<std::string>::failure(info.error());
          return zed::core::Result<std::string>::success(
              "forked session: " + info.value().metadata.title + " [" +
              info.value().metadata.id + "]\n");
        }
        if (action == "open")
          return open_session(remainder);
        if (!action.empty() && action != "list")
          return open_session(trim_ascii_whitespace(arguments));
        if (action == "list" && !remainder.empty()) {
          return zed::core::Result<std::string>::failure({
              zed::core::ErrorCode::invalid_argument,
              "usage: /session list",
          });
        }

        const auto sessions = zed::session::list_sessions(session_directory);
        if (!sessions)
          return zed::core::Result<std::string>::failure(sessions.error());
        const auto active_info = session.inspect();
        if (!active_info)
          return zed::core::Result<std::string>::failure(active_info.error());

        std::string result = "active: " + active_info.value().metadata.title +
                             " [" + active_info.value().metadata.id + "]\n";
        result += "path: " + session.path().string() + "\n";
        result +=
            "turns: " + std::to_string(active_info.value().turn_count) +
            ", messages: " + std::to_string(active_info.value().message_count) +
            "\n";
        if (sessions.value().empty()) {
          result += "saved sessions: none\n";
        } else {
          result += "saved sessions:\n";
          for (const auto &entry : sessions.value()) {
            const bool active = entry.path.lexically_normal() ==
                                session.path().lexically_normal();
            result += active ? "  * " : "    ";
            if (!entry.valid) {
              result += entry.name + " [not Session v2]\n";
              continue;
            }
            result += entry.title + " [" + entry.name + "] — " +
                      std::to_string(entry.turn_count) + " turns";
            if (entry.interrupted)
              result += " — interrupted";
            result += "\n";
          }
        }
        result += "usage: /session new [title] | open <id-or-title> | "
                  "rename <title> | fork [title]\n";
        return zed::core::Result<std::string>::success(std::move(result));
      },
      std::move(options),
      {},
  };
}

extensions::Command BuiltinCommandRegistrar::create_configure_command(
    std::vector<extensions::CommandOption> options) {
  return {
      "configure",
      "Inspect or persist terminal configuration; changes apply after restart.",
      [this](std::string_view arguments) {
        const auto usage = [] {
          return std::string(
              "usage:\n"
              "  /configure [show]\n"
              "  /configure model [id]\n"
              "  /configure reasoning [effort]\n"
              "  /configure agent list|select <id>|set <field> <value>\n"
              "  /configure subagent list|set <explorer|id> <field> <value>\n"
              "  /configure skill list|enable <id>|disable <id>\n"
              "  /configure context set <field> <value>\n"
              "agent fields: model, reasoning, max-turns, max-output-tokens, "
              "temperature, compaction, trigger, tools, system-prompt\n"
              "tools accepts * or a comma-separated list of registered tool "
              "names\n"
              "subagent fields: enabled, model, reasoning, max-turns, "
              "max-output-tokens, system-prompt\n"
              "context fields: model, max-tokens, reserved-output-tokens, "
              "trigger, max-output-tokens, max-concurrency, timeout-ms, "
              "max-output-bytes, experimental-mode, system-prompt\n");
        };
        const auto loaded_config =
            load_workspace_config(runtime_config.workspace);
        if (!loaded_config)
          return core::Result<std::string>::failure(loaded_config.error());
        const auto loaded_prompts =
            load_workspace_prompts(runtime_config.workspace);
        if (!loaded_prompts)
          return core::Result<std::string>::failure(loaded_prompts.error());
        const auto loaded_management = load_agent_management(
            runtime_config.workspace, loaded_config.value(),
            loaded_prompts.value());
        if (!loaded_management) {
          return core::Result<std::string>::failure(loaded_management.error());
        }
        const auto loaded_skills =
            skills::load_workspace_skills(runtime_config.workspace);
        if (!loaded_skills)
          return core::Result<std::string>::failure(loaded_skills.error());

        auto config = loaded_config.value();
        auto prompts = loaded_prompts.value();
        auto management = loaded_management.value();
        auto managed_skills = loaded_skills.value();
        const auto active_profile = [&]() -> AgentProfile * {
          const auto profile =
              std::find_if(management.agents.begin(), management.agents.end(),
                           [&](const AgentProfile &candidate) {
                             return candidate.id == management.active_agent;
                           });
          return profile == management.agents.end() ? nullptr : &*profile;
        };
        const auto summary = [&]() {
          const auto *active = active_profile();
          std::string result =
              "workspace: " + runtime_config.workspace.string() + "\nconfig: " +
              workspace_config_path(runtime_config.workspace).string() + "\n";
          if (active != nullptr) {
            result += "active agent: " + active->id + " (" +
                      active->config.model.model + ", " +
                      std::string(core::reasoning_effort_name(
                          active->config.reasoning_effort)) +
                      ")\n";
            result +=
                "  turns: " + std::to_string(active->config.max_turns) +
                ", output: " +
                std::to_string(active->config.max_output_tokens) +
                ", temperature: " + std::to_string(active->config.temperature) +
                ", compaction: " +
                (active->automatic_context_compaction ? "on" : "off") +
                ", trigger: " +
                std::to_string(active->compaction_trigger_tokens) + "\n";
          }
          result +=
              "context: " + config.context.model.model + ", max: " +
              std::to_string(config.context.limits.max_context_tokens) +
              ", reserved: " +
              std::to_string(config.context.limits.reserved_output_tokens) +
              ", output: " + std::to_string(config.context.max_output_tokens) +
              ", experimental mode: " +
              (config.context.experimental_mode ? "on" : "off") + "\n";
          result += "subagent execution: concurrency " +
                    std::to_string(config.subagent_execution.max_concurrency) +
                    ", timeout " +
                    std::to_string(config.subagent_execution.total_timeout_ms) +
                    " ms, output " +
                    std::to_string(
                        config.subagent_execution.max_aggregate_output_bytes) +
                    " bytes\n";
          result += "subagents: explorer";
          for (const auto &subagent : management.subagents)
            result += ", " + subagent.name;
          result += "\nskills: " + std::to_string(managed_skills.size()) +
                    "\nrestart required after saved changes\n";
          return result;
        };
        const auto save = [&](bool write_prompts, bool write_config,
                              bool write_management,
                              bool write_skills) -> core::Result<std::string> {
          auto *active = active_profile();
          if (active == nullptr) {
            return core::Result<std::string>::failure({
                core::ErrorCode::invalid_argument,
                "active Agent does not exist",
            });
          }
          config.agent = active->config;
          config.context.limits.compaction_trigger_tokens =
              active->compaction_trigger_tokens;
          prompts.agent = active->system_prompt;
          const auto valid_config =
              parse_workspace_config(serialize_workspace_config(config));
          const auto valid_prompts = validate_workspace_prompts(prompts);
          const auto valid_management = validate_agent_management(management);
          const auto valid_skills =
              skills::validate_workspace_skills(managed_skills);
          if (!valid_config || !valid_prompts || !valid_management ||
              !valid_skills) {
            const auto *error = !valid_config       ? &valid_config.error()
                                : !valid_prompts    ? &valid_prompts.error()
                                : !valid_management ? &valid_management.error()
                                                    : &valid_skills.error();
            return core::Result<std::string>::failure(*error);
          }
          const auto validate_model =
              [&](std::string_view model_id, core::ReasoningEffort effort,
                  std::string_view label) -> core::Result<void> {
            const auto *model =
                providers::find_opencode_go_model(model_catalog, model_id);
            if (model == nullptr) {
              return core::Result<void>::failure({
                  core::ErrorCode::not_found,
                  std::string(label) +
                      " model is not in the current catalog: " +
                      std::string(model_id),
              });
            }
            if (!providers::supports_reasoning_effort(*model, effort)) {
              return core::Result<void>::failure({
                  core::ErrorCode::invalid_argument,
                  std::string(label) +
                      " reasoning effort is not supported by " +
                      std::string(model_id),
              });
            }
            return core::Result<void>::success();
          };
          const auto main_model =
              validate_model(active->config.model.model,
                             active->config.reasoning_effort, "active Agent");
          if (!main_model)
            return core::Result<std::string>::failure(main_model.error());
          const auto context_model =
              validate_model(config.context.model.model,
                             core::ReasoningEffort::automatic, "context");
          if (!context_model)
            return core::Result<std::string>::failure(context_model.error());
          const auto explorer_model =
              validate_model(config.explorer.model.model,
                             config.explorer.reasoning_effort, "Explorer");
          if (!explorer_model)
            return core::Result<std::string>::failure(explorer_model.error());
          for (const auto &subagent : management.subagents) {
            const auto valid_subagent =
                validate_model(subagent.model.model, subagent.reasoning_effort,
                               "Sub Agent " + subagent.name);
            if (!valid_subagent)
              return core::Result<std::string>::failure(valid_subagent.error());
          }
          if (write_prompts) {
            const auto saved_prompts =
                save_workspace_prompts(runtime_config.workspace, prompts);
            if (!saved_prompts) {
              return core::Result<std::string>::failure(saved_prompts.error());
            }
          }
          if (write_config) {
            const auto saved_config =
                save_workspace_config(runtime_config.workspace, config);
            if (!saved_config) {
              return core::Result<std::string>::failure(saved_config.error());
            }
          }
          if (write_management) {
            const auto saved_management =
                save_agent_management(runtime_config.workspace, management);
            if (!saved_management) {
              return core::Result<std::string>::failure(
                  saved_management.error());
            }
          }
          if (write_skills) {
            const auto saved_skills = skills::save_workspace_skills(
                runtime_config.workspace, managed_skills);
            if (!saved_skills) {
              return core::Result<std::string>::failure(saved_skills.error());
            }
          }
          return core::Result<std::string>::success(
              "configuration saved to " +
              workspace_config_path(runtime_config.workspace).string() +
              "\nrestart zeda for changes to take effect\n");
        };
        const auto model_valid = [&](std::string_view requested) {
          return providers::find_opencode_go_model(model_catalog, requested) !=
                 nullptr;
        };
        const auto set_active_model =
            [&](std::string_view requested) -> core::Result<std::string> {
          auto *profile = active_profile();
          const auto *model_info =
              providers::find_opencode_go_model(model_catalog, requested);
          if (profile == nullptr || model_info == nullptr) {
            return core::Result<std::string>::failure({
                core::ErrorCode::not_found,
                "model not found; use /model list",
            });
          }
          profile->config.model.model = std::string(requested);
          if (!providers::supports_reasoning_effort(
                  *model_info, profile->config.reasoning_effort)) {
            profile->config.reasoning_effort = core::ReasoningEffort::automatic;
            return core::Result<std::string>::success(
                "reasoning reset to auto\n");
          }
          return core::Result<std::string>::success({});
        };
        const auto set_active_reasoning =
            [&](std::string_view requested) -> core::Result<void> {
          const auto effort = core::reasoning_effort_from_name(requested);
          auto *profile = active_profile();
          if (!effort.has_value()) {
            return core::Result<void>::failure({
                core::ErrorCode::invalid_argument,
                "unknown reasoning effort",
            });
          }
          const auto *model_info =
              profile == nullptr
                  ? nullptr
                  : providers::find_opencode_go_model(
                        model_catalog, profile->config.model.model);
          if (profile == nullptr || model_info == nullptr ||
              !providers::supports_reasoning_effort(*model_info, *effort)) {
            return core::Result<void>::failure({
                core::ErrorCode::invalid_argument,
                "reasoning effort is not supported by the active Agent model",
            });
          }
          profile->config.reasoning_effort = *effort;
          return core::Result<void>::success();
        };
        const auto arguments_trimmed = trim_ascii_whitespace(arguments);
        if (arguments_trimmed.empty() || arguments_trimmed == "show")
          return core::Result<std::string>::success(summary() + usage());

        const auto [group, after_group] =
            split_first_argument(arguments_trimmed);
        const auto [action, after_action] = split_first_argument(after_group);
        if (group == "model") {
          if (after_group.empty()) {
            const auto *profile = active_profile();
            return core::Result<std::string>::success(
                "default model: " + profile->config.model.model + "\n" +
                "usage: /configure model [id]\n");
          }
          const auto [requested, remainder] = split_first_argument(after_group);
          if (!remainder.empty())
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument,
                 "usage: /configure model [id]"});
          const auto updated = set_active_model(requested);
          if (!updated)
            return core::Result<std::string>::failure(updated.error());
          const auto saved = save(true, true, true, false);
          if (!saved)
            return saved;
          return core::Result<std::string>::success(saved.value() +
                                                    updated.value());
        }
        if (group == "reasoning") {
          if (after_group.empty()) {
            const auto *profile = active_profile();
            return core::Result<std::string>::success(
                "default reasoning: " +
                std::string(core::reasoning_effort_name(
                    profile->config.reasoning_effort)) +
                "\nusage: /configure reasoning [effort]\n");
          }
          const auto [requested, remainder] = split_first_argument(after_group);
          if (!remainder.empty())
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument,
                 "usage: /configure reasoning [effort]"});
          const auto updated = set_active_reasoning(requested);
          if (!updated)
            return core::Result<std::string>::failure(updated.error());
          return save(true, true, true, false);
        }
        if (group == "agent") {
          if (action == "list") {
            if (!after_action.empty())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument,
                   "usage: /configure agent list"});
            std::string result;
            for (const auto &profile : management.agents) {
              result += profile.id == management.active_agent ? "* " : "  ";
              result += profile.id + " — " + profile.name + " — " +
                        profile.config.model.model + "\n";
            }
            return core::Result<std::string>::success(std::move(result));
          }
          if (action == "select") {
            if (after_action.empty())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument,
                   "usage: /configure agent select <id>"});
            const auto profile =
                std::find_if(management.agents.begin(), management.agents.end(),
                             [&](const AgentProfile &candidate) {
                               return candidate.id == after_action;
                             });
            if (profile == management.agents.end())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::not_found,
                   "Agent not found: " + after_action});
            management.active_agent = profile->id;
            return save(true, true, true, false);
          }
          if (action != "set")
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          const auto [field, value] = split_first_argument(after_action);
          auto *profile = active_profile();
          if (field.empty() || value.empty() || profile == nullptr)
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          std::string change_note;
          if (field == "model") {
            const auto updated = set_active_model(value);
            if (!updated)
              return core::Result<std::string>::failure(updated.error());
            change_note = updated.value();
          } else if (field == "reasoning") {
            const auto updated = set_active_reasoning(value);
            if (!updated)
              return core::Result<std::string>::failure(updated.error());
          } else if (field == "max-turns" || field == "max-output-tokens" ||
                     field == "trigger") {
            const auto number = parse_size(value, field);
            if (!number)
              return core::Result<std::string>::failure(number.error());
            if (field == "max-turns")
              profile->config.max_turns = number.value();
            else if (field == "max-output-tokens")
              profile->config.max_output_tokens = number.value();
            else
              profile->compaction_trigger_tokens = number.value();
          } else if (field == "temperature") {
            const auto temperature = parse_temperature(value);
            if (!temperature)
              return core::Result<std::string>::failure(temperature.error());
            if (!std::isfinite(temperature.value())) {
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument,
                   "temperature must be finite and between 0 and 2"});
            }
            profile->config.temperature = temperature.value();
          } else if (field == "compaction") {
            const auto enabled = parse_on_off(value, field);
            if (!enabled)
              return core::Result<std::string>::failure(enabled.error());
            profile->automatic_context_compaction = enabled.value();
          } else if (field == "tools") {
            profile->tools = split_csv(value);
          } else if (field == "system-prompt") {
            profile->system_prompt = value;
          } else {
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          }
          const auto saved = save(true, true, true, false);
          if (!saved)
            return saved;
          return core::Result<std::string>::success(saved.value() +
                                                    change_note);
        }

        if (group == "subagent") {
          if (action == "list") {
            if (!after_action.empty())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument,
                   "usage: /configure subagent list"});
            std::string result =
                "explorer — " + config.explorer.model.model +
                (config.explorer.enabled ? " — enabled\n" : " — disabled\n");
            for (const auto &subagent : management.subagents) {
              result += subagent.name + " — " + subagent.model.model +
                        (subagent.enabled ? " — enabled\n" : " — disabled\n");
            }
            return core::Result<std::string>::success(std::move(result));
          }
          if (action != "set")
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          const auto [id, after_id] = split_first_argument(after_action);
          const auto [field, value] = split_first_argument(after_id);
          if (id.empty() || field.empty() || value.empty())
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          subagents::ExplorerAgentConfig *target = nullptr;
          if (id == "explorer") {
            target = &config.explorer;
          } else {
            const auto found = std::find_if(
                management.subagents.begin(), management.subagents.end(),
                [&](const subagents::ExplorerAgentConfig &candidate) {
                  return candidate.name == id;
                });
            if (found == management.subagents.end())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::not_found, "Sub Agent not found: " + id});
            target = &*found;
          }
          if (field == "enabled") {
            const auto enabled = parse_on_off(value, field);
            if (!enabled)
              return core::Result<std::string>::failure(enabled.error());
            target->enabled = enabled.value();
          } else if (field == "model") {
            if (!model_valid(value))
              return core::Result<std::string>::failure(
                  {core::ErrorCode::not_found,
                   "model not found; use /model list"});
            target->model.model = value;
          } else if (field == "reasoning") {
            const auto effort = core::reasoning_effort_from_name(value);
            if (!effort.has_value())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument,
                   "unknown reasoning effort"});
            target->reasoning_effort = *effort;
          } else if (field == "max-turns" || field == "max-output-tokens") {
            const auto number = parse_size(value, field);
            if (!number)
              return core::Result<std::string>::failure(number.error());
            if (field == "max-turns")
              target->max_turns = number.value();
            else
              target->max_output_tokens = number.value();
          } else if (field == "system-prompt") {
            target->system_prompt = value;
            if (id == "explorer")
              prompts.explorer = value;
          } else {
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          }
          return save(id == "explorer", id == "explorer", id != "explorer",
                      false);
        }

        if (group == "skill") {
          if (action == "list") {
            if (!after_action.empty())
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument,
                   "usage: /configure skill list"});
            std::string result;
            for (const auto &skill : managed_skills) {
              result += skill.id + " — " +
                        (skill.enabled ? "enabled" : "disabled") + " — " +
                        skill.name + "\n";
            }
            return core::Result<std::string>::success(
                result.empty() ? "no managed workspace skills\n"
                               : std::move(result));
          }
          if ((action != "enable" && action != "disable") ||
              after_action.empty())
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          const auto skill =
              std::find_if(managed_skills.begin(), managed_skills.end(),
                           [&](const skills::ManagedSkill &candidate) {
                             return candidate.id == after_action;
                           });
          if (skill == managed_skills.end())
            return core::Result<std::string>::failure(
                {core::ErrorCode::not_found,
                 "Skill not found: " + after_action});
          skill->enabled = action == "enable";
          return save(false, false, false, true);
        }

        if (group == "context" && action == "set") {
          const auto [field, value] = split_first_argument(after_action);
          if (field.empty() || value.empty())
            return core::Result<std::string>::failure(
                {core::ErrorCode::invalid_argument, usage()});
          if (field == "model") {
            if (!model_valid(value))
              return core::Result<std::string>::failure(
                  {core::ErrorCode::not_found,
                   "model not found; use /model list"});
            config.context.model.model = value;
          } else if (field == "system-prompt") {
            prompts.context = value;
          } else if (field == "experimental-mode") {
            const auto enabled = parse_on_off(value, field);
            if (!enabled)
              return core::Result<std::string>::failure(enabled.error());
            config.context.experimental_mode = enabled.value();
          } else {
            const auto number = parse_size(value, field);
            if (!number)
              return core::Result<std::string>::failure(number.error());
            if (field == "max-tokens")
              config.context.limits.max_context_tokens = number.value();
            else if (field == "reserved-output-tokens")
              config.context.limits.reserved_output_tokens = number.value();
            else if (field == "trigger")
              active_profile()->compaction_trigger_tokens = number.value();
            else if (field == "max-output-tokens")
              config.context.max_output_tokens = number.value();
            else if (field == "max-concurrency")
              config.subagent_execution.max_concurrency = number.value();
            else if (field == "timeout-ms")
              config.subagent_execution.total_timeout_ms = number.value();
            else if (field == "max-output-bytes")
              config.subagent_execution.max_aggregate_output_bytes =
                  number.value();
            else
              return core::Result<std::string>::failure(
                  {core::ErrorCode::invalid_argument, usage()});
          }
          return save(true, true, true, false);
        }
        return core::Result<std::string>::failure(
            {core::ErrorCode::invalid_argument, usage()});
      },
      std::move(options),
      {},
  };
}

bool BuiltinCommandRegistrar::register_commands() {
  const auto register_command = [&](zed::extensions::Command command) {
    const auto result = extensions.register_command(std::move(command));
    if (!result)
      std::cerr << "command registration failed: " << result.error().message
                << "\n";
    return result.has_value();
  };
  std::vector<zed::extensions::CommandOption> skill_options;
  skill_options.reserve(skills.all().size());
  for (const auto &skill : skills.all()) {
    skill_options.push_back({skill.name, skill.description});
  }
  std::vector<zed::extensions::CommandOption> model_options;
  model_options.reserve(model_catalog.size() + 2);
  model_options.push_back({"list", "List available OpenCode Go models."});
  model_options.push_back(
      {"refresh", "Refresh models from the local OpenCode installation."});
  for (const auto &model_info : model_catalog) {
    model_options.push_back(
        {model_info.id, model_info.name + " (" +
                            std::string(zed::providers::open_code_protocol_name(
                                model_info.protocol)) +
                            ")"});
  }
  std::vector<zed::extensions::CommandOption> session_options;
  session_options.push_back({"list", "List Session v2 files."});
  session_options.push_back({"new", "Create and open a new Session."});
  session_options.push_back({"open", "Open a saved Session."});
  session_options.push_back({"rename", "Rename the active Session."});
  session_options.push_back({"fork", "Fork the active Session."});
  const auto discovered_sessions =
      zed::session::list_sessions(session_directory);
  if (!discovered_sessions) {
    std::cerr << "session discovery warning: "
              << discovered_sessions.error().message << "\n";
  } else {
    session_options.reserve(session_options.size() +
                            discovered_sessions.value().size());
    for (const auto &entry : discovered_sessions.value()) {
      if (entry.valid) {
        session_options.push_back({entry.name, "Open Session: " + entry.title});
      }
    }
  }
  if (!register_command({
          "help",
          "Show available commands.",
          [&](std::string_view) {
            std::string result = "commands:\n";
            for (const auto &command : extensions.commands_snapshot()) {
              result +=
                  "  /" + command.name + " — " + command.description + "\n";
            }
            result += "  /exit — quit\n";
            return zed::core::Result<std::string>::success(std::move(result));
          },
          {},
          {},
      }))
    return false;
  if (!register_command({
          "skills",
          "List discovered skills.",
          [&](std::string_view) {
            std::string result;
            if (skills.all().empty())
              return zed::core::Result<std::string>::success(
                  "no skills found\n");
            for (const auto &skill : skills.all()) {
              result += skill.name + " — " + skill.description + "\n";
            }
            return zed::core::Result<std::string>::success(std::move(result));
          },
          {},
          {},
      }))
    return false;
  if (!register_command({
          "agents",
          "List configured subagents and availability.",
          [&](std::string_view arguments) {
            if (!trim_ascii_whitespace(arguments).empty()) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "usage: /agents",
              });
            }
            return zed::core::Result<std::string>::success(
                zed::subagents::format_agents(built_in_agents));
          },
          {},
          {},
      }))
    return false;
  if (!register_command({
          "configure-web",
          "Open the local Agent, Sub Agent, Skill, and context manager.",
          [&](std::string_view arguments) {
            if (!trim_ascii_whitespace(arguments).empty()) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "usage: /configure-web",
              });
            }
            const auto opened = configure_web.open(
                model_catalog, true, tools.registered_definitions());
            if (!opened)
              return zed::core::Result<std::string>::failure(opened.error());
            return zed::core::Result<std::string>::success(
                "configuration: " + opened.value() +
                "\nsettings are stored in " +
                runtime_config.workspace_config_path.string() +
                " and take effect after restarting zeda\n");
          },
          {},
          {},
      }))
    return false;
  if (!register_command({
          "skill",
          "Activate a skill: /skill <name>.",
          [&](std::string_view arguments) {
            const std::string name(arguments);
            const auto *skill = skills.find(name);
            if (skill == nullptr) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::not_found,
                  "skill not found: " + name,
              });
            }
            active_skill = skill->name;
            return zed::core::Result<std::string>::success(
                "active skill: " + active_skill + "\n");
          },
          std::move(skill_options),
          {},
      }))
    return false;
  if (!register_command(create_model_command(std::move(model_options))))
    return false;
  if (!register_command({
          "theme",
          "Show or set theme: /theme <light|monaka>.",
          [&](std::string_view arguments) {
            if (arguments.empty()) {
              return zed::core::Result<std::string>::success(
                  "theme: " + std::string(zed::ui::theme_name(active_theme)) +
                  "\nusage: /theme <light|monaka>\n");
            }
            const auto theme = zed::ui::theme_kind_from_name(arguments);
            if (!theme.has_value()) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "theme must be one of: light, monaka",
              });
            }
            active_theme = *theme;
            return zed::core::Result<std::string>::success(
                "theme: " + std::string(zed::ui::theme_name(active_theme)) +
                "\n");
          },
          {
              {"light", "Use the OpenCode-inspired light theme."},
              {"monaka", "Use the Monaka dark theme."},
          },
          {},
      }))
    return false;
  if (!register_command({
          "quick-bash",
          "Show or set Quick Bash: /quick-bash <on|off>.",
          [&](std::string_view arguments) {
            if (arguments.empty()) {
              return zed::core::Result<std::string>::success(
                  std::string("quick bash: ") +
                  (quick_bash.enabled() ? "on" : "off") +
                  "\nusage: /quick-bash <on|off>\n");
            }
            if (arguments == "on") {
              quick_bash.set_enabled(true);
            } else if (arguments == "off") {
              quick_bash.set_enabled(false);
            } else {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "quick bash must be one of: on, off",
              });
            }
            return zed::core::Result<std::string>::success(
                std::string("quick bash: ") +
                (quick_bash.enabled() ? "on" : "off") + "\n");
          },
          {
              {"on", "Enable direct execution of safe simple commands."},
              {"off", "Send all input through the agent loop."},
          },
          {},
      }))
    return false;
  if (!register_command({
          "reasoning",
          "Show or set reasoning for the active model.",
          [&](std::string_view arguments) {
            const auto *model_info = zed::providers::find_opencode_go_model(
                model_catalog, active_model.model);
            std::string allowed = "auto";
            if (model_info != nullptr) {
              for (const auto effort : model_info->reasoning_efforts) {
                allowed += ", ";
                allowed += zed::core::reasoning_effort_name(effort);
              }
            } else {
              allowed += ", none, minimal, low, medium, high, xhigh, max, "
                         "thinking";
            }
            if (arguments.empty()) {
              return zed::core::Result<std::string>::success(
                  "reasoning: " +
                  std::string(zed::core::reasoning_effort_name(
                      active_reasoning_effort)) +
                  "\nmodel: " + active_model.model + "\navailable: " + allowed +
                  "\nusage: /reasoning <effort>\n");
            }
            const auto effort =
                zed::core::reasoning_effort_from_name(arguments);
            if (!effort.has_value()) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "unknown reasoning effort; available for " +
                      active_model.model + ": " + allowed,
              });
            }
            if (model_info != nullptr &&
                !zed::providers::supports_reasoning_effort(*model_info,
                                                           *effort)) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "reasoning effort '" + std::string(arguments) +
                      "' is not supported by " + active_model.model +
                      "; available: " + allowed,
              });
            }
            active_reasoning_effort = *effort;
            loop.set_reasoning_effort(*effort);
            return zed::core::Result<std::string>::success(
                "reasoning: " +
                std::string(
                    zed::core::reasoning_effort_name(active_reasoning_effort)) +
                "\n");
          },
          {
              {"auto", "Use the model's default reasoning behavior."},
              {"none", "Disable reasoning when the model supports it."},
              {"minimal", "Use minimal reasoning."},
              {"low", "Use fast, lightweight reasoning."},
              {"medium", "Use balanced reasoning."},
              {"high", "Use deeper, slower reasoning."},
              {"xhigh", "Use extra-high reasoning."},
              {"max", "Use the model's maximum reasoning effort."},
              {"thinking", "Enable adaptive thinking."},
          },
          {},
      }))
    return false;
  if (!register_command(create_session_command(std::move(session_options))))
    return false;
  if (!register_command({
          "new",
          "Create and open a new Session: /new [title].",
          [this](std::string_view arguments) {
            return extensions.execute(
                "session", "new " + trim_ascii_whitespace(arguments));
          },
          {},
          {},
      }))
    return false;
  if (!register_command(create_configure_command({
          {"show", "Show saved workspace configuration."},
          {"model", "Show or persist the default startup model."},
          {"reasoning",
           "Show or persist the default startup reasoning effort."},
          {"agent", "Manage active Agent profiles."},
          {"subagent", "Inspect or edit Explorer and custom Sub Agents."},
          {"skill", "List or enable/disable managed workspace Skills."},
          {"context", "Edit context and Sub Agent execution limits."},
      })))
    return false;
  if (!register_command({
          "plugins",
          "Show discovered external plugins.",
          [&](std::string_view) {
            return zed::core::Result<std::string>::success(
                plugins.status_report());
          },
          {},
          {},
      }))
    return false;
  return true;
}

} // namespace zed::app
