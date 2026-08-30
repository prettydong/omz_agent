#include "builtin_commands.hpp"

#include "zed/session/session_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
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
    plugins::PluginManager &plugins)
    : extensions(extensions), runtime_config(runtime_config),
      model_catalog(model_catalog), built_in_agents(built_in_agents),
      subagent_tool_handle(subagent_tool_handle), tools(tools),
      configure_web(configure_web), skills(skills), active_skill(active_skill),
      active_model(active_model),
      active_reasoning_effort(active_reasoning_effort),
      active_context_limits(active_context_limits), active_theme(active_theme),
      loop(loop), model(model), quick_bash(quick_bash),
      session_directory(session_directory), session(session),
      session_metadata(session_metadata), plugins(plugins) {}

extensions::Command BuiltinCommandRegistrar::create_model_command(
    std::vector<extensions::CommandOption> options) {
  return extensions::Command{
      .name = "model",
      .description = "Show, set, or refresh OpenCode Go models: /model "
                     "<id|list|refresh>.",
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
            for (const auto &command : extensions.commands()) {
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
