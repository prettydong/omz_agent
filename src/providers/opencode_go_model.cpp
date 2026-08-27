#include "zed/providers/opencode_go_model.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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

std::string request_json(const ModelRequest &request) {
  Json root = Json::object();
  root["model"] = request.model.model;
  root["stream"] = true;
  root["store"] = false;
  if (request.max_output_tokens.has_value()) {
    root["max_output_tokens"] = static_cast<double>(*request.max_output_tokens);
  }
  root["temperature"] = request.temperature;
  if (request.reasoning_effort != core::ReasoningEffort::none) {
    root["reasoning"] = {
        {"effort", core::reasoning_effort_name(request.reasoning_effort)},
    };
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

core::Result<void> process_sse(std::string_view line,
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

core::Result<void>
run_curl(const OpenCodeGoConfig &config, std::string_view body,
         core::CancellationToken cancellation,
         const std::function<core::Result<void>(std::string_view)> &on_line) {
  std::string temporary_template =
      (std::filesystem::temp_directory_path() / "zed-request-XXXXXX").string();
  std::vector<char> temporary_path(temporary_template.begin(),
                                   temporary_template.end());
  temporary_path.push_back('\0');
  const int temporary_fd = mkstemp(temporary_path.data());
  if (temporary_fd == -1)
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot create request file"});
  const ssize_t written = write(temporary_fd, body.data(), body.size());
  close(temporary_fd);
  if (written != static_cast<ssize_t>(body.size())) {
    unlink(temporary_path.data());
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot write request body"});
  }

  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    unlink(temporary_path.data());
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot create HTTP output pipe"});
  }
  int config_pipe[2];
  if (pipe(config_pipe) != 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    unlink(temporary_path.data());
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot create curl config pipe"});
  }
  const pid_t child = fork();
  if (child == -1) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    close(config_pipe[0]);
    close(config_pipe[1]);
    unlink(temporary_path.data());
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot start curl"});
  }
  if (child == 0) {
    close(output_pipe[0]);
    close(config_pipe[1]);
    setpgid(0, 0);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(config_pipe[0], STDIN_FILENO);
    close(output_pipe[1]);
    close(config_pipe[0]);
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

  setpgid(child, child);

  close(output_pipe[1]);
  close(config_pipe[0]);
  const std::string curl_config =
      "url = \"" + curl_config_escape(config.endpoint) + "\"\n" +
      "request = \"POST\"\n" + "header = \"Authorization: Bearer " +
      curl_config_escape(config.api_key) + "\"\n" +
      "header = \"Content-Type: application/json\"\n" + "data-binary = \"@" +
      curl_config_escape(temporary_path.data()) + "\"\n";
  std::size_t config_offset = 0;
  while (config_offset < curl_config.size()) {
    const ssize_t written =
        write(config_pipe[1], curl_config.data() + config_offset,
              curl_config.size() - config_offset);
    if (written > 0) {
      config_offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    break;
  }
  close(config_pipe[1]);
  if (config_offset != curl_config.size()) {
    kill(-child, SIGTERM);
    waitpid(child, nullptr, 0);
    close(output_pipe[0]);
    unlink(temporary_path.data());
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot write curl config"});
  }
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags < 0 || fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
    kill(-child, SIGTERM);
    waitpid(child, nullptr, 0);
    close(output_pipe[0]);
    unlink(temporary_path.data());
    return core::Result<void>::failure(
        {ErrorCode::model_error, "cannot configure curl output"});
  }

  bool cancelled = false;
  bool timed_out = false;
  bool pipe_closed = false;
  bool child_finished = false;
  int status = 0;
  std::string pending;
  auto process_pending = [&](bool flush) -> core::Result<void> {
    while (true) {
      const auto newline = pending.find('\n');
      if (newline == std::string::npos)
        break;
      std::string line = pending.substr(0, newline);
      pending.erase(0, newline + 1);
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const auto processed = on_line(line);
      if (!processed)
        return processed;
    }
    if (flush && !pending.empty()) {
      if (!pending.empty() && pending.back() == '\r')
        pending.pop_back();
      const auto processed = on_line(pending);
      pending.clear();
      if (!processed)
        return processed;
    }
    return core::Result<void>::success();
  };

  const auto started_at = std::chrono::steady_clock::now();
  while (!pipe_closed || !child_finished) {
    if (cancellation.is_cancelled()) {
      cancelled = true;
      kill(-child, SIGTERM);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();
    if (elapsed >= static_cast<long long>(config.request_timeout_ms)) {
      timed_out = true;
      kill(-child, SIGTERM);
    }

    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int poll_result = poll(&descriptor, 1, 50);
    if (poll_result < 0 && errno != EINTR) {
      kill(-child, SIGTERM);
      waitpid(child, nullptr, 0);
      close(output_pipe[0]);
      unlink(temporary_path.data());
      return core::Result<void>::failure(
          {ErrorCode::model_error, "cannot poll curl output"});
    }
    if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[8192];
      while (true) {
        const ssize_t read_count = read(output_pipe[0], buffer, sizeof(buffer));
        if (read_count > 0) {
          pending.append(buffer, static_cast<std::size_t>(read_count));
          const auto processed = process_pending(false);
          if (!processed) {
            kill(-child, SIGTERM);
            waitpid(child, nullptr, 0);
            close(output_pipe[0]);
            unlink(temporary_path.data());
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

    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      child_finished = true;
    if ((cancelled || timed_out) && !child_finished) {
      kill(-child, SIGKILL);
      waitpid(child, &status, 0);
      child_finished = true;
    }
  }

  const auto processed = process_pending(true);
  close(output_pipe[0]);
  unlink(temporary_path.data());
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
    : config_(std::move(config)) {}

core::ModelCapabilities OpenCodeGoModel::capabilities() const {
  return {
      1'000'000, true, true, true, true,
  };
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

  AssistantResponse response;
  const auto result = run_curl(config_, request_json(request), cancellation,
                               [&](std::string_view line) {
                                 return process_sse(line, response, on_delta);
                               });
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
