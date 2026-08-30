#include <array>
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

#include "builtin_commands.hpp"
#include "zed/app/application.hpp"
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
#include "zed/skills/skill_registry.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/subagents/subagent_runner.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/tools/clangd_tool.hpp"
#include "zed/tools/multi_bash_tool.hpp"
#include "zed/tools/subagent_tool.hpp"
#include "zed/ui/terminal.hpp"

namespace {

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

zed::core::Result<zed::tools::SubagentTool *>
register_builtin_tools(zed::core::ToolRegistry &tools,
                       const zed::app::RuntimeConfig &config,
                       zed::lsp::ClangdClient &clangd,
                       zed::subagents::ProcessSubagentRunner &subagent_runner,
                       std::vector<zed::subagents::AgentDefinition> &agents) {
  const auto add = [&](std::unique_ptr<zed::core::Tool> tool) {
    return tools.register_tool(std::move(tool));
  };
  const auto &root = config.workspace;
  const auto &limits = config.tool_limits;
  std::array<std::unique_ptr<zed::core::Tool>, 9> builtin_tools{
      std::make_unique<zed::tools::ReadFileTool>(root, limits),
      std::make_unique<zed::tools::WriteFileTool>(root, limits, &clangd),
      std::make_unique<zed::tools::BashTool>(root, limits),
      std::make_unique<zed::tools::MultiBashTool>(root, limits),
      std::make_unique<zed::tools::GrepTool>(root, limits),
      std::make_unique<zed::tools::FindFilesTool>(root, limits),
      std::make_unique<zed::tools::ListDirectoryTool>(root, limits),
      std::make_unique<zed::tools::EditFileTool>(root, limits, &clangd),
      std::make_unique<zed::tools::ClangdTool>(root, clangd, limits),
  };
  for (auto &tool : builtin_tools) {
    const auto registered = add(std::move(tool));
    if (!registered)
      return zed::core::Result<zed::tools::SubagentTool *>::failure(
          registered.error());
  }

  zed::tools::SubagentToolConfig subagent_config;
  subagent_config.max_concurrency = config.subagent_execution.max_concurrency;
  subagent_config.max_aggregate_output_bytes =
      config.subagent_execution.max_aggregate_output_bytes;
  subagent_config.total_timeout =
      std::chrono::milliseconds(config.subagent_execution.total_timeout_ms);
  auto subagent_tool = std::make_unique<zed::tools::SubagentTool>(
      subagent_runner, agents, subagent_config);
  auto *handle = subagent_tool.get();
  const auto registered = add(std::move(subagent_tool));
  if (!registered) {
    return zed::core::Result<zed::tools::SubagentTool *>::failure(
        registered.error());
  }
  return zed::core::Result<zed::tools::SubagentTool *>::success(handle);
}

int run_user_interface(
    std::string workspace, std::string &model, std::string version,
    const zed::ui::TerminalStartupTiming &startup,
    zed::core::TokenCount &max_context_tokens,
    zed::core::ReasoningEffort &reasoning_effort, zed::ui::ThemeKind &theme,
    zed::ui::TerminalApplication::QuickBashState quick_bash,
    zed::ui::TerminalApplication::SessionNameState session_name,
    zed::ui::TerminalApplication::SessionLoader session_loader,
    zed::ui::TerminalApplication::InitialActivity initial_activity,
    std::vector<zed::ui::TerminalCommandHint> command_hints,
    zed::ui::TerminalApplication::SubmitHandler submit,
    zed::ui::TerminalApplication::CommandHandler command) {
  if (isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0) {
    zed::ui::TerminalApplication application(
        std::move(workspace), model, std::move(version), startup,
        max_context_tokens, reasoning_effort, theme, std::move(quick_bash),
        std::move(session_name), std::move(session_loader),
        std::move(initial_activity), std::move(command_hints),
        std::move(submit), std::move(command));
    const auto result = application.run();
    if (!result) {
      std::cerr << result.error().message << '\n';
      return 4;
    }
    return 0;
  }

  zed::ui::TerminalRenderer renderer(std::cout,
                                     {isatty(STDOUT_FILENO) != 0, theme});
  renderer.banner(workspace, model, version, startup, reasoning_effort);
  zed::ui::TerminalInput input_reader(std::cin);
  while (true) {
    renderer.prompt();
    const auto line = input_reader.read_line();
    if (!line || line.value() == "/exit")
      break;
    if (line.value().empty())
      continue;

    if (line.value().starts_with('/')) {
      const auto parsed = zed::ui::parse_terminal_command(line.value());
      const auto result = command(parsed.name, parsed.arguments, {}, {});
      renderer.set_theme(theme);
      if (!result)
        renderer.error(result.error().message);
      else
        std::cout << result.value() << std::flush;
      continue;
    }

    const auto result =
        submit(line.value(), {}, [&](const zed::core::AgentEvent &event) {
          renderer.render(event);
        });
    if (!result)
      renderer.error(result.error().message);
  }
  return 0;
}

} // namespace

namespace zed::app {

int run_application(std::string_view executable, std::string_view version) {
  const auto startup_started_at = std::chrono::steady_clock::now();
  const auto runtime = load_runtime_config();
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
      subagent_worker_executable(executable),
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
        {},
        {},
        {},
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
  const auto registered_tools = register_builtin_tools(
      tools, runtime_config, clangd, subagent_runner, built_in_agents);
  if (!registered_tools) {
    std::cerr << "tool registration failed: "
              << registered_tools.error().message << '\n';
    return 3;
  }
  auto *subagent_tool_handle = registered_tools.value();

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
  const auto *active_model_info =
      zed::providers::find_opencode_go_model(model_catalog, active_model.model);
  auto active_context_limits = zed::core::cap_context_limits(
      runtime_config.context_limits,
      active_model_info == nullptr ? 0 : active_model_info->max_context_tokens);
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
  BuiltinCommandRegistrar command_registrar(
      extensions, runtime_config, model_catalog, built_in_agents,
      subagent_tool_handle, tools, configure_web, skills, active_skill,
      active_model, active_reasoning_effort, active_context_limits,
      active_theme, loop, model, quick_bash, session_directory, session,
      session_metadata, plugins);
  if (!command_registrar.register_commands())
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

  return run_user_interface(
      runtime_config.workspace.string(), active_model.model,
      std::string(version), startup, active_context_limits.max_context_tokens,
      active_reasoning_effort, active_theme,
      [&] { return quick_bash.enabled(); }, active_session_label,
      [&] { return session.load(); }, initial_activity,
      std::move(command_hints), submit_handler, command_handler);
}

} // namespace zed::app
