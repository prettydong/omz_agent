#include "zed/providers/opencode_go_model.hpp"

#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

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

std::string curl_config_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char character : value) {
    if (character == '\\' || character == '"')
      escaped.push_back('\\');
    if (character == '\n' || character == '\r')
      continue;
    escaped.push_back(character);
  }
  return escaped;
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
                           AssistantResponse &response) {
  parse_usage(response_value, response.usage);
  if (const auto *output = field(response_value, "output");
      output != nullptr && output->is_array()) {
    for (const auto &item : *output)
      parse_output_item(item, response.tool_calls);
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
    parse_response_output(*response_value, response);
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
    parse_response_output(*response_value, response);
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

ToolCall &chat_tool_call(ChatStreamState &state, std::size_t index) {
  if (state.indexed_calls.size() <= index)
    state.indexed_calls.resize(index + 1);
  return state.indexed_calls[index];
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
          auto &call = chat_tool_call(state, index_value->get<std::size_t>());
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
      const auto index = index_value->get<std::size_t>();
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
      const auto index = index_value->get<std::size_t>();
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
      const auto iterator = state.blocks.find(index_value->get<std::size_t>());
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

bool write_all(int descriptor, std::string_view content) {
  std::size_t offset = 0;
  while (offset < content.size()) {
    const auto written =
        write(descriptor, content.data() + offset, content.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

class TemporaryRequestFile {
public:
  static core::Result<TemporaryRequestFile> create(std::string_view body) {
    std::string path_template =
        (std::filesystem::temp_directory_path() / "zed-request-XXXXXX")
            .string();
    std::vector<char> path(path_template.begin(), path_template.end());
    path.push_back('\0');
    support::UniqueFd descriptor(mkstemp(path.data()));
    if (!descriptor.valid()) {
      return core::Result<TemporaryRequestFile>::failure(
          {ErrorCode::model_error, "cannot create request file"});
    }
    TemporaryRequestFile file(std::string(path.data()));
    if (!write_all(descriptor.get(), body)) {
      return core::Result<TemporaryRequestFile>::failure(
          {ErrorCode::model_error, "cannot write request body"});
    }
    return core::Result<TemporaryRequestFile>::success(std::move(file));
  }

  TemporaryRequestFile(const TemporaryRequestFile &) = delete;
  TemporaryRequestFile &operator=(const TemporaryRequestFile &) = delete;

  TemporaryRequestFile(TemporaryRequestFile &&other) noexcept
      : path_(std::exchange(other.path_, {})) {}

  ~TemporaryRequestFile() {
    if (!path_.empty())
      static_cast<void>(unlink(path_.c_str()));
  }

  [[nodiscard]] const std::string &path() const { return path_; }

private:
  explicit TemporaryRequestFile(std::string path) : path_(std::move(path)) {}

  std::string path_;
};

class LineBuffer {
public:
  explicit LineBuffer(
      const std::function<core::Result<void>(std::string_view)> &on_line)
      : on_line_(on_line) {}

  core::Result<void> append(std::string_view chunk) {
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
      std::string line = pending_.substr(0, newline);
      pending_.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto processed = on_line_(line);
      if (!processed)
        return processed;
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
};

core::Result<void>
run_curl(const OpenCodeGoConfig &config, std::string_view endpoint,
         const std::vector<std::string> &extra_headers, std::string_view body,
         core::CancellationToken cancellation,
         const std::function<core::Result<void>(std::string_view)> &on_line) {
  auto request_file = TemporaryRequestFile::create(body);
  if (!request_file)
    return core::Result<void>::failure(request_file.error());

  auto spawn_lock = support::lock_process_spawn();
  int output_pipe[2];
  if (!support::create_cloexec_pipe(output_pipe))
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot create HTTP output pipe"});
  support::UniqueFd output_read(output_pipe[0]);
  support::UniqueFd output_write(output_pipe[1]);

  int config_pipe[2];
  if (!support::create_cloexec_pipe(config_pipe))
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot create curl config pipe"});
  support::UniqueFd config_read(config_pipe[0]);
  support::UniqueFd config_write(config_pipe[1]);

  const pid_t child = fork();
  if (child == -1)
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot start curl"});
  if (child == 0) {
    output_read.reset();
    config_write.reset();
    setpgid(0, 0);
    dup2(output_write.get(), STDOUT_FILENO);
    dup2(config_read.get(), STDIN_FILENO);
    output_write.reset();
    config_read.reset();
    const int error_fd = open("/dev/null", O_WRONLY);
    if (error_fd >= 0) {
      dup2(error_fd, STDERR_FILENO);
      close(error_fd);
    }
    std::vector<std::string> arguments{
        "curl", "-sS", "--no-buffer", "--fail-with-body", "--config", "-",
    };
    std::vector<char *> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (auto &argument : arguments)
      raw_arguments.push_back(argument.data());
    raw_arguments.push_back(nullptr);
    execvp(raw_arguments[0], raw_arguments.data());
    _exit(127);
  }

  spawn_lock.unlock();

  setpgid(child, child);

  output_write.reset();
  config_read.reset();
  bool child_finished = false;
  int status = 0;
  const auto terminate_child = [&] {
    support::terminate_process_group(child, std::chrono::milliseconds(250),
                                     child_finished, status);
  };
  std::string curl_config = "url = \"" + curl_config_escape(endpoint) + "\"\n" +
                            "request = \"POST\"\n" +
                            "header = \"Authorization: Bearer " +
                            curl_config_escape(config.api_key) + "\"\n" +
                            "header = \"Content-Type: application/json\"\n";
  for (const auto &header : extra_headers) {
    curl_config += "header = \"" + curl_config_escape(header) + "\"\n";
  }
  curl_config += "data-binary = \"@" +
                 curl_config_escape(request_file.value().path()) + "\"\n";
  const bool wrote_config = write_all(config_write.get(), curl_config);
  config_write.reset();
  if (!wrote_config) {
    terminate_child();
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot write curl config"});
  }
  const int flags = fcntl(output_read.get(), F_GETFL, 0);
  if (flags < 0 || fcntl(output_read.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
    terminate_child();
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot configure curl output"});
  }

  bool cancelled = false;
  bool timed_out = false;
  bool pipe_closed = false;
  LineBuffer line_buffer(on_line);
  const auto started_at = std::chrono::steady_clock::now();
  while (!pipe_closed || !child_finished) {
    if (cancellation.is_cancelled()) {
      cancelled = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();
    if (elapsed >= static_cast<long long>(config.request_timeout_ms)) {
      timed_out = true;
    }

    pollfd descriptor{output_read.get(), POLLIN | POLLHUP, 0};
    const int poll_result = poll(&descriptor, 1, 50);
    if (poll_result < 0 && errno != EINTR) {
      terminate_child();
      return core::Result<void>::failure(
          {ErrorCode::model_error, "cannot poll curl output"});
    }
    if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[8192];
      while (true) {
        const ssize_t read_count =
            read(output_read.get(), buffer, sizeof(buffer));
        if (read_count > 0) {
          const auto processed = line_buffer.append(
              std::string_view(buffer, static_cast<std::size_t>(read_count)));
          if (!processed) {
            terminate_child();
            return processed;
          }
          continue;
        }
        if (read_count == 0) {
          pipe_closed = true;
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        pipe_closed = true;
        break;
      }
    }

    if (!child_finished)
      child_finished = support::try_reap_child(child, status);
    if ((cancelled || timed_out) && !child_finished) {
      terminate_child();
    }
  }

  const auto processed = line_buffer.flush();
  if (!processed)
    return processed;
  if (cancelled || cancellation.is_cancelled())
    return core::Result<void>::failure(
        {ErrorCode::cancelled, "model request cancelled"});
  if (timed_out)
    return core::Result<void>::failure(
        {ErrorCode::timeout, "OpenCode request timed out"});
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    return core::Result<void>::failure(
        {ErrorCode::model_error, "OpenCode HTTP request failed"});
  return core::Result<void>::success();
}

} // namespace

OpenCodeGoModel::OpenCodeGoModel(OpenCodeGoConfig config)
    : config_(std::move(config)) {
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

  const auto *model_info =
      find_opencode_go_model(config_.models, request.model.model);
  const auto protocol = model_info == nullptr
                            ? infer_open_code_protocol(request.model.model)
                            : model_info->protocol;
  const std::string endpoint = endpoint_for(config_, protocol);

  AssistantResponse response;
  core::Result<void> result = core::Result<void>::success();
  if (protocol == OpenCodeProtocol::responses) {
    result = run_curl(config_, endpoint, {},
                      responses_request_json(request, model_info), cancellation,
                      [&](std::string_view line) {
                        return process_responses_sse(line, response, on_delta);
                      });
  } else if (protocol == OpenCodeProtocol::chat_completions) {
    ChatStreamState state;
    result =
        run_curl(config_, endpoint, {}, chat_request_json(request, model_info),
                 cancellation, [&](std::string_view line) {
                   return process_chat_sse(line, state, on_delta);
                 });
    state.response.tool_calls = std::move(state.indexed_calls);
    response = std::move(state.response);
  } else {
    MessagesStreamState state;
    result = run_curl(
        config_, endpoint,
        {"x-api-key: " + config_.api_key, "anthropic-version: 2023-06-01"},
        messages_request_json(request, model_info), cancellation,
        [&](std::string_view line) {
          return process_messages_sse(line, state, on_delta);
        });
    response = std::move(state.response);
    if (result && !state.message_stopped) {
      result = core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode Messages stream ended without message_stop",
      });
    }
  }
  if (!result)
    return core::Result<AssistantResponse>::failure(result.error());
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
