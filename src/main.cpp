#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include "zed/app/config.hpp"
#include "zed/app/configure_web.hpp"
#include "zed/core/agent_loop.hpp"
#include "zed/core/model_context_controller.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/extensions/quick_bash_input.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/plugins/plugin_manager.hpp"
#include "zed/providers/opencode_go_model.hpp"
#include "zed/session/jsonl_session_store.hpp"
#include "zed/session/session_catalog.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/subagents/subagent_runner.hpp"
#include "zed/subagents/worker.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/tools/clangd_tool.hpp"
#include "zed/tools/multi_bash_tool.hpp"
#include "zed/tools/subagent_tool.hpp"
#include "zed/ui/terminal.hpp"

namespace {

constexpr std::string_view kVersion = ZEDA_VERSION;

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

zed::core::ContextLimits
model_context_limits(zed::core::ContextLimits configured,
                     const zed::providers::OpenCodeGoModelInfo *model) {
  if (model == nullptr || model->max_context_tokens == 0 ||
      model->max_context_tokens >= configured.max_context_tokens) {
    return configured;
  }
  configured.max_context_tokens = model->max_context_tokens;
  if (configured.reserved_output_tokens >= configured.max_context_tokens)
    configured.reserved_output_tokens = configured.max_context_tokens / 8;
  const auto available =
      configured.max_context_tokens - configured.reserved_output_tokens;
  if (configured.compaction_trigger_tokens >= available)
    configured.compaction_trigger_tokens = 0;
  return configured;
}

std::string subagent_worker_executable(std::string_view argument_zero) {
  const std::filesystem::path candidate(argument_zero);
  if (!candidate.has_parent_path())
    return std::string(argument_zero);
  std::error_code error;
  auto absolute = std::filesystem::absolute(candidate, error);
  if (error)
    return std::string(argument_zero);
  auto canonical = std::filesystem::weakly_canonical(absolute, error);
  return error ? absolute.string() : canonical.string();
}

void print_usage(std::ostream &output) {
  output << "Usage: zeda [--help] [--version]\n"
         << "\n"
         << "Start the zeda coding agent in the current directory.\n"
         << "\n"
         << "Options:\n"
         << "  -h, --help     Show this help.\n"
         << "  -V, --version  Show the installed version.\n";
}

} // namespace

int main(int argc, char *argv[]) {
  const auto startup_started_at = std::chrono::steady_clock::now();
  if (argc > 1) {
    const std::string_view argument(argv[1]);
    if (argc == 2 && argument == "--subagent-worker") {
      return zed::subagents::run_explorer_worker(std::cin, std::cout,
                                                 std::cerr);
    }
    if (argc == 2 && (argument == "--help" || argument == "-h")) {
      print_usage(std::cout);
      return 0;
    }
    if (argc == 2 && (argument == "--version" || argument == "-V")) {
      std::cout << "zeda " << kVersion << "\n";
      return 0;
    }
    std::cerr << "unknown argument: " << argument << "\n\n";
    print_usage(std::cerr);
    return 2;
  }

  const auto runtime = zed::app::load_runtime_config();
  if (!runtime) {
    std::cerr << runtime.error().message << "\n";
    return 2;
  }
  const auto &runtime_config = runtime.value();
  const auto config_ready_at = std::chrono::steady_clock::now();
  auto model_catalog = zed::providers::default_opencode_go_models();
  zed::app::ConfigureWebServer configure_web(runtime_config.workspace);
  auto built_in_agents = zed::subagents::configured_agents(
      model_catalog, runtime_config.subagents);
  zed::subagents::ProcessSubagentRunner subagent_runner({
      subagent_worker_executable(argv[0]),
      runtime_config.workspace,
  });
  zed::lsp::ClangdClient clangd({
      runtime_config.workspace,
      runtime_config.clangd_path,
      zed::lsp::discover_compile_commands_directory(runtime_config.workspace),
  });
  zed::providers::OpenCodeGoModel model({
      runtime_config.opencode_go_api_key,
      runtime_config.opencode_endpoint,
      runtime_config.opencode_request_timeout_ms,
      model_catalog,
  });
  zed::core::ModelBackedContextController context_controller(
      model, runtime_config.context_model, runtime_config.context_system_prompt,
      runtime_config.context_max_output_tokens);
  zed::core::ApproximateTokenEstimator estimator;
  zed::core::BasicContextManager context(estimator, &context_controller);
  zed::session::JsonlSessionStore session(runtime_config.session_path);
  const auto session_directory = runtime_config.workspace / ".zed" / "sessions";
  const auto session_metadata = [&](const std::filesystem::path &path,
                                    std::string title = {}) {
    const auto id = path.stem().string();
    return zed::session::SessionMetadata{
        id,
        title.empty() ? id : std::move(title),
        runtime_config.workspace.string(),
        runtime_config.main_model.provider,
        runtime_config.main_model.model,
    };
  };
  const auto core_ready_at = std::chrono::steady_clock::now();
  const auto initialized =
      session.initialize(session_metadata(runtime_config.session_path));
  if (!initialized) {
    std::cerr << initialized.error().message << "\n";
    return 3;
  }
  const auto startup_recovery = session.recover_interrupted_turn();
  if (!startup_recovery) {
    std::cerr << startup_recovery.error().message << "\n";
    return 3;
  }
  const auto session_ready_at = std::chrono::steady_clock::now();

  zed::core::ToolRegistry tools(runtime_config.agent_tools);
  if (!tools.register_tool(std::make_unique<zed::tools::ReadFileTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::WriteFileTool>(
          runtime_config.workspace, runtime_config.tool_limits, &clangd)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::BashTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::MultiBashTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::GrepTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::FindFilesTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::ListDirectoryTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::EditFileTool>(
          runtime_config.workspace, runtime_config.tool_limits, &clangd)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::ClangdTool>(
          runtime_config.workspace, clangd, runtime_config.tool_limits)))
    return 3;
  zed::tools::SubagentToolConfig subagent_config;
  subagent_config.max_concurrency =
      runtime_config.subagent_execution.max_concurrency;
  subagent_config.max_aggregate_output_bytes =
      runtime_config.subagent_execution.max_aggregate_output_bytes;
  subagent_config.total_timeout = std::chrono::milliseconds(
      runtime_config.subagent_execution.total_timeout_ms);
  auto subagent_tool = std::make_unique<zed::tools::SubagentTool>(
      subagent_runner, built_in_agents, subagent_config);
  auto *subagent_tool_handle = subagent_tool.get();
  if (!tools.register_tool(std::move(subagent_tool)))
    return 3;

  zed::extensions::QuickBashInput quick_bash(tools,
                                             runtime_config.quick_bash_enabled);

  zed::skills::SkillRegistry skills;
  const auto skill_discovery =
      skills.discover({runtime_config.workspace / ".zed" / "skills"});
  if (!skill_discovery) {
    std::cerr << "skill discovery warning: " << skill_discovery.error().message
              << "\n";
  }

  std::string active_skill;
  auto active_model = runtime_config.main_model;
  auto active_reasoning_effort = runtime_config.reasoning_effort;
  if (const auto *model_info = zed::providers::find_opencode_go_model(
          model_catalog, active_model.model);
      model_info != nullptr && !zed::providers::supports_reasoning_effort(
                                   *model_info, active_reasoning_effort)) {
    active_reasoning_effort = zed::core::ReasoningEffort::automatic;
  }
  auto active_context_limits = model_context_limits(
      runtime_config.context_limits, zed::providers::find_opencode_go_model(
                                         model_catalog, active_model.model));
  auto active_theme =
      *zed::ui::theme_kind_from_name(runtime_config.terminal_theme);

  zed::core::AgentLoopConfig loop_config;
  loop_config.model_request.model = active_model;
  loop_config.model_request.temperature = runtime_config.main_temperature;
  loop_config.model_request.reasoning_effort = active_reasoning_effort;
  if (runtime_config.main_max_output_tokens > 0) {
    loop_config.model_request.max_output_tokens =
        runtime_config.main_max_output_tokens;
  }
  loop_config.context_limits = active_context_limits;
  loop_config.max_turns = runtime_config.max_turns;
  loop_config.system_prompt = runtime_config.system_prompt;

  zed::core::AgentLoop loop(model, tools, session, context, loop_config);
  zed::extensions::ExtensionRegistry extensions;
  zed::plugins::PluginManager plugins(
      {runtime_config.workspace, zed::plugins::default_plugin_search_paths(),
       active_model, active_reasoning_effort},
      extensions, tools, model, clangd);
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
      }))
    return 3;
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
      }))
    return 3;
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
      }))
    return 3;
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
      }))
    return 3;
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
      }))
    return 3;
  if (!register_command(zed::extensions::Command{
          .name = "model",
          .description = "Show, set, or refresh OpenCode Go models: /model "
                         "<id|list|refresh>.",
          .options = std::move(model_options),
          .execute_with_events =
              [&](std::string_view arguments,
                  zed::core::CancellationToken cancellation,
                  zed::core::AgentEventCallback) {
                const auto describe = [&](const auto &model_info) {
                  std::string result =
                      model_info.id + " — " + model_info.name + " [" +
                      std::string(zed::providers::open_code_protocol_name(
                          model_info.protocol)) +
                      "]";
                  if (!model_info.reasoning_efforts.empty()) {
                    result += " — reasoning: ";
                    for (std::size_t index = 0;
                         index < model_info.reasoning_efforts.size(); ++index) {
                      if (index > 0)
                        result += ",";
                      result += zed::core::reasoning_effort_name(
                          model_info.reasoning_efforts[index]);
                    }
                  }
                  return result;
                };

                std::string requested = trim_ascii_whitespace(arguments);
                if (requested == "refresh") {
                  const auto discovered =
                      zed::providers::discover_opencode_go_models(
                          runtime_config.opencode_path, 5'000, cancellation);
                  if (!discovered) {
                    return zed::core::Result<std::string>::failure(
                        discovered.error());
                  }
                  model_catalog = discovered.value();
                  model.set_models(model_catalog);
                  built_in_agents = zed::subagents::configured_agents(
                      model_catalog, runtime_config.subagents);
                  subagent_tool_handle->set_agents(built_in_agents);

                  std::string result = "refreshed OpenCode Go models: " +
                                       std::to_string(model_catalog.size()) +
                                       "\n";
                  const auto *active_info =
                      zed::providers::find_opencode_go_model(
                          model_catalog, active_model.model);
                  if (active_info == nullptr) {
                    result += "warning: active model is no longer available: " +
                              active_model.model + "\n";
                  } else {
                    active_context_limits = model_context_limits(
                        runtime_config.context_limits, active_info);
                    loop.set_context_limits(active_context_limits);
                    if (!zed::providers::supports_reasoning_effort(
                            *active_info, active_reasoning_effort)) {
                      active_reasoning_effort =
                          zed::core::ReasoningEffort::automatic;
                      loop.set_reasoning_effort(active_reasoning_effort);
                      result += "reasoning reset to auto\n";
                    }
                  }
                  if (zed::providers::find_opencode_go_model(
                          model_catalog, runtime_config.context_model.model) ==
                      nullptr) {
                    result +=
                        "warning: context model is no longer available: " +
                        runtime_config.context_model.model + "\n";
                  }
                  result +=
                      "use /model list to inspect the refreshed catalog\n";
                  return zed::core::Result<std::string>::success(
                      std::move(result));
                }
                if (requested == "list") {
                  std::string result;
                  for (const auto &model_info : model_catalog) {
                    result +=
                        (model_info.id == active_model.model ? "* " : "  ") +
                        describe(model_info) + "\n";
                  }
                  return zed::core::Result<std::string>::success(
                      std::move(result));
                }
                if (requested.empty()) {
                  const auto *model_info =
                      zed::providers::find_opencode_go_model(
                          model_catalog, active_model.model);
                  std::string result = "main: " + active_model.model + "\n";
                  if (model_info != nullptr)
                    result += describe(*model_info) + "\n";
                  result += "context: " + runtime_config.context_model.model +
                            "\nusage: /model <id|list|refresh>\n";
                  return zed::core::Result<std::string>::success(
                      std::move(result));
                }
                constexpr std::string_view kProviderPrefix = "opencode-go/";
                if (requested.starts_with(kProviderPrefix))
                  requested.erase(0, kProviderPrefix.size());
                const auto *model_info = zed::providers::find_opencode_go_model(
                    model_catalog, requested);
                if (model_info == nullptr) {
                  return zed::core::Result<std::string>::failure({
                      zed::core::ErrorCode::not_found,
                      "OpenCode Go model not found: " + requested +
                          "; use /model list or /model refresh",
                  });
                }

                active_model.model = requested;
                loop.set_model(active_model);
                active_context_limits = model_context_limits(
                    runtime_config.context_limits, model_info);
                loop.set_context_limits(active_context_limits);
                if (!zed::providers::supports_reasoning_effort(
                        *model_info, active_reasoning_effort)) {
                  active_reasoning_effort =
                      zed::core::ReasoningEffort::automatic;
                  loop.set_reasoning_effort(active_reasoning_effort);
                }
                return zed::core::Result<std::string>::success(
                    "model: " + describe(*model_info) + "\nreasoning: " +
                    std::string(zed::core::reasoning_effort_name(
                        active_reasoning_effort)) +
                    "\n");
              },
      }))
    return 3;
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
      }))
    return 3;
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
      }))
    return 3;
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
      }))
    return 3;
  if (!register_command({
          "session",
          "Manage Session v2: /session [list|new|open|rename|fork].",
          [&](std::string_view arguments) {
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
                return zed::core::Result<std::string>::failure(
                    selected.error());
              const auto switched = session.switch_to(selected.value().path);
              if (!switched)
                return zed::core::Result<std::string>::failure(
                    switched.error());
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
              const auto path =
                  zed::session::new_session_path(session_directory);
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
                return zed::core::Result<std::string>::failure(
                    switched.error());
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
              const auto path =
                  zed::session::new_session_path(session_directory);
              const auto forked = session.fork_to(path, remainder);
              if (!forked)
                return zed::core::Result<std::string>::failure(forked.error());
              const auto switched = session.switch_to(path);
              if (!switched)
                return zed::core::Result<std::string>::failure(
                    switched.error());
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

            const auto sessions =
                zed::session::list_sessions(session_directory);
            if (!sessions)
              return zed::core::Result<std::string>::failure(sessions.error());
            const auto active_info = session.inspect();
            if (!active_info)
              return zed::core::Result<std::string>::failure(
                  active_info.error());

            std::string result =
                "active: " + active_info.value().metadata.title + " [" +
                active_info.value().metadata.id + "]\n";
            result += "path: " + session.path().string() + "\n";
            result +=
                "turns: " + std::to_string(active_info.value().turn_count) +
                ", messages: " +
                std::to_string(active_info.value().message_count) + "\n";
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
          std::move(session_options),
      }))
    return 3;
  if (!register_command({
          "plugins",
          "Show discovered external plugins.",
          [&](std::string_view) {
            return zed::core::Result<std::string>::success(
                plugins.status_report());
          },
      }))
    return 3;
  const auto plugins_started_at = std::chrono::steady_clock::now();
  const auto plugin_discovery = plugins.discover_and_load();
  if (!plugin_discovery) {
    std::cerr << "plugin discovery warning: "
              << plugin_discovery.error().message << "\n";
  }
  for (const auto &status : plugins.statuses()) {
    if (!status.loaded) {
      std::cerr << "plugin warning: " << status.detail << " ("
                << status.manifest_path.string() << ")\n";
    }
  }
  const auto plugins_ready_at = std::chrono::steady_clock::now();
  const auto command_handler = [&](std::string_view name,
                                   std::string_view arguments,
                                   zed::core::CancellationToken cancellation,
                                   zed::core::AgentEventCallback on_event) {
    return extensions.execute(name, arguments, cancellation,
                              std::move(on_event));
  };
  const auto submit_handler = [&](std::string prompt,
                                  zed::core::CancellationToken cancellation,
                                  zed::core::AgentEventCallback on_event) {
    const auto quick_command = quick_bash.classify(prompt);
    if (!quick_command)
      return zed::core::Result<std::string>::failure(quick_command.error());
    if (quick_command.value().has_value()) {
      if (on_event) {
        on_event({zed::core::AgentEventType::tool_start,
                  std::string(zed::extensions::quick_bash_purpose())});
      }
      const auto execution =
          quick_bash.execute(*quick_command.value(), cancellation);
      if (!execution)
        return zed::core::Result<std::string>::failure(execution.error());
      const std::string visible_output = execution.value().content.empty()
                                             ? "(no output)"
                                             : execution.value().content;
      if (on_event) {
        on_event({zed::core::AgentEventType::tool_result, visible_output,
                  std::nullopt, execution.value()});
        on_event({zed::core::AgentEventType::agent_end});
      }
      return zed::core::Result<std::string>::success(execution.value().content);
    }

    const auto skill_context = skills.prompt_context(active_skill);
    return loop.run(std::move(prompt), cancellation, std::move(on_event),
                    skill_context);
  };
  const auto initial_activity = [&](std::string_view input) {
    const auto quick_command = quick_bash.classify(input);
    if (!quick_command || quick_command.value().has_value())
      return zed::ui::TerminalActivity::action;
    return zed::ui::TerminalActivity::thinking;
  };
  std::vector<zed::ui::TerminalCommandHint> command_hints;
  command_hints.reserve(extensions.commands().size() + 1);
  for (const auto &command : extensions.commands()) {
    std::vector<zed::ui::TerminalCommandOption> options;
    options.reserve(command.options.size());
    for (const auto &option : command.options) {
      options.push_back({option.value, option.description});
    }
    command_hints.push_back(
        {command.name, command.description, std::move(options)});
  }
  command_hints.push_back({"exit", "Quit zeda.", {}});
  const auto active_session_label = [&] {
    const auto info = session.inspect();
    if (!info)
      return session.path().stem().string();
    return info.value().metadata.title + " [" + info.value().metadata.id + "]" +
           (info.value().has_interrupted_turn ? " — interrupted" : "");
  };
  const auto startup_ready_at = std::chrono::steady_clock::now();
  const auto elapsed = [](auto begin, auto end) {
    return std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
  };
  const zed::ui::TerminalStartupTiming startup{
      elapsed(startup_started_at, config_ready_at),
      elapsed(config_ready_at, core_ready_at),
      elapsed(core_ready_at, session_ready_at),
      elapsed(session_ready_at, plugins_started_at),
      elapsed(plugins_started_at, plugins_ready_at),
      elapsed(plugins_ready_at, startup_ready_at),
  };

  if (isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0) {
    zed::ui::TerminalApplication application(
        runtime_config.workspace.string(), active_model.model,
        std::string(kVersion), startup,
        active_context_limits.max_context_tokens, active_reasoning_effort,
        active_theme, [&] { return quick_bash.enabled(); },
        active_session_label, [&] { return session.load(); }, initial_activity,
        std::move(command_hints), submit_handler, command_handler);
    const auto result = application.run();
    if (!result) {
      std::cerr << result.error().message << "\n";
      return 4;
    }
    return 0;
  }

  zed::ui::TerminalRenderer renderer(
      std::cout, {isatty(STDOUT_FILENO) != 0, active_theme});
  renderer.banner(runtime_config.workspace.string(), active_model.model,
                  kVersion, startup, active_reasoning_effort);
  zed::ui::TerminalInput input_reader(std::cin);

  while (true) {
    renderer.prompt();
    const auto line = input_reader.read_line();
    if (!line)
      break;
    if (line.value() == "/exit")
      break;
    if (line.value().empty())
      continue;

    if (line.value().starts_with('/')) {
      const auto separator = line.value().find(' ');
      const std::string command = line.value().substr(
          1,
          separator == std::string::npos ? std::string::npos : separator - 1);
      const std::string arguments = separator == std::string::npos
                                        ? ""
                                        : line.value().substr(separator + 1);
      const auto command_result = extensions.execute(command, arguments);
      renderer.set_theme(active_theme);
      if (!command_result)
        renderer.error(command_result.error().message);
      else
        std::cout << command_result.value() << std::flush;
      continue;
    }

    const auto result = submit_handler(
        line.value(), {},
        [&](const zed::core::AgentEvent &event) { renderer.render(event); });
    if (!result)
      renderer.error(result.error().message);
  }

  return 0;
}
