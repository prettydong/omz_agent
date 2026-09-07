#include "zed/providers/opencode_go_model.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace zed::providers {

namespace {

using core::AssistantResponse;
using core::ErrorCode;
using core::FinishReason;
using core::Message;
using core::ModelRequest;
using core::Role;
using core::ToolCall;
using Json = nlohmann::json;

constexpr std::size_t kMaximumSseLineBytes = 1024 * 1024;
constexpr std::size_t kMaximumStreamBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumToolCalls = 128;

std::string next_fallback_session_id() {
  static std::atomic_uint64_t sequence{0};
  const auto nonce = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return "omz-agent-" + std::to_string(nonce) + "-" +
         std::to_string(++sequence);
}

bool is_valid_header_value(std::string_view value) {
  if (value.empty())
    return false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (character <= 0x20 || character == 0x7f)
      return false;
  }
  return true;
}

const Json *field(const Json &object, std::string_view name) {
  if (!object.is_object())
    return nullptr;
  const auto iterator = object.find(std::string(name));
  return iterator == object.end() ? nullptr : &*iterator;
}

std::string_view trim_ascii(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return value;
}

const char *role_name(Role role) {
  switch (role) {
  case Role::user:
    return "user";
  case Role::assistant:
    return "assistant";
  case Role::tool:
    return "tool";
  case Role::system:
    return "system";
  }
  return "user";
}

Json message_input(const Message &message) {
  Json object = Json::object();
  object["role"] = role_name(message.role);
  object["content"] = message.content;
  if (message.role == Role::tool && message.tool_call_id.has_value()) {
    object["type"] = "function_call_output";
    object["call_id"] = *message.tool_call_id;
    object["output"] = message.content;
    object.erase("role");
    object.erase("content");
  }
  return Json(std::move(object));
}

bool should_apply_effort(const OpenCodeGoModelInfo *model,
                         core::ReasoningEffort effort) {
  if (effort == core::ReasoningEffort::automatic)
    return false;
  return model == nullptr || supports_reasoning_effort(*model, effort);
}

std::string base_endpoint(std::string endpoint) {
  while (!endpoint.empty() && endpoint.back() == '/')
    endpoint.pop_back();
  for (const std::string_view suffix :
       {"/chat/completions", "/responses", "/messages"}) {
    if (endpoint.ends_with(suffix)) {
      endpoint.erase(endpoint.size() - suffix.size());
      break;
    }
  }
  return endpoint;
}

std::string endpoint_for(const OpenCodeGoConfig &config,
                         OpenCodeProtocol protocol) {
  const std::string base = base_endpoint(config.endpoint);
  switch (protocol) {
  case OpenCodeProtocol::responses:
    return base + "/responses";
  case OpenCodeProtocol::chat_completions:
    return base + "/chat/completions";
  case OpenCodeProtocol::messages:
    return base + "/messages";
  }
  return base + "/chat/completions";
}

std::string responses_request_json(const ModelRequest &request,
                                   const OpenCodeGoModelInfo *model) {
  Json root = Json::object();
  root["model"] = request.model.model;
  root["stream"] = true;
  root["store"] = false;
  if (request.max_output_tokens.has_value()) {
    root["max_output_tokens"] = static_cast<double>(*request.max_output_tokens);
  }
  if (model == nullptr || model->supports_temperature)
    root["temperature"] = request.temperature;
  if (should_apply_effort(model, request.reasoning_effort)) {
    root["reasoning"] = {
        {"effort", core::reasoning_effort_name(request.reasoning_effort)},
        {"summary", "auto"},
    };
    root["include"] = Json::array({"reasoning.encrypted_content"});
  }

  std::string instructions;
  Json input = Json::array();
  for (const auto &message : request.messages) {
    if (message.role == Role::system) {
      if (!instructions.empty())
        instructions += "\n\n";
      instructions += message.content;
      continue;
    }
    if (message.role == Role::assistant && !message.tool_calls.empty()) {
      if (!message.content.empty())
        input.push_back(message_input(message));
      for (const auto &call : message.tool_calls) {
        Json item = Json::object();
        item["type"] = "function_call";
        item["call_id"] = call.id;
        item["name"] = call.name;
        item["arguments"] = call.arguments_json;
        input.emplace_back(std::move(item));
      }
      continue;
    }
    input.push_back(message_input(message));
  }
  root["input"] = std::move(input);
  if (!instructions.empty())
    root["instructions"] = std::move(instructions);

  Json tools = Json::array();
  for (const auto &definition : request.tools) {
    Json tool = Json::object();
    tool["type"] = "function";
    tool["name"] = definition.name;
    tool["description"] = definition.description;
    try {
      tool["parameters"] = Json::parse(definition.input_schema_json);
    } catch (const Json::parse_error &) {
      tool["parameters"] = Json::object();
    }
    tools.emplace_back(std::move(tool));
  }
  root["tools"] = std::move(tools);
  return root.dump();
}

Json chat_message(const Message &message) {
  Json object = Json::object();
  object["role"] = role_name(message.role);
  object["content"] = message.content;
  if (message.role == Role::tool && message.tool_call_id.has_value())
    object["tool_call_id"] = *message.tool_call_id;
  if (message.role == Role::assistant && !message.tool_calls.empty()) {
    Json calls = Json::array();
    for (const auto &call : message.tool_calls) {
      calls.push_back({
          {"id", call.id},
          {"type", "function"},
          {"function",
           {{"name", call.name}, {"arguments", call.arguments_json}}},
      });
    }
    object["tool_calls"] = std::move(calls);
  }
  return object;
}

std::string chat_request_json(const ModelRequest &request,
                              const OpenCodeGoModelInfo *model) {
  Json root = Json::object();
  root["model"] = request.model.model;
  root["stream"] = true;
  root["stream_options"] = {{"include_usage", true}};
  if (request.max_output_tokens.has_value())
    root["max_tokens"] = *request.max_output_tokens;
  if (model == nullptr || model->supports_temperature)
    root["temperature"] = request.temperature;
  if (should_apply_effort(model, request.reasoning_effort)) {
    root["reasoning_effort"] =
        core::reasoning_effort_name(request.reasoning_effort);
  }

  root["messages"] = Json::array();
  for (const auto &message : request.messages)
    root["messages"].push_back(chat_message(message));

  root["tools"] = Json::array();
  for (const auto &definition : request.tools) {
    Json parameters = Json::object();
    try {
      parameters = Json::parse(definition.input_schema_json);
    } catch (const Json::parse_error &) {
    }
    root["tools"].push_back({
        {"type", "function"},
        {"function",
         {{"name", definition.name},
          {"description", definition.description},
          {"parameters", std::move(parameters)}}},
    });
  }
  return root.dump();
}

Json anthropic_content(const Message &message) {
  Json content = Json::array();
  if (!message.content.empty())
    content.push_back({{"type", "text"}, {"text", message.content}});
  if (message.role == Role::assistant) {
    for (const auto &call : message.tool_calls) {
      Json input = Json::object();
      try {
        input = Json::parse(call.arguments_json);
      } catch (const Json::parse_error &) {
      }
      content.push_back({
          {"type", "tool_use"},
          {"id", call.id},
          {"name", call.name},
          {"input", std::move(input)},
      });
    }
  }
  if (message.role == Role::tool && message.tool_call_id.has_value()) {
    content = Json::array({{
        {"type", "tool_result"},
        {"tool_use_id", *message.tool_call_id},
        {"content", message.content},
        {"is_error", message.is_error},
    }});
  }
  return content;
}

std::string messages_request_json(const ModelRequest &request,
                                  const OpenCodeGoModelInfo *model) {
  Json root = Json::object();
  root["model"] = request.model.model;
  root["stream"] = true;
  root["max_tokens"] = request.max_output_tokens.value_or(4096);

  std::string system;
  root["messages"] = Json::array();
  for (const auto &message : request.messages) {
    if (message.role == Role::system) {
      if (!system.empty())
        system += "\n\n";
      system += message.content;
      continue;
    }
    const char *role = message.role == Role::assistant ? "assistant" : "user";
    root["messages"].push_back(
        {{"role", role}, {"content", anthropic_content(message)}});
  }
  if (!system.empty())
    root["system"] = std::move(system);

  if (model == nullptr || model->supports_temperature) {
    if (request.reasoning_effort == core::ReasoningEffort::automatic ||
        request.reasoning_effort == core::ReasoningEffort::none) {
      root["temperature"] = request.temperature;
    }
  }
  if (should_apply_effort(model, request.reasoning_effort)) {
    if (request.reasoning_effort == core::ReasoningEffort::thinking) {
      root["thinking"] = {{"type", "adaptive"}};
    } else if (request.reasoning_effort == core::ReasoningEffort::none) {
      root["thinking"] = {{"type", "disabled"}};
    } else {
      root["output_config"] = {
          {"effort", core::reasoning_effort_name(request.reasoning_effort)},
      };
    }
  }

  root["tools"] = Json::array();
  for (const auto &definition : request.tools) {
    Json input_schema = Json::object();
    try {
      input_schema = Json::parse(definition.input_schema_json);
    } catch (const Json::parse_error &) {
    }
    root["tools"].push_back({
        {"name", definition.name},
        {"description", definition.description},
        {"input_schema", std::move(input_schema)},
    });
  }
  return root.dump();
}

void add_tool_call(std::vector<ToolCall> &calls, ToolCall call) {
  for (auto &existing : calls) {
    if (existing.id == call.id) {
      if (!call.name.empty())
        existing.name = std::move(call.name);
      if (!call.arguments_json.empty())
        existing.arguments_json = std::move(call.arguments_json);
      return;
    }
  }
  calls.push_back(std::move(call));
}

void append_tool_call_arguments(std::vector<ToolCall> &calls, std::string id,
                                std::string name, std::string delta) {
  for (auto &existing : calls) {
    if (existing.id == id) {
      if (!name.empty())
        existing.name = std::move(name);
      existing.arguments_json += delta;
      return;
    }
  }
  calls.push_back({std::move(id), std::move(name), std::move(delta)});
}

void parse_output_item(const Json &item, std::vector<ToolCall> &calls) {
  if (!item.is_object() || !item.contains("type") ||
      !item.at("type").is_string() ||
      item.at("type").get<std::string>() != "function_call")
    return;
  const auto *id = field(item, "call_id");
  const auto *name = field(item, "name");
  const auto *arguments = field(item, "arguments");
  if (id == nullptr || name == nullptr || arguments == nullptr ||
      !id->is_string() || !name->is_string() || !arguments->is_string())
    return;
  add_tool_call(calls, {
                           id->get<std::string>(),
                           name->get<std::string>(),
                           arguments->get<std::string>(),
                       });
}

void parse_usage(const Json &response_value, core::ModelUsage &usage) {
  const auto *usage_value = field(response_value, "usage");
  if (usage_value == nullptr || !usage_value->is_object())
    return;
  if (const auto *value = field(*usage_value, "input_tokens");
      value != nullptr && value->is_number()) {
    usage.input_tokens = value->get<core::TokenCount>();
  }
  if (const auto *value = field(*usage_value, "output_tokens");
      value != nullptr && value->is_number()) {
    usage.output_tokens = value->get<core::TokenCount>();
  }
  if (const auto *details = field(*usage_value, "input_tokens_details");
      details != nullptr && details->is_object()) {
    if (const auto *value = field(*details, "cached_tokens");
        value != nullptr && value->is_number()) {
      usage.cached_input_tokens = value->get<core::TokenCount>();
    }
  }
}

void parse_response_output(const Json &response_value,
                           AssistantResponse &response,
                           const core::StreamCallback &on_delta) {
  parse_usage(response_value, response.usage);
  const bool restore_content = response.content.empty();
  if (const auto *output = field(response_value, "output");
      output != nullptr && output->is_array()) {
    for (const auto &item : *output) {
      parse_output_item(item, response.tool_calls);
      if (!restore_content || !item.is_object())
        continue;
      const auto *item_type = field(item, "type");
      const auto *content = field(item, "content");
      if (item_type == nullptr || !item_type->is_string() ||
          item_type->get<std::string>() != "message" || content == nullptr ||
          !content->is_array()) {
        continue;
      }
      for (const auto &block : *content) {
        const auto *block_type = field(block, "type");
        const auto *text = field(block, "text");
        if (block_type != nullptr && block_type->is_string() &&
            block_type->get<std::string>() == "output_text" &&
            text != nullptr && text->is_string()) {
          const auto value = text->get<std::string>();
          response.content += value;
          if (on_delta)
            on_delta({value});
        }
      }
    }
  }
}

std::string response_error_message(const Json &response_value,
                                   std::string_view fallback) {
  const auto *error_value = field(response_value, "error");
  const auto *message =
      error_value == nullptr ? nullptr : field(*error_value, "message");
  return message == nullptr || !message->is_string()
             ? std::string(fallback)
             : message->get<std::string>();
}

core::FinishReason incomplete_finish_reason(const Json &response_value) {
  const auto *details = field(response_value, "incomplete_details");
  const auto *reason = details == nullptr ? nullptr : field(*details, "reason");
  if (reason == nullptr || !reason->is_string())
    return FinishReason::unknown;
  const auto name = reason->get<std::string>();
  if (name == "max_tokens" || name == "max_output_tokens")
    return FinishReason::length;
  if (name == "content_filter")
    return FinishReason::content_filter;
  return FinishReason::unknown;
}

core::Result<void> process_responses_sse(std::string_view line,
                                         AssistantResponse &response,
                                         const core::StreamCallback &on_delta) {
  if (!line.starts_with("data:"))
    return core::Result<void>::success();
  const auto payload = line.substr(5);
  if (payload == " [DONE]" || payload == "[DONE]")
    return core::Result<void>::success();

  Json event;
  try {
    event = Json::parse(payload);
  } catch (const Json::parse_error &error) {
    return core::Result<void>::failure({
        ErrorCode::model_error,
        "invalid SSE JSON: " + std::string(error.what()),
    });
  }
  const auto *type_value = field(event, "type");
  const std::string type = type_value == nullptr || !type_value->is_string()
                               ? std::string{}
                               : type_value->get<std::string>();
  if (type == "response.output_text.delta") {
    const auto *delta = field(event, "delta");
    if (delta != nullptr && delta->is_string()) {
      response.content += delta->get<std::string>();
      if (on_delta)
        on_delta({delta->get<std::string>()});
    }
  } else if (type == "response.output_item.done") {
    if (const auto *item = field(event, "item"); item != nullptr)
      parse_output_item(*item, response.tool_calls);
  } else if (type == "response.function_call_arguments.delta") {
    const auto *id = field(event, "call_id");
    const auto *name = field(event, "name");
    const auto *delta = field(event, "delta");
    if (id != nullptr && id->is_string() && delta != nullptr &&
        delta->is_string()) {
      append_tool_call_arguments(response.tool_calls, id->get<std::string>(),
                                 name == nullptr || !name->is_string()
                                     ? std::string{}
                                     : name->get<std::string>(),
                                 delta->get<std::string>());
    }
  } else if (type == "response.function_call_arguments.done") {
    const auto *id = field(event, "call_id");
    const auto *name = field(event, "name");
    const auto *arguments = field(event, "arguments");
    if (id != nullptr && id->is_string() && name != nullptr &&
        name->is_string() && arguments != nullptr && arguments->is_string()) {
      add_tool_call(response.tool_calls, {
                                             id->get<std::string>(),
                                             name->get<std::string>(),
                                             arguments->get<std::string>(),
                                         });
    }
  } else if (type == "response.completed") {
    const auto *response_value = field(event, "response");
    if (response_value == nullptr || !response_value->is_object()) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned a completed event without response details",
      });
    }
    if (const auto *status = field(*response_value, "status");
        status != nullptr && status->is_string() &&
        status->get<std::string>() != "completed") {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned a completed event with status " +
              status->get<std::string>(),
      });
    }
    parse_response_output(*response_value, response, on_delta);
    response.finish_reason = response.tool_calls.empty()
                                 ? FinishReason::stop
                                 : FinishReason::tool_calls;
  } else if (type == "response.incomplete") {
    const auto *response_value = field(event, "response");
    if (response_value == nullptr || !response_value->is_object()) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned an incomplete response without response details",
      });
    }
    parse_response_output(*response_value, response, on_delta);
    response.finish_reason = incomplete_finish_reason(*response_value);
    if (response.finish_reason == FinishReason::unknown) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned an incomplete response with an unknown reason",
      });
    }
  } else if (type == "response.failed") {
    const auto *response_value = field(event, "response");
    return core::Result<void>::failure({
        ErrorCode::model_error,
        response_value == nullptr
            ? "OpenCode response failed"
            : response_error_message(*response_value,
                                     "OpenCode response failed"),
    });
  } else if (type == "error") {
    const auto *error_value = field(event, "error");
    const auto *message =
        error_value == nullptr ? nullptr : field(*error_value, "message");
    return core::Result<void>::failure({
        ErrorCode::model_error,
        message == nullptr || !message->is_string()
            ? "OpenCode returned an error"
            : message->get<std::string>(),
    });
  }
  return core::Result<void>::success();
}

FinishReason finish_reason_from_chat(std::string_view reason) {
  if (reason == "stop")
    return FinishReason::stop;
  if (reason == "tool_calls" || reason == "function_call")
    return FinishReason::tool_calls;
  if (reason == "length")
    return FinishReason::length;
  if (reason == "content_filter")
    return FinishReason::content_filter;
  return FinishReason::unknown;
}

void parse_chat_usage(const Json &value, core::ModelUsage &usage) {
  const auto *usage_value = field(value, "usage");
  if (usage_value == nullptr || !usage_value->is_object())
    return;
  if (const auto *tokens = field(*usage_value, "prompt_tokens");
      tokens != nullptr && tokens->is_number()) {
    usage.input_tokens = tokens->get<core::TokenCount>();
  }
  if (const auto *tokens = field(*usage_value, "completion_tokens");
      tokens != nullptr && tokens->is_number()) {
    usage.output_tokens = tokens->get<core::TokenCount>();
  }
  if (const auto *details = field(*usage_value, "prompt_tokens_details");
      details != nullptr && details->is_object()) {
    if (const auto *tokens = field(*details, "cached_tokens");
        tokens != nullptr && tokens->is_number()) {
      usage.cached_input_tokens = tokens->get<core::TokenCount>();
    }
  }
}

struct ChatStreamState {
  AssistantResponse response;
  std::vector<ToolCall> indexed_calls;
};

core::Result<ToolCall *> chat_tool_call(ChatStreamState &state,
                                        std::size_t index) {
  if (index >= kMaximumToolCalls) {
    return core::Result<ToolCall *>::failure({
        ErrorCode::model_error,
        "OpenCode returned a tool call index above the limit of " +
            std::to_string(kMaximumToolCalls),
    });
  }
  if (state.indexed_calls.size() <= index)
    state.indexed_calls.resize(index + 1);
  return core::Result<ToolCall *>::success(&state.indexed_calls[index]);
}

core::Result<void> process_chat_sse(std::string_view line,
                                    ChatStreamState &state,
                                    const core::StreamCallback &on_delta) {
  if (!line.starts_with("data:"))
    return core::Result<void>::success();
  const auto payload = trim_ascii(line.substr(5));
  if (payload == "[DONE]")
    return core::Result<void>::success();

  Json event;
  try {
    event = Json::parse(payload);
  } catch (const Json::parse_error &error) {
    return core::Result<void>::failure({
        ErrorCode::model_error,
        "invalid Chat Completions SSE JSON: " + std::string(error.what()),
    });
  }
  if (const auto *error_value = field(event, "error"); error_value != nullptr) {
    const auto *message = field(*error_value, "message");
    return core::Result<void>::failure({
        ErrorCode::model_error,
        message != nullptr && message->is_string()
            ? message->get<std::string>()
            : "OpenCode Chat Completions request failed",
    });
  }
  parse_chat_usage(event, state.response.usage);
  const auto *choices = field(event, "choices");
  if (choices == nullptr || !choices->is_array())
    return core::Result<void>::success();

  for (const auto &choice : *choices) {
    if (const auto *delta = field(choice, "delta");
        delta != nullptr && delta->is_object()) {
      if (const auto *content = field(*delta, "content");
          content != nullptr && content->is_string()) {
        const auto text = content->get<std::string>();
        state.response.content += text;
        if (on_delta)
          on_delta({text});
      }
      if (const auto *calls = field(*delta, "tool_calls");
          calls != nullptr && calls->is_array()) {
        for (const auto &call_delta : *calls) {
          const auto *index_value = field(call_delta, "index");
          if (index_value == nullptr || !index_value->is_number_unsigned())
            continue;
          const auto raw_index = index_value->get<std::uint64_t>();
          if (raw_index >= kMaximumToolCalls) {
            return core::Result<void>::failure({
                ErrorCode::model_error,
                "OpenCode returned a tool call index above the limit of " +
                    std::to_string(kMaximumToolCalls),
            });
          }
          const auto call_result =
              chat_tool_call(state, static_cast<std::size_t>(raw_index));
          if (!call_result)
            return core::Result<void>::failure(call_result.error());
          auto &call = *call_result.value();
          if (const auto *id = field(call_delta, "id");
              id != nullptr && id->is_string()) {
            call.id = id->get<std::string>();
          }
          if (const auto *function = field(call_delta, "function");
              function != nullptr && function->is_object()) {
            if (const auto *name = field(*function, "name");
                name != nullptr && name->is_string()) {
              call.name += name->get<std::string>();
            }
            if (const auto *arguments = field(*function, "arguments");
                arguments != nullptr && arguments->is_string()) {
              call.arguments_json += arguments->get<std::string>();
            }
          }
        }
      }
    }
    if (const auto *reason = field(choice, "finish_reason");
        reason != nullptr && reason->is_string()) {
      state.response.finish_reason =
          finish_reason_from_chat(reason->get<std::string>());
    }
  }
  return core::Result<void>::success();
}

FinishReason finish_reason_from_messages(std::string_view reason) {
  if (reason == "end_turn" || reason == "stop_sequence" ||
      reason == "pause_turn")
    return FinishReason::stop;
  if (reason == "tool_use")
    return FinishReason::tool_calls;
  if (reason == "max_tokens")
    return FinishReason::length;
  if (reason == "refusal")
    return FinishReason::content_filter;
  return FinishReason::unknown;
}

struct AnthropicBlock {
  std::string id;
  std::string name;
  std::string arguments;
};

struct MessagesStreamState {
  AssistantResponse response;
  std::unordered_map<std::size_t, AnthropicBlock> blocks;
  bool message_stopped{false};
};

void parse_anthropic_usage(const Json &usage_value, core::ModelUsage &usage) {
  if (!usage_value.is_object())
    return;
  if (const auto *tokens = field(usage_value, "input_tokens");
      tokens != nullptr && tokens->is_number()) {
    usage.input_tokens = tokens->get<core::TokenCount>();
  }
  if (const auto *tokens = field(usage_value, "output_tokens");
      tokens != nullptr && tokens->is_number()) {
    usage.output_tokens = tokens->get<core::TokenCount>();
  }
  core::TokenCount cached{};
  if (const auto *tokens = field(usage_value, "cache_read_input_tokens");
      tokens != nullptr && tokens->is_number()) {
    cached += tokens->get<core::TokenCount>();
  }
  if (const auto *tokens = field(usage_value, "cache_creation_input_tokens");
      tokens != nullptr && tokens->is_number()) {
    cached += tokens->get<core::TokenCount>();
  }
  if (cached > 0)
    usage.cached_input_tokens = cached;
}

core::Result<void> process_messages_sse(std::string_view line,
                                        MessagesStreamState &state,
                                        const core::StreamCallback &on_delta) {
  if (!line.starts_with("data:"))
    return core::Result<void>::success();
  const auto payload = trim_ascii(line.substr(5));
  if (payload.empty() || payload == "[DONE]")
    return core::Result<void>::success();

  Json event;
  try {
    event = Json::parse(payload);
  } catch (const Json::parse_error &error) {
    return core::Result<void>::failure({
        ErrorCode::model_error,
        "invalid Messages SSE JSON: " + std::string(error.what()),
    });
  }
  const auto *type_value = field(event, "type");
  const std::string type = type_value != nullptr && type_value->is_string()
                               ? type_value->get<std::string>()
                               : std::string{};
  if (type == "error") {
    const auto *error_value = field(event, "error");
    const auto *message =
        error_value == nullptr ? nullptr : field(*error_value, "message");
    return core::Result<void>::failure({
        ErrorCode::model_error,
        message != nullptr && message->is_string()
            ? message->get<std::string>()
            : "OpenCode Messages request failed",
    });
  }
  if (type == "message_start") {
    if (const auto *message = field(event, "message");
        message != nullptr && message->is_object()) {
      if (const auto *usage = field(*message, "usage"); usage != nullptr)
        parse_anthropic_usage(*usage, state.response.usage);
    }
  } else if (type == "content_block_start") {
    const auto *index_value = field(event, "index");
    const auto *block = field(event, "content_block");
    if (index_value != nullptr && index_value->is_number_unsigned() &&
        block != nullptr && block->is_object()) {
      const auto raw_index = index_value->get<std::uint64_t>();
      if (raw_index >= kMaximumToolCalls) {
        return core::Result<void>::failure({
            ErrorCode::model_error,
            "OpenCode returned a content block index above the limit of " +
                std::to_string(kMaximumToolCalls),
        });
      }
      const auto index = static_cast<std::size_t>(raw_index);
      if (const auto *block_type = field(*block, "type");
          block_type != nullptr && block_type->is_string() &&
          block_type->get<std::string>() == "tool_use") {
        auto &tool = state.blocks[index];
        if (const auto *id = field(*block, "id");
            id != nullptr && id->is_string())
          tool.id = id->get<std::string>();
        if (const auto *name = field(*block, "name");
            name != nullptr && name->is_string())
          tool.name = name->get<std::string>();
        if (const auto *input = field(*block, "input");
            input != nullptr && input->is_object() && !input->empty())
          tool.arguments = input->dump();
      }
    }
  } else if (type == "content_block_delta") {
    const auto *index_value = field(event, "index");
    const auto *delta = field(event, "delta");
    if (index_value != nullptr && index_value->is_number_unsigned() &&
        delta != nullptr && delta->is_object()) {
      const auto raw_index = index_value->get<std::uint64_t>();
      if (raw_index >= kMaximumToolCalls) {
        return core::Result<void>::failure({
            ErrorCode::model_error,
            "OpenCode returned a content block index above the limit of " +
                std::to_string(kMaximumToolCalls),
        });
      }
      const auto index = static_cast<std::size_t>(raw_index);
      const auto *delta_type = field(*delta, "type");
      const std::string delta_name =
          delta_type != nullptr && delta_type->is_string()
              ? delta_type->get<std::string>()
              : std::string{};
      if (delta_name == "text_delta") {
        if (const auto *text = field(*delta, "text");
            text != nullptr && text->is_string()) {
          const auto value = text->get<std::string>();
          state.response.content += value;
          if (on_delta)
            on_delta({value});
        }
      } else if (delta_name == "input_json_delta") {
        if (const auto *partial = field(*delta, "partial_json");
            partial != nullptr && partial->is_string()) {
          state.blocks[index].arguments += partial->get<std::string>();
        }
      }
    }
  } else if (type == "content_block_stop") {
    const auto *index_value = field(event, "index");
    if (index_value != nullptr && index_value->is_number_unsigned()) {
      const auto raw_index = index_value->get<std::uint64_t>();
      if (raw_index >= kMaximumToolCalls) {
        return core::Result<void>::failure({
            ErrorCode::model_error,
            "OpenCode returned a content block index above the limit of " +
                std::to_string(kMaximumToolCalls),
        });
      }
      const auto iterator =
          state.blocks.find(static_cast<std::size_t>(raw_index));
      if (iterator != state.blocks.end()) {
        auto arguments = iterator->second.arguments;
        if (arguments.empty())
          arguments = "{}";
        add_tool_call(
            state.response.tool_calls,
            {iterator->second.id, iterator->second.name, std::move(arguments)});
        state.blocks.erase(iterator);
      }
    }
  } else if (type == "message_delta") {
    if (const auto *delta = field(event, "delta");
        delta != nullptr && delta->is_object()) {
      if (const auto *reason = field(*delta, "stop_reason");
          reason != nullptr && reason->is_string()) {
        state.response.finish_reason =
            finish_reason_from_messages(reason->get<std::string>());
      }
    }
    if (const auto *usage = field(event, "usage"); usage != nullptr)
      parse_anthropic_usage(*usage, state.response.usage);
  } else if (type == "message_stop") {
    state.message_stopped = true;
  }
  return core::Result<void>::success();
}

core::Result<void> validate_tool_calls(const std::vector<ToolCall> &calls) {
  if (calls.size() > kMaximumToolCalls) {
    return core::Result<void>::failure({
        ErrorCode::model_error,
        "OpenCode returned more than " + std::to_string(kMaximumToolCalls) +
            " tool calls",
    });
  }
  for (const auto &call : calls) {
    if (call.id.empty() || call.name.empty() || call.arguments_json.empty()) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned an incomplete function call",
      });
    }
    try {
      const auto arguments = Json::parse(call.arguments_json);
      if (arguments.is_object())
        continue;
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned invalid function arguments for " + call.name +
              ": expected object",
      });
    } catch (const Json::parse_error &error) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned invalid function arguments for " + call.name +
              ": " + error.what(),
      });
    }
  }
  return core::Result<void>::success();
}

class LineBuffer {
public:
  explicit LineBuffer(
      const std::function<core::Result<void>(std::string_view)> &on_line)
      : on_line_(on_line) {}

  core::Result<void> append(std::string_view chunk) {
    if (chunk.size() > kMaximumStreamBytes - total_bytes_) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode response exceeded the 16 MiB stream limit",
      });
    }
    total_bytes_ += chunk.size();
    pending_.append(chunk);
    return process(false);
  }

  core::Result<void> flush() { return process(true); }

private:
  core::Result<void> process(bool flush) {
    while (true) {
      const auto newline = pending_.find('\n');
      if (newline == std::string::npos)
        break;
      if (newline > kMaximumSseLineBytes) {
        return core::Result<void>::failure({
            ErrorCode::model_error,
            "OpenCode response contained an SSE line above the 1 MiB limit",
        });
      }
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto processed = on_line_(line);
      if (!processed)
        return processed;
    }
    if (pending_.size() > kMaximumSseLineBytes) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode response contained an SSE line above the 1 MiB limit",
      });
    }
    if (flush && !pending_.empty()) {
      if (pending_.back() == '\r')
        pending_.pop_back();
      const auto processed = on_line_(pending_);
      pending_.clear();
      if (!processed)
        return processed;
    }
    return core::Result<void>::success();
  }

  const std::function<core::Result<void>(std::string_view)> &on_line_;
  std::string pending_;
  std::size_t total_bytes_{};
};

struct CurlTransfer {
  CurlTransfer() = default;

  ~CurlTransfer() {
    if (added && multi != nullptr && easy != nullptr)
      static_cast<void>(curl_multi_remove_handle(multi, easy));
    if (headers != nullptr)
      curl_slist_free_all(headers);
    if (easy != nullptr)
      curl_easy_cleanup(easy);
    if (multi != nullptr)
      curl_multi_cleanup(multi);
  }

  CurlTransfer(const CurlTransfer &) = delete;
  CurlTransfer &operator=(const CurlTransfer &) = delete;

  CURL *easy{nullptr};
  CURLM *multi{nullptr};
  curl_slist *headers{nullptr};
  bool added{false};
};

void release_unused_heap_memory() noexcept {
#if defined(__APPLE__)
  constexpr std::size_t kMinimumReclaimableBytes = 8 * 1024 * 1024;
  auto *zone = malloc_default_zone();
  malloc_statistics_t statistics{};
  malloc_zone_statistics(zone, &statistics);
  if (statistics.size_allocated >= statistics.size_in_use &&
      statistics.size_allocated - statistics.size_in_use >=
          kMinimumReclaimableBytes) {
    static_cast<void>(malloc_zone_pressure_relief(
        zone, statistics.size_allocated - statistics.size_in_use));
  }
#elif defined(__GLIBC__)
  constexpr std::size_t kMinimumReclaimableBytes = 8 * 1024 * 1024;
  const auto statistics = mallinfo2();
  if (statistics.fordblks >= kMinimumReclaimableBytes)
    static_cast<void>(malloc_trim(0));
#endif
}

struct HeapPressureRelief {
  ~HeapPressureRelief() { release_unused_heap_memory(); }
};

struct CurlTransferState {
  LineBuffer &lines;
  core::CancellationToken cancellation;
  std::chrono::steady_clock::time_point deadline;
  std::optional<core::Error> callback_error;
  long response_status{};
  bool receive_body{false};
  std::optional<std::chrono::milliseconds> retry_after;
};

struct HttpAttemptResult {
  core::Result<void> result;
  long status{};
  std::optional<std::chrono::milliseconds> retry_after;
  bool retryable_response{false};
};

core::Result<void> initialize_libcurl() {
  static std::once_flag once;
  static CURLcode result = CURLE_FAILED_INIT;
  std::call_once(once, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
  if (result != CURLE_OK) {
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot initialize in-process HTTP client"});
  }
  return core::Result<void>::success();
}

bool append_header(CurlTransfer &transfer, std::string_view header) {
  if (header.find_first_of("\r\n") != std::string_view::npos)
    return false;
  const std::string owned(header);
  auto *updated = curl_slist_append(transfer.headers, owned.c_str());
  if (updated == nullptr)
    return false;
  transfer.headers = updated;
  return true;
}

std::size_t receive_http_body(char *data, std::size_t size, std::size_t count,
                              void *user_data) noexcept {
  auto &state = *static_cast<CurlTransferState *>(user_data);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    state.callback_error =
        core::Error{ErrorCode::model_error, "OpenCode response size overflow"};
    return 0;
  }
  const auto bytes = size * count;
  if (!state.receive_body)
    return bytes;
  try {
    const auto processed = state.lines.append(std::string_view(data, bytes));
    if (!processed) {
      state.callback_error = processed.error();
      return 0;
    }
    return bytes;
  } catch (...) {
    state.callback_error =
        core::Error{ErrorCode::model_error,
                    "OpenCode response callback failed with an unknown error"};
    return 0;
  }
}

bool starts_with_ascii_case_insensitive(std::string_view value,
                                        std::string_view prefix) {
  if (value.size() < prefix.size())
    return false;
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    const auto left = static_cast<unsigned char>(value[index]);
    const auto right = static_cast<unsigned char>(prefix[index]);
    const auto lower_left =
        left >= 'A' && left <= 'Z' ? left + ('a' - 'A') : left;
    const auto lower_right =
        right >= 'A' && right <= 'Z' ? right + ('a' - 'A') : right;
    if (lower_left != lower_right)
      return false;
  }
  return true;
}

std::optional<std::chrono::milliseconds>
parse_retry_after(std::string_view value) {
  value = trim_ascii(value);
  if (value.empty())
    return std::nullopt;
  using MillisecondsRep = std::chrono::milliseconds::rep;
  constexpr auto kMaximumMilliseconds =
      std::numeric_limits<MillisecondsRep>::max();
  constexpr MillisecondsRep kMillisecondsPerSecond = 1000;
  const bool decimal_seconds =
      std::all_of(value.begin(), value.end(), [](char character) {
        return character >= '0' && character <= '9';
      });
  if (decimal_seconds) {
    MillisecondsRep seconds{};
    for (const char character : value) {
      const auto digit = static_cast<MillisecondsRep>(character - '0');
      if (seconds > (kMaximumMilliseconds - digit) / 10)
        return std::chrono::milliseconds::max();
      seconds = seconds * 10 + digit;
    }
    if (seconds > kMaximumMilliseconds / kMillisecondsPerSecond)
      return std::chrono::milliseconds::max();
    return std::chrono::milliseconds(seconds * kMillisecondsPerSecond);
  }
  const auto retry_time = curl_getdate(std::string(value).c_str(), nullptr);
  if (retry_time == -1)
    return std::nullopt;
  const auto now = std::time(nullptr);
  if (retry_time <= now)
    return std::chrono::milliseconds::zero();
  const auto seconds_until = static_cast<std::uint64_t>(retry_time - now);
  if (seconds_until > static_cast<std::uint64_t>(
                          std::chrono::milliseconds::max().count() / 1000))
    return std::chrono::milliseconds::max();
  return std::chrono::seconds(seconds_until);
}

std::size_t receive_http_header(char *data, std::size_t size, std::size_t count,
                                void *user_data) noexcept {
  auto &state = *static_cast<CurlTransferState *>(user_data);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
    return 0;
  try {
    const std::string_view header(data, size * count);
    if (header.starts_with("HTTP/")) {
      const auto first_space = header.find(' ');
      if (first_space != std::string_view::npos &&
          header.size() >= first_space + 4) {
        const auto code = header.substr(first_space + 1, 3);
        if (code[0] >= '0' && code[0] <= '9' && code[1] >= '0' &&
            code[1] <= '9' && code[2] >= '0' && code[2] <= '9') {
          state.response_status =
              (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
          state.receive_body =
              state.response_status >= 200 && state.response_status < 300;
          state.retry_after.reset();
        }
      }
    } else if (starts_with_ascii_case_insensitive(header, "Retry-After:")) {
      state.retry_after = parse_retry_after(header.substr(12));
    }
    return size * count;
  } catch (...) {
    state.callback_error = core::Error{
        ErrorCode::model_error,
        "OpenCode response header callback failed with an unknown error"};
    return 0;
  }
}

int inspect_http_progress(void *user_data, curl_off_t, curl_off_t, curl_off_t,
                          curl_off_t) noexcept {
  const auto &state = *static_cast<CurlTransferState *>(user_data);
  return state.cancellation.is_cancelled() ||
                 std::chrono::steady_clock::now() >= state.deadline
             ? 1
             : 0;
}

HttpAttemptResult run_http_request(
    const OpenCodeGoConfig &config, std::string_view endpoint,
    const std::vector<std::string> &extra_headers, std::string_view body,
    core::CancellationToken cancellation,
    std::chrono::steady_clock::time_point deadline,
    const std::function<core::Result<void>(std::string_view)> &on_line) {
  const auto initialized = initialize_libcurl();
  if (!initialized)
    return {initialized, 0, std::nullopt};
  if (body.size() >
      static_cast<std::size_t>(std::numeric_limits<curl_off_t>::max())) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error, "OpenCode request body is too large"}),
            0, std::nullopt};
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) {
    return {core::Result<void>::failure(
                {ErrorCode::timeout, "OpenCode request timed out"}),
            0, std::nullopt};
  }

  HeapPressureRelief heap_pressure_relief;
  CurlTransfer transfer;
  transfer.easy = curl_easy_init();
  transfer.multi = curl_multi_init();
  if (transfer.easy == nullptr || transfer.multi == nullptr) {
    return {
        core::Result<void>::failure(
            {ErrorCode::model_error, "cannot create in-process HTTP request"}),
        0, std::nullopt};
  }
  const std::string owned_endpoint(endpoint);
  const std::string authorization = "Authorization: Bearer " + config.api_key;
  if (!append_header(transfer, authorization) ||
      !append_header(transfer, "Content-Type: application/json") ||
      !append_header(transfer, "Accept: text/event-stream") ||
      !append_header(transfer, "Expect:")) {
    return {
        core::Result<void>::failure(
            {ErrorCode::model_error, "cannot configure HTTP request headers"}),
        0, std::nullopt};
  }
  for (const auto &header : extra_headers) {
    if (!append_header(transfer, header)) {
      return {core::Result<void>::failure(
                  {ErrorCode::model_error,
                   "cannot configure HTTP request headers"}),
              0, std::nullopt};
    }
  }

  const auto timeout_value = static_cast<std::size_t>(
      std::max(
          std::chrono::milliseconds(1),
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))
          .count());
  const auto timeout_ms = static_cast<long>(
      std::min(timeout_value, static_cast<std::size_t>(LONG_MAX)));
  LineBuffer line_buffer(on_line);
  CurlTransferState state{
      line_buffer,  std::move(cancellation), deadline, std::nullopt, 0, false,
      std::nullopt,
  };
  const auto configured =
      curl_easy_setopt(transfer.easy, CURLOPT_URL, owned_endpoint.c_str()) ==
          CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_POST, 1L) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_POSTFIELDS, body.data()) ==
          CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_POSTFIELDSIZE_LARGE,
                       static_cast<curl_off_t>(body.size())) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_HTTPHEADER, transfer.headers) ==
          CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_WRITEFUNCTION,
                       receive_http_body) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_WRITEDATA, &state) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_HEADERFUNCTION,
                       receive_http_header) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_HEADERDATA, &state) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_XFERINFOFUNCTION,
                       inspect_http_progress) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_XFERINFODATA, &state) ==
          CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_NOPROGRESS, 0L) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_TIMEOUT_MS, timeout_ms) ==
          CURLE_OK &&
      curl_easy_setopt(transfer.easy, CURLOPT_HTTP_VERSION,
                       CURL_HTTP_VERSION_2TLS) == CURLE_OK;
  if (!configured) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error,
                 "cannot configure in-process HTTP request"}),
            0, std::nullopt};
  }

  const auto added = curl_multi_add_handle(transfer.multi, transfer.easy);
  if (added != CURLM_OK) {
    return {
        core::Result<void>::failure(
            {ErrorCode::model_error, "cannot start in-process HTTP request"}),
        0, std::nullopt};
  }
  transfer.added = true;

  int running = 0;
  CURLMcode multi_result = CURLM_OK;
  auto next_memory_relief =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  do {
    multi_result = curl_multi_perform(transfer.multi, &running);
  } while (multi_result == CURLM_CALL_MULTI_PERFORM);
  while (multi_result == CURLM_OK && running > 0 &&
         !state.cancellation.is_cancelled() &&
         std::chrono::steady_clock::now() < state.deadline) {
    int descriptors = 0;
    multi_result =
        curl_multi_poll(transfer.multi, nullptr, 0, 50, &descriptors);
    if (multi_result != CURLM_OK)
      break;
    do {
      multi_result = curl_multi_perform(transfer.multi, &running);
    } while (multi_result == CURLM_CALL_MULTI_PERFORM);
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_memory_relief) {
      release_unused_heap_memory();
      next_memory_relief = now + std::chrono::seconds(1);
    }
  }

  if (state.callback_error.has_value())
    return {core::Result<void>::failure(*state.callback_error),
            state.response_status, state.retry_after};
  if (state.cancellation.is_cancelled()) {
    return {core::Result<void>::failure(
                {ErrorCode::cancelled, "model request cancelled"}),
            state.response_status, state.retry_after};
  }
  if (std::chrono::steady_clock::now() >= state.deadline) {
    return {core::Result<void>::failure(
                {ErrorCode::timeout, "OpenCode request timed out"}),
            state.response_status, state.retry_after};
  }
  if (multi_result != CURLM_OK) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error, "in-process HTTP polling failed"}),
            state.response_status, state.retry_after};
  }

  CURLcode transfer_result = CURLE_FAILED_INIT;
  bool transfer_finished = false;
  int messages_remaining = 0;
  while (const auto *message =
             curl_multi_info_read(transfer.multi, &messages_remaining)) {
    if (message->msg == CURLMSG_DONE && message->easy_handle == transfer.easy) {
      transfer_result = message->data.result;
      transfer_finished = true;
      break;
    }
  }
  if (!transfer_finished) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error,
                 "in-process HTTP request ended without a completion status"}),
            state.response_status, state.retry_after};
  }
  if (transfer_result == CURLE_OPERATION_TIMEDOUT) {
    return {core::Result<void>::failure(
                {ErrorCode::timeout, "OpenCode request timed out"}),
            state.response_status, state.retry_after};
  }
  if (transfer_result != CURLE_OK) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error,
                 "OpenCode HTTP request failed: " +
                     std::string(curl_easy_strerror(transfer_result))}),
            state.response_status, state.retry_after};
  }

  long status = 0;
  if (curl_easy_getinfo(transfer.easy, CURLINFO_RESPONSE_CODE, &status) !=
      CURLE_OK) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error, "cannot read OpenCode HTTP status"}),
            state.response_status, state.retry_after};
  }
  if (status < 200 || status >= 300) {
    return {core::Result<void>::failure(
                {ErrorCode::model_error,
                 "OpenCode HTTP request failed with status " +
                     std::to_string(status)}),
            status, state.retry_after,
            status == 429 || status == 500 || status == 502 || status == 503 ||
                status == 504};
  }
  return {line_buffer.flush(), status, state.retry_after};
}

core::Result<void>
wait_for_retry(std::chrono::milliseconds delay,
               std::chrono::steady_clock::time_point deadline,
               const core::CancellationToken &cancellation) {
  if (cancellation.is_cancelled()) {
    return core::Result<void>::failure(
        {ErrorCode::cancelled, "model request cancelled"});
  }
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline ||
      delay >= std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                     now)) {
    return core::Result<void>::failure(
        {ErrorCode::timeout, "OpenCode request timed out"});
  }
  const auto wake_at = now + delay;
  while (std::chrono::steady_clock::now() < wake_at) {
    if (cancellation.is_cancelled()) {
      return core::Result<void>::failure(
          {ErrorCode::cancelled, "model request cancelled"});
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return core::Result<void>::failure(
          {ErrorCode::timeout, "OpenCode request timed out"});
    }
    const auto remaining = wake_at - std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::min(
        std::chrono::milliseconds(25),
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining)));
  }
  return core::Result<void>::success();
}

core::Error retry_error(core::Error error, long status, int attempts) {
  error.message += " (HTTP " + std::to_string(status) + "; attempted " +
                   std::to_string(attempts) + " times)";
  return error;
}

} // namespace

OpenCodeGoModel::OpenCodeGoModel(OpenCodeGoConfig config)
    : config_(std::move(config)),
      fallback_session_id_(next_fallback_session_id()) {
  if (config_.models.empty())
    config_.models = default_opencode_go_models();
}

core::ModelCapabilities OpenCodeGoModel::capabilities() const {
  return {
      1'000'000, true, true, true, true,
  };
}

const std::vector<OpenCodeGoModelInfo> &OpenCodeGoModel::models() const {
  return config_.models;
}

void OpenCodeGoModel::set_models(std::vector<OpenCodeGoModelInfo> models) {
  config_.models =
      models.empty() ? default_opencode_go_models() : std::move(models);
}

core::Result<core::AssistantResponse>
OpenCodeGoModel::complete(const ModelRequest &request,
                          const core::StreamCallback &on_delta,
                          core::CancellationToken cancellation) {
  if (config_.api_key.empty())
    return core::Result<AssistantResponse>::failure(
        {ErrorCode::invalid_argument, "OpenCode Go API key is empty"});
  if (request.model.model.empty())
    return core::Result<AssistantResponse>::failure(
        {ErrorCode::invalid_argument, "model id is empty"});

  const std::string &session_id =
      request.session_id.empty() ? fallback_session_id_ : request.session_id;
  if (!is_valid_header_value(session_id)) {
    return core::Result<AssistantResponse>::failure(
        {ErrorCode::invalid_argument, "OpenCode session id is invalid"});
  }
  const std::vector<std::string> request_headers = {
      "User-Agent: omz-agent/0.2",
      "x-opencode-session: " + session_id,
  };

  const auto *model_info =
      find_opencode_go_model(config_.models, request.model.model);
  const auto protocol = model_info == nullptr
                            ? infer_open_code_protocol(request.model.model)
                            : model_info->protocol;
  const std::string endpoint = endpoint_for(config_, protocol);

  std::string body;
  auto headers = request_headers;
  if (protocol == OpenCodeProtocol::responses) {
    body = responses_request_json(request, model_info);
  } else if (protocol == OpenCodeProtocol::chat_completions) {
    body = chat_request_json(request, model_info);
  } else {
    body = messages_request_json(request, model_info);
    headers.push_back("x-api-key: " + config_.api_key);
    headers.push_back("anthropic-version: 2023-06-01");
  }

  using MillisecondsRep = std::chrono::milliseconds::rep;
  const auto started_at = std::chrono::steady_clock::now();
  const auto maximum_timeout = static_cast<std::size_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::time_point::max() - started_at)
          .count());
  const auto timeout_value = std::max<std::size_t>(
      1, std::min(config_.request_timeout_ms, maximum_timeout));
  const auto deadline =
      started_at +
      std::chrono::milliseconds(static_cast<MillisecondsRep>(timeout_value));
  AssistantResponse response;
  for (int retry = 0; retry <= 2; ++retry) {
    if (cancellation.is_cancelled()) {
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::cancelled, "model request cancelled"});
    }
    AssistantResponse attempt_response;
    HttpAttemptResult attempt{core::Result<void>::success(), 0, std::nullopt};
    if (protocol == OpenCodeProtocol::responses) {
      attempt = run_http_request(config_, endpoint, headers, body, cancellation,
                                 deadline, [&](std::string_view line) {
                                   return process_responses_sse(
                                       line, attempt_response, on_delta);
                                 });
    } else if (protocol == OpenCodeProtocol::chat_completions) {
      ChatStreamState state;
      attempt =
          run_http_request(config_, endpoint, headers, body, cancellation,
                           deadline, [&](std::string_view line) {
                             return process_chat_sse(line, state, on_delta);
                           });
      state.response.tool_calls = std::move(state.indexed_calls);
      attempt_response = std::move(state.response);
    } else {
      MessagesStreamState state;
      attempt =
          run_http_request(config_, endpoint, headers, body, cancellation,
                           deadline, [&](std::string_view line) {
                             return process_messages_sse(line, state, on_delta);
                           });
      attempt_response = std::move(state.response);
      if (attempt.result && !state.message_stopped) {
        attempt.result = core::Result<void>::failure({
            ErrorCode::model_error,
            "OpenCode Messages stream ended without message_stop",
        });
      }
    }
    if (attempt.result) {
      response = std::move(attempt_response);
      break;
    }
    if (cancellation.is_cancelled()) {
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::cancelled, "model request cancelled"});
    }
    if (!attempt.retryable_response || retry == 2) {
      const auto error =
          retry == 2 && attempt.retryable_response
              ? retry_error(attempt.result.error(), attempt.status, retry + 1)
              : attempt.result.error();
      return core::Result<AssistantResponse>::failure(error);
    }
    const auto delay = attempt.retry_after.value_or(
        std::chrono::milliseconds(retry == 0 ? 250 : 500));
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline ||
        delay >= std::chrono::duration_cast<std::chrono::milliseconds>(
                     deadline - now)) {
      return core::Result<AssistantResponse>::failure(
          retry_error(attempt.result.error(), attempt.status, retry + 1));
    }
    if (on_delta) {
      on_delta({"HTTP " + std::to_string(attempt.status) + "; retrying (" +
                    std::to_string(retry + 1) + "/2) in " +
                    std::to_string(delay.count()) + " ms",
                core::ModelDeltaKind::retry});
    }
    if (cancellation.is_cancelled()) {
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::cancelled, "model request cancelled"});
    }
    const auto waited = wait_for_retry(delay, deadline, cancellation);
    if (!waited)
      return core::Result<AssistantResponse>::failure(waited.error());
  }
  if (response.finish_reason == FinishReason::unknown) {
    return core::Result<AssistantResponse>::failure({
        ErrorCode::model_error,
        "OpenCode stream ended without a terminal response event",
    });
  }
  const auto valid_calls = validate_tool_calls(response.tool_calls);
  if (!valid_calls)
    return core::Result<AssistantResponse>::failure(valid_calls.error());
  return core::Result<AssistantResponse>::success(std::move(response));
}

} // namespace zed::providers
