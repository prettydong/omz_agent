#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "zed/core/session_store.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/extensions/quick_bash_input.hpp"
#include "zed/tools/basic_tools.hpp"

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("zed-quick-bash-" + std::to_string(getpid()));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);

  zed::core::ToolRegistry tools;
  zed::tools::ToolLimits limits;
  limits.max_command_output_bytes = 256;
  limits.command_timeout_ms = 1'000;
  const auto registered =
      tools.register_tool(std::make_unique<zed::tools::BashTool>(root, limits));
  assert(registered);

  zed::extensions::QuickBashInput quick_bash(tools);
  assert(quick_bash.enabled());
  quick_bash.set_enabled(false);
  const auto disabled = quick_bash.classify("pwd");
  assert(disabled);
  assert(!disabled.value().has_value());

  quick_bash.set_enabled(true);
  assert(quick_bash.enabled());
  constexpr std::array<std::string_view, 8> allowed{
      "pwd", "ls", "ps", "pgrep", "kill", "pkill", "which", "whoami",
  };
  for (const auto command : allowed) {
    const auto classified = quick_bash.classify(command);
    assert(classified);
    assert(classified.value().has_value());
  }

  for (const auto command :
       {"ls -la", "ls \"file name\"", "ls 'a|b'", "ls escaped\\ name"}) {
    const auto classified = quick_bash.classify(command);
    assert(classified);
    assert(classified.value().has_value());
  }

  for (const auto input :
       {"cd ..", "hello world", "git status", "hello; pwd"}) {
    const auto classified = quick_bash.classify(input);
    assert(classified);
    assert(!classified.value().has_value());
  }

  for (const auto input :
       {"ls | pwd", "ls > output", "pwd; ls", "ls && pwd", "ls || pwd",
        "ls $(pwd)", "ls `pwd`", "ls\n", "ls \"unterminated"}) {
    const auto classified = quick_bash.classify(input);
    assert(!classified);
    assert(classified.error().code == zed::core::ErrorCode::invalid_argument);
  }

  {
    std::ofstream fixture(root / "visible-file.txt");
    fixture << "fixture";
  }
  const auto pwd = quick_bash.execute("pwd");
  assert(pwd);
  assert(pwd.value().content.find(root.string()) != std::string::npos);
  const auto ls = quick_bash.execute("ls");
  assert(ls);
  assert(ls.value().content.find("visible-file.txt") != std::string::npos);

  zed::core::InMemorySessionStore session;
  const auto untouched_session = session.load();
  assert(untouched_session);
  assert(untouched_session.value().empty());

  for (int index = 0; index < 40; ++index) {
    std::ofstream fixture(
        root / ("long-fixture-name-" + std::to_string(index) + ".txt"));
    fixture << index;
  }
  const auto truncated = quick_bash.execute("ls");
  assert(truncated);
  assert(truncated.value().content.find("[output truncated]") !=
         std::string::npos);

  zed::core::CancellationSource cancellation;
  cancellation.cancel();
  const auto cancelled = quick_bash.execute("pwd", cancellation.token());
  assert(!cancelled);
  assert(cancelled.error().code == zed::core::ErrorCode::cancelled);

  quick_bash.set_enabled(false);
  const auto disabled_execution = quick_bash.execute("pwd");
  assert(!disabled_execution);

  std::filesystem::remove_all(root);
  return 0;
}
