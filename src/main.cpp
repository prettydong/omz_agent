#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "zed/app/config.hpp"
#include "zed/core/agent_loop.hpp"
#include "zed/core/model_context_controller.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/extensions/quick_bash_input.hpp"
#include "zed/providers/opencode_go_model.hpp"
#include "zed/session/jsonl_session_store.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/ui/terminal.hpp"

namespace {

constexpr std::string_view kVersion = ZEDA_VERSION;

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
  if (argc > 1) {
    const std::string_view argument(argv[1]);
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
  const std::string session_name = runtime_config.session_path.stem().string();

  zed::providers::OpenCodeGoModel model({
      runtime_config.opencode_go_api_key,
      runtime_config.opencode_endpoint,
      runtime_config.opencode_request_timeout_ms,
  });
  zed::core::ModelBackedContextController context_controller(
      model, runtime_config.context_model);
  zed::core::ApproximateTokenEstimator estimator;
  zed::core::BasicContextManager context(estimator, &context_controller);
  zed::session::JsonlSessionStore session(runtime_config.session_path);

  zed::core::ToolRegistry tools;
  if (!tools.register_tool(std::make_unique<zed::tools::ReadFileTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::WriteFileTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::BashTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::GrepTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
    return 3;
  if (!tools.register_tool(std::make_unique<zed::tools::EditFileTool>(
          runtime_config.workspace, runtime_config.tool_limits)))
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
  auto active_reasoning_effort = runtime_config.reasoning_effort;
  auto active_theme =
      *zed::ui::theme_kind_from_name(runtime_config.terminal_theme);

  zed::core::AgentLoopConfig loop_config;
  loop_config.model_request.model = runtime_config.main_model;
  loop_config.model_request.temperature = 0.0;
  loop_config.model_request.reasoning_effort = active_reasoning_effort;
  loop_config.context_limits = runtime_config.context_limits;
  loop_config.max_turns = runtime_config.max_turns;

  zed::core::AgentLoop loop(model, tools, session, context, loop_config);
  zed::extensions::ExtensionRegistry extensions;
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
  if (!register_command({
          "model",
          "Show the active models.",
          [&](std::string_view) {
            return zed::core::Result<std::string>::success(
                "main: " + runtime_config.main_model.model +
                "\ncontext: " + runtime_config.context_model.model + "\n");
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
          "Show or set reasoning: /reasoning <none|low|medium|high>.",
          [&](std::string_view arguments) {
            if (arguments.empty()) {
              return zed::core::Result<std::string>::success(
                  "reasoning: " +
                  std::string(zed::core::reasoning_effort_name(
                      active_reasoning_effort)) +
                  "\nusage: /reasoning <none|low|medium|high>\n");
            }
            const auto effort =
                zed::core::reasoning_effort_from_name(arguments);
            if (!effort.has_value()) {
              return zed::core::Result<std::string>::failure({
                  zed::core::ErrorCode::invalid_argument,
                  "reasoning effort must be one of: none, low, medium, high",
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
              {"none", "Disable model reasoning."},
              {"low", "Use fast, lightweight reasoning."},
              {"medium", "Use balanced reasoning."},
              {"high", "Use deeper, slower reasoning."},
          },
      }))
    return 3;
  if (!register_command({
          "session",
          "Show the active session.",
          [&](std::string_view) {
            return zed::core::Result<std::string>::success(
                runtime_config.session_path.string() + "\n");
          },
      }))
    return 3;
  const auto command_handler = [&](std::string_view name,
                                   std::string_view arguments) {
    return extensions.execute(name, arguments);
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
    if (!skill_context.empty()) {
      prompt = skill_context + "\n\nUser request:\n" + prompt;
    }
    return loop.run(std::move(prompt), cancellation, std::move(on_event));
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

  if (isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0) {
    zed::ui::TerminalApplication application(
        runtime_config.workspace.string(), runtime_config.main_model.model,
        std::string(kVersion), active_reasoning_effort, active_theme,
        [&] { return quick_bash.enabled(); }, initial_activity,
        std::move(command_hints), session_name, submit_handler,
        command_handler);
    const auto result = application.run();
    if (!result) {
      std::cerr << result.error().message << "\n";
      return 4;
    }
    return 0;
  }

  zed::ui::TerminalRenderer renderer(
      std::cout, {isatty(STDOUT_FILENO) != 0, active_theme});
  renderer.banner(runtime_config.workspace.string(),
                  runtime_config.main_model.model, kVersion, session_name,
                  quick_bash.enabled());
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
