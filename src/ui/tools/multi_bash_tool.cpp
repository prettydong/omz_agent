#include "zed/tools/multi_bash_tool.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace zed::tools {

namespace {

using core::ErrorCode;
using core::ToolCall;
using core::ToolDefinition;
using core::ToolResult;
using Json = nlohmann::json;

constexpr std::size_t kMaxCommands = 8;
constexpr std::size_t kDefaultConcurrency = 4;
constexpr std::size_t kMaximumTimeoutMs = 24U * 60U * 60U * 1000U;

const ToolDefinition &multi_bash_definition() {
  static const ToolDefinition definition{
      "multi_bash",
      "Run two or more independent shell commands concurrently in one tool "
      "call. Prefer this over separate bash calls when no intermediate result "
      "is needed; results remain in command order.",
      R"({"type":"object","required":["purpose","commands"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the shared goal without including exact shell commands."},"commands":{"type":"array","minItems":2,"maxItems":8,"items":{"type":"object","required":["command"],"properties":{"command":{"type":"string","minLength":1},"working_dir":{"type":"string","minLength":1},"timeout_ms":{"type":"integer","minimum":1,"maximum":86400000},"max_output_bytes":{"type":"integer","minimum":1}},"additionalProperties":false}},"max_concurrency":{"type":"integer","minimum":1,"maximum":8,"description":"Maximum commands to run simultaneously. Defaults to 4."},"max_output_bytes":{"type":"integer","minimum":1,"description":"Maximum combined output bytes, capped by the configured tool output limit."}},"additionalProperties":false})",
  };
  return definition;
}

core::Result<Json> parse_arguments(const ToolCall &call) {
  try {
    auto arguments = Json::parse(call.arguments_json);
    if (!arguments.is_object()) {
      return core::Result<Json>::failure({
          ErrorCode::invalid_argument,
          "invalid JSON arguments for tool multi_bash: expected object",
      });
    }
    return core::Result<Json>::success(std::move(arguments));
  } catch (const Json::parse_error &error) {
    return core::Result<Json>::failure({
        ErrorCode::invalid_argument,
        "invalid JSON arguments for tool multi_bash: " +
            std::string(error.what()),
    });
  }
}

core::Result<std::size_t>
positive_size(const Json &value, std::string_view name, std::size_t maximum) {
  std::uint64_t parsed = 0;
  if (value.is_number_unsigned()) {
    parsed = value.get<std::uint64_t>();
  } else if (value.is_number_integer()) {
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value <= 0) {
      return core::Result<std::size_t>::failure({
          ErrorCode::invalid_argument,
          std::string(name) + " must be greater than zero",
      });
    }
    parsed = static_cast<std::uint64_t>(signed_value);
  } else {
    return core::Result<std::size_t>::failure({
        ErrorCode::invalid_argument,
        std::string(name) + " must be an integer",
    });
  }

  if (parsed == 0 || parsed > maximum) {
    return core::Result<std::size_t>::failure({
        ErrorCode::invalid_argument,
        std::string(name) + " must be between 1 and " + std::to_string(maximum),
    });
  }
  return core::Result<std::size_t>::success(static_cast<std::size_t>(parsed));
}

std::string result_status(const core::Result<ToolResult> &result) {
  if (!result)
    return "error";
  return result.value().is_error ? "error" : "ok";
}

} // namespace

const ToolDefinition &MultiBashTool::definition() const {
  return multi_bash_definition();
}

core::Result<ToolResult>
MultiBashTool::execute(const ToolCall &call,
                       core::CancellationToken cancellation) {
  if (cancellation.is_cancelled()) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "multi_bash cancelled"});
  }

  const auto parsed = parse_arguments(call);
  if (!parsed)
    return core::Result<ToolResult>::failure(parsed.error());
  const auto &arguments = parsed.value();
  const auto commands_iterator = arguments.find("commands");
  if (commands_iterator == arguments.end() || !commands_iterator->is_array()) {
    return core::Result<ToolResult>::failure({
        ErrorCode::invalid_argument,
        "multi_bash commands must be an array",
    });
  }
  if (commands_iterator->size() < 2 ||
      commands_iterator->size() > kMaxCommands) {
    return core::Result<ToolResult>::failure({
        ErrorCode::invalid_argument,
        "multi_bash commands must contain between 2 and " +
            std::to_string(kMaxCommands) + " entries",
    });
  }

  std::size_t concurrency =
      std::min(kDefaultConcurrency, commands_iterator->size());
  if (const auto iterator = arguments.find("max_concurrency");
      iterator != arguments.end()) {
    const auto value =
        positive_size(*iterator, "max_concurrency", kMaxCommands);
    if (!value)
      return core::Result<ToolResult>::failure(value.error());
    concurrency = std::min(value.value(), commands_iterator->size());
  }

  std::size_t combined_output_limit = limits().max_command_output_bytes;
  if (const auto iterator = arguments.find("max_output_bytes");
      iterator != arguments.end()) {
    const auto value = positive_size(*iterator, "max_output_bytes",
                                     limits().max_command_output_bytes);
    if (!value)
      return core::Result<ToolResult>::failure(value.error());
    combined_output_limit = value.value();
  }

  std::vector<ToolCall> prepared;
  prepared.reserve(commands_iterator->size());
  for (std::size_t index = 0; index < commands_iterator->size(); ++index) {
    const auto &command = (*commands_iterator)[index];
    if (!command.is_object()) {
      return core::Result<ToolResult>::failure({
          ErrorCode::invalid_argument,
          "multi_bash command " + std::to_string(index + 1) +
              " must be an object",
      });
    }
    const auto command_iterator = command.find("command");
    if (command_iterator == command.end() || !command_iterator->is_string() ||
        command_iterator->get_ref<const std::string &>().empty()) {
      return core::Result<ToolResult>::failure({
          ErrorCode::invalid_argument,
          "multi_bash command " + std::to_string(index + 1) +
              " requires a non-empty command string",
      });
    }

    Json normalized{{"command", *command_iterator}};
    if (const auto iterator = command.find("working_dir");
        iterator != command.end()) {
      if (!iterator->is_string() ||
          iterator->get_ref<const std::string &>().empty()) {
        return core::Result<ToolResult>::failure({
            ErrorCode::invalid_argument,
            "multi_bash command " + std::to_string(index + 1) +
                " working_dir must be a non-empty string",
        });
      }
      const auto resolved = resolve_path(iterator->get<std::string>());
      if (!resolved)
        return core::Result<ToolResult>::failure(resolved.error());
      normalized["working_dir"] = *iterator;
    }
    if (const auto iterator = command.find("timeout_ms");
        iterator != command.end()) {
      const auto value =
          positive_size(*iterator, "timeout_ms", kMaximumTimeoutMs);
      if (!value)
        return core::Result<ToolResult>::failure(value.error());
      normalized["timeout_ms"] = value.value();
    }
    if (const auto iterator = command.find("max_output_bytes");
        iterator != command.end()) {
      const auto value = positive_size(*iterator, "max_output_bytes",
                                       limits().max_command_output_bytes);
      if (!value)
        return core::Result<ToolResult>::failure(value.error());
      normalized["max_output_bytes"] = value.value();
    }
    prepared.push_back(
        {call.id + "-" + std::to_string(index + 1), "bash", normalized.dump()});
  }

  std::vector<std::optional<core::Result<ToolResult>>> results(prepared.size());
  std::atomic_size_t next_index{0};
  std::vector<std::jthread> workers;
  workers.reserve(concurrency);
  const auto root = workspace_root();
  const auto tool_limits = limits();
  for (std::size_t worker_index = 0; worker_index < concurrency;
       ++worker_index) {
    workers.emplace_back(
        [&prepared, &results, &next_index, root, tool_limits, cancellation]() {
          BashTool bash(root, tool_limits);
          while (true) {
            const auto index = next_index.fetch_add(1);
            if (index >= prepared.size())
              return;
            results[index] = bash.execute(prepared[index], cancellation);
          }
        });
  }
  workers.clear();

  if (cancellation.is_cancelled()) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "multi_bash cancelled"});
  }

  bool failed = false;
  std::string output = "summary:";
  for (std::size_t index = 0; index < results.size(); ++index) {
    if (!results[index].has_value()) {
      return core::Result<ToolResult>::failure({
          ErrorCode::internal,
          "multi_bash did not produce result " + std::to_string(index + 1),
      });
    }
    output +=
        " " + std::to_string(index + 1) + "=" + result_status(*results[index]);
    failed = failed || !*results[index] || results[index]->value().is_error;
  }

  for (std::size_t index = 0; index < results.size(); ++index) {
    output += "\n\n[" + std::to_string(index + 1) + "]\n";
    const auto &result = *results[index];
    if (!result) {
      output += "tool execution failed: " + result.error().message;
    } else if (result.value().content.empty()) {
      output += "[no output]";
    } else {
      output += result.value().content;
    }
  }

  bool truncated = false;
  if (output.size() > combined_output_limit) {
    output.resize(combined_output_limit);
    truncated = true;
  }
  if (truncated)
    output += "\n[multi_bash output truncated]";
  return core::Result<ToolResult>::success(
      {call.id, std::move(output), failed});
}

} // namespace zed::tools
