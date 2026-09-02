#include "zed/subagents/worker_protocol.hpp"

#include "zed/core/utf8.hpp"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace zed::subagents {

namespace {

using Json = nlohmann::json;
using core::ErrorCode;

core::Result<Json> parse_object(std::string_view input,
                                std::string_view operation) {
  try {
    auto value = Json::parse(input);
    if (!value.is_object()) {
      return core::Result<Json>::failure(
          {ErrorCode::invalid_argument,
           std::string(operation) + " must be a JSON object"});
    }
    return core::Result<Json>::success(std::move(value));
  } catch (const Json::exception &error) {
    return core::Result<Json>::failure(
        {ErrorCode::invalid_argument,
         "invalid " + std::string(operation) + " JSON: " + error.what()});
  }
}

core::Result<void>
require_exact_fields(const Json &value,
                     std::initializer_list<std::string_view> allowed,
                     std::string_view operation) {
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    const auto accepted = std::find(allowed.begin(), allowed.end(),
                                    iterator.key()) != allowed.end();
    if (!accepted) {
      return core::Result<void>::failure(
          {ErrorCode::invalid_argument,
           std::string(operation) +
               " contains unexpected field: " + iterator.key()});
    }
  }
  return core::Result<void>::success();
}

core::Result<std::string> required_string(const Json &value,
                                          std::string_view name,
                                          bool allow_empty = false) {
  const auto iterator = value.find(std::string(name));
  if (iterator == value.end() || !iterator->is_string()) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "missing non-empty string field: " + std::string(name)});
  }
  auto text = iterator->get<std::string>();
  const bool has_visible_character =
      std::any_of(text.begin(), text.end(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) == 0;
      });
  if (!allow_empty && !has_visible_character) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "missing non-empty string field: " + std::string(name)});
  }
  return core::Result<std::string>::success(std::move(text));
}

core::Result<void> require_version(const Json &value) {
  const auto iterator = value.find("version");
  if (iterator == value.end() || !iterator->is_number_unsigned() ||
      iterator->get<std::size_t>() != kWorkerProtocolVersion) {
    return core::Result<void>::failure(
        {ErrorCode::invalid_argument,
         "unsupported subagent worker protocol version"});
  }
  return core::Result<void>::success();
}

core::Result<std::string> require_request_id(const Json &value) {
  const auto request_id = required_string(value, "id");
  if (!request_id)
    return request_id;
  if (request_id.value().size() > 64) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "subagent worker request id exceeds 64 bytes"});
  }
  return request_id;
}

std::string_view event_type_name(WorkerEventType type) {
  switch (type) {
  case WorkerEventType::started:
    return "started";
  case WorkerEventType::tool_start:
    return "tool_start";
  case WorkerEventType::completed:
    return "completed";
  case WorkerEventType::failed:
    return "failed";
  case WorkerEventType::cancelled:
    return "cancelled";
  }
  return "failed";
}

core::Result<WorkerEventType> event_type_from_name(std::string_view name) {
  if (name == "started")
    return core::Result<WorkerEventType>::success(WorkerEventType::started);
  if (name == "tool_start")
    return core::Result<WorkerEventType>::success(WorkerEventType::tool_start);
  if (name == "completed")
    return core::Result<WorkerEventType>::success(WorkerEventType::completed);
  if (name == "failed")
    return core::Result<WorkerEventType>::success(WorkerEventType::failed);
  if (name == "cancelled")
    return core::Result<WorkerEventType>::success(WorkerEventType::cancelled);
  return core::Result<WorkerEventType>::failure(
      {ErrorCode::invalid_argument,
       "unknown subagent worker event type: " + std::string(name)});
}

void parse_usage(const Json &value, core::ModelUsage &usage) {
  if (!value.is_object())
    return;
  if (const auto iterator = value.find("input_tokens");
      iterator != value.end() && iterator->is_number_unsigned()) {
    usage.input_tokens = iterator->get<core::TokenCount>();
  }
  if (const auto iterator = value.find("cached_input_tokens");
      iterator != value.end() && iterator->is_number_unsigned()) {
    usage.cached_input_tokens = iterator->get<core::TokenCount>();
  }
  if (const auto iterator = value.find("output_tokens");
      iterator != value.end() && iterator->is_number_unsigned()) {
    usage.output_tokens = iterator->get<core::TokenCount>();
  }
}

std::string safe_text(std::string_view value) {
  return core::sanitize_utf8(value).text;
}

} // namespace

core::Result<WorkerCommand> parse_worker_command(std::string_view json_line) {
  if (json_line.size() > kMaximumTaskBytes + 1024) {
    return core::Result<WorkerCommand>::failure(
        {ErrorCode::invalid_argument, "subagent worker request is too large"});
  }
  const auto parsed = parse_object(json_line, "subagent worker command");
  if (!parsed)
    return core::Result<WorkerCommand>::failure(parsed.error());
  const auto version = require_version(parsed.value());
  if (!version)
    return core::Result<WorkerCommand>::failure(version.error());
  const auto type = required_string(parsed.value(), "type");
  if (!type)
    return core::Result<WorkerCommand>::failure(type.error());
  const auto request_id = require_request_id(parsed.value());
  if (!request_id)
    return core::Result<WorkerCommand>::failure(request_id.error());
  if (type.value() == "cancel") {
    const auto fields = require_exact_fields(
        parsed.value(), {"version", "type", "id"}, "worker cancellation");
    if (!fields)
      return core::Result<WorkerCommand>::failure(fields.error());
    return core::Result<WorkerCommand>::success(
        {WorkerCommandType::cancel, {request_id.value(), {}, {}}});
  }
  if (type.value() != "run") {
    return core::Result<WorkerCommand>::failure(
        {ErrorCode::invalid_argument,
         "unknown subagent worker command type: " + type.value()});
  }
  const auto fields = require_exact_fields(
      parsed.value(), {"version", "type", "id", "agent", "task"},
      "worker request");
  if (!fields)
    return core::Result<WorkerCommand>::failure(fields.error());
  const auto agent = required_string(parsed.value(), "agent");
  const auto task = required_string(parsed.value(), "task");
  if (!agent)
    return core::Result<WorkerCommand>::failure(agent.error());
  if (!task)
    return core::Result<WorkerCommand>::failure(task.error());
  if (task.value().size() > kMaximumTaskBytes) {
    return core::Result<WorkerCommand>::failure(
        {ErrorCode::invalid_argument, "subagent task exceeds 32 KiB"});
  }
  return core::Result<WorkerCommand>::success(
      {WorkerCommandType::run,
       {request_id.value(), agent.value(), task.value()}});
}

std::string serialize_worker_request(const WorkerRequest &request) {
  return Json{{"version", kWorkerProtocolVersion},
              {"type", "run"},
              {"id", safe_text(request.request_id)},
              {"agent", safe_text(request.agent)},
              {"task", safe_text(request.task)}}
      .dump();
}

std::string serialize_worker_cancellation(std::string_view request_id) {
  return Json{{"version", kWorkerProtocolVersion},
              {"type", "cancel"},
              {"id", safe_text(request_id)}}
      .dump();
}

core::Result<WorkerEvent> parse_worker_event(std::string_view json_line) {
  const auto parsed = parse_object(json_line, "subagent worker event");
  if (!parsed)
    return core::Result<WorkerEvent>::failure(parsed.error());
  const auto version = require_version(parsed.value());
  if (!version)
    return core::Result<WorkerEvent>::failure(version.error());
  const auto request_id = require_request_id(parsed.value());
  if (!request_id)
    return core::Result<WorkerEvent>::failure(request_id.error());
  const auto type_name = required_string(parsed.value(), "type");
  if (!type_name)
    return core::Result<WorkerEvent>::failure(type_name.error());
  const auto type = event_type_from_name(type_name.value());
  if (!type)
    return core::Result<WorkerEvent>::failure(type.error());

  WorkerEvent event;
  event.type = type.value();
  event.request_id = request_id.value();
  core::Result<void> fields = core::Result<void>::success();
  switch (event.type) {
  case WorkerEventType::started: {
    fields = require_exact_fields(
        parsed.value(), {"version", "type", "id", "agent"}, "started event");
    if (!fields)
      break;
    const auto agent = required_string(parsed.value(), "agent");
    if (!agent)
      return core::Result<WorkerEvent>::failure(agent.error());
    event.agent = agent.value();
    break;
  }
  case WorkerEventType::tool_start: {
    fields = require_exact_fields(parsed.value(),
                                  {"version", "type", "id", "tool", "purpose"},
                                  "tool_start event");
    if (!fields)
      break;
    const auto tool = required_string(parsed.value(), "tool");
    const auto purpose = required_string(parsed.value(), "purpose");
    if (!tool)
      return core::Result<WorkerEvent>::failure(tool.error());
    if (!purpose)
      return core::Result<WorkerEvent>::failure(purpose.error());
    event.tool = tool.value();
    event.purpose = purpose.value();
    break;
  }
  case WorkerEventType::completed: {
    fields = require_exact_fields(parsed.value(),
                                  {"version", "type", "id", "content", "usage"},
                                  "completed event");
    if (!fields)
      break;
    const auto content = required_string(parsed.value(), "content", true);
    if (!content)
      return core::Result<WorkerEvent>::failure(content.error());
    event.content = content.value();
    const auto usage = parsed.value().find("usage");
    if (usage == parsed.value().end() || !usage->is_object()) {
      return core::Result<WorkerEvent>::failure(
          {ErrorCode::invalid_argument,
           "completed event is missing an object usage field"});
    }
    const auto usage_fields = require_exact_fields(
        *usage, {"input_tokens", "cached_input_tokens", "output_tokens"},
        "worker usage");
    if (!usage_fields)
      return core::Result<WorkerEvent>::failure(usage_fields.error());
    for (const auto *name :
         {"input_tokens", "cached_input_tokens", "output_tokens"}) {
      const auto value = usage->find(name);
      if (value == usage->end() || !value->is_number_unsigned()) {
        return core::Result<WorkerEvent>::failure(
            {ErrorCode::invalid_argument,
             "worker usage field must be an unsigned integer: " +
                 std::string(name)});
      }
    }
    parse_usage(*usage, event.usage);
    break;
  }
  case WorkerEventType::failed: {
    fields = require_exact_fields(parsed.value(),
                                  {"version", "type", "id", "error", "usage"},
                                  "failed event");
    if (!fields)
      break;
    const auto error = required_string(parsed.value(), "error");
    if (!error)
      return core::Result<WorkerEvent>::failure(error.error());
    event.error = error.value();
    const auto usage = parsed.value().find("usage");
    if (usage == parsed.value().end() || !usage->is_object()) {
      return core::Result<WorkerEvent>::failure(
          {ErrorCode::invalid_argument,
           "failed event is missing an object usage field"});
    }
    const auto usage_fields = require_exact_fields(
        *usage, {"input_tokens", "cached_input_tokens", "output_tokens"},
        "worker usage");
    if (!usage_fields)
      return core::Result<WorkerEvent>::failure(usage_fields.error());
    for (const auto *name :
         {"input_tokens", "cached_input_tokens", "output_tokens"}) {
      const auto value = usage->find(name);
      if (value == usage->end() || !value->is_number_unsigned()) {
        return core::Result<WorkerEvent>::failure(
            {ErrorCode::invalid_argument,
             "worker usage field must be an unsigned integer: " +
                 std::string(name)});
      }
    }
    parse_usage(*usage, event.usage);
    break;
  }
  case WorkerEventType::cancelled:
    fields = require_exact_fields(parsed.value(), {"version", "type", "id"},
                                  "cancelled event");
    break;
  }
  if (!fields)
    return core::Result<WorkerEvent>::failure(fields.error());
  return core::Result<WorkerEvent>::success(std::move(event));
}

std::string serialize_worker_event(const WorkerEvent &event) {
  Json value{{"version", kWorkerProtocolVersion},
             {"type", event_type_name(event.type)},
             {"id", safe_text(event.request_id)}};
  switch (event.type) {
  case WorkerEventType::started:
    value["agent"] = safe_text(event.agent);
    break;
  case WorkerEventType::tool_start:
    value["tool"] = safe_text(event.tool);
    value["purpose"] = safe_text(event.purpose);
    break;
  case WorkerEventType::completed:
    value["content"] = safe_text(event.content);
    value["usage"] = {
        {"input_tokens", event.usage.input_tokens},
        {"cached_input_tokens", event.usage.cached_input_tokens},
        {"output_tokens", event.usage.output_tokens},
    };
    break;
  case WorkerEventType::failed:
    value["error"] = safe_text(event.error);
    value["usage"] = {
        {"input_tokens", event.usage.input_tokens},
        {"cached_input_tokens", event.usage.cached_input_tokens},
        {"output_tokens", event.usage.output_tokens},
    };
    break;
  case WorkerEventType::cancelled:
    break;
  }
  return value.dump();
}

bool is_terminal_event(WorkerEventType type) {
  return type == WorkerEventType::completed ||
         type == WorkerEventType::failed || type == WorkerEventType::cancelled;
}

} // namespace zed::subagents
