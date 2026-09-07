#include "zed/providers/opencode_go_model.hpp"

#include "zed/core/session_store.hpp"
#include "zed/core/utf8.hpp"
#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <poll.h>
#include <set>
#include <stdexcept>
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

constexpr std::size_t kMaximumSseLineBytes = 1024 * 1024;
constexpr std::size_t kMaximumStreamBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaximumToolCalls = 128;

// Thrown only inside the provider boundary and converted by complete().
class ProtocolError : public std::runtime_error {
public:
  explicit ProtocolError(std::string message, bool can_retry = false,
                         ErrorCode error_code = ErrorCode::model_error)
      : std::runtime_error(std::move(message)), retryable(can_retry),
        code(error_code) {}
  bool retryable;
  ErrorCode code;
};

Json parse_json(std::string_view text) {
  return Json::parse(text, [](int depth, Json::parse_event_t, Json &) {
    if (depth > 128)
      throw ProtocolError("OpenCode JSON nesting exceeds 128 levels");
    return true;
  });
}

std::string required_text(const Json &object, std::string_view name) {
  const auto it = object.find(std::string(name));
  if (!object.is_object() || it == object.end() || !it->is_string())
    throw ProtocolError("OpenCode expected string field " + std::string(name));
  return it->get<std::string>();
}

core::TokenCount token_count(const Json &value) {
  if (!value.is_number_unsigned() ||
      value.get<std::uint64_t>() > std::numeric_limits<core::TokenCount>::max())
    throw ProtocolError(
        "OpenCode usage requires nonnegative integer token counts");
  return value.get<core::TokenCount>();
}

void read_count(const Json &object, std::string_view name,
                core::TokenCount &target) {
  const auto it = object.find(std::string(name));
  if (it != object.end() && !it->is_null())
    target = token_count(*it);
}

Json calls_json(const std::vector<ToolCall> &calls) {
  Json result = Json::array();
  for (const auto &call : calls)
    result.push_back({{"id", call.id},
                      {"name", call.name},
                      {"arguments", call.arguments_json}});
  return result;
}

std::string continuation(const ModelRequest &request, std::string_view protocol,
                         const AssistantResponse &response, const Json &data) {
  return Json{{"version", 1},
              {"model", request.model.model},
              {"protocol", protocol},
              {"content", response.content},
              {"calls", calls_json(response.tool_calls)},
              {"data", data}}
      .dump();
}

Json replay(const Message &message, const ModelRequest &request,
            std::string_view protocol) {
  if (message.model_state.empty())
    return nullptr;
  const auto state = parse_json(message.model_state);
  if (!state.is_object() || state.value("version", 0) != 1)
    throw ProtocolError("OpenCode invalid continuation version");
  // A model switch must never send another model's opaque reasoning state.
  if (required_text(state, "model") != request.model.model ||
      required_text(state, "protocol") != protocol)
    return nullptr;
  if (state.at("content") != message.content ||
      state.at("calls") != calls_json(message.tool_calls))
    throw ProtocolError(
        "OpenCode continuation no longer matches assistant content/tool calls");
  const auto &data = state.at("data");
  if (protocol == "chat") {
    if (!data.is_string())
      throw ProtocolError("OpenCode invalid reasoning continuation");
    return data;
  }
  if (!data.is_array() || data.size() > kMaximumToolCalls)
    throw ProtocolError("OpenCode invalid continuation block array");
  std::string content;
  std::vector<ToolCall> calls;
  for (const auto &item : data) {
    const auto type = required_text(item, "type");
    if (protocol == "responses") {
      if (type == "function_call")
        calls.push_back({required_text(item, "call_id"),
                         required_text(item, "name"),
                         required_text(item, "arguments")});
      else if (type == "message") {
        if (required_text(item, "role") != "assistant" ||
            !item.at("content").is_array())
          throw ProtocolError("OpenCode invalid continuation message");
        for (const auto &part : item.at("content")) {
          const auto kind = required_text(part, "type");
          if (kind != "output_text" && kind != "refusal")
            throw ProtocolError("OpenCode unsupported continuation content");
          content +=
              required_text(part, kind == "refusal" ? "refusal" : "text");
        }
      } else if (type != "reasoning")
        throw ProtocolError("OpenCode unsupported continuation item");
    } else {
      if (type == "text")
        content += required_text(item, "text");
      else if (type == "tool_use") {
        if (!item.at("input").is_object())
          throw ProtocolError("OpenCode invalid continuation tool input");
        calls.push_back({required_text(item, "id"), required_text(item, "name"),
                         item.at("input").dump()});
      } else if (type == "thinking")
        required_text(item, "thinking");
      else if (type == "redacted_thinking")
        required_text(item, "data");
      else
        throw ProtocolError("OpenCode unsupported continuation block");
    }
  }
  if (content != message.content ||
      calls_json(calls) != calls_json(message.tool_calls))
    throw ProtocolError(
        "OpenCode continuation data disagrees with assistant output");
  return data;
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
  root["prompt_cache_key"] = request.session_id;
  root["include"] = Json::array({"reasoning.encrypted_content"});
  if (request.max_output_tokens.has_value()) {
    root["max_output_tokens"] = *request.max_output_tokens;
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
    if (message.role == Role::assistant) {
      const auto data = replay(message, request, "responses");
      if (!data.is_null()) {
        if (!data.is_array())
          throw ProtocolError("OpenCode invalid Responses continuation");
        for (const auto &item : data) {
          const auto type = required_text(item, "type");
          if (type != "message" && type != "reasoning" &&
              type != "function_call")
            throw ProtocolError(
                "OpenCode unsupported Responses continuation item");
          input.push_back(item);
        }
        continue;
      }
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
    tool["strict"] = false;
    tool["parameters"] = parse_json(definition.input_schema_json);
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
  for (const auto &message : request.messages) {
    auto item = chat_message(message);
    if (message.role == Role::assistant) {
      const auto reasoning = replay(message, request, "chat");
      if (!reasoning.is_null()) {
        if (!reasoning.is_string())
          throw ProtocolError("OpenCode invalid Chat continuation");
        item["reasoning_content"] = reasoning;
      }
    }
    root["messages"].push_back(std::move(item));
  }

  root["tools"] = Json::array();
  for (const auto &definition : request.tools) {
    Json parameters = Json::object();
    parameters = parse_json(definition.input_schema_json);
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
      input = parse_json(call.arguments_json);
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
  root["max_tokens"] = request.max_output_tokens.value_or(
      model != nullptr && model->max_output_tokens > 0
          ? model->max_output_tokens
          : 4096);
  if (request.model.model.starts_with("minimax-m3"))
    root["thinking"] = {{"type", "adaptive"}};

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
    auto content = message.role == Role::assistant
                       ? replay(message, request, "messages")
                       : Json(nullptr);
    if (content.is_null())
      content = anthropic_content(message);
    if (!content.is_array())
      throw ProtocolError("OpenCode invalid Messages continuation");
    // Parallel tool results must form one user message immediately after
    // tool_use.
    if (!root["messages"].empty() &&
        root["messages"].back().at("role") == role) {
      for (const auto &block : content)
        root["messages"].back()["content"].push_back(block);
    } else {
      root["messages"].push_back(
          {{"role", role}, {"content", std::move(content)}});
    }
  }
  if (!system.empty())
    root["system"] =
        Json::array({{{"type", "text"},
                      {"text", std::move(system)},
                      {"cache_control", {{"type", "ephemeral"}}}}});

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
    input_schema = parse_json(definition.input_schema_json);
    root["tools"].push_back({
        {"name", definition.name},
        {"description", definition.description},
        {"input_schema", std::move(input_schema)},
    });
  }
  std::size_t marked = 0;
  for (auto it = root["messages"].rbegin();
       it != root["messages"].rend() && marked < 2; ++it) {
    auto &content = (*it)["content"];
    if (content.empty())
      continue;
    auto &last = content.back();
    const auto type = last.value("type", "");
    if (type == "thinking" || type == "redacted_thinking")
      continue;
    last["cache_control"] = {{"type", "ephemeral"}};
    ++marked;
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

std::string response_error_message(const Json &value,
                                   std::string_view fallback) {
  const auto *error = field(value, "error");
  const auto *message = field(error != nullptr ? *error : value, "message");
  return message != nullptr && message->is_string()
             ? message->get<std::string>().substr(0, 512)
             : std::string(fallback);
}

void parse_usage(const Json &event, core::ModelUsage &usage,
                 bool chat = false) {
  const auto *value = field(event, "usage");
  if (value == nullptr || value->is_null())
    return;
  if (!value->is_object())
    throw ProtocolError("OpenCode usage must be an object");
  read_count(*value, chat ? "prompt_tokens" : "input_tokens",
             usage.input_tokens);
  read_count(*value, chat ? "completion_tokens" : "output_tokens",
             usage.output_tokens);
  if (chat)
    read_count(*value, "prompt_cache_hit_tokens", usage.cached_input_tokens);
  if (const auto *details = field(*value, chat ? "prompt_tokens_details"
                                               : "input_tokens_details");
      details != nullptr && !details->is_null()) {
    if (!details->is_object())
      throw ProtocolError("OpenCode token details must be an object");
    read_count(*details, "cached_tokens", usage.cached_input_tokens);
    read_count(*details, "cache_write_tokens", usage.cache_write_input_tokens);
  }
}

std::size_t event_index(const Json &value, std::string_view name = "index") {
  const auto *index = field(value, name);
  if (index == nullptr || !index->is_number_unsigned())
    throw ProtocolError("OpenCode expected a nonnegative integer index");
  const auto result = index->get<std::uint64_t>();
  if (result >= kMaximumToolCalls)
    throw ProtocolError("OpenCode returned an index above the limit of " +
                        std::to_string(kMaximumToolCalls));
  return static_cast<std::size_t>(result);
}

ToolCall parse_output_call(const Json &item) {
  return {required_text(item, "call_id"), required_text(item, "name"),
          required_text(item, "arguments")};
}

struct ResponsesStreamState {
  AssistantResponse response;
  std::map<std::size_t, Json> items;
  bool terminal{false};
};

void collect_responses_output(ResponsesStreamState &state) {
  std::string content;
  std::vector<ToolCall> calls;
  for (const auto &[index, item] : state.items) {
    static_cast<void>(index);
    const auto type = required_text(item, "type");
    if (type == "function_call")
      calls.push_back(parse_output_call(item));
    else if (type == "message") {
      if (required_text(item, "role") != "assistant" ||
          !item.at("content").is_array())
        throw ProtocolError("OpenCode invalid assistant output message");
      for (const auto &part : item.at("content")) {
        const auto part_type = required_text(part, "type");
        if (part_type == "output_text")
          content += required_text(part, "text");
        else if (part_type == "refusal")
          content += required_text(part, "refusal");
        else
          throw ProtocolError("OpenCode unsupported message output part: " +
                              part_type);
      }
    } else if (type != "reasoning") {
      throw ProtocolError("OpenCode unsupported output item: " + type);
    }
  }
  if (!calls.empty())
    state.response.tool_calls = std::move(calls);
  if (!content.empty())
    state.response.content = std::move(content);
}

core::Result<void> process_responses_sse(std::string_view line,
                                         ResponsesStreamState &state,
                                         const core::StreamCallback &on_delta) {
  const auto payload = trim_ascii(line.substr(5));
  if (payload == "[DONE]")
    return core::Result<void>::success();
  const auto event = parse_json(payload);
  const auto type = required_text(event, "type");
  if (type == "error" || type == "response.failed")
    throw ProtocolError(response_error_message(
        type == "response.failed" ? event.at("response") : event,
        "OpenCode response failed"));
  // Go appends a billing ping after response.completed.
  if (type == "ping")
    return core::Result<void>::success();
  if (state.terminal)
    throw ProtocolError("OpenCode event after terminal response: " + type);
  if (type == "response.output_text.delta" ||
      type == "response.refusal.delta") {
    const auto delta = required_text(event, "delta");
    state.response.content += delta;
    if (on_delta)
      on_delta({delta});
  } else if (type == "response.reasoning_summary_text.delta" ||
             type == "response.reasoning_text.delta") {
    const auto delta = required_text(event, "delta");
    if (on_delta)
      on_delta({delta, core::ModelDeltaKind::reasoning});
  } else if (type == "response.output_item.added" ||
             type == "response.output_item.done") {
    const auto &item = event.at("item");
    required_text(item, "type");
    if (event.contains("output_index")) {
      state.items[event_index(event, "output_index")] = item;
    } else if (item.at("type") == "function_call") {
      add_tool_call(state.response.tool_calls, parse_output_call(item));
    } else
      throw ProtocolError("OpenCode output item has no output_index");
  } else if (type == "response.function_call_arguments.delta" ||
             type == "response.function_call_arguments.done") {
    const auto text =
        required_text(event, type.ends_with(".delta") ? "delta" : "arguments");
    Json *item = nullptr;
    if (event.contains("output_index")) {
      auto found = state.items.find(event_index(event, "output_index"));
      if (found != state.items.end())
        item = &found->second;
    }
    if (item == nullptr && event.contains("item_id")) {
      const auto id = required_text(event, "item_id");
      for (auto &[index, candidate] : state.items) {
        static_cast<void>(index);
        if (candidate.value("id", "") == id) {
          item = &candidate;
          break;
        }
      }
    }
    if (item != nullptr) {
      if (item->at("type") != "function_call")
        throw ProtocolError("OpenCode arguments for non-tool item");
      if (type.ends_with(".delta"))
        (*item)["arguments"] = required_text(*item, "arguments") + text;
      else
        (*item)["arguments"] = text;
    } else if (event.contains("call_id")) {
      const auto id = required_text(event, "call_id");
      const auto name = event.value("name", "");
      if (type.ends_with(".delta"))
        append_tool_call_arguments(state.response.tool_calls, id, name, text);
      else
        add_tool_call(state.response.tool_calls, {id, name, text});
    } else
      throw ProtocolError(
          "OpenCode arguments reference an unknown output item");
  } else if (type == "response.completed" || type == "response.incomplete") {
    const auto &value = event.at("response");
    const auto expected =
        type == "response.completed" ? "completed" : "incomplete";
    if (required_text(value, "status") != expected)
      throw ProtocolError(
          "OpenCode response status disagrees with terminal event");
    parse_usage(value, state.response.usage);
    if (const auto *output = field(value, "output"); output != nullptr) {
      if (!output->is_array() || output->size() > kMaximumToolCalls)
        throw ProtocolError("OpenCode invalid response output array");
      if (!output->empty()) {
        state.items.clear();
        for (std::size_t i = 0; i < output->size(); ++i)
          state.items[i] = (*output)[i];
      }
    }
    collect_responses_output(state);
    if (type == "response.completed") {
      state.response.finish_reason = state.response.tool_calls.empty()
                                         ? FinishReason::stop
                                         : FinishReason::tool_calls;
    } else {
      const auto reason =
          required_text(value.at("incomplete_details"), "reason");
      if (reason == "max_tokens" || reason == "max_output_tokens")
        state.response.finish_reason = FinishReason::length;
      else if (reason == "content_filter")
        state.response.finish_reason = FinishReason::content_filter;
      else
        throw ProtocolError("OpenCode unknown incomplete response reason");
    }
    state.terminal = true;
  }
  return core::Result<void>::success();
}

FinishReason finish_reason_from_chat(std::string_view reason) {
  if (reason == "stop")
    return FinishReason::stop;
  if (reason == "tool_calls")
    return FinishReason::tool_calls;
  if (reason == "length")
    return FinishReason::length;
  if (reason == "content_filter")
    return FinishReason::content_filter;
  throw ProtocolError("OpenCode unsupported Chat finish_reason: " +
                      std::string(reason));
}

struct ChatStreamState {
  AssistantResponse response;
  std::vector<ToolCall> indexed_calls;
  std::string reasoning;
  bool terminal{false};
};

core::Result<void> process_chat_sse(std::string_view line,
                                    ChatStreamState &state,
                                    const core::StreamCallback &on_delta) {
  const auto payload = trim_ascii(line.substr(5));
  if (payload == "[DONE]")
    return core::Result<void>::success();
  const auto event = parse_json(payload);
  if (event.contains("error"))
    throw ProtocolError(
        response_error_message(event, "OpenCode Chat request failed"));
  parse_usage(event, state.response.usage, true);
  const auto *choices = field(event, "choices");
  if (choices == nullptr || !choices->is_array())
    throw ProtocolError("OpenCode Chat choices must be an array");
  if (choices->size() > 1)
    throw ProtocolError(
        "OpenCode returned multiple choices for a single-choice request");
  for (const auto &choice : *choices) {
    if (state.terminal) {
      // Some Go upstreams repeat an empty choice with their final usage chunk.
      if (const auto *delta = field(choice, "delta");
          delta != nullptr && !delta->is_null()) {
        if (!delta->is_object())
          throw ProtocolError("OpenCode invalid trailing Chat delta");
        for (const auto &[key, value] : delta->items()) {
          if (value.is_null() ||
              (value.is_string() &&
               value.get_ref<const std::string &>().empty()) ||
              (value.is_array() && value.empty()) ||
              (key == "role" && value == "assistant"))
            continue;
          throw ProtocolError("OpenCode Chat content after finish_reason");
        }
      }
      if (const auto *reason = field(choice, "finish_reason");
          reason != nullptr && !reason->is_null() &&
          finish_reason_from_chat(required_text(choice, "finish_reason")) !=
              state.response.finish_reason)
        throw ProtocolError("OpenCode conflicting trailing Chat finish_reason");
      continue;
    }
    if (choice.contains("index") && event_index(choice) != 0)
      throw ProtocolError("OpenCode unexpected choice index");
    if (const auto *delta = field(choice, "delta");
        delta != nullptr && !delta->is_null()) {
      if (!delta->is_object())
        throw ProtocolError("OpenCode Chat delta must be an object");
      if (const auto *content = field(*delta, "content");
          content != nullptr && !content->is_null()) {
        const auto text = required_text(*delta, "content");
        state.response.content += text;
        if (on_delta)
          on_delta({text});
      }
      const auto reasoning_key = delta->contains("reasoning_content")
                                     ? "reasoning_content"
                                     : "reasoning";
      if (const auto *reasoning = field(*delta, reasoning_key);
          reasoning != nullptr && !reasoning->is_null()) {
        const auto text = required_text(*delta, reasoning_key);
        state.reasoning += text;
        if (on_delta)
          on_delta({text, core::ModelDeltaKind::reasoning});
      }
      if (const auto *calls = field(*delta, "tool_calls");
          calls != nullptr && !calls->is_null()) {
        if (!calls->is_array())
          throw ProtocolError("OpenCode tool_calls must be an array");
        for (const auto &part : *calls) {
          const auto index = event_index(part);
          if (state.indexed_calls.size() <= index)
            state.indexed_calls.resize(index + 1);
          auto &call = state.indexed_calls[index];
          if (part.contains("id") && !part.at("id").is_null()) {
            const auto id = required_text(part, "id");
            if (!call.id.empty() && call.id != id)
              throw ProtocolError(
                  "OpenCode tool call id changed during stream");
            call.id = id;
          }
          if (part.contains("type") && !part.at("type").is_null() &&
              required_text(part, "type") != "function")
            throw ProtocolError("OpenCode unsupported tool call type");
          if (part.contains("function")) {
            const auto &function = part.at("function");
            if (!function.is_object())
              throw ProtocolError("OpenCode function must be an object");
            if (function.contains("name") && !function.at("name").is_null())
              call.name += required_text(function, "name");
            if (function.contains("arguments") &&
                !function.at("arguments").is_null())
              call.arguments_json += required_text(function, "arguments");
          }
        }
      }
    }
    if (const auto *reason = field(choice, "finish_reason");
        reason != nullptr && !reason->is_null()) {
      state.response.finish_reason =
          finish_reason_from_chat(required_text(choice, "finish_reason"));
      state.terminal = true;
    }
  }
  return core::Result<void>::success();
}

struct MessagesStreamState {
  AssistantResponse response;
  std::map<std::size_t, Json> blocks;
  std::map<std::size_t, std::string> arguments;
  std::set<std::size_t> stopped;
  core::TokenCount uncached_input{};
  bool message_stopped{false};
};

void parse_anthropic_usage(const Json &value, MessagesStreamState &state) {
  if (!value.is_object())
    throw ProtocolError("OpenCode Messages usage must be an object");
  auto &usage = state.response.usage;
  read_count(value, "input_tokens", state.uncached_input);
  read_count(value, "output_tokens", usage.output_tokens);
  read_count(value, "cache_read_input_tokens", usage.cached_input_tokens);
  read_count(value, "cache_creation_input_tokens",
             usage.cache_write_input_tokens);
  const auto maximum = std::numeric_limits<core::TokenCount>::max();
  if (usage.cached_input_tokens > maximum - state.uncached_input ||
      usage.cache_write_input_tokens >
          maximum - state.uncached_input - usage.cached_input_tokens)
    throw ProtocolError("OpenCode input token count overflow");
  usage.input_tokens = state.uncached_input + usage.cached_input_tokens +
                       usage.cache_write_input_tokens;
}

core::Result<void> process_messages_sse(std::string_view line,
                                        MessagesStreamState &state,
                                        const core::StreamCallback &on_delta) {
  const auto event = parse_json(trim_ascii(line.substr(5)));
  const auto type = required_text(event, "type");
  if (type == "error")
    throw ProtocolError(
        response_error_message(event, "OpenCode Messages request failed"));
  if (type == "ping")
    return core::Result<void>::success();
  if (state.message_stopped)
    throw ProtocolError("OpenCode event after message_stop");
  if (type == "message_start") {
    const auto &message = event.at("message");
    if (message.contains("usage"))
      parse_anthropic_usage(message.at("usage"), state);
  } else if (type == "content_block_start") {
    const auto index = event_index(event);
    if (state.blocks.contains(index))
      throw ProtocolError("OpenCode duplicate content block");
    const auto &block = event.at("content_block");
    const auto kind = required_text(block, "type");
    if (kind != "text" && kind != "thinking" && kind != "redacted_thinking" &&
        kind != "tool_use")
      throw ProtocolError("OpenCode unsupported content block: " + kind);
    state.blocks[index] = block;
    if (kind == "text" || kind == "thinking") {
      const auto text =
          required_text(block, kind == "text" ? "text" : "thinking");
      if (kind == "text")
        state.response.content += text;
      if (on_delta && !text.empty())
        on_delta({text, kind == "text" ? core::ModelDeltaKind::output_text
                                       : core::ModelDeltaKind::reasoning});
    }
  } else if (type == "content_block_delta") {
    const auto index = event_index(event);
    if (!state.blocks.contains(index) || state.stopped.contains(index))
      throw ProtocolError("OpenCode delta for an inactive block");
    auto &block = state.blocks.at(index);
    const auto &delta = event.at("delta");
    const auto kind = required_text(delta, "type");
    if (kind == "text_delta" || kind == "thinking_delta" ||
        kind == "signature_delta") {
      const auto key = kind == "text_delta"       ? "text"
                       : kind == "thinking_delta" ? "thinking"
                                                  : "signature";
      const auto expected = kind == "text_delta" ? "text" : "thinking";
      if (block.at("type") != expected)
        throw ProtocolError("OpenCode delta type disagrees with content block");
      const auto text = required_text(delta, key);
      block[key] = block.value(key, "") + text;
      if (kind == "text_delta")
        state.response.content += text;
      if (on_delta && kind != "signature_delta")
        on_delta({text, kind == "text_delta"
                            ? core::ModelDeltaKind::output_text
                            : core::ModelDeltaKind::reasoning});
    } else if (kind == "input_json_delta") {
      if (block.at("type") != "tool_use")
        throw ProtocolError("OpenCode tool arguments for a non-tool block");
      state.arguments[index] += required_text(delta, "partial_json");
    } else
      throw ProtocolError("OpenCode unsupported content delta: " + kind);
  } else if (type == "content_block_stop") {
    const auto index = event_index(event);
    if (!state.blocks.contains(index) || !state.stopped.insert(index).second)
      throw ProtocolError("OpenCode stop for an inactive block");
    auto &block = state.blocks.at(index);
    if (block.at("type") == "tool_use") {
      if (state.arguments.contains(index))
        block["input"] = parse_json(state.arguments.at(index));
      if (!block.at("input").is_object())
        throw ProtocolError("OpenCode tool input must be an object");
      state.response.tool_calls.push_back({required_text(block, "id"),
                                           required_text(block, "name"),
                                           block.at("input").dump()});
    }
  } else if (type == "message_delta") {
    const auto &delta = event.at("delta");
    if (delta.contains("stop_reason") && !delta.at("stop_reason").is_null()) {
      if (state.response.finish_reason != FinishReason::unknown)
        throw ProtocolError("OpenCode duplicate stop_reason");
      const auto reason = required_text(delta, "stop_reason");
      if (reason == "end_turn" || reason == "stop_sequence")
        state.response.finish_reason = FinishReason::stop;
      else if (reason == "tool_use")
        state.response.finish_reason = FinishReason::tool_calls;
      else if (reason == "max_tokens" ||
               reason == "model_context_window_exceeded")
        state.response.finish_reason = FinishReason::length;
      else if (reason == "refusal")
        state.response.finish_reason = FinishReason::content_filter;
      else
        throw ProtocolError("OpenCode unsupported Messages stop_reason: " +
                            reason);
    }
    if (event.contains("usage"))
      parse_anthropic_usage(event.at("usage"), state);
  } else if (type == "message_stop") {
    if (state.stopped.size() != state.blocks.size())
      throw ProtocolError(
          "OpenCode message_stop with unfinished content blocks");
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
  std::set<std::string> ids;
  for (const auto &call : calls) {
    if (!ids.insert(call.id).second)
      throw ProtocolError("OpenCode duplicate tool call id");
    if (call.id.empty() || call.name.empty() || call.arguments_json.empty()) {
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned an incomplete function call",
      });
    }
    try {
      const auto arguments = parse_json(call.arguments_json);
      if (arguments.is_object())
        continue;
      return core::Result<void>::failure({
          ErrorCode::model_error,
          "OpenCode returned invalid function arguments for " + call.name +
              ": expected object",
      });
    } catch (const Json::parse_error &) {
      throw ProtocolError("OpenCode returned invalid function argument JSON");
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

  support::UniqueFd error_output(open("/dev/null", O_WRONLY | O_CLOEXEC));
  support::SpawnOptions spawn_options;
  spawn_options.executable = "curl";
  spawn_options.arguments = {
      "--disable",
      "-sS",
      "--no-buffer",
      "--include",
      "--suppress-connect-headers",
      "--proto",
      "=https,http",
      "--config",
      "-",
  };
  spawn_options.duplicate_descriptors = {
      {output_write.get(), STDOUT_FILENO},
      {config_read.get(), STDIN_FILENO},
  };
  if (error_output.valid()) {
    spawn_options.duplicate_descriptors.push_back(
        {error_output.get(), STDERR_FILENO});
  }
  spawn_options.close_descriptors = {
      output_read.get(),
      output_write.get(),
      config_read.get(),
      config_write.get(),
  };
  if (error_output.valid())
    spawn_options.close_descriptors.push_back(error_output.get());
  pid_t child = -1;
  const int spawn_error = support::spawn_process(spawn_options, child);
  if (spawn_error != 0) {
    return core::Result<void>::failure(
        {ErrorCode::model_error,
         "cannot start curl: " + std::string(std::strerror(spawn_error))});
  }

  spawn_lock.unlock();

  output_write.reset();
  config_read.reset();
  bool child_finished = false;
  int status = 0;
  const auto terminate_child = [&] {
    support::terminate_process_group(child, std::chrono::milliseconds(250),
                                     child_finished, status);
  };
  struct ChildGuard {
    const decltype(terminate_child) &terminate;
    bool &finished;
    ~ChildGuard() {
      if (!finished)
        terminate();
    }
  } child_guard{terminate_child, child_finished};
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
  bool termination_requested = false;
  int http_status = 0;
  bool reading_headers = true;
  bool event_stream = false;
  std::string error_body;
  std::string event_data;
  const auto dispatch_event = [&]() -> core::Result<void> {
    if (event_data.empty())
      return core::Result<void>::success();
    auto data = std::exchange(event_data, {});
    return on_line("data: " + data);
  };
  const std::function<core::Result<void>(std::string_view)> consume_line =
      [&](std::string_view line) -> core::Result<void> {
    if (reading_headers) {
      if (line.starts_with("HTTP/")) {
        const auto space = line.find(' ');
        if (space == std::string_view::npos || space + 4 > line.size())
          throw ProtocolError("OpenCode invalid HTTP status line");
        http_status = 0;
        for (const char digit : line.substr(space + 1, 3)) {
          if (digit < '0' || digit > '9')
            throw ProtocolError("OpenCode invalid HTTP status");
          http_status = http_status * 10 + digit - '0';
        }
        event_stream = false;
      } else if (line.empty()) {
        reading_headers = http_status >= 100 && http_status < 200;
      } else {
        std::string lower(line);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                         return static_cast<char>(
                             c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
                       });
        if (lower.starts_with("content-type:") &&
            lower.find("text/event-stream") != std::string::npos)
          event_stream = true;
      }
      return core::Result<void>::success();
    }
    if (http_status != 200 || !event_stream) {
      if (error_body.size() < 8192) {
        error_body.append(line.substr(0, 8192 - error_body.size()));
        error_body += '\n';
      }
      return core::Result<void>::success();
    }
    if (line.empty())
      return dispatch_event();
    if (line.starts_with("data:")) {
      auto data = line.substr(5);
      if (data.starts_with(' '))
        data.remove_prefix(1);
      if (event_data.size() + data.size() + 1 > kMaximumSseLineBytes)
        throw ProtocolError("OpenCode SSE event exceeds 1 MiB limit");
      if (!event_data.empty())
        event_data += '\n';
      event_data += data;
    }
    return core::Result<void>::success();
  };
  LineBuffer line_buffer(consume_line);
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

    if (cancelled || timed_out)
      break;
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
          if (cancellation.is_cancelled()) {
            cancelled = true;
            break;
          }
          if (std::chrono::steady_clock::now() - started_at >=
              std::chrono::milliseconds(config.request_timeout_ms)) {
            timed_out = true;
            break;
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
    if ((cancelled || timed_out) && !termination_requested) {
      terminate_child();
      termination_requested = true;
      output_read.reset();
      pipe_closed = true;
    }
  }

  if (cancelled || cancellation.is_cancelled())
    return core::Result<void>::failure(
        {ErrorCode::cancelled, "model request cancelled"});
  if (timed_out)
    return core::Result<void>::failure(
        {ErrorCode::timeout, "OpenCode request timed out", true});
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    throw ProtocolError(
        "OpenCode HTTP transport failed (curl exit " +
        std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
  const auto processed = line_buffer.flush();
  if (!processed)
    return processed;
  if (http_status != 200) {
    std::string detail;
    try {
      const auto error = parse_json(error_body);
      if (error.is_object())
        detail =
            ": " + response_error_message(error, "upstream request rejected");
    } catch (const Json::exception &) {
      detail = ": non-JSON or truncated error body";
    } catch (const ProtocolError &) {
      detail = ": error body exceeds JSON nesting limit";
    }
    throw ProtocolError(
        "OpenCode HTTP " + std::to_string(http_status) + detail,
        http_status == 408 || http_status == 429 || http_status >= 500,
        http_status == 400 || http_status == 401 || http_status == 403
            ? ErrorCode::invalid_argument
            : ErrorCode::model_error);
  }
  if (!event_stream)
    throw ProtocolError(
        "OpenCode HTTP 200 without text/event-stream content type");
  if (!event_data.empty())
    throw ProtocolError("OpenCode stream ended inside an SSE event");
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
OpenCodeGoModel::complete(const ModelRequest &input,
                          const core::StreamCallback &on_delta,
                          core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<AssistantResponse>::failure(
        {ErrorCode::cancelled, "model request cancelled"});
  try {
    if (config_.api_key.empty())
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::invalid_argument, "OpenCode Go API key is empty"});
    if (input.messages.size() > 65'536 ||
        input.tools.size() > kMaximumToolCalls)
      throw ProtocolError("OpenCode request exceeds message/tool count limit",
                          false, ErrorCode::invalid_argument);
    std::size_t input_bytes = 0;
    const auto check_text = [&](std::string_view text) {
      if (text.size() > kMaximumStreamBytes - input_bytes)
        throw ProtocolError("OpenCode request exceeds 16 MiB limit", false,
                            ErrorCode::invalid_argument);
      input_bytes += text.size();
      if (!core::is_valid_utf8(text))
        throw ProtocolError("OpenCode request contains invalid UTF-8", false,
                            ErrorCode::invalid_argument);
    };
    check_text(input.model.model);
    check_text(input.session_id);
    for (const auto &message : input.messages) {
      check_text(message.id);
      check_text(message.content);
      check_text(message.model_state);
      if (message.tool_call_id)
        check_text(*message.tool_call_id);
      if (message.tool_calls.size() > kMaximumToolCalls)
        throw ProtocolError("OpenCode request exceeds tool call limit", false,
                            ErrorCode::invalid_argument);
      for (const auto &call : message.tool_calls) {
        check_text(call.id);
        check_text(call.name);
        check_text(call.arguments_json);
      }
    }
    for (const auto &tool : input.tools) {
      check_text(tool.name);
      check_text(tool.description);
      check_text(tool.input_schema_json);
    }
    ModelRequest request = input;
    if (request.session_id.empty())
      request.session_id = core::new_conversation_id();
    if (request.session_id.size() > 64 ||
        std::any_of(request.session_id.begin(), request.session_id.end(),
                    [](unsigned char c) {
                      return !(c >= 'a' && c <= 'z') &&
                             !(c >= 'A' && c <= 'Z') &&
                             !(c >= '0' && c <= '9') && c != '-' && c != '_';
                    }))
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::invalid_argument,
           "OpenCode session_id must contain 1-64 ASCII letters, digits, "
           "hyphens or underscores"});
    if (request.model.model.empty() || !std::isfinite(request.temperature) ||
        (request.max_output_tokens && *request.max_output_tokens == 0) ||
        (config_.request_timeout_ms == 0 ||
         config_.request_timeout_ms >
             static_cast<std::size_t>(std::numeric_limits<long long>::max())))
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::invalid_argument,
           "OpenCode invalid model, temperature, output limit or timeout"});
    if (config_.api_key.size() > 4096 || config_.endpoint.size() > 4096 ||
        config_.api_key.find_first_of("\r\n") != std::string::npos ||
        config_.endpoint.find_first_of("\r\n") != std::string::npos)
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::invalid_argument, "OpenCode invalid API key header"});
    for (const auto &message : request.messages) {
      const auto valid = validate_tool_calls(message.tool_calls);
      if (!valid)
        return core::Result<AssistantResponse>::failure(valid.error());
    }
    std::sort(request.tools.begin(), request.tools.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    std::set<std::string> names;
    for (const auto &tool : request.tools) {
      if (tool.name.empty() || !names.insert(tool.name).second ||
          !parse_json(tool.input_schema_json).is_object())
        throw ProtocolError("OpenCode invalid or duplicate tool schema");
    }
    const auto *model_info =
        find_opencode_go_model(config_.models, request.model.model);
    const auto protocol = model_info == nullptr
                              ? infer_open_code_protocol(request.model.model)
                              : model_info->protocol;
    if (model_info && request.max_output_tokens &&
        model_info->max_output_tokens &&
        *request.max_output_tokens > model_info->max_output_tokens)
      return core::Result<AssistantResponse>::failure(
          {ErrorCode::invalid_argument,
           "OpenCode max_output_tokens exceeds model limit"});
    const auto endpoint = endpoint_for(config_, protocol);
    std::vector<std::string> headers = {
        "User-Agent: zeda/" ZEDA_VERSION, "x-opencode-client: zeda",
        "x-opencode-session: " + request.session_id};
    AssistantResponse response;
    core::Result<void> result = core::Result<void>::success();
    const auto run = [&](const std::string &body, const auto &consume) {
      if (body.size() > kMaximumStreamBytes)
        throw ProtocolError("OpenCode request exceeds 16 MiB limit");
      return run_curl(config_, endpoint, headers, body, cancellation, consume);
    };
    if (protocol == OpenCodeProtocol::responses) {
      ResponsesStreamState state;
      result = run(responses_request_json(request, model_info),
                   [&](std::string_view line) {
                     return process_responses_sse(line, state, on_delta);
                   });
      response = std::move(state.response);
      if (result && !state.items.empty()) {
        Json items = Json::array();
        for (auto &[index, item] : state.items) {
          static_cast<void>(index);
          items.push_back(std::move(item));
        }
        response.model_state =
            continuation(request, "responses", response, items);
      }
    } else if (protocol == OpenCodeProtocol::chat_completions) {
      ChatStreamState state;
      result = run(chat_request_json(request, model_info),
                   [&](std::string_view line) {
                     return process_chat_sse(line, state, on_delta);
                   });
      state.response.tool_calls = std::move(state.indexed_calls);
      response = std::move(state.response);
      if (result && !state.reasoning.empty())
        response.model_state =
            continuation(request, "chat", response, state.reasoning);
    } else {
      headers.push_back("x-api-key: " + config_.api_key);
      headers.push_back("anthropic-version: 2023-06-01");
      MessagesStreamState state;
      result = run(messages_request_json(request, model_info),
                   [&](std::string_view line) {
                     return process_messages_sse(line, state, on_delta);
                   });
      response = std::move(state.response);
      if (result && !state.message_stopped)
        throw ProtocolError(
            "OpenCode Messages stream ended without message_stop");
      if (result && !state.blocks.empty()) {
        Json blocks = Json::array();
        for (auto &[index, block] : state.blocks) {
          static_cast<void>(index);
          blocks.push_back(std::move(block));
        }
        response.model_state =
            continuation(request, "messages", response, blocks);
      }
    }
    if (!result)
      return core::Result<AssistantResponse>::failure(result.error());
    if (response.finish_reason == FinishReason::unknown)
      throw ProtocolError(
          "OpenCode stream ended without a terminal response event");
    if (response.usage.cached_input_tokens > response.usage.input_tokens)
      throw ProtocolError("OpenCode cached token count exceeds total input");
    // Length/refusal responses may contain incomplete arguments; core handles
    // their terminal state.
    if (response.finish_reason == FinishReason::stop ||
        response.finish_reason == FinishReason::tool_calls) {
      const auto valid = validate_tool_calls(response.tool_calls);
      if (!valid)
        return core::Result<AssistantResponse>::failure(valid.error());
      if ((response.finish_reason == FinishReason::tool_calls) !=
          !response.tool_calls.empty())
        throw ProtocolError("OpenCode tool calls disagree with finish reason");
    }
    return core::Result<AssistantResponse>::success(std::move(response));
  } catch (const ProtocolError &error) {
    std::string message = error.what();
    if (!config_.api_key.empty()) {
      std::size_t position = 0;
      while ((position = message.find(config_.api_key, position)) !=
             std::string::npos) {
        message.replace(position, config_.api_key.size(), "[redacted]");
        position += std::string_view("[redacted]").size();
      }
    }
    return core::Result<AssistantResponse>::failure(
        {error.code, std::move(message), error.retryable});
  } catch (const Json::exception &error) {
    return core::Result<AssistantResponse>::failure(
        {ErrorCode::model_error, "OpenCode invalid protocol JSON (error " +
                                     std::to_string(error.id) + ")"});
  } catch (const std::exception &) {
    return core::Result<AssistantResponse>::failure(
        {ErrorCode::model_error,
         "OpenCode request failed at provider boundary"});
  }
}

} // namespace zed::providers
