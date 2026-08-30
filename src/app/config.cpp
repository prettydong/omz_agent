#include "zed/app/config.hpp"

#include "zed/core/default_system_prompt.hpp"
#include "zed/core/model_context_controller.hpp"
#include "zed/core/utf8.hpp"
#include "zed/session/session_catalog.hpp"
#include "zed/support/atomic_file.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>

#include <nlohmann/json.hpp>

namespace zed::app {

namespace {

constexpr std::string_view kSystemPromptRelativePath =
    ".zed/zed_system_propmt.md";
constexpr std::string_view kExplorerSystemPromptRelativePath =
    ".zed/explorer_system_prompt.md";
constexpr std::string_view kContextSystemPromptRelativePath =
    ".zed/context_system_prompt.md";
constexpr std::string_view kAgentManagementRelativePath =
    ".zed/agent_management.json";
constexpr std::uintmax_t kMaxWorkspaceConfigBytes = 64 * 1024;
constexpr std::uintmax_t kMaxAgentManagementBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumManagedAgents = 16;
constexpr std::size_t kMaximumAgentTools = 128;

using Json = nlohmann::json;

core::Error invalid_config(std::string message) {
  return {core::ErrorCode::invalid_argument,
          "invalid workspace configuration: " + std::move(message)};
}

core::Result<void> validate_fields(std::string_view context, const Json &object,
                                   std::span<const std::string_view> allowed,
                                   std::span<const std::string_view> required) {
  if (!object.is_object()) {
    return core::Result<void>::failure(
        invalid_config(std::string(context) + " must be an object"));
  }
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (std::find(allowed.begin(), allowed.end(), iterator.key()) ==
        allowed.end()) {
      return core::Result<void>::failure(
          invalid_config(std::string(context) + " contains unknown field '" +
                         iterator.key() + "'"));
    }
  }
  for (const auto field : required) {
    if (!object.contains(field)) {
      return core::Result<void>::failure(
          invalid_config(std::string(context) + " is missing field '" +
                         std::string(field) + "'"));
    }
  }
  return core::Result<void>::success();
}

core::Result<void> exact_fields(std::string_view context, const Json &object,
                                std::span<const std::string_view> expected) {
  return validate_fields(context, object, expected, expected);
}

core::Result<std::string> config_string(const Json &object,
                                        std::string_view field,
                                        std::string_view context) {
  const auto iterator = object.find(field);
  if (iterator == object.end() || !iterator->is_string() || iterator->empty()) {
    return core::Result<std::string>::failure(
        invalid_config(std::string(context) + "." + std::string(field) +
                       " must be a non-empty string"));
  }
  auto value = iterator->get<std::string>();
  if (value.size() > 256 || !core::is_valid_utf8(value) ||
      std::any_of(value.begin(), value.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      })) {
    return core::Result<std::string>::failure(invalid_config(
        std::string(context) + "." + std::string(field) +
        " must be valid UTF-8 without control characters and at most 256 "
        "bytes"));
  }
  return core::Result<std::string>::success(std::move(value));
}

core::Result<std::size_t> config_size(const Json &object,
                                      std::string_view field,
                                      std::string_view context,
                                      std::size_t maximum,
                                      bool allow_zero = false) {
  const auto iterator = object.find(field);
  if (iterator == object.end() || !iterator->is_number_unsigned()) {
    return core::Result<std::size_t>::failure(
        invalid_config(std::string(context) + "." + std::string(field) +
                       " must be an unsigned integer"));
  }
  const auto value = iterator->get<std::uint64_t>();
  if ((!allow_zero && value == 0) || value > maximum) {
    return core::Result<std::size_t>::failure(invalid_config(
        std::string(context) + "." + std::string(field) + " must be " +
        (allow_zero ? "between 0 and " : "between 1 and ") +
        std::to_string(maximum)));
  }
  return core::Result<std::size_t>::success(static_cast<std::size_t>(value));
}

core::Result<double> config_double(const Json &object, std::string_view field,
                                   std::string_view context, double minimum,
                                   double maximum) {
  const auto iterator = object.find(field);
  if (iterator == object.end() || !iterator->is_number()) {
    return core::Result<double>::failure(invalid_config(
        std::string(context) + "." + std::string(field) + " must be a number"));
  }
  const auto value = iterator->get<double>();
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    return core::Result<double>::failure(invalid_config(
        std::string(context) + "." + std::string(field) + " must be between " +
        std::to_string(minimum) + " and " + std::to_string(maximum)));
  }
  return core::Result<double>::success(value);
}

core::Result<std::string> config_text(const Json &object,
                                      std::string_view field,
                                      std::string_view context,
                                      std::size_t maximum,
                                      bool allow_empty = false) {
  const auto iterator = object.find(field);
  if (iterator == object.end() || !iterator->is_string()) {
    return core::Result<std::string>::failure(invalid_config(
        std::string(context) + "." + std::string(field) + " must be a string"));
  }
  auto value = iterator->get<std::string>();
  if ((!allow_empty && value.empty()) || value.size() > maximum ||
      !core::is_valid_utf8(value)) {
    return core::Result<std::string>::failure(
        invalid_config(std::string(context) + "." + std::string(field) +
                       " has an invalid UTF-8 value or byte length"));
  }
  return core::Result<std::string>::success(std::move(value));
}

bool valid_identifier(std::string_view value) {
  if (value.empty() || value.size() > 64 ||
      std::isalnum(static_cast<unsigned char>(value.front())) == 0) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_';
  });
}

bool valid_tool_name(std::string_view value) {
  if (value == "*")
    return true;
  if (value.empty() || value.size() > 128)
    return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_' || character == '.' || character == ':';
  });
}

core::Result<core::ReasoningEffort> config_reasoning(const Json &object,
                                                     std::string_view context) {
  const auto value = config_string(object, "reasoning", context);
  if (!value)
    return core::Result<core::ReasoningEffort>::failure(value.error());
  const auto effort = core::reasoning_effort_from_name(value.value());
  if (!effort.has_value()) {
    return core::Result<core::ReasoningEffort>::failure(invalid_config(
        std::string(context) +
        ".reasoning must be one of: auto, none, minimal, low, medium, high, "
        "xhigh, max, thinking"));
  }
  return core::Result<core::ReasoningEffort>::success(*effort);
}

core::Result<void> validate_workspace_config(const WorkspaceConfig &config) {
  if (config.version != kWorkspaceConfigVersion) {
    return core::Result<void>::failure(invalid_config(
        "version must be " + std::to_string(kWorkspaceConfigVersion)));
  }
  const auto valid_model = [](const core::ModelRef &model) {
    return model.provider == "opencode-go" && !model.model.empty() &&
           model.model.size() <= 256 && core::is_valid_utf8(model.model) &&
           std::none_of(model.model.begin(), model.model.end(),
                        [](unsigned char character) {
                          return character < 0x20 || character == 0x7f;
                        });
  };
  if (!valid_model(config.agent.model) || !valid_model(config.context.model) ||
      !valid_model(config.explorer.model)) {
    return core::Result<void>::failure(invalid_config(
        "models must be non-empty OpenCode Go model identifiers"));
  }
  if (config.agent.max_turns == 0 || config.agent.max_turns > 512 ||
      config.explorer.max_turns == 0 || config.explorer.max_turns > 512) {
    return core::Result<void>::failure(
        invalid_config("agent max_turns must be between 1 and 512"));
  }
  if (config.agent.max_output_tokens > 1'048'576) {
    return core::Result<void>::failure(invalid_config(
        "agent.max_output_tokens must be between 0 and 1048576"));
  }
  if (!std::isfinite(config.agent.temperature) ||
      config.agent.temperature < 0.0 || config.agent.temperature > 2.0) {
    return core::Result<void>::failure(
        invalid_config("agent.temperature must be between 0 and 2"));
  }
  if (config.explorer.max_output_tokens == 0 ||
      config.explorer.max_output_tokens > 1'048'576) {
    return core::Result<void>::failure(invalid_config(
        "subagents.explorer.max_output_tokens must be between 1 and "
        "1048576"));
  }
  if (config.subagent_execution.max_concurrency == 0 ||
      config.subagent_execution.max_concurrency > 8) {
    return core::Result<void>::failure(
        invalid_config("subagents.max_concurrency must be between 1 and 8"));
  }
  if (config.subagent_execution.total_timeout_ms == 0 ||
      config.subagent_execution.total_timeout_ms > 3'600'000) {
    return core::Result<void>::failure(invalid_config(
        "subagents.total_timeout_ms must be between 1 and 3600000"));
  }
  if (config.subagent_execution.max_aggregate_output_bytes == 0 ||
      config.subagent_execution.max_aggregate_output_bytes > 1'048'576) {
    return core::Result<void>::failure(invalid_config(
        "subagents.max_aggregate_output_bytes must be between 1 and 1048576"));
  }
  const auto &limits = config.context.limits;
  if (limits.max_context_tokens == 0 ||
      limits.max_context_tokens > 16'777'216 ||
      limits.reserved_output_tokens >= limits.max_context_tokens) {
    return core::Result<void>::failure(invalid_config(
        "context.max_tokens must exceed reserved_output_tokens and be at "
        "most 16777216"));
  }
  const auto available =
      limits.max_context_tokens - limits.reserved_output_tokens;
  if (limits.compaction_trigger_tokens > available) {
    return core::Result<void>::failure(invalid_config(
        "context.compaction_trigger_tokens must be zero or fit within the "
        "available context budget"));
  }
  if (config.context.max_output_tokens == 0 ||
      config.context.max_output_tokens > 1'048'576) {
    return core::Result<void>::failure(invalid_config(
        "context.max_output_tokens must be between 1 and 1048576"));
  }
  return core::Result<void>::success();
}

std::string environment_or(const char *name, std::string fallback = {}) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::move(fallback) : std::string(value);
}

std::filesystem::path opencode_auth_path() {
  const std::string configured = environment_or("ZED_OPENCODE_AUTH_PATH");
  if (!configured.empty())
    return configured;

  const std::string data_home = environment_or("XDG_DATA_HOME");
  if (!data_home.empty())
    return std::filesystem::path(data_home) / "opencode" / "auth.json";

  const std::string home = environment_or("HOME");
  if (!home.empty())
    return std::filesystem::path(home) / ".local" / "share" / "opencode" /
           "auth.json";

  return {};
}

core::Result<std::string> load_opencode_go_api_key() {
  const std::string environment_key = environment_or("OPENCODE_GO_API_KEY");
  if (!environment_key.empty())
    return core::Result<std::string>::success(environment_key);

  const std::filesystem::path auth_path = opencode_auth_path();
  if (auth_path.empty()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OPENCODE_GO_API_KEY is not set and the OpenCode credential path "
        "cannot be resolved",
    });
  }

  std::ifstream input(auth_path);
  if (!input) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OPENCODE_GO_API_KEY is not set and the OpenCode Go credential "
        "cannot be read from " +
            auth_path.string(),
    });
  }

  const nlohmann::json credentials =
      nlohmann::json::parse(input, nullptr, false);
  if (credentials.is_discarded() || !credentials.is_object()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot parse OpenCode credentials from " + auth_path.string(),
    });
  }

  const auto provider = credentials.find("opencode-go");
  if (provider == credentials.end() || !provider->is_object()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OpenCode Go credential is missing from " + auth_path.string(),
    });
  }

  const auto key = provider->find("key");
  if (key == provider->end() || !key->is_string() || key->empty()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OpenCode Go credential has no API key in " + auth_path.string(),
    });
  }

  return core::Result<std::string>::success(key->get<std::string>());
}

core::Result<std::size_t> size_environment(const char *name,
                                           std::size_t fallback,
                                           bool allow_zero = true) {
  const std::string value = environment_or(name);
  if (value.empty())
    return core::Result<std::size_t>::success(fallback);
  std::size_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return core::Result<std::size_t>::failure({
        core::ErrorCode::invalid_argument,
        std::string(name) + " must be a positive integer",
    });
  }
  if (!allow_zero && parsed == 0) {
    return core::Result<std::size_t>::failure({
        core::ErrorCode::invalid_argument,
        std::string(name) + " must be greater than zero",
    });
  }
  return core::Result<std::size_t>::success(parsed);
}

core::Result<core::ReasoningEffort>
reasoning_effort_environment(core::ReasoningEffort fallback) {
  const std::string value =
      environment_or("ZED_REASONING_EFFORT",
                     std::string(core::reasoning_effort_name(fallback)));
  const auto effort = core::reasoning_effort_from_name(value);
  if (effort.has_value())
    return core::Result<core::ReasoningEffort>::success(*effort);
  return core::Result<core::ReasoningEffort>::failure({
      core::ErrorCode::invalid_argument,
      "ZED_REASONING_EFFORT must be one of: auto, none, minimal, low, medium, "
      "high, xhigh, max, thinking",
  });
}

core::Result<bool> boolean_environment(const char *name, bool fallback) {
  const std::string value = environment_or(name);
  if (value.empty())
    return core::Result<bool>::success(fallback);
  if (value == "on" || value == "true" || value == "1")
    return core::Result<bool>::success(true);
  if (value == "off" || value == "false" || value == "0")
    return core::Result<bool>::success(false);
  return core::Result<bool>::failure({
      core::ErrorCode::invalid_argument,
      std::string(name) + " must be one of: on, off, true, false, 1, 0",
  });
}

core::Result<std::string>
load_or_install_prompt(const std::filesystem::path &path,
                       std::string_view default_prompt,
                       std::string_view description) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error == std::errc::no_such_file_or_directory ||
      (!status_error && !std::filesystem::exists(status))) {
    const auto installed = support::write_private_file_atomically(
        path, default_prompt, description);
    if (!installed)
      return core::Result<std::string>::failure(installed.error());
    return core::Result<std::string>::success(std::string(default_prompt));
  }
  if (status_error) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot inspect " + std::string(description) + " file " +
            path.string() + ": " + status_error.message(),
    });
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) +
            " path must be a regular file, not a symlink: " + path.string(),
    });
  }
  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot inspect " + std::string(description) + " size " +
            path.string() + ": " + size_error.message(),
    });
  }
  if (size > kMaximumPromptBytes) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) + " file exceeds 1 MiB: " + path.string(),
    });
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot read " + std::string(description) + " file " + path.string(),
    });
  }
  std::string prompt{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot finish reading " + std::string(description) + " file " +
            path.string(),
    });
  }
  if (prompt.starts_with("\xEF\xBB\xBF"))
    prompt.erase(0, 3);
  if (!core::is_valid_utf8(prompt)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) + " file is not valid UTF-8: " + path.string(),
    });
  }
  if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) + " file cannot be empty: " + path.string(),
    });
  }
  return core::Result<std::string>::success(std::move(prompt));
}

core::Result<void> validate_prompt(std::string_view prompt,
                                   std::string_view description) {
  if (prompt.empty() || prompt.size() > kMaximumPromptBytes) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) + " must contain between 1 byte and 1 MiB",
    });
  }
  if (!core::is_valid_utf8(prompt)) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) + " must be valid UTF-8",
    });
  }
  if (prompt.find_first_not_of(" \t\r\n") == std::string_view::npos) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        std::string(description) + " cannot be blank",
    });
  }
  return core::Result<void>::success();
}

bool valid_display_text(std::string_view value, std::size_t maximum,
                        bool allow_empty = false) {
  if ((!allow_empty && value.empty()) || value.size() > maximum ||
      !core::is_valid_utf8(value)) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

Json agent_profile_json(const AgentProfile &profile) {
  return {
      {"id", profile.id},
      {"name", profile.name},
      {"description", profile.description},
      {"model", profile.config.model.model},
      {"reasoning",
       core::reasoning_effort_name(profile.config.reasoning_effort)},
      {"max_turns", profile.config.max_turns},
      {"max_output_tokens", profile.config.max_output_tokens},
      {"temperature", profile.config.temperature},
      {"automatic_context_compaction", profile.automatic_context_compaction},
      {"compaction_trigger_tokens", profile.compaction_trigger_tokens},
      {"tools", profile.tools},
      {"system_prompt", profile.system_prompt},
  };
}

Json subagent_config_json(const subagents::ExplorerAgentConfig &config) {
  return {
      {"id", config.name},
      {"description", config.description},
      {"enabled", config.enabled},
      {"model", config.model.model},
      {"reasoning", core::reasoning_effort_name(config.reasoning_effort)},
      {"max_turns", config.max_turns},
      {"max_output_tokens", config.max_output_tokens},
      {"system_prompt", config.system_prompt},
  };
}

core::Result<AgentManagementConfig> parse_agent_management_json(
    std::string_view json_text,
    std::size_t legacy_compaction_trigger_tokens = 800'000) {
  if (json_text.empty() || json_text.size() > kMaxAgentManagementBytes ||
      !core::is_valid_utf8(json_text)) {
    return core::Result<AgentManagementConfig>::failure(invalid_config(
        "agent management file is empty, oversized, or invalid UTF-8"));
  }
  const auto root = Json::parse(json_text, nullptr, false);
  if (root.is_discarded()) {
    return core::Result<AgentManagementConfig>::failure(
        invalid_config("agent management file is not valid JSON"));
  }
  constexpr std::string_view root_fields[]{"version", "active_agent", "agents",
                                           "subagents"};
  const auto shape = exact_fields("agent_management", root, root_fields);
  if (!shape)
    return core::Result<AgentManagementConfig>::failure(shape.error());
  const auto version =
      config_size(root, "version", "agent_management", kAgentManagementVersion);
  const auto active = config_text(root, "active_agent", "agent_management", 64);
  if (!version || !active) {
    return core::Result<AgentManagementConfig>::failure(
        !version ? version.error() : active.error());
  }
  if (version.value() != 1 && version.value() != kAgentManagementVersion) {
    return core::Result<AgentManagementConfig>::failure(
        invalid_config("agent management version must be 1 or " +
                       std::to_string(kAgentManagementVersion)));
  }
  if (!root.at("agents").is_array() || !root.at("subagents").is_array() ||
      root.at("agents").empty() ||
      root.at("agents").size() > kMaximumManagedAgents ||
      root.at("subagents").size() > kMaximumManagedAgents) {
    return core::Result<AgentManagementConfig>::failure(invalid_config(
        "agent management supports 1-16 Agents and 0-16 custom Sub Agents"));
  }

  AgentManagementConfig management;
  management.version = kAgentManagementVersion;
  management.active_agent = active.value();
  constexpr std::string_view legacy_agent_fields[]{
      "id",           "name",      "description",       "model",
      "reasoning",    "max_turns", "max_output_tokens", "temperature",
      "system_prompt"};
  constexpr std::string_view agent_fields[]{"id",
                                            "name",
                                            "description",
                                            "model",
                                            "reasoning",
                                            "max_turns",
                                            "max_output_tokens",
                                            "temperature",
                                            "automatic_context_compaction",
                                            "compaction_trigger_tokens",
                                            "tools",
                                            "system_prompt"};
  for (std::size_t index = 0; index < root.at("agents").size(); ++index) {
    const auto &value = root.at("agents").at(index);
    const auto context =
        "agent_management.agents[" + std::to_string(index) + "]";
    const auto item_shape =
        version.value() == 1 ? exact_fields(context, value, legacy_agent_fields)
                             : exact_fields(context, value, agent_fields);
    if (!item_shape)
      return core::Result<AgentManagementConfig>::failure(item_shape.error());
    const auto id = config_text(value, "id", context, 64);
    const auto name = config_text(value, "name", context, 128);
    const auto description =
        config_text(value, "description", context, 1'024, true);
    const auto model = config_string(value, "model", context);
    const auto reasoning = config_reasoning(value, context);
    const auto turns = config_size(value, "max_turns", context, 512);
    const auto output =
        config_size(value, "max_output_tokens", context, 1'048'576, true);
    const auto temperature =
        config_double(value, "temperature", context, 0.0, 2.0);
    const auto prompt =
        config_text(value, "system_prompt", context, kMaximumPromptBytes);
    const core::Error *error = !id            ? &id.error()
                               : !name        ? &name.error()
                               : !description ? &description.error()
                               : !model       ? &model.error()
                               : !reasoning   ? &reasoning.error()
                               : !turns       ? &turns.error()
                               : !output      ? &output.error()
                               : !temperature ? &temperature.error()
                               : !prompt      ? &prompt.error()
                                              : nullptr;
    if (error != nullptr)
      return core::Result<AgentManagementConfig>::failure(*error);
    AgentProfile profile;
    profile.id = id.value();
    profile.name = name.value();
    profile.description = description.value();
    profile.config.model.model = model.value();
    profile.config.reasoning_effort = reasoning.value();
    profile.config.max_turns = turns.value();
    profile.config.max_output_tokens = output.value();
    profile.config.temperature = temperature.value();
    if (version.value() == 1) {
      profile.automatic_context_compaction = true;
      profile.compaction_trigger_tokens = legacy_compaction_trigger_tokens;
      profile.tools = {"*"};
    } else {
      if (!value.at("automatic_context_compaction").is_boolean() ||
          !value.at("tools").is_array()) {
        return core::Result<AgentManagementConfig>::failure(invalid_config(
            context +
            ".automatic_context_compaction must be boolean and tools must be "
            "an array"));
      }
      const auto trigger = config_size(value, "compaction_trigger_tokens",
                                       context, 16'777'216, true);
      if (!trigger)
        return core::Result<AgentManagementConfig>::failure(trigger.error());
      profile.automatic_context_compaction =
          value.at("automatic_context_compaction").get<bool>();
      profile.compaction_trigger_tokens = trigger.value();
      profile.tools.clear();
      for (const auto &tool : value.at("tools")) {
        if (!tool.is_string()) {
          return core::Result<AgentManagementConfig>::failure(
              invalid_config(context + ".tools must contain strings"));
        }
        profile.tools.push_back(tool.get<std::string>());
      }
    }
    profile.system_prompt = prompt.value();
    management.agents.push_back(std::move(profile));
  }

  constexpr std::string_view subagent_fields[]{
      "id",        "description", "enabled",           "model",
      "reasoning", "max_turns",   "max_output_tokens", "system_prompt"};
  for (std::size_t index = 0; index < root.at("subagents").size(); ++index) {
    const auto &value = root.at("subagents").at(index);
    const auto context =
        "agent_management.subagents[" + std::to_string(index) + "]";
    const auto item_shape = exact_fields(context, value, subagent_fields);
    if (!item_shape)
      return core::Result<AgentManagementConfig>::failure(item_shape.error());
    if (!value.at("enabled").is_boolean()) {
      return core::Result<AgentManagementConfig>::failure(
          invalid_config(context + ".enabled must be a boolean"));
    }
    const auto id = config_text(value, "id", context, 64);
    const auto description =
        config_text(value, "description", context, 1'024, true);
    const auto model = config_string(value, "model", context);
    const auto reasoning = config_reasoning(value, context);
    const auto turns = config_size(value, "max_turns", context, 512);
    const auto output =
        config_size(value, "max_output_tokens", context, 1'048'576);
    const auto prompt =
        config_text(value, "system_prompt", context, kMaximumPromptBytes);
    const core::Error *error = !id            ? &id.error()
                               : !description ? &description.error()
                               : !model       ? &model.error()
                               : !reasoning   ? &reasoning.error()
                               : !turns       ? &turns.error()
                               : !output      ? &output.error()
                               : !prompt      ? &prompt.error()
                                              : nullptr;
    if (error != nullptr)
      return core::Result<AgentManagementConfig>::failure(*error);
    subagents::ExplorerAgentConfig subagent;
    subagent.name = id.value();
    subagent.description = description.value();
    subagent.enabled = value.at("enabled").get<bool>();
    subagent.model.model = model.value();
    subagent.reasoning_effort = reasoning.value();
    subagent.max_turns = turns.value();
    subagent.max_output_tokens = output.value();
    subagent.system_prompt = prompt.value();
    management.subagents.push_back(std::move(subagent));
  }
  const auto valid = validate_agent_management(management);
  if (!valid)
    return core::Result<AgentManagementConfig>::failure(valid.error());
  return core::Result<AgentManagementConfig>::success(std::move(management));
}

std::string
serialize_agent_management_json(const AgentManagementConfig &management) {
  Json agents = Json::array();
  for (const auto &profile : management.agents)
    agents.push_back(agent_profile_json(profile));
  Json subagents = Json::array();
  for (const auto &config : management.subagents)
    subagents.push_back(subagent_config_json(config));
  return Json{{"version", management.version},
              {"active_agent", management.active_agent},
              {"agents", std::move(agents)},
              {"subagents", std::move(subagents)}}
             .dump(2) +
         "\n";
}

} // namespace

std::filesystem::path
workspace_config_path(const std::filesystem::path &workspace) {
  return workspace / ".zed" / "config.json";
}

std::filesystem::path
agent_management_path(const std::filesystem::path &workspace) {
  return workspace / kAgentManagementRelativePath;
}

core::Result<AgentManagementConfig>
parse_agent_management_config(std::string_view json_text) {
  return parse_agent_management_json(json_text);
}

std::string
serialize_agent_management_config(const AgentManagementConfig &management) {
  return serialize_agent_management_json(management);
}

WorkspaceConfig default_workspace_config() { return {}; }

core::Result<WorkspaceConfig>
parse_workspace_config(std::string_view json_text) {
  if (json_text.empty() || json_text.size() > kMaxWorkspaceConfigBytes) {
    return core::Result<WorkspaceConfig>::failure(
        invalid_config("file must contain between 1 byte and 64 KiB"));
  }
  if (!core::is_valid_utf8(json_text)) {
    return core::Result<WorkspaceConfig>::failure(
        invalid_config("file is not valid UTF-8"));
  }
  const auto root = Json::parse(json_text, nullptr, false);
  if (root.is_discarded()) {
    return core::Result<WorkspaceConfig>::failure(
        invalid_config("file is not valid JSON"));
  }
  constexpr std::string_view root_fields[]{"version", "agent", "subagents",
                                           "context"};
  const auto root_shape = exact_fields("root", root, root_fields);
  if (!root_shape)
    return core::Result<WorkspaceConfig>::failure(root_shape.error());

  const auto version = config_size(root, "version", "root", 1);
  if (!version)
    return core::Result<WorkspaceConfig>::failure(version.error());
  if (version.value() != kWorkspaceConfigVersion) {
    return core::Result<WorkspaceConfig>::failure(invalid_config(
        "version must be " + std::to_string(kWorkspaceConfigVersion)));
  }

  const auto &agent = root.at("agent");
  constexpr std::string_view agent_fields[]{"model", "reasoning", "max_turns",
                                            "max_output_tokens", "temperature"};
  constexpr std::string_view required_agent_fields[]{"model", "reasoning",
                                                     "max_turns"};
  const auto agent_shape =
      validate_fields("agent", agent, agent_fields, required_agent_fields);
  if (!agent_shape)
    return core::Result<WorkspaceConfig>::failure(agent_shape.error());

  const auto &subagents = root.at("subagents");
  constexpr std::string_view subagent_fields[]{"explorer", "max_concurrency",
                                               "total_timeout_ms",
                                               "max_aggregate_output_bytes"};
  constexpr std::string_view required_subagent_fields[]{"explorer"};
  const auto subagents_shape = validate_fields(
      "subagents", subagents, subagent_fields, required_subagent_fields);
  if (!subagents_shape)
    return core::Result<WorkspaceConfig>::failure(subagents_shape.error());
  const auto &explorer = subagents.at("explorer");
  constexpr std::string_view explorer_fields[]{
      "enabled", "model", "reasoning", "max_turns", "max_output_tokens"};
  const auto explorer_shape =
      exact_fields("subagents.explorer", explorer, explorer_fields);
  if (!explorer_shape)
    return core::Result<WorkspaceConfig>::failure(explorer_shape.error());
  if (!explorer.at("enabled").is_boolean()) {
    return core::Result<WorkspaceConfig>::failure(
        invalid_config("subagents.explorer.enabled must be a boolean"));
  }

  const auto &context = root.at("context");
  constexpr std::string_view context_fields[]{
      "model", "max_tokens", "reserved_output_tokens",
      "compaction_trigger_tokens", "max_output_tokens"};
  constexpr std::string_view required_context_fields[]{
      "model", "max_tokens", "reserved_output_tokens",
      "compaction_trigger_tokens"};
  const auto context_shape = validate_fields("context", context, context_fields,
                                             required_context_fields);
  if (!context_shape)
    return core::Result<WorkspaceConfig>::failure(context_shape.error());

  const auto agent_model = config_string(agent, "model", "agent");
  const auto agent_reasoning = config_reasoning(agent, "agent");
  const auto agent_turns = config_size(agent, "max_turns", "agent", 512);
  const auto agent_output =
      agent.contains("max_output_tokens")
          ? config_size(agent, "max_output_tokens", "agent", 1'048'576, true)
          : core::Result<std::size_t>::success(0);
  const auto agent_temperature =
      agent.contains("temperature")
          ? config_double(agent, "temperature", "agent", 0.0, 2.0)
          : core::Result<double>::success(0.0);
  const auto explorer_model =
      config_string(explorer, "model", "subagents.explorer");
  const auto explorer_reasoning =
      config_reasoning(explorer, "subagents.explorer");
  const auto explorer_turns =
      config_size(explorer, "max_turns", "subagents.explorer", 512);
  const auto explorer_output = config_size(explorer, "max_output_tokens",
                                           "subagents.explorer", 1'048'576);
  const auto subagent_concurrency =
      subagents.contains("max_concurrency")
          ? config_size(subagents, "max_concurrency", "subagents", 8)
          : core::Result<std::size_t>::success(4);
  const auto subagent_timeout =
      subagents.contains("total_timeout_ms")
          ? config_size(subagents, "total_timeout_ms", "subagents", 3'600'000)
          : core::Result<std::size_t>::success(600'000);
  const auto subagent_output =
      subagents.contains("max_aggregate_output_bytes")
          ? config_size(subagents, "max_aggregate_output_bytes", "subagents",
                        1'048'576)
          : core::Result<std::size_t>::success(256 * 1024);
  const auto context_model = config_string(context, "model", "context");
  const auto context_max =
      config_size(context, "max_tokens", "context", 16'777'216);
  const auto context_reserved = config_size(context, "reserved_output_tokens",
                                            "context", 16'777'216, true);
  const auto context_trigger = config_size(context, "compaction_trigger_tokens",
                                           "context", 16'777'216, true);
  const auto context_output =
      context.contains("max_output_tokens")
          ? config_size(context, "max_output_tokens", "context", 1'048'576)
          : core::Result<std::size_t>::success(1'024);
  const core::Error *error = !agent_model          ? &agent_model.error()
                             : !agent_reasoning    ? &agent_reasoning.error()
                             : !agent_turns        ? &agent_turns.error()
                             : !agent_output       ? &agent_output.error()
                             : !agent_temperature  ? &agent_temperature.error()
                             : !explorer_model     ? &explorer_model.error()
                             : !explorer_reasoning ? &explorer_reasoning.error()
                             : !explorer_turns     ? &explorer_turns.error()
                             : !explorer_output    ? &explorer_output.error()
                             : !subagent_concurrency
                                 ? &subagent_concurrency.error()
                             : !subagent_timeout ? &subagent_timeout.error()
                             : !subagent_output  ? &subagent_output.error()
                             : !context_model    ? &context_model.error()
                             : !context_max      ? &context_max.error()
                             : !context_reserved ? &context_reserved.error()
                             : !context_trigger  ? &context_trigger.error()
                             : !context_output   ? &context_output.error()
                                                 : nullptr;
  if (error != nullptr)
    return core::Result<WorkspaceConfig>::failure(*error);

  WorkspaceConfig config;
  config.version = version.value();
  config.agent.model.model = agent_model.value();
  config.agent.reasoning_effort = agent_reasoning.value();
  config.agent.max_turns = agent_turns.value();
  config.agent.max_output_tokens = agent_output.value();
  config.agent.temperature = agent_temperature.value();
  config.explorer.enabled = explorer.at("enabled").get<bool>();
  config.explorer.model.model = explorer_model.value();
  config.explorer.reasoning_effort = explorer_reasoning.value();
  config.explorer.max_turns = explorer_turns.value();
  config.explorer.max_output_tokens = explorer_output.value();
  config.subagent_execution = {subagent_concurrency.value(),
                               subagent_timeout.value(),
                               subagent_output.value()};
  config.context.model.model = context_model.value();
  config.context.limits = {context_max.value(), context_reserved.value(),
                           context_trigger.value()};
  config.context.max_output_tokens = context_output.value();
  const auto valid = validate_workspace_config(config);
  if (!valid)
    return core::Result<WorkspaceConfig>::failure(valid.error());
  return core::Result<WorkspaceConfig>::success(std::move(config));
}

std::string serialize_workspace_config(const WorkspaceConfig &config) {
  Json root{
      {"version", config.version},
      {"agent",
       {{"model", config.agent.model.model},
        {"reasoning",
         core::reasoning_effort_name(config.agent.reasoning_effort)},
        {"max_turns", config.agent.max_turns},
        {"max_output_tokens", config.agent.max_output_tokens},
        {"temperature", config.agent.temperature}}},
      {"subagents",
       {{"explorer",
         {{"enabled", config.explorer.enabled},
          {"model", config.explorer.model.model},
          {"reasoning",
           core::reasoning_effort_name(config.explorer.reasoning_effort)},
          {"max_turns", config.explorer.max_turns},
          {"max_output_tokens", config.explorer.max_output_tokens}}},
        {"max_concurrency", config.subagent_execution.max_concurrency},
        {"total_timeout_ms", config.subagent_execution.total_timeout_ms},
        {"max_aggregate_output_bytes",
         config.subagent_execution.max_aggregate_output_bytes}}},
      {"context",
       {{"model", config.context.model.model},
        {"max_tokens", config.context.limits.max_context_tokens},
        {"reserved_output_tokens",
         config.context.limits.reserved_output_tokens},
        {"compaction_trigger_tokens",
         config.context.limits.compaction_trigger_tokens},
        {"max_output_tokens", config.context.max_output_tokens}}},
  };
  return root.dump(2) + "\n";
}

core::Result<WorkspaceConfig>
load_workspace_config(const std::filesystem::path &workspace) {
  const auto path = workspace_config_path(workspace);
  std::error_code filesystem_error;
  const auto status = std::filesystem::symlink_status(path, filesystem_error);
  if (filesystem_error == std::errc::no_such_file_or_directory ||
      (!filesystem_error && !std::filesystem::exists(status))) {
    return core::Result<WorkspaceConfig>::success(default_workspace_config());
  }
  if (filesystem_error) {
    return core::Result<WorkspaceConfig>::failure({
        core::ErrorCode::invalid_argument,
        "cannot inspect workspace configuration " + path.string() + ": " +
            filesystem_error.message(),
    });
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return core::Result<WorkspaceConfig>::failure({
        core::ErrorCode::invalid_argument,
        "workspace configuration path must be a regular file, not a symlink: " +
            path.string(),
    });
  }
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size == 0 || size > kMaxWorkspaceConfigBytes) {
    return core::Result<WorkspaceConfig>::failure({
        core::ErrorCode::invalid_argument,
        "workspace configuration must contain between 1 byte and 64 KiB: " +
            path.string(),
    });
  }
  std::ifstream input(path, std::ios::binary);
  std::string content{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  if (!input || input.bad()) {
    return core::Result<WorkspaceConfig>::failure({
        core::ErrorCode::invalid_argument,
        "cannot read workspace configuration " + path.string(),
    });
  }
  const auto parsed = parse_workspace_config(content);
  if (!parsed) {
    return core::Result<WorkspaceConfig>::failure({
        parsed.error().code,
        parsed.error().message + " (" + path.string() + ")",
    });
  }
  return parsed;
}

core::Result<void> save_workspace_config(const std::filesystem::path &workspace,
                                         const WorkspaceConfig &config) {
  const auto valid = validate_workspace_config(config);
  if (!valid)
    return valid;
  return support::write_private_file_atomically(
      workspace_config_path(workspace), serialize_workspace_config(config),
      "workspace configuration");
}

core::Result<WorkspacePrompts>
load_workspace_prompts(const std::filesystem::path &workspace,
                       bool load_agent_prompt) {
  WorkspacePrompts prompts;
  if (load_agent_prompt) {
    const auto agent = load_or_install_prompt(
        workspace / kSystemPromptRelativePath, core::kDefaultSystemPrompt,
        "main Agent system prompt");
    if (!agent)
      return core::Result<WorkspacePrompts>::failure(agent.error());
    prompts.agent = agent.value();
  }

  const auto explorer = load_or_install_prompt(
      workspace / kExplorerSystemPromptRelativePath,
      subagents::default_explorer_system_prompt(), "Explorer system prompt");
  if (!explorer)
    return core::Result<WorkspacePrompts>::failure(explorer.error());
  prompts.explorer = explorer.value();

  const auto context = load_or_install_prompt(
      workspace / kContextSystemPromptRelativePath,
      core::default_context_system_prompt(), "context system prompt");
  if (!context)
    return core::Result<WorkspacePrompts>::failure(context.error());
  prompts.context = context.value();
  return core::Result<WorkspacePrompts>::success(std::move(prompts));
}

core::Result<void> validate_workspace_prompts(const WorkspacePrompts &prompts) {
  const auto agent = validate_prompt(prompts.agent, "main Agent system prompt");
  const auto explorer =
      validate_prompt(prompts.explorer, "Explorer system prompt");
  const auto context =
      validate_prompt(prompts.context, "context system prompt");
  if (!agent || !explorer || !context) {
    return core::Result<void>::failure(
        !agent ? agent.error()
               : (!explorer ? explorer.error() : context.error()));
  }
  return core::Result<void>::success();
}

core::Result<void>
save_workspace_prompts(const std::filesystem::path &workspace,
                       const WorkspacePrompts &prompts) {
  const auto valid = validate_workspace_prompts(prompts);
  if (!valid)
    return valid;
  const auto saved_agent = support::write_private_file_atomically(
      workspace / kSystemPromptRelativePath, prompts.agent,
      "main Agent system prompt");
  if (!saved_agent)
    return saved_agent;
  const auto saved_explorer = support::write_private_file_atomically(
      workspace / kExplorerSystemPromptRelativePath, prompts.explorer,
      "Explorer system prompt");
  if (!saved_explorer)
    return saved_explorer;
  return support::write_private_file_atomically(
      workspace / kContextSystemPromptRelativePath, prompts.context,
      "context system prompt");
}

core::Result<void>
validate_agent_management(const AgentManagementConfig &management) {
  if (management.version != kAgentManagementVersion ||
      management.agents.empty() ||
      management.agents.size() > kMaximumManagedAgents ||
      management.subagents.size() > kMaximumManagedAgents ||
      !valid_identifier(management.active_agent)) {
    return core::Result<void>::failure(invalid_config(
        "agent management version, counts, or active Agent is invalid"));
  }

  std::vector<std::string> agent_ids;
  for (const auto &profile : management.agents) {
    if (!valid_identifier(profile.id) ||
        !valid_display_text(profile.name, 128) ||
        !valid_display_text(profile.description, 1'024, true) ||
        profile.compaction_trigger_tokens > 16'777'216 ||
        profile.tools.size() > kMaximumAgentTools ||
        std::find(agent_ids.begin(), agent_ids.end(), profile.id) !=
            agent_ids.end()) {
      return core::Result<void>::failure(invalid_config(
          "Agent ids must be unique lowercase identifiers with valid names"));
    }
    std::vector<std::string> tool_names;
    for (const auto &tool : profile.tools) {
      if (!valid_tool_name(tool) ||
          std::find(tool_names.begin(), tool_names.end(), tool) !=
              tool_names.end()) {
        return core::Result<void>::failure(invalid_config(
            "Agent tool permissions must contain unique valid tool names"));
      }
      tool_names.push_back(tool);
    }
    if (profile.tools.size() > 1 &&
        std::find(profile.tools.begin(), profile.tools.end(), "*") !=
            profile.tools.end()) {
      return core::Result<void>::failure(invalid_config(
          "Agent wildcard tool permission cannot be combined with names"));
    }
    WorkspaceConfig probe;
    probe.agent = profile.config;
    const auto valid_config = validate_workspace_config(probe);
    const auto valid_system_prompt =
        validate_prompt(profile.system_prompt, "Agent system prompt");
    if (!valid_config || !valid_system_prompt) {
      return core::Result<void>::failure(
          !valid_config ? valid_config.error() : valid_system_prompt.error());
    }
    agent_ids.push_back(profile.id);
  }
  if (std::find(agent_ids.begin(), agent_ids.end(), management.active_agent) ==
      agent_ids.end()) {
    return core::Result<void>::failure(
        invalid_config("active Agent does not exist"));
  }

  std::vector<std::string> subagent_ids{"explorer"};
  for (const auto &subagent : management.subagents) {
    if (!valid_identifier(subagent.name) ||
        !valid_display_text(subagent.description, 1'024, true) ||
        std::find(subagent_ids.begin(), subagent_ids.end(), subagent.name) !=
            subagent_ids.end()) {
      return core::Result<void>::failure(invalid_config(
          "Sub Agent ids must be unique lowercase identifiers and cannot use "
          "the reserved explorer id"));
    }
    WorkspaceConfig probe;
    probe.explorer = subagent;
    const auto valid_config = validate_workspace_config(probe);
    const auto valid_system_prompt =
        validate_prompt(subagent.system_prompt, "Sub Agent system prompt");
    if (!valid_config || !valid_system_prompt) {
      return core::Result<void>::failure(
          !valid_config ? valid_config.error() : valid_system_prompt.error());
    }
    subagent_ids.push_back(subagent.name);
  }
  return core::Result<void>::success();
}

core::Result<AgentManagementConfig>
load_agent_management(const std::filesystem::path &workspace,
                      const WorkspaceConfig &workspace_config,
                      const WorkspacePrompts &prompts) {
  const auto path = agent_management_path(workspace);
  std::error_code filesystem_error;
  const auto status = std::filesystem::symlink_status(path, filesystem_error);
  if (filesystem_error == std::errc::no_such_file_or_directory ||
      (!filesystem_error && !std::filesystem::exists(status))) {
    AgentManagementConfig management;
    AgentProfile profile;
    profile.config = workspace_config.agent;
    profile.compaction_trigger_tokens =
        workspace_config.context.limits.compaction_trigger_tokens;
    profile.system_prompt = prompts.agent;
    management.agents.push_back(std::move(profile));
    return core::Result<AgentManagementConfig>::success(std::move(management));
  }
  if (filesystem_error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return core::Result<AgentManagementConfig>::failure({
        core::ErrorCode::invalid_argument,
        "agent management path must be a regular file: " + path.string(),
    });
  }
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || size == 0 || size > kMaxAgentManagementBytes) {
    return core::Result<AgentManagementConfig>::failure({
        core::ErrorCode::invalid_argument,
        "agent management file must contain between 1 byte and 16 MiB: " +
            path.string(),
    });
  }
  std::ifstream input(path, std::ios::binary);
  std::string content{std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>()};
  if (!input || input.bad()) {
    return core::Result<AgentManagementConfig>::failure({
        core::ErrorCode::invalid_argument,
        "cannot read agent management file " + path.string(),
    });
  }
  const auto parsed = parse_agent_management_json(
      content, workspace_config.context.limits.compaction_trigger_tokens);
  if (!parsed) {
    return core::Result<AgentManagementConfig>::failure(
        {parsed.error().code,
         parsed.error().message + " (" + path.string() + ")"});
  }
  return parsed;
}

core::Result<void>
save_agent_management(const std::filesystem::path &workspace,
                      const AgentManagementConfig &management) {
  const auto valid = validate_agent_management(management);
  if (!valid)
    return valid;
  return support::write_private_file_atomically(
      agent_management_path(workspace),
      serialize_agent_management_config(management),
      "agent management configuration");
}

core::Result<RuntimeConfig>
load_runtime_config(RuntimeConfigLoadOptions options) {
  RuntimeConfig config;
  config.workspace =
      environment_or("ZED_WORKSPACE", std::filesystem::current_path().string());
  config.workspace = std::filesystem::weakly_canonical(config.workspace);
  config.workspace_config_path = workspace_config_path(config.workspace);
  const auto workspace_settings = load_workspace_config(config.workspace);
  if (!workspace_settings)
    return core::Result<RuntimeConfig>::failure(workspace_settings.error());
  const bool has_workspace_config =
      std::filesystem::is_regular_file(config.workspace_config_path);

  const auto api_key = load_opencode_go_api_key();
  if (!api_key)
    return core::Result<RuntimeConfig>::failure(api_key.error());
  config.opencode_go_api_key = api_key.value();

  config.opencode_endpoint =
      environment_or("ZED_OPENCODE_ENDPOINT", "https://opencode.ai/zen/go/v1");
  config.opencode_path = environment_or("ZED_OPENCODE_PATH", "opencode");
  if (config.opencode_path.empty()) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_OPENCODE_PATH cannot be empty",
    });
  }
  config.clangd_path = environment_or("ZED_CLANGD_PATH", "clangd");
  if (config.clangd_path.empty()) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_CLANGD_PATH cannot be empty",
    });
  }

  config.system_prompt_path = config.workspace / kSystemPromptRelativePath;
  config.explorer_system_prompt_path =
      config.workspace / kExplorerSystemPromptRelativePath;
  config.context_system_prompt_path =
      config.workspace / kContextSystemPromptRelativePath;
  const auto prompts = load_workspace_prompts(
      config.workspace, options.load_workspace_system_prompt);
  if (!prompts)
    return core::Result<RuntimeConfig>::failure(prompts.error());
  config.context_system_prompt = prompts.value().context;
  const auto management = load_agent_management(
      config.workspace, workspace_settings.value(), prompts.value());
  if (!management)
    return core::Result<RuntimeConfig>::failure(management.error());
  const auto active_agent = std::find_if(
      management.value().agents.begin(), management.value().agents.end(),
      [&](const AgentProfile &profile) {
        return profile.id == management.value().active_agent;
      });
  if (active_agent == management.value().agents.end()) {
    return core::Result<RuntimeConfig>::failure(
        {core::ErrorCode::invalid_argument,
         "active Agent is missing from agent management configuration"});
  }
  if (options.load_workspace_system_prompt)
    config.system_prompt = active_agent->system_prompt;
  const auto configured_session = environment_or("ZED_SESSION_PATH");
  config.session_path = configured_session.empty()
                            ? zed::session::new_session_path(
                                  config.workspace / ".zed" / "sessions")
                            : std::filesystem::path(configured_session);

  const std::string model =
      environment_or("ZED_MODEL", active_agent->config.model.model);
  const std::string context_model = environment_or(
      "ZED_CONTEXT_MODEL", has_workspace_config
                               ? workspace_settings.value().context.model.model
                               : model);
  if (model.empty() || context_model.empty()) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_MODEL and ZED_CONTEXT_MODEL cannot be empty",
    });
  }
  config.main_model = {"opencode-go", model};
  config.context_model = {"opencode-go", context_model};

  const auto reasoning_effort =
      reasoning_effort_environment(active_agent->config.reasoning_effort);
  if (!reasoning_effort) {
    return core::Result<RuntimeConfig>::failure(reasoning_effort.error());
  }
  config.reasoning_effort = reasoning_effort.value();

  config.terminal_theme = environment_or("ZED_THEME", "light");
  if (config.terminal_theme != "light" && config.terminal_theme != "monaka") {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_THEME must be one of: light, monaka",
    });
  }

  const auto quick_bash = boolean_environment("ZED_QUICK_BASH", true);
  if (!quick_bash)
    return core::Result<RuntimeConfig>::failure(quick_bash.error());
  config.quick_bash_enabled = quick_bash.value();

  const auto max_context = size_environment(
      "ZED_MAX_CONTEXT_TOKENS",
      workspace_settings.value().context.limits.max_context_tokens, false);
  const auto reserved = size_environment(
      "ZED_RESERVED_OUTPUT_TOKENS",
      workspace_settings.value().context.limits.reserved_output_tokens);
  const auto trigger = size_environment(
      "ZED_CONTEXT_TRIGGER_TOKENS", active_agent->compaction_trigger_tokens);
  const auto max_turns =
      size_environment("ZED_MAX_TURNS", active_agent->config.max_turns, false);
  const auto request_timeout =
      size_environment("ZED_REQUEST_TIMEOUT_MS", 120'000, false);
  if (!max_context || !reserved || !trigger || !max_turns || !request_timeout) {
    const auto *error = !max_context ? &max_context.error()
                        : !reserved  ? &reserved.error()
                        : !trigger   ? &trigger.error()
                        : !max_turns ? &max_turns.error()
                                     : &request_timeout.error();
    return core::Result<RuntimeConfig>::failure(*error);
  }
  config.context_limits = {max_context.value(), reserved.value(),
                           trigger.value(),
                           active_agent->automatic_context_compaction};
  config.max_turns = max_turns.value();
  config.main_max_output_tokens = active_agent->config.max_output_tokens;
  config.main_temperature = active_agent->config.temperature;
  config.explorer = workspace_settings.value().explorer;
  config.explorer.system_prompt = prompts.value().explorer;
  config.subagents = {config.explorer};
  config.subagents.insert(config.subagents.end(),
                          management.value().subagents.begin(),
                          management.value().subagents.end());
  config.subagent_execution = workspace_settings.value().subagent_execution;
  config.context_max_output_tokens =
      workspace_settings.value().context.max_output_tokens;
  config.agent_tools = active_agent->tools;
  config.opencode_request_timeout_ms = request_timeout.value();
  if (config.context_limits.max_context_tokens <=
      config.context_limits.reserved_output_tokens) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_MAX_CONTEXT_TOKENS must exceed ZED_RESERVED_OUTPUT_TOKENS",
    });
  }
  const auto available = config.context_limits.max_context_tokens -
                         config.context_limits.reserved_output_tokens;
  if (config.context_limits.compaction_trigger_tokens > available) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_CONTEXT_TRIGGER_TOKENS must be zero or fit within the available "
        "context budget",
    });
  }
  return core::Result<RuntimeConfig>::success(std::move(config));
}

} // namespace zed::app
