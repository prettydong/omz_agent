#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "zed/providers/opencode_go_catalog.hpp"
#include "zed/providers/opencode_go_model.hpp"

namespace {

using namespace zed::core;

void require(bool condition) {
  if (!condition)
    std::abort();
}

struct FixtureServer {
  std::uint16_t port{};
  std::thread thread;
};

struct FixtureReply {
  int status{200};
  std::string body;
  std::vector<std::string> headers;
};

FixtureServer start_sequence_server(
    std::vector<FixtureReply> replies,
    std::function<void(std::string_view, const nlohmann::json &)> validate,
    std::vector<std::string> *captured_requests = nullptr) {
  const int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  require(server_socket >= 0);
  int reuse = 1;
  require(setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse)) == 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  require(bind(server_socket, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) == 0);
  socklen_t address_length = sizeof(address);
  require(getsockname(server_socket, reinterpret_cast<sockaddr *>(&address),
                      &address_length) == 0);
  require(listen(server_socket, static_cast<int>(replies.size())) == 0);
  const auto port = ntohs(address.sin_port);

  std::thread server([server_socket, replies = std::move(replies),
                      validate = std::move(validate), captured_requests]() {
    for (const auto &reply : replies) {
      const int client = accept(server_socket, nullptr, nullptr);
      require(client >= 0);
      std::string request;
      std::size_t expected_size = 0;
      while (expected_size == 0 || request.size() < expected_size) {
        char buffer[4096];
        const auto received = recv(client, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR)
          continue;
        require(received > 0);
        request.append(buffer, static_cast<std::size_t>(received));
        const auto header_end = request.find("\r\n\r\n");
        const auto length_header = request.find("Content-Length: ");
        if (header_end == std::string::npos ||
            length_header == std::string::npos)
          continue;
        const auto length_start =
            length_header + std::string_view("Content-Length: ").size();
        const auto length_end = request.find("\r\n", length_start);
        require(length_end != std::string::npos);
        expected_size = header_end + 4 +
                        static_cast<std::size_t>(std::stoull(request.substr(
                            length_start, length_end - length_start)));
      }
      const auto first_line_end = request.find("\r\n");
      const auto body_start = request.find("\r\n\r\n");
      require(first_line_end != std::string::npos);
      require(body_start != std::string::npos);
      validate(std::string_view(request).substr(0, first_line_end),
               nlohmann::json::parse(request.substr(body_start + 4)));
      if (captured_requests != nullptr)
        captured_requests->push_back(request);

      std::string response = "HTTP/1.1 " + std::to_string(reply.status) +
                             (reply.status == 200 ? " OK" : " Error") +
                             "\r\nContent-Type: text/event-stream\r\n";
      for (const auto &header : reply.headers)
        response += header + "\r\n";
      response += "Content-Length: " + std::to_string(reply.body.size()) +
                  "\r\nConnection: close\r\n\r\n" + reply.body;
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count =
            send(client, response.data() + sent, response.size() - sent, 0);
        if (count < 0 && errno == EINTR)
          continue;
        require(count > 0);
        sent += static_cast<std::size_t>(count);
      }
      close(client);
    }
    close(server_socket);
  });
  return {port, std::move(server)};
}

FixtureServer start_server(
    std::string response_body,
    std::function<void(std::string_view, const nlohmann::json &)> validate,
    int status = 200, std::vector<std::string> *captured_requests = nullptr,
    int request_count = 1) {
  const int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  require(server_socket >= 0);
  int reuse = 1;
  require(setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse)) == 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  require(bind(server_socket, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) == 0);
  socklen_t address_length = sizeof(address);
  require(getsockname(server_socket, reinterpret_cast<sockaddr *>(&address),
                      &address_length) == 0);
  require(listen(server_socket, 1) == 0);
  const auto port = ntohs(address.sin_port);

  std::thread server([server_socket, response_body = std::move(response_body),
                      validate = std::move(validate), status, captured_requests,
                      request_count]() {
    for (int index = 0; index < request_count; ++index) {
      const int client = accept(server_socket, nullptr, nullptr);
      require(client >= 0);
      std::string request;
      std::size_t expected_size = 0;
      while (expected_size == 0 || request.size() < expected_size) {
        char buffer[4096];
        const auto received = recv(client, buffer, sizeof(buffer), 0);
        if (received < 0 && errno == EINTR)
          continue;
        require(received > 0);
        request.append(buffer, static_cast<std::size_t>(received));
        const auto header_end = request.find("\r\n\r\n");
        const auto length_header = request.find("Content-Length: ");
        if (header_end == std::string::npos ||
            length_header == std::string::npos)
          continue;
        const auto length_start =
            length_header + std::string_view("Content-Length: ").size();
        const auto length_end = request.find("\r\n", length_start);
        require(length_end != std::string::npos);
        const auto content_length = std::stoull(
            request.substr(length_start, length_end - length_start));
        expected_size =
            header_end + 4 + static_cast<std::size_t>(content_length);
      }

      const auto first_line_end = request.find("\r\n");
      const auto body_start = request.find("\r\n\r\n");
      require(first_line_end != std::string::npos);
      require(body_start != std::string::npos);
      const auto body = nlohmann::json::parse(request.substr(body_start + 4));
      validate(std::string_view(request).substr(0, first_line_end), body);
      if (captured_requests != nullptr)
        captured_requests->push_back(request);

      const std::string response = "HTTP/1.1 " + std::to_string(status) +
                                   (status == 200 ? " OK" : " Error") +
                                   "\r\nContent-Type: "
                                   "text/event-stream\r\nContent-Length: " +
                                   std::to_string(response_body.size()) +
                                   "\r\nConnection: close\r\n\r\n" +
                                   response_body;
      std::size_t sent = 0;
      while (sent < response.size()) {
        const auto count =
            send(client, response.data() + sent, response.size() - sent, 0);
        if (count < 0 && errno == EINTR)
          continue;
        require(count > 0);
        sent += static_cast<std::size_t>(count);
      }
      close(client);
    }
    close(server_socket);
  });
  return {port, std::move(server)};
}

FixtureServer start_stalling_server(std::chrono::milliseconds duration) {
  const int server_socket = socket(AF_INET, SOCK_STREAM, 0);
  require(server_socket >= 0);
  int reuse = 1;
  require(setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse)) == 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  require(bind(server_socket, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) == 0);
  socklen_t address_length = sizeof(address);
  require(getsockname(server_socket, reinterpret_cast<sockaddr *>(&address),
                      &address_length) == 0);
  require(listen(server_socket, 1) == 0);
  const auto port = ntohs(address.sin_port);

  std::thread server([server_socket, duration]() {
    const int client = accept(server_socket, nullptr, nullptr);
    require(client >= 0);
    std::this_thread::sleep_for(duration);
    close(client);
    close(server_socket);
  });
  return {port, std::move(server)};
}

ModelRequest request_for(std::string model, ReasoningEffort effort) {
  ModelRequest request;
  request.model = {"opencode-go", std::move(model)};
  request.reasoning_effort = effort;
  request.messages = {
      {"system", Role::system, "Be concise.", {}, std::nullopt},
      {"user", Role::user, "hello", {}, std::nullopt},
  };
  request.tools = {
      {"read", "Read a file.",
       R"({"type":"object","properties":{"purpose":{"type":"string"},"path":{"type":"string"}},"required":["purpose","path"]})"}};
  return request;
}

void require_header(std::string_view request, std::string_view header) {
  require(request.find("\r\n" + std::string(header) + "\r\n") !=
          std::string_view::npos);
}

std::string header_value(std::string_view request, std::string_view name) {
  const std::string prefix = "\r\n" + std::string(name) + ": ";
  const auto value_start = request.find(prefix);
  require(value_start != std::string_view::npos);
  const auto content_start = value_start + prefix.size();
  const auto value_end = request.find("\r\n", content_start);
  require(value_end != std::string_view::npos);
  return std::string(request.substr(content_start, value_end - content_start));
}

} // namespace

int main() {
  constexpr std::string_view catalog_fixture = R"(opencode-go/chat-model
{
  "id": "chat-model",
  "name": "Chat Model",
  "api": {"npm": "@ai-sdk/openai-compatible"},
  "limit": {"context": 200000, "output": 32000},
  "capabilities": {"temperature": true},
  "variants": {"low": {"reasoningEffort": "low"}, "max": {"reasoningEffort": "max"}}
}
opencode-go/messages-model
{
  "id": "messages-model",
  "name": "Messages Model",
  "api": {"npm": "@ai-sdk/anthropic"},
  "limit": {"context": 1000000, "output": 64000},
  "capabilities": {"temperature": false},
  "variants": {"none": {"thinking": {"type": "disabled"}}, "thinking": {"thinking": {"type": "adaptive"}}}
}
opencode-go/responses-model
{
  "id": "responses-model",
  "name": "Responses Model",
  "api": {"npm": "@ai-sdk/openai"},
  "limit": {"context": 1050000, "output": 128000},
  "capabilities": {"temperature": false},
  "variants": {"minimal": {"reasoningEffort": "minimal"}, "xhigh": {"reasoningEffort": "xhigh"}}
}
)";
  const auto parsed = zed::providers::parse_opencode_go_models(catalog_fixture);
  assert(parsed);
  assert(parsed.value().size() == 3);
  assert(parsed.value()[0].protocol ==
         zed::providers::OpenCodeProtocol::chat_completions);
  assert(parsed.value()[1].protocol ==
         zed::providers::OpenCodeProtocol::messages);
  assert(parsed.value()[2].protocol ==
         zed::providers::OpenCodeProtocol::responses);
  assert(parsed.value()[0].max_context_tokens == 200000);
  assert(zed::providers::supports_reasoning_effort(parsed.value()[0],
                                                   ReasoningEffort::max));
  assert(!zed::providers::supports_reasoning_effort(parsed.value()[0],
                                                    ReasoningEffort::high));
  assert(zed::providers::supports_reasoning_effort(parsed.value()[0],
                                                   ReasoningEffort::automatic));

  const std::string chat_events =
      "data: {\"choices\":[{\"delta\":{\"content\":\"hello "
      "\"},\"finish_reason\":null}]}\n\n"
      "data: "
      "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-"
      "chat\",\"function\":{\"name\":\"read\",\"arguments\":\"{\\\"purpose\\\":"
      "\\\"Read fixture\\\",\"}}]},\"finish_reason\":null}]}\n\n"
      "data: "
      "{\"choices\":[{\"delta\":{\"content\":\"world\",\"tool_calls\":[{"
      "\"index\":0,\"function\":{\"arguments\":\"\\\"path\\\":\\\"x.txt\\\"}\"}"
      "}]},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":12,"
      "\"completion_tokens\":4,\"prompt_tokens_details\":{\"cached_tokens\":7}}"
      "}\n\n"
      "data: [DONE]\n\n";
  std::vector<std::string> chat_requests;
  auto chat_server = start_server(
      chat_events,
      [](std::string_view request_line, const nlohmann::json &body) {
        assert(request_line == "POST /v1/chat/completions HTTP/1.1");
        assert(body.at("model") == "chat-model");
        assert(body.at("reasoning_effort") == "max");
        assert(body.at("messages").is_array());
        assert(body.at("tools").at(0).at("function").at("name") == "read");
      },
      200, &chat_requests);
  zed::providers::OpenCodeGoModel chat_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(chat_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  std::string chat_streamed;
  auto chat_request = request_for("chat-model", ReasoningEffort::max);
  chat_request.session_id = "chat-session";
  const auto chat_response = chat_model.complete(
      chat_request,
      [&](const ModelDelta &delta) { chat_streamed += delta.text; }, {});
  chat_server.thread.join();
  assert(chat_response);
  assert(chat_streamed == "hello world");
  assert(chat_response.value().finish_reason == FinishReason::tool_calls);
  assert(chat_response.value().tool_calls.size() == 1);
  assert(chat_response.value().tool_calls[0].id == "call-chat");
  assert(chat_response.value().tool_calls[0].arguments_json ==
         R"({"purpose":"Read fixture","path":"x.txt"})");
  assert(chat_response.value().usage.input_tokens == 12);
  assert(chat_response.value().usage.cached_input_tokens == 7);
  assert(chat_requests.size() == 1);
  require_header(chat_requests[0], "User-Agent: omz-agent/0.2");
  require_header(chat_requests[0], "x-opencode-session: chat-session");

  const std::string responses_events =
      "data: {\"type\":\"response.output_text.delta\",\"delta\":\"done\"}\n\n"
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
      "\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_"
      "text\","
      "\"text\":\"done\"}]}]}"
      "}\n\n";
  std::vector<std::string> responses_requests;
  auto responses_server = start_server(
      responses_events,
      [](std::string_view request_line, const nlohmann::json &body) {
        assert(request_line == "POST /v1/responses HTTP/1.1");
        assert(body.at("model") == "responses-model");
      },
      200, &responses_requests);
  zed::providers::OpenCodeGoModel responses_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(responses_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  auto responses_request =
      request_for("responses-model", ReasoningEffort::minimal);
  responses_request.session_id = "responses-session";
  std::string responses_streamed;
  const auto responses_response = responses_model.complete(
      responses_request,
      [&](const ModelDelta &delta) { responses_streamed += delta.text; }, {});
  responses_server.thread.join();
  assert(responses_response);
  assert(responses_response.value().content == "done");
  assert(responses_streamed == "done");
  assert(responses_requests.size() == 1);
  require_header(responses_requests[0], "User-Agent: omz-agent/0.2");
  require_header(responses_requests[0],
                 "x-opencode-session: responses-session");

  const std::string completed_output_events =
      "data: "
      "{\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
      "\"output\":[{\"type\":\"message\",\"content\":[{\"type\":\"output_"
      "text\","
      "\"text\":\"first "
      "\"},{\"type\":\"output_text\",\"text\":\"second\"}]}]}}\n\n";
  auto completed_output_server = start_server(
      completed_output_events, [](std::string_view, const nlohmann::json &) {});
  zed::providers::OpenCodeGoModel completed_output_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(completed_output_server.port) +
          "/v1",
      5'000,
      parsed.value(),
  });
  std::string completed_output_streamed;
  const auto completed_output_response = completed_output_model.complete(
      request_for("responses-model", ReasoningEffort::minimal),
      [&](const ModelDelta &delta) { completed_output_streamed += delta.text; },
      {});
  completed_output_server.thread.join();
  assert(completed_output_response);
  assert(completed_output_response.value().content == "first second");
  assert(completed_output_streamed == "first second");

  const std::string invalid_index_events =
      R"(data: {"choices":[{"delta":{"tool_calls":[{"index":128,"id":"too-large","function":{"name":"read","arguments":"{}"}}]}}]}

)";
  auto invalid_index_server = start_server(
      invalid_index_events, [](std::string_view, const nlohmann::json &) {});
  zed::providers::OpenCodeGoModel invalid_index_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(invalid_index_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  const auto invalid_index_response = invalid_index_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {});
  invalid_index_server.thread.join();
  assert(!invalid_index_response);
  assert(invalid_index_response.error().message.find("index above the limit") !=
         std::string::npos);

  const std::string recovered_events =
      "data: {\"choices\":[{\"delta\":{\"content\":\"recovered\"},"
      "\"finish_reason\":\"stop\"}]}\n\n";
  std::vector<std::string> retry_requests;
  auto retry_server = start_sequence_server(
      {{503, "data: not valid SSE\n\n", {}}, {200, recovered_events, {}}},
      [](std::string_view, const nlohmann::json &) {}, &retry_requests);
  zed::providers::OpenCodeGoModel retry_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(retry_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  std::vector<ModelDeltaKind> retry_kinds;
  const auto retry_response = retry_model.complete(
      request_for("chat-model", ReasoningEffort::max),
      [&](const ModelDelta &delta) { retry_kinds.push_back(delta.kind); }, {});
  retry_server.thread.join();
  assert(retry_response);
  assert(retry_response.value().content == "recovered");
  assert(retry_requests.size() == 2);
  assert(retry_kinds.size() == 2);
  assert(retry_kinds[0] == ModelDeltaKind::retry);
  assert(retry_kinds[1] == ModelDeltaKind::text);

  std::vector<std::string> throttled_requests;
  auto throttled_server = start_sequence_server(
      {{429, "rate limited", {"Retry-After: 0"}}, {200, recovered_events, {}}},
      [](std::string_view, const nlohmann::json &) {}, &throttled_requests);
  zed::providers::OpenCodeGoModel throttled_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(throttled_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  assert(throttled_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {}));
  throttled_server.thread.join();
  assert(throttled_requests.size() == 2);

  std::vector<std::string> past_date_requests;
  const auto past_date_start = std::chrono::steady_clock::now();
  auto past_date_server = start_sequence_server(
      {{429, "rate limited", {"Retry-After: Sun, 06 Sep 2026 00:00:00 GMT"}},
       {200, recovered_events, {}}},
      [](std::string_view, const nlohmann::json &) {}, &past_date_requests);
  zed::providers::OpenCodeGoModel past_date_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(past_date_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  assert(past_date_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {}));
  past_date_server.thread.join();
  assert(past_date_requests.size() == 2);
  assert(std::chrono::steady_clock::now() - past_date_start <
         std::chrono::milliseconds(200));

  std::vector<std::string> exhausted_requests;
  auto exhausted_server = start_sequence_server(
      {{503, "unavailable", {}},
       {503, "unavailable", {}},
       {503, "unavailable", {}}},
      [](std::string_view, const nlohmann::json &) {}, &exhausted_requests);
  zed::providers::OpenCodeGoModel exhausted_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(exhausted_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  const auto exhausted_response = exhausted_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {});
  exhausted_server.thread.join();
  assert(!exhausted_response);
  assert(exhausted_requests.size() == 3);
  assert(exhausted_response.error().message.find("status 503") !=
         std::string::npos);
  assert(exhausted_response.error().message.find("attempted 3 times") !=
         std::string::npos);

  for (const int status : {400, 401}) {
    std::vector<std::string> permanent_requests;
    auto permanent_server = start_sequence_server(
        {{status, "permanent failure", {}}},
        [](std::string_view, const nlohmann::json &) {}, &permanent_requests);
    zed::providers::OpenCodeGoModel permanent_model({
        "fixture-key",
        "http://127.0.0.1:" + std::to_string(permanent_server.port) + "/v1",
        5'000,
        parsed.value(),
    });
    const auto permanent_response = permanent_model.complete(
        request_for("chat-model", ReasoningEffort::max), {}, {});
    permanent_server.thread.join();
    assert(!permanent_response);
    assert(permanent_requests.size() == 1);
  }

  auto budget_server =
      start_sequence_server({{429, "rate limited", {"Retry-After: 1"}}},
                            [](std::string_view, const nlohmann::json &) {});
  zed::providers::OpenCodeGoModel budget_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(budget_server.port) + "/v1",
      50,
      parsed.value(),
  });
  const auto budget_response = budget_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {});
  budget_server.thread.join();
  assert(!budget_response);
  assert(budget_response.error().code == ErrorCode::model_error);
  assert(budget_response.error().message.find("status 429") !=
         std::string::npos);

  const auto huge_retry_start = std::chrono::steady_clock::now();
  auto huge_retry_server = start_sequence_server(
      {{429, "rate limited", {"Retry-After: 999999999999999999999999"}}},
      [](std::string_view, const nlohmann::json &) {});
  zed::providers::OpenCodeGoModel huge_retry_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(huge_retry_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  const auto huge_retry_response = huge_retry_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {});
  huge_retry_server.thread.join();
  assert(!huge_retry_response);
  assert(huge_retry_response.error().code == ErrorCode::model_error);
  assert(std::chrono::steady_clock::now() - huge_retry_start <
         std::chrono::milliseconds(200));

  std::vector<std::string> retry_cancel_requests;
  auto retry_cancel_server = start_sequence_server(
      {{503, "unavailable", {"Retry-After: 0"}}},
      [](std::string_view, const nlohmann::json &) {}, &retry_cancel_requests);
  zed::providers::OpenCodeGoModel retry_cancel_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(retry_cancel_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  CancellationSource retry_cancellation;
  const auto retry_cancel_response = retry_cancel_model.complete(
      request_for("chat-model", ReasoningEffort::max),
      [&retry_cancellation](const ModelDelta &delta) {
        if (delta.kind == ModelDeltaKind::retry)
          retry_cancellation.cancel();
      },
      retry_cancellation.token());
  retry_cancel_server.thread.join();
  assert(!retry_cancel_response);
  assert(retry_cancel_response.error().code == ErrorCode::cancelled);
  assert(retry_cancel_requests.size() == 1);

  auto cancellation_server =
      start_stalling_server(std::chrono::milliseconds(300));
  zed::providers::OpenCodeGoModel cancellation_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(cancellation_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  CancellationSource cancellation;
  std::thread cancel_request([&cancellation]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancellation.cancel();
  });
  const auto cancelled_response = cancellation_model.complete(
      request_for("chat-model", ReasoningEffort::max), {},
      cancellation.token());
  cancel_request.join();
  cancellation_server.thread.join();
  assert(!cancelled_response);
  assert(cancelled_response.error().code == ErrorCode::cancelled);

  auto timeout_server = start_stalling_server(std::chrono::milliseconds(300));
  zed::providers::OpenCodeGoModel timeout_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(timeout_server.port) + "/v1",
      50,
      parsed.value(),
  });
  const auto timeout_response = timeout_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {});
  timeout_server.thread.join();
  assert(!timeout_response);
  assert(timeout_response.error().code == ErrorCode::timeout);

  const std::string oversized_line =
      "data: " + std::string(1024 * 1024 + 1, 'x') + "\n";
  auto oversized_line_server = start_server(
      oversized_line, [](std::string_view, const nlohmann::json &) {});
  zed::providers::OpenCodeGoModel oversized_line_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(oversized_line_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  const auto oversized_line_response = oversized_line_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {});
  oversized_line_server.thread.join();
  assert(!oversized_line_response);
  assert(oversized_line_response.error().message.find("1 MiB limit") !=
         std::string::npos);

  const std::string messages_events =
      "event: message_start\ndata: "
      "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":9,"
      "\"cache_read_input_tokens\":5}}}\n\n"
      "event: content_block_start\ndata: "
      "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
      "\"type\":\"text\",\"text\":\"\"}}\n\n"
      "event: content_block_delta\ndata: "
      "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
      "\"text_delta\",\"text\":\"done\"}}\n\n"
      "event: content_block_stop\ndata: "
      "{\"type\":\"content_block_stop\",\"index\":0}\n\n"
      "event: message_delta\ndata: "
      "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
      "\"usage\":{\"output_tokens\":3}}\n\n"
      "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
  std::vector<std::string> messages_requests;
  auto messages_server = start_server(
      messages_events,
      [](std::string_view request_line, const nlohmann::json &body) {
        assert(request_line == "POST /v1/messages HTTP/1.1");
        assert(body.at("model") == "messages-model");
        assert(body.at("thinking").at("type") == "adaptive");
        assert(!body.contains("temperature"));
        assert(body.at("system") == "Be concise.");
        assert(body.at("tools").at(0).at("input_schema").is_object());
      },
      200, &messages_requests);
  zed::providers::OpenCodeGoModel messages_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(messages_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  auto messages_request =
      request_for("messages-model", ReasoningEffort::thinking);
  messages_request.session_id = "messages-session";
  const auto messages_response =
      messages_model.complete(messages_request, {}, {});
  messages_server.thread.join();
  assert(messages_response);
  assert(messages_response.value().content == "done");
  assert(messages_response.value().finish_reason == FinishReason::stop);
  assert(messages_response.value().usage.input_tokens == 9);
  assert(messages_response.value().usage.output_tokens == 3);
  assert(messages_response.value().usage.cached_input_tokens == 5);
  assert(messages_requests.size() == 1);
  require_header(messages_requests[0], "User-Agent: omz-agent/0.2");
  require_header(messages_requests[0], "x-opencode-session: messages-session");

  std::vector<std::string> fallback_requests;
  auto fallback_server = start_server(
      chat_events, [](std::string_view, const nlohmann::json &) {}, 200,
      &fallback_requests, 2);
  zed::providers::OpenCodeGoModel fallback_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(fallback_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  assert(fallback_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {}));
  assert(fallback_model.complete(
      request_for("chat-model", ReasoningEffort::max), {}, {}));
  fallback_server.thread.join();
  assert(fallback_requests.size() == 2);
  const auto fallback_first =
      header_value(fallback_requests[0], "x-opencode-session");
  assert(!fallback_first.empty());
  assert(fallback_first ==
         header_value(fallback_requests[1], "x-opencode-session"));

  std::vector<std::string> switched_requests;
  auto switched_server = start_server(
      chat_events, [](std::string_view, const nlohmann::json &) {}, 200,
      &switched_requests, 2);
  zed::providers::OpenCodeGoModel switched_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(switched_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  auto first_session_request = request_for("chat-model", ReasoningEffort::max);
  first_session_request.session_id = "session-one";
  auto second_session_request = request_for("chat-model", ReasoningEffort::max);
  second_session_request.session_id = "session-two";
  assert(switched_model.complete(first_session_request, {}, {}));
  assert(switched_model.complete(second_session_request, {}, {}));
  switched_server.thread.join();
  assert(header_value(switched_requests[0], "x-opencode-session") ==
         "session-one");
  assert(header_value(switched_requests[1], "x-opencode-session") ==
         "session-two");

  auto invalid_session_request =
      request_for("chat-model", ReasoningEffort::max);
  invalid_session_request.session_id = "session\r\nInjected: value";
  const auto invalid_session_response =
      switched_model.complete(invalid_session_request, {}, {});
  assert(!invalid_session_response);
  assert(invalid_session_response.error().code == ErrorCode::invalid_argument);
  return 0;
}
