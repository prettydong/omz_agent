#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

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

FixtureServer start_server(
    std::string response_body,
    std::function<void(std::string_view, const nlohmann::json &)> validate) {
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
                      validate = std::move(validate)]() {
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
      if (header_end == std::string::npos || length_header == std::string::npos)
        continue;
      const auto length_start =
          length_header + std::string_view("Content-Length: ").size();
      const auto length_end = request.find("\r\n", length_start);
      require(length_end != std::string::npos);
      const auto content_length =
          std::stoull(request.substr(length_start, length_end - length_start));
      expected_size = header_end + 4 + static_cast<std::size_t>(content_length);
    }

    const auto first_line_end = request.find("\r\n");
    const auto body_start = request.find("\r\n\r\n");
    require(first_line_end != std::string::npos);
    require(body_start != std::string::npos);
    const auto body = nlohmann::json::parse(request.substr(body_start + 4));
    validate(std::string_view(request).substr(0, first_line_end), body);

    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: "
                                 "text/event-stream\r\nContent-Length: " +
                                 std::to_string(response_body.size()) +
                                 "\r\nConnection: close\r\n\r\n" +
                                 response_body;
    const auto sent = send(client, response.data(), response.size(), 0);
    require(sent == static_cast<ssize_t>(response.size()));
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
  auto chat_server = start_server(chat_events, [](std::string_view request_line,
                                                  const nlohmann::json &body) {
    assert(request_line == "POST /v1/chat/completions HTTP/1.1");
    assert(body.at("model") == "chat-model");
    assert(body.at("reasoning_effort") == "max");
    assert(body.at("messages").is_array());
    assert(body.at("tools").at(0).at("function").at("name") == "read");
  });
  zed::providers::OpenCodeGoModel chat_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(chat_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  std::string chat_streamed;
  const auto chat_response = chat_model.complete(
      request_for("chat-model", ReasoningEffort::max),
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
  auto messages_server =
      start_server(messages_events, [](std::string_view request_line,
                                       const nlohmann::json &body) {
        assert(request_line == "POST /v1/messages HTTP/1.1");
        assert(body.at("model") == "messages-model");
        assert(body.at("thinking").at("type") == "adaptive");
        assert(!body.contains("temperature"));
        assert(body.at("system") == "Be concise.");
        assert(body.at("tools").at(0).at("input_schema").is_object());
      });
  zed::providers::OpenCodeGoModel messages_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(messages_server.port) + "/v1",
      5'000,
      parsed.value(),
  });
  const auto messages_response = messages_model.complete(
      request_for("messages-model", ReasoningEffort::thinking), {}, {});
  messages_server.thread.join();
  assert(messages_response);
  assert(messages_response.value().content == "done");
  assert(messages_response.value().finish_reason == FinishReason::stop);
  assert(messages_response.value().usage.input_tokens == 9);
  assert(messages_response.value().usage.output_tokens == 3);
  assert(messages_response.value().usage.cached_input_tokens == 5);
  return 0;
}
