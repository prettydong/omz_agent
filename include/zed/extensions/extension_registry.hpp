#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

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
};

class ExtensionRegistry {
public:
  core::Result<void> register_command(Command command);

  [[nodiscard]] const std::vector<Command> &commands() const {
    return commands_;
  }

  core::Result<std::string> execute(std::string_view name,
                                    std::string_view arguments) const;

private:
  std::vector<Command> commands_;
};

} // namespace zed::extensions
