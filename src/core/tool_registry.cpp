#include "zed/core/tool_registry.hpp"

#include "zed/core/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zed::core {

namespace {

using Json = nlohmann::json;

Result<Json> schema_with_purpose(const ToolDefinition &definition) {
  try {
    auto schema = Json::parse(definition.input_schema_json);
    if (!schema.is_object()) {
      return Result<Json>::failure({
          ErrorCode::invalid_argument,
          "tool input schema must be an object: " + definition.name,
      });
    }

    auto &properties = schema["properties"];
    if (properties.is_null())
      properties = Json::object();
    if (!properties.is_object()) {
      return Result<Json>::failure({
          ErrorCode::invalid_argument,
          "tool schema properties must be an object: " + definition.name,
      });
    }
    properties["purpose"] = {
        {"type", "string"},
        {"minLength", 1},
        {"description",
         "Brief user-facing reason for this tool call. Describe the goal, "
         "not the exact command."},
    };

    auto &required = schema["required"];
    if (required.is_null())
      required = Json::array();
    if (!required.is_array()) {
      return Result<Json>::failure({
          ErrorCode::invalid_argument,
          "tool schema required field must be an array: " + definition.name,
      });
    }
    const auto has_purpose = std::find(required.begin(), required.end(),
                                       "purpose") != required.end();
    if (!has_purpose)
      required.push_back("purpose");
    return Result<Json>::success(std::move(schema));
  } catch (const Json::exception &error) {
    return Result<Json>::failure({
        ErrorCode::invalid_argument,
        "invalid input schema for tool " + definition.name + ": " +
            error.what(),
    });
  }
}

} // namespace

ToolRegistry::ToolRegistry(std::vector<std::string> allowed_tools)
    : allowed_tools_(std::in_place, allowed_tools.begin(),
                     allowed_tools.end()) {}

Result<void>
ToolRegistry::validate_tool(const ToolDefinition &definition) const {
  if (definition.name.empty()) {
    return Result<void>::failure({
        ErrorCode::invalid_argument,
        "tool name cannot be empty",
    });
  }
  const auto prepared_schema = schema_with_purpose(definition);
  if (!prepared_schema)
    return Result<void>::failure(prepared_schema.error());

  std::scoped_lock lock(mutex_);
  const auto duplicate =
      std::find_if(tools_.begin(), tools_.end(), [&](const auto &registered) {
        return registered->definition().name == definition.name;
      });
  if (duplicate != tools_.end()) {
    return Result<void>::failure({
        ErrorCode::conflict,
        "tool already registered: " + definition.name,
    });
  }
  return Result<void>::success();
}

bool ToolRegistry::allowed(std::string_view name) const {
  return !allowed_tools_.has_value() || allowed_tools_->contains("*") ||
         allowed_tools_->contains(std::string(name));
}

Result<std::string> tool_call_purpose(const ToolCall &call) {
  try {
    const auto arguments = Json::parse(call.arguments_json);
    if (!arguments.is_object()) {
      return Result<std::string>::failure({
          ErrorCode::invalid_argument,
          "tool arguments must be an object: " + call.name,
      });
    }
    const auto iterator = arguments.find("purpose");
    if (iterator == arguments.end() || !iterator->is_string()) {
      return Result<std::string>::failure({
          ErrorCode::invalid_argument,
          "tool purpose must be a non-empty string: " + call.name,
      });
    }
    auto purpose = iterator->get<std::string>();
    const auto has_visible_character =
        std::any_of(purpose.begin(), purpose.end(), [](char character) {
          return std::isspace(static_cast<unsigned char>(character)) == 0;
        });
    if (!has_visible_character) {
      return Result<std::string>::failure({
          ErrorCode::invalid_argument,
          "tool purpose must be a non-empty string: " + call.name,
      });
    }
    return Result<std::string>::success(std::move(purpose));
  } catch (const Json::exception &error) {
    return Result<std::string>::failure({
        ErrorCode::invalid_argument,
        "invalid JSON arguments for tool " + call.name + ": " + error.what(),
    });
  }
}

Result<void> ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
  if (tool == nullptr) {
    return Result<void>::failure({
        ErrorCode::invalid_argument,
        "cannot register a null tool",
    });
  }

  const auto &definition = tool->definition();
  const auto validation = validate_tool(definition);
  if (!validation)
    return validation;
  const auto prepared_schema = schema_with_purpose(definition);
  if (!prepared_schema)
    return Result<void>::failure(prepared_schema.error());

  std::scoped_lock lock(mutex_);
  const auto duplicate =
      std::find_if(tools_.begin(), tools_.end(), [&](const auto &registered) {
        return registered->definition().name == definition.name;
      });
  if (duplicate != tools_.end()) {
    return Result<void>::failure({
        ErrorCode::conflict,
        "tool already registered: " + definition.name,
    });
  }

  auto prepared_definition = definition;
  prepared_definition.input_schema_json = prepared_schema.value().dump();
  tools_.reserve(tools_.size() + 1);
  prepared_definitions_.reserve(prepared_definitions_.size() + 1);
  prepared_definitions_.push_back(std::move(prepared_definition));
  tools_.push_back(std::move(tool));
  return Result<void>::success();
}

bool ToolRegistry::unregister_tool(std::string_view name) {
  std::scoped_lock lock(mutex_);
  const auto iterator =
      std::find_if(tools_.begin(), tools_.end(), [&](const auto &tool) {
        return tool->definition().name == name;
      });
  if (iterator == tools_.end())
    return false;
  const auto index =
      static_cast<std::size_t>(std::distance(tools_.begin(), iterator));
  tools_.erase(iterator);
  prepared_definitions_.erase(prepared_definitions_.begin() +
                              static_cast<std::ptrdiff_t>(index));
  return true;
}

std::vector<ToolDefinition> ToolRegistry::definitions() const {
  std::scoped_lock lock(mutex_);
  std::vector<ToolDefinition> result;
  result.reserve(tools_.size());
  for (const auto &definition : prepared_definitions_) {
    if (!allowed(definition.name))
      continue;
    result.push_back(definition);
  }
  return result;
}

std::vector<ToolDefinition> ToolRegistry::registered_definitions() const {
  std::scoped_lock lock(mutex_);
  return prepared_definitions_;
}

Result<ToolResult>
ToolRegistry::execute(const ToolCall &call, CancellationToken cancellation,
                      const ToolProgressCallback &on_progress) {
  if (cancellation.is_cancelled()) {
    return Result<ToolResult>::failure({
        ErrorCode::cancelled,
        "tool execution cancelled",
    });
  }
  if (!allowed(call.name)) {
    return Result<ToolResult>::failure({
        ErrorCode::not_found,
        "tool is not permitted for the active Agent: " + call.name,
    });
  }
  const auto purpose = tool_call_purpose(call);
  if (!purpose)
    return Result<ToolResult>::failure(purpose.error());

  Tool *target = nullptr;
  {
    std::scoped_lock lock(mutex_);
    const auto iterator =
        std::find_if(tools_.begin(), tools_.end(), [&](const auto &tool) {
          return tool->definition().name == call.name;
        });
    if (iterator == tools_.end()) {
      return Result<ToolResult>::failure({
          ErrorCode::not_found,
          "tool not found: " + call.name,
      });
    }
    target = iterator->get();
  }

  auto execution =
      target->execute_with_progress(call, cancellation, on_progress);
  if (!execution)
    return execution;

  auto result = std::move(execution.value());
  auto sanitized = sanitize_utf8(result.content);
  if (sanitized.replacement_count > 0) {
    sanitized.text += "\n[warning: replaced " +
                      std::to_string(sanitized.replacement_count) +
                      " invalid UTF-8 byte(s) in tool output]";
  }
  result.content = std::move(sanitized.text);
  return Result<ToolResult>::success(std::move(result));
}

} // namespace zed::core
