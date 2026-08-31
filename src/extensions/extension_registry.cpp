#include "zed/extensions/extension_registry.hpp"

#include <algorithm>
#include <cstddef>

namespace zed::extensions {

core::Result<void>
ExtensionRegistry::validate_command(const Command &command) const {
  if (command.name.empty() ||
      (!command.execute && !command.execute_with_events)) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "extension command needs a name and handler",
    });
  }
  for (std::size_t index = 0; index < command.options.size(); ++index) {
    const auto &option = command.options[index];
    if (option.value.empty()) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "extension command option needs a value: " + command.name,
      });
    }
    const auto duplicate_option = std::find_if(
        command.options.begin(),
        command.options.begin() + static_cast<std::ptrdiff_t>(index),
        [&](const CommandOption &existing) {
          return existing.value == option.value;
        });
    if (duplicate_option !=
        command.options.begin() + static_cast<std::ptrdiff_t>(index)) {
      return core::Result<void>::failure({
          core::ErrorCode::conflict,
          "extension command option already registered: " + command.name + " " +
              option.value,
      });
    }
  }
  std::scoped_lock lock(mutex_);
  const auto duplicate = std::find_if(
      commands_.begin(), commands_.end(),
      [&](const Command &existing) { return existing.name == command.name; });
  if (duplicate != commands_.end()) {
    return core::Result<void>::failure({
        core::ErrorCode::conflict,
        "extension command already registered: " + command.name,
    });
  }
  return core::Result<void>::success();
}

core::Result<void> ExtensionRegistry::register_command(Command command) {
  const auto validation = validate_command(command);
  if (!validation)
    return validation;

  std::scoped_lock lock(mutex_);
  const auto duplicate = std::find_if(
      commands_.begin(), commands_.end(),
      [&](const Command &existing) { return existing.name == command.name; });
  if (duplicate != commands_.end()) {
    return core::Result<void>::failure({
        core::ErrorCode::conflict,
        "extension command already registered: " + command.name,
    });
  }
  commands_.push_back(std::move(command));
  return core::Result<void>::success();
}

bool ExtensionRegistry::unregister_command(std::string_view name) {
  std::scoped_lock lock(mutex_);
  const auto iterator = std::find_if(
      commands_.begin(), commands_.end(),
      [&](const Command &command) { return command.name == name; });
  if (iterator == commands_.end())
    return false;
  commands_.erase(iterator);
  return true;
}

std::vector<Command> ExtensionRegistry::commands_snapshot() const {
  std::scoped_lock lock(mutex_);
  return commands_;
}

core::Result<std::string>
ExtensionRegistry::execute(std::string_view name, std::string_view arguments,
                           core::CancellationToken cancellation,
                           core::AgentEventCallback on_event) const {
  Command handlers;
  {
    std::scoped_lock lock(mutex_);
    const auto iterator = std::find_if(
        commands_.begin(), commands_.end(),
        [&](const Command &command) { return command.name == name; });
    if (iterator == commands_.end()) {
      return core::Result<std::string>::failure({
          core::ErrorCode::not_found,
          "extension command not found: " + std::string(name),
      });
    }
    handlers.execute = iterator->execute;
    handlers.execute_with_events = iterator->execute_with_events;
  }
  if (handlers.execute_with_events)
    return handlers.execute_with_events(arguments, cancellation,
                                        std::move(on_event));
  return handlers.execute(arguments);
}

} // namespace zed::extensions
