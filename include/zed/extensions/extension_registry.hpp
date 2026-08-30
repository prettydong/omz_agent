#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/agent_event.hpp"
#include "zed/core/cancellation.hpp"
#include "zed/core/result.hpp"

namespace zed::extensions {

struct CommandOption {
  std::string value;
  std::string description;
};

struct Command {
  std::string name;
  std::string description;
  std::function<core::Result<std::string>(std::string_view)> execute;
  std::vector<CommandOption> options;
  std::function<core::Result<std::string>(
      std::string_view, core::CancellationToken, core::AgentEventCallback)>
      execute_with_events;
};

class ExtensionRegistry {
public:
  [[nodiscard]] core::Result<void>
  validate_command(const Command &command) const;
  core::Result<void> register_command(Command command);
  bool unregister_command(std::string_view name);

  [[nodiscard]] const std::vector<Command> &commands() const {
    return commands_;
  }

  core::Result<std::string>
  execute(std::string_view name, std::string_view arguments,
          core::CancellationToken cancellation = {},
          core::AgentEventCallback on_event = {}) const;

private:
  std::vector<Command> commands_;
};

} // namespace zed::extensions
