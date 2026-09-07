#include "zed/core/agent_hooks.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <set>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace zed::core {

namespace {

using Json = nlohmann::json;

Error payload_error(HookPoint point, std::string detail) {
  return {ErrorCode::hook_error, "invalid replacement from " +
                                     std::string(hook_point_name(point)) +
                                     " hook: " + std::move(detail)};
}

const char *role_name(Role role) {
  switch (role) {
  case Role::system:
    return "system";
  case Role::user:
    return "user";
  case Role::assistant:
    return "assistant";
  case Role::tool:
    return "tool";
  }
  return "user";
}

std::optional<Role> role_from_name(std::string_view name) {
  if (name == "system")
    return Role::system;
  if (name == "user")
    return Role::user;
  if (name == "assistant")
    return Role::assistant;
  if (name == "tool")
    return Role::tool;
  return std::nullopt;
}

const char *finish_reason_name(FinishReason reason) {
  switch (reason) {
  case FinishReason::unknown:
    return "unknown";
  case FinishReason::stop:
    return "stop";
  case FinishReason::tool_calls:
    return "tool_calls";
  case FinishReason::length:
    return "length";
  case FinishReason::content_filter:
    return "content_filter";
  case FinishReason::cancelled:
    return "cancelled";
  }
  return "unknown";
}

std::optional<FinishReason> finish_reason_from_name(std::string_view name) {
  if (name == "unknown")
    return FinishReason::unknown;
  if (name == "stop")
    return FinishReason::stop;
  if (name == "tool_calls")
    return FinishReason::tool_calls;
  if (name == "length")
    return FinishReason::length;
  if (name == "content_filter")
    return FinishReason::content_filter;
  if (name == "cancelled")
    return FinishReason::cancelled;
  return std::nullopt;
}

const char *outcome_name(SessionTurnOutcome outcome) {
  switch (outcome) {
  case SessionTurnOutcome::completed:
    return "completed";
  case SessionTurnOutcome::failed:
    return "failed";
  case SessionTurnOutcome::cancelled:
    return "cancelled";
  case SessionTurnOutcome::interrupted:
    return "interrupted";
  }
  return "failed";
}

std::optional<SessionTurnOutcome> outcome_from_name(std::string_view name) {
  if (name == "completed")
    return SessionTurnOutcome::completed;
  if (name == "failed")
    return SessionTurnOutcome::failed;
  if (name == "cancelled")
    return SessionTurnOutcome::cancelled;
  if (name == "interrupted")
    return SessionTurnOutcome::interrupted;
  return std::nullopt;
}

Json tool_call_json(const ToolCall &call) {
  return {{"id", call.id},
          {"name", call.name},
          {"arguments_json", call.arguments_json}};
}

Result<ToolCall> parse_tool_call(const Json &document, HookPoint point) {
  if (!document.is_object())
    return Result<ToolCall>::failure(
        payload_error(point, "call is not an object"));
  if (!document.contains("id") || !document.at("id").is_string() ||
      !document.contains("name") || !document.at("name").is_string() ||
      !document.contains("arguments_json") ||
      !document.at("arguments_json").is_string()) {
    return Result<ToolCall>::failure(payload_error(
        point, "call requires string id, name, and arguments_json"));
  }
  ToolCall call{document.at("id").get<std::string>(),
                document.at("name").get<std::string>(),
                document.at("arguments_json").get<std::string>()};
  if (call.id.empty() || call.name.empty() || call.arguments_json.empty()) {
    return Result<ToolCall>::failure(
        payload_error(point, "call fields cannot be empty"));
  }
  if (!Json::accept(call.arguments_json)) {
    return Result<ToolCall>::failure(
        payload_error(point, "call arguments_json is invalid JSON"));
  }
  return Result<ToolCall>::success(std::move(call));
}

Json message_json(const Message &message) {
  Json calls = Json::array();
  for (const auto &call : message.tool_calls)
    calls.push_back(tool_call_json(call));
  Json document{{"id", message.id},
                {"role", role_name(message.role)},
                {"content", message.content},
                {"tool_calls", std::move(calls)},
                {"is_error", message.is_error},
                {"model_state", message.model_state}};
  document["tool_call_id"] = message.tool_call_id.has_value()
                                 ? Json(*message.tool_call_id)
                                 : Json(nullptr);
  return document;
}

Result<Message> parse_message(const Json &document, HookPoint point) {
  if (!document.is_object() || !document.contains("id") ||
      !document.at("id").is_string() || !document.contains("role") ||
      !document.at("role").is_string() || !document.contains("content") ||
      !document.at("content").is_string() || !document.contains("tool_calls") ||
      !document.at("tool_calls").is_array() || !document.contains("is_error") ||
      !document.at("is_error").is_boolean()) {
    return Result<Message>::failure(
        payload_error(point, "message has invalid or missing fields"));
  }
  const auto role = role_from_name(document.at("role").get<std::string>());
  if (!role.has_value())
    return Result<Message>::failure(
        payload_error(point, "message role is invalid"));

  Message message{document.at("id").get<std::string>(),
                  *role,
                  document.at("content").get<std::string>(),
                  {},
                  std::nullopt,
                  document.at("is_error").get<bool>()};
  if (document.contains("model_state")) {
    if (!document.at("model_state").is_string())
      return Result<Message>::failure(
          payload_error(point, "model_state must be a string"));
    message.model_state = document.at("model_state").get<std::string>();
  }
  if (message.id.empty())
    return Result<Message>::failure(
        payload_error(point, "message id is empty"));
  std::set<ToolCallId> call_ids;
  for (const auto &call_document : document.at("tool_calls")) {
    auto call = parse_tool_call(call_document, point);
    if (!call)
      return Result<Message>::failure(call.error());
    if (!call_ids.insert(call.value().id).second) {
      return Result<Message>::failure(
          payload_error(point, "message contains duplicate tool call ids"));
    }
    message.tool_calls.push_back(std::move(call.value()));
  }
  if (document.contains("tool_call_id") &&
      !document.at("tool_call_id").is_null()) {
    if (!document.at("tool_call_id").is_string() ||
        document.at("tool_call_id").get<std::string>().empty()) {
      return Result<Message>::failure(
          payload_error(point, "message tool_call_id is invalid"));
    }
    message.tool_call_id = document.at("tool_call_id").get<std::string>();
  }
  if ((message.role == Role::system || message.role == Role::user) &&
      (!message.tool_calls.empty() || message.tool_call_id.has_value() ||
       message.is_error)) {
    return Result<Message>::failure(
        payload_error(point, "system/user message has tool-only fields"));
  }
  if (message.role == Role::assistant &&
      (message.tool_call_id.has_value() || message.is_error)) {
    return Result<Message>::failure(
        payload_error(point, "assistant message has tool-result fields"));
  }
  if (message.role == Role::tool &&
      (!message.tool_calls.empty() || !message.tool_call_id.has_value())) {
    return Result<Message>::failure(
        payload_error(point, "tool message must identify one tool call"));
  }
  return Result<Message>::success(std::move(message));
}

Json usage_json(const ModelUsage &usage) {
  Json document{{"input_tokens", usage.input_tokens},
                {"cached_input_tokens", usage.cached_input_tokens},
                {"cache_write_input_tokens", usage.cache_write_input_tokens},
                {"output_tokens", usage.output_tokens},
                {"output_tokens_per_second", usage.output_tokens_per_second}};
  if (usage.context_breakdown.has_value()) {
    const auto &breakdown = *usage.context_breakdown;
    document["context_breakdown"] = {
        {"system_tokens", breakdown.system_tokens},
        {"user_tokens", breakdown.user_tokens},
        {"assistant_tokens", breakdown.assistant_tokens},
        {"tool_tokens", breakdown.tool_tokens},
        {"tool_definition_tokens", breakdown.tool_definition_tokens},
        {"other_tokens", breakdown.other_tokens},
    };
  } else {
    document["context_breakdown"] = nullptr;
  }
  return document;
}

Json request_json(const ModelRequest &request) {
  Json messages = Json::array();
  for (const auto &message : request.messages)
    messages.push_back(message_json(message));
  Json tools = Json::array();
  for (const auto &tool : request.tools) {
    tools.push_back({{"name", tool.name},
                     {"description", tool.description},
                     {"input_schema_json", tool.input_schema_json}});
  }
  Json document{
      {"model",
       {{"provider", request.model.provider}, {"model", request.model.model}}},
      {"messages", std::move(messages)},
      {"tools", std::move(tools)},
      {"session_id", request.session_id},
      {"temperature", request.temperature},
      {"reasoning_effort",
       std::string(reasoning_effort_name(request.reasoning_effort))}};
  document["max_output_tokens"] = request.max_output_tokens.has_value()
                                      ? Json(*request.max_output_tokens)
                                      : Json(nullptr);
  return document;
}

Result<ModelRequest> parse_request(const Json &document, HookPoint point) {
  if (!document.is_object() || !document.contains("model") ||
      !document.at("model").is_object() ||
      !document.at("model").contains("provider") ||
      !document.at("model").at("provider").is_string() ||
      !document.at("model").contains("model") ||
      !document.at("model").at("model").is_string() ||
      !document.contains("messages") || !document.at("messages").is_array() ||
      !document.contains("tools") || !document.at("tools").is_array() ||
      !document.contains("temperature") ||
      !document.at("temperature").is_number() ||
      !document.contains("reasoning_effort") ||
      !document.at("reasoning_effort").is_string()) {
    return Result<ModelRequest>::failure(
        payload_error(point, "model request has invalid or missing fields"));
  }

  ModelRequest request;
  if (document.contains("session_id")) {
    if (!document.at("session_id").is_string())
      return Result<ModelRequest>::failure(
          payload_error(point, "session_id must be a string"));
    request.session_id = document.at("session_id").get<std::string>();
  }
  request.model.provider =
      document.at("model").at("provider").get<std::string>();
  request.model.model = document.at("model").at("model").get<std::string>();
  if (request.model.provider.empty() || request.model.model.empty()) {
    return Result<ModelRequest>::failure(
        payload_error(point, "model provider and id cannot be empty"));
  }
  request.temperature = document.at("temperature").get<double>();
  if (!std::isfinite(request.temperature)) {
    return Result<ModelRequest>::failure(
        payload_error(point, "model temperature must be finite"));
  }
  const auto effort = reasoning_effort_from_name(
      document.at("reasoning_effort").get<std::string>());
  if (!effort.has_value()) {
    return Result<ModelRequest>::failure(
        payload_error(point, "reasoning_effort is invalid"));
  }
  request.reasoning_effort = *effort;
  if (document.contains("max_output_tokens") &&
      !document.at("max_output_tokens").is_null()) {
    if (!document.at("max_output_tokens").is_number_unsigned()) {
      return Result<ModelRequest>::failure(
          payload_error(point, "max_output_tokens must be null or unsigned"));
    }
    request.max_output_tokens =
        document.at("max_output_tokens").get<std::size_t>();
  }
  for (const auto &message_document : document.at("messages")) {
    auto message = parse_message(message_document, point);
    if (!message)
      return Result<ModelRequest>::failure(message.error());
    request.messages.push_back(std::move(message.value()));
  }
  if (request.messages.empty() ||
      request.messages.front().role != Role::system) {
    return Result<ModelRequest>::failure(
        payload_error(point, "model request must begin with a system message"));
  }
  std::unordered_set<MessageId> message_ids;
  std::unordered_set<ToolCallId> pending_calls;
  for (const auto &message : request.messages) {
    if (!message_ids.insert(message.id).second) {
      return Result<ModelRequest>::failure(
          payload_error(point, "model request contains duplicate message ids"));
    }
    if (message.role == Role::assistant) {
      if (!pending_calls.empty()) {
        return Result<ModelRequest>::failure(payload_error(
            point, "assistant message precedes unresolved tool calls"));
      }
      for (const auto &call : message.tool_calls)
        pending_calls.insert(call.id);
    } else if (message.role == Role::tool) {
      if (!message.tool_call_id.has_value() ||
          pending_calls.erase(*message.tool_call_id) != 1) {
        return Result<ModelRequest>::failure(payload_error(
            point, "tool message does not match a pending tool call"));
      }
    } else if (!pending_calls.empty()) {
      return Result<ModelRequest>::failure(payload_error(
          point, "request message precedes unresolved tool calls"));
    }
  }
  if (!pending_calls.empty()) {
    return Result<ModelRequest>::failure(
        payload_error(point, "model request has unresolved tool calls"));
  }
  std::set<std::string> tool_names;
  for (const auto &tool : document.at("tools")) {
    if (!tool.is_object() || !tool.contains("name") ||
        !tool.at("name").is_string() || !tool.contains("description") ||
        !tool.at("description").is_string() ||
        !tool.contains("input_schema_json") ||
        !tool.at("input_schema_json").is_string()) {
      return Result<ModelRequest>::failure(
          payload_error(point, "tool definition has invalid fields"));
    }
    ToolDefinition definition{tool.at("name").get<std::string>(),
                              tool.at("description").get<std::string>(),
                              tool.at("input_schema_json").get<std::string>()};
    if (definition.name.empty() ||
        !Json::accept(definition.input_schema_json)) {
      return Result<ModelRequest>::failure(
          payload_error(point, "tool name or input schema is invalid"));
    }
    const auto schema = Json::parse(definition.input_schema_json);
    if (!schema.is_object() || !tool_names.insert(definition.name).second) {
      return Result<ModelRequest>::failure(payload_error(
          point, "tool schema is not an object or name is duplicated"));
    }
    request.tools.push_back(std::move(definition));
  }
  return Result<ModelRequest>::success(std::move(request));
}

Json response_json(const AssistantResponse &response) {
  Json calls = Json::array();
  for (const auto &call : response.tool_calls)
    calls.push_back(tool_call_json(call));
  return {{"content", response.content},
          {"tool_calls", std::move(calls)},
          {"finish_reason", finish_reason_name(response.finish_reason)},
          {"usage", usage_json(response.usage)},
          {"model_state", response.model_state}};
}

Result<AssistantResponse> parse_response(const Json &document, HookPoint point,
                                         ModelUsage usage) {
  if (!document.is_object() || !document.contains("content") ||
      !document.at("content").is_string() || !document.contains("tool_calls") ||
      !document.at("tool_calls").is_array() ||
      !document.contains("finish_reason") ||
      !document.at("finish_reason").is_string()) {
    return Result<AssistantResponse>::failure(
        payload_error(point, "model response has invalid or missing fields"));
  }
  const auto reason =
      finish_reason_from_name(document.at("finish_reason").get<std::string>());
  if (!reason.has_value()) {
    return Result<AssistantResponse>::failure(
        payload_error(point, "finish_reason is invalid"));
  }
  AssistantResponse response{
      document.at("content").get<std::string>(), {}, *reason, std::move(usage)};
  std::set<ToolCallId> call_ids;
  for (const auto &call_document : document.at("tool_calls")) {
    auto call = parse_tool_call(call_document, point);
    if (!call)
      return Result<AssistantResponse>::failure(call.error());
    if (!call_ids.insert(call.value().id).second) {
      return Result<AssistantResponse>::failure(
          payload_error(point, "response contains duplicate tool call ids"));
    }
    response.tool_calls.push_back(std::move(call.value()));
  }
  if (document.contains("model_state")) {
    if (!document.at("model_state").is_string())
      return Result<AssistantResponse>::failure(
          payload_error(point, "model_state must be a string"));
    response.model_state = document.at("model_state").get<std::string>();
  }
  return Result<AssistantResponse>::success(std::move(response));
}

Json tool_result_json(const ToolResult &result) {
  Json document{{"tool_call_id", result.tool_call_id},
                {"content", result.content},
                {"is_error", result.is_error}};
  document["model_usage"] = result.model_usage.has_value()
                                ? usage_json(*result.model_usage)
                                : Json(nullptr);
  return document;
}

Result<Json> dispatch(HookRegistry *registry, HookPoint point,
                      std::string_view turn_id, Json payload,
                      CancellationToken cancellation) {
  try {
    payload["schema_version"] = 1;
    payload["hook"] = hook_point_name(point);
    payload["turn_id"] = turn_id;
    if (registry == nullptr)
      return Result<Json>::success(std::move(payload));
    const auto result = registry->dispatch(point, payload.dump(), cancellation);
    if (!result)
      return Result<Json>::failure(result.error());
    auto replacement = Json::parse(result.value());
    if (!replacement.is_object() ||
        replacement.value("schema_version", 0) != 1 ||
        replacement.value("hook", std::string{}) != hook_point_name(point) ||
        replacement.value("turn_id", std::string{}) != turn_id) {
      return Result<Json>::failure(
          payload_error(point, "the hook envelope is missing or was modified"));
    }
    return Result<Json>::success(std::move(replacement));
  } catch (const Json::exception &error) {
    return Result<Json>::failure(payload_error(
        point, std::string("payload is invalid JSON: ") + error.what()));
  } catch (const std::exception &error) {
    return Result<Json>::failure(payload_error(
        point, std::string("payload processing failed: ") + error.what()));
  } catch (...) {
    return Result<Json>::failure(payload_error(
        point, "payload processing failed with an unknown error"));
  }
}

Result<void> validate_iteration(const Json &payload, HookPoint point,
                                std::size_t expected) {
  if (!payload.contains("iteration") ||
      !payload.at("iteration").is_number_unsigned() ||
      payload.at("iteration").get<std::size_t>() != expected) {
    return Result<void>::failure(
        payload_error(point, "iteration is missing or was modified"));
  }
  return Result<void>::success();
}

Result<void> validate_call_index(const Json &payload, HookPoint point,
                                 std::size_t expected) {
  if (!payload.contains("call_index") ||
      !payload.at("call_index").is_number_unsigned() ||
      payload.at("call_index").get<std::size_t>() != expected) {
    return Result<void>::failure(
        payload_error(point, "call_index is missing or was modified"));
  }
  return Result<void>::success();
}

} // namespace

Result<std::string>
AgentHooks::agent_turn_start(std::string_view turn_id, std::string user_input,
                             CancellationToken cancellation) const {
  auto payload =
      dispatch(registry_, HookPoint::agent_turn_start, turn_id,
               {{"user_input", std::move(user_input)}}, cancellation);
  if (!payload)
    return Result<std::string>::failure(payload.error());
  if (!payload.value().contains("user_input") ||
      !payload.value().at("user_input").is_string() ||
      payload.value().at("user_input").get<std::string>().empty()) {
    return Result<std::string>::failure(payload_error(
        HookPoint::agent_turn_start, "user_input must be a non-empty string"));
  }
  return Result<std::string>::success(
      payload.value().at("user_input").get<std::string>());
}

Result<Message>
AgentHooks::user_message_submit(std::string_view turn_id, Message message,
                                CancellationToken cancellation) const {
  const auto original_id = message.id;
  auto payload = dispatch(registry_, HookPoint::user_message_submit, turn_id,
                          {{"message", message_json(message)}}, cancellation);
  if (!payload)
    return Result<Message>::failure(payload.error());
  if (!payload.value().contains("message")) {
    return Result<Message>::failure(
        payload_error(HookPoint::user_message_submit, "message is missing"));
  }
  auto replacement = parse_message(payload.value().at("message"),
                                   HookPoint::user_message_submit);
  if (!replacement)
    return replacement;
  if (replacement.value().model_state != message.model_state ||
      (!message.model_state.empty() &&
       replacement.value().content != message.content))
    return Result<Message>::failure(payload_error(
        HookPoint::session_write, "provider continuation and its assistant "
                                  "content must remain unchanged"));
  if (replacement.value().id != original_id ||
      replacement.value().role != Role::user) {
    return Result<Message>::failure(payload_error(
        HookPoint::user_message_submit, "message id or role was modified"));
  }
  if (replacement.value().content.empty()) {
    return Result<Message>::failure(payload_error(
        HookPoint::user_message_submit, "message content cannot be empty"));
  }
  return replacement;
}

Result<ModelRequest>
AgentHooks::before_model_request(std::string_view turn_id,
                                 std::size_t iteration, ModelRequest request,
                                 CancellationToken cancellation) const {
  auto payload =
      dispatch(registry_, HookPoint::before_model_request, turn_id,
               {{"iteration", iteration}, {"request", request_json(request)}},
               cancellation);
  if (!payload)
    return Result<ModelRequest>::failure(payload.error());
  const auto iteration_validation = validate_iteration(
      payload.value(), HookPoint::before_model_request, iteration);
  if (!iteration_validation)
    return Result<ModelRequest>::failure(iteration_validation.error());
  if (!payload.value().contains("request")) {
    return Result<ModelRequest>::failure(
        payload_error(HookPoint::before_model_request, "request is missing"));
  }
  auto replacement = parse_request(payload.value().at("request"),
                                   HookPoint::before_model_request);
  if (replacement && replacement.value().session_id != request.session_id)
    return Result<ModelRequest>::failure(payload_error(
        HookPoint::before_model_request, "session_id cannot be modified"));
  return replacement;
}

Result<AssistantResponse> AgentHooks::after_model_response(
    std::string_view turn_id, std::size_t iteration, AssistantResponse response,
    CancellationToken cancellation) const {
  auto usage = response.usage;
  auto payload = dispatch(
      registry_, HookPoint::after_model_response, turn_id,
      {{"iteration", iteration}, {"response", response_json(response)}},
      cancellation);
  if (!payload)
    return Result<AssistantResponse>::failure(payload.error());
  const auto iteration_validation = validate_iteration(
      payload.value(), HookPoint::after_model_response, iteration);
  if (!iteration_validation)
    return Result<AssistantResponse>::failure(iteration_validation.error());
  if (!payload.value().contains("response")) {
    return Result<AssistantResponse>::failure(
        payload_error(HookPoint::after_model_response, "response is missing"));
  }
  auto replacement =
      parse_response(payload.value().at("response"),
                     HookPoint::after_model_response, std::move(usage));
  if (replacement && (replacement.value().model_state != response.model_state ||
                      (!response.model_state.empty() &&
                       (replacement.value().content != response.content ||
                        response_json(replacement.value()).at("tool_calls") !=
                            response_json(response).at("tool_calls")))))
    return Result<AssistantResponse>::failure(
        payload_error(HookPoint::after_model_response,
                      "provider continuation and its assistant output must "
                      "remain unchanged"));
  return replacement;
}

Result<ToolCall>
AgentHooks::before_tool_call(std::string_view turn_id, std::size_t iteration,
                             std::size_t call_index, ToolCall call,
                             CancellationToken cancellation) const {
  const auto original_id = call.id;
  auto payload = dispatch(registry_, HookPoint::before_tool_call, turn_id,
                          {{"iteration", iteration},
                           {"call_index", call_index},
                           {"call", tool_call_json(call)}},
                          cancellation);
  if (!payload)
    return Result<ToolCall>::failure(payload.error());
  const auto iteration_validation = validate_iteration(
      payload.value(), HookPoint::before_tool_call, iteration);
  if (!iteration_validation)
    return Result<ToolCall>::failure(iteration_validation.error());
  const auto index_validation = validate_call_index(
      payload.value(), HookPoint::before_tool_call, call_index);
  if (!index_validation)
    return Result<ToolCall>::failure(index_validation.error());
  if (!payload.value().contains("call")) {
    return Result<ToolCall>::failure(
        payload_error(HookPoint::before_tool_call, "call is missing"));
  }
  auto replacement =
      parse_tool_call(payload.value().at("call"), HookPoint::before_tool_call);
  if (!replacement)
    return replacement;
  if (replacement.value().id != original_id) {
    return Result<ToolCall>::failure(payload_error(
        HookPoint::before_tool_call, "tool call id was modified"));
  }
  return replacement;
}

Result<ToolResult>
AgentHooks::after_tool_result(std::string_view turn_id, std::size_t iteration,
                              std::size_t call_index,
                              std::string_view tool_name, ToolResult result,
                              CancellationToken cancellation) const {
  const auto original_id = result.tool_call_id;
  auto usage = result.model_usage;
  auto payload = dispatch(registry_, HookPoint::after_tool_result, turn_id,
                          {{"iteration", iteration},
                           {"call_index", call_index},
                           {"tool_name", tool_name},
                           {"result", tool_result_json(result)}},
                          cancellation);
  if (!payload)
    return Result<ToolResult>::failure(payload.error());
  const auto iteration_validation = validate_iteration(
      payload.value(), HookPoint::after_tool_result, iteration);
  if (!iteration_validation)
    return Result<ToolResult>::failure(iteration_validation.error());
  const auto index_validation = validate_call_index(
      payload.value(), HookPoint::after_tool_result, call_index);
  if (!index_validation)
    return Result<ToolResult>::failure(index_validation.error());
  if (!payload.value().contains("tool_name") ||
      !payload.value().at("tool_name").is_string() ||
      payload.value().at("tool_name").get<std::string>() != tool_name ||
      !payload.value().contains("result") ||
      !payload.value().at("result").is_object()) {
    return Result<ToolResult>::failure(
        payload_error(HookPoint::after_tool_result,
                      "tool_name changed or result is missing"));
  }
  const auto &document = payload.value().at("result");
  if (!document.contains("tool_call_id") ||
      !document.at("tool_call_id").is_string() ||
      document.at("tool_call_id").get<std::string>() != original_id ||
      !document.contains("content") || !document.at("content").is_string() ||
      !document.contains("is_error") || !document.at("is_error").is_boolean()) {
    return Result<ToolResult>::failure(payload_error(
        HookPoint::after_tool_result, "tool result fields are invalid"));
  }
  return Result<ToolResult>::success(
      {original_id, document.at("content").get<std::string>(),
       document.at("is_error").get<bool>(), std::move(usage)});
}

Result<Message>
AgentHooks::session_write_message(std::string_view operation,
                                  std::string_view turn_id, Message message,
                                  CancellationToken cancellation) const {
  const auto original_id = message.id;
  const auto original_role = message.role;
  const auto original_tool_call_id = message.tool_call_id;
  const auto original_tool_calls = message.tool_calls;
  auto payload =
      dispatch(registry_, HookPoint::session_write, turn_id,
               {{"operation", operation}, {"message", message_json(message)}},
               cancellation);
  if (!payload)
    return Result<Message>::failure(payload.error());
  if (!payload.value().contains("operation") ||
      !payload.value().at("operation").is_string() ||
      payload.value().at("operation").get<std::string>() != operation ||
      !payload.value().contains("message")) {
    return Result<Message>::failure(payload_error(
        HookPoint::session_write, "operation changed or message is missing"));
  }
  auto replacement =
      parse_message(payload.value().at("message"), HookPoint::session_write);
  if (!replacement)
    return replacement;
  if (replacement.value().model_state != message.model_state ||
      (!message.model_state.empty() &&
       replacement.value().content != message.content))
    return Result<Message>::failure(payload_error(
        HookPoint::session_write, "provider continuation and its assistant "
                                  "content must remain unchanged"));
  if (replacement.value().id != original_id ||
      replacement.value().role != original_role ||
      replacement.value().tool_call_id != original_tool_call_id) {
    return Result<Message>::failure(
        payload_error(HookPoint::session_write,
                      "message identity, role, or tool_call_id was modified"));
  }
  const bool tool_calls_changed =
      replacement.value().tool_calls.size() != original_tool_calls.size() ||
      !std::equal(replacement.value().tool_calls.begin(),
                  replacement.value().tool_calls.end(),
                  original_tool_calls.begin(),
                  [](const ToolCall &left, const ToolCall &right) {
                    return left.id == right.id && left.name == right.name &&
                           left.arguments_json == right.arguments_json;
                  });
  if (tool_calls_changed) {
    return Result<Message>::failure(payload_error(
        HookPoint::session_write,
        "message tool calls cannot be modified at the persistence boundary"));
  }
  if (replacement.value().role == Role::user &&
      replacement.value().content.empty()) {
    return Result<Message>::failure(payload_error(
        HookPoint::session_write, "user message content cannot be empty"));
  }
  return replacement;
}

Result<HookSessionFinish>
AgentHooks::session_write_finish(std::string_view turn_id,
                                 SessionTurnOutcome outcome, std::string detail,
                                 CancellationToken cancellation) const {
  auto payload = dispatch(registry_, HookPoint::session_write, turn_id,
                          {{"operation", "finish_turn"},
                           {"outcome", outcome_name(outcome)},
                           {"detail", std::move(detail)}},
                          cancellation);
  if (!payload)
    return Result<HookSessionFinish>::failure(payload.error());
  if (!payload.value().contains("operation") ||
      !payload.value().at("operation").is_string() ||
      payload.value().at("operation").get<std::string>() != "finish_turn" ||
      !payload.value().contains("outcome") ||
      !payload.value().at("outcome").is_string() ||
      !payload.value().contains("detail") ||
      !payload.value().at("detail").is_string()) {
    return Result<HookSessionFinish>::failure(payload_error(
        HookPoint::session_write, "finish_turn fields are invalid"));
  }
  const auto replacement =
      outcome_from_name(payload.value().at("outcome").get<std::string>());
  if (!replacement.has_value()) {
    return Result<HookSessionFinish>::failure(
        payload_error(HookPoint::session_write, "outcome is invalid"));
  }
  return Result<HookSessionFinish>::success(
      {*replacement, payload.value().at("detail").get<std::string>()});
}

Result<HookTurnEnd>
AgentHooks::agent_turn_end(std::string_view turn_id, SessionTurnOutcome outcome,
                           std::string detail,
                           CancellationToken cancellation) const {
  auto payload = dispatch(
      registry_, HookPoint::agent_turn_end, turn_id,
      {{"outcome", outcome_name(outcome)}, {"detail", std::move(detail)}},
      cancellation);
  if (!payload)
    return Result<HookTurnEnd>::failure(payload.error());
  if (!payload.value().contains("outcome") ||
      !payload.value().at("outcome").is_string() ||
      !payload.value().contains("detail") ||
      !payload.value().at("detail").is_string()) {
    return Result<HookTurnEnd>::failure(payload_error(
        HookPoint::agent_turn_end, "turn end fields are invalid"));
  }
  const auto replacement =
      outcome_from_name(payload.value().at("outcome").get<std::string>());
  if (!replacement.has_value()) {
    return Result<HookTurnEnd>::failure(
        payload_error(HookPoint::agent_turn_end, "outcome is invalid"));
  }
  return Result<HookTurnEnd>::success(
      {*replacement, payload.value().at("detail").get<std::string>()});
}

} // namespace zed::core
