#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "zed/core/context.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/providers/opencode_go_model.hpp"
#include "zed/session/jsonl_session_store.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/ui/terminal.hpp"

namespace {

using namespace zed::core;

ToolCall call(std::string id, std::string name, std::string arguments) {
  auto parsed = nlohmann::json::parse(arguments);
  if (parsed.is_object() && !parsed.contains("purpose")) {
    parsed["purpose"] = "Exercise the tool in the smoke test";
  }
  return {std::move(id), std::move(name), parsed.dump()};
}

void require(bool condition) {
  if (!condition) {
    std::abort();
  }
}

struct SseFixtureServer {
  std::uint16_t port{};
  std::thread thread;
};

SseFixtureServer start_sse_fixture_server(std::string body) {
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

  std::thread server([server_socket, body = std::move(body)]() {
    const int client = accept(server_socket, nullptr, nullptr);
    require(client >= 0);
    std::string request_text;
    std::size_t expected_size = 0;
    while (expected_size == 0 || request_text.size() < expected_size) {
      char request_buffer[4096];
      const auto received =
          recv(client, request_buffer, sizeof(request_buffer), 0);
      if (received < 0 && errno == EINTR)
        continue;
      require(received > 0);
      request_text.append(request_buffer, static_cast<std::size_t>(received));
      const auto header_end = request_text.find("\r\n\r\n");
      const auto length_header = request_text.find("Content-Length: ");
      if (header_end == std::string::npos ||
          length_header == std::string::npos) {
        continue;
      }
      const auto length_start =
          length_header + std::string_view("Content-Length: ").size();
      const auto length_end = request_text.find("\r\n", length_start);
      require(length_end != std::string::npos);
      const auto content_length = std::stoull(
          request_text.substr(length_start, length_end - length_start));
      expected_size = header_end + 4 + static_cast<std::size_t>(content_length);
    }

    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: "
                                 "text/event-stream\r\nContent-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + body;
    const auto written = send(client, response.data(), response.size(), 0);
    require(written == static_cast<ssize_t>(response.size()));
    close(client);
    close(server_socket);
  });
  return {port, std::move(server)};
}

class RecordingContextController final : public ContextController {
public:
  Result<ContextDecision> decide(const ContextRequest &request,
                                 CancellationToken) override {
    called = true;
    ContextDecision decision;
    for (const auto &candidate : request.candidates) {
      decision.selected_ids.push_back(candidate.id);
    }
    return Result<ContextDecision>::success(std::move(decision));
  }

  bool called{false};
};

} // namespace

int main() {
  const auto root =
      std::filesystem::temp_directory_path() / "zed-modules-smoke";
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  std::filesystem::create_directories(root);

  zed::session::JsonlSessionStore session(root / "session.jsonl");
  const Message original{
      "user-1",     Role::user,
      "你好，zed",  {call("call-1", "echo", R"({"text":"hello"})")},
      std::nullopt,
  };
  assert(session.append(original));
  const auto loaded = session.load();
  assert(loaded);
  assert(loaded.value().size() == 1);
  assert(loaded.value()[0].content == original.content);
  assert(loaded.value()[0].tool_calls[0].arguments_json ==
         original.tool_calls[0].arguments_json);
  {
    std::ofstream partial(session.path(), std::ios::app);
    partial << R"({"version":1,"type":"message")";
  }
  const auto recovered = session.load();
  assert(recovered);
  assert(recovered.value().size() == 1);

  zed::core::ToolRegistry registry;
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::ReadFileTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::WriteFileTool>(root))));
  require(static_cast<bool>(
      registry.register_tool(std::make_unique<zed::tools::BashTool>(root))));
  require(static_cast<bool>(
      registry.register_tool(std::make_unique<zed::tools::GrepTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::EditFileTool>(root))));

  for (const auto &definition : registry.definitions()) {
    const auto schema = nlohmann::json::parse(definition.input_schema_json);
    require(schema["properties"].contains("purpose"));
    require(std::find(schema["required"].begin(), schema["required"].end(),
                      "purpose") != schema["required"].end());
  }
  const auto missing_purpose = registry.execute(
      {"missing-purpose", "read", R"({"path":"hello.txt"})"}, {});
  require(!missing_purpose);
  require(missing_purpose.error().code == ErrorCode::invalid_argument);
  const auto blank_purpose = registry.execute(
      {"blank-purpose", "read", R"({"purpose":"   ","path":"hello.txt"})"}, {});
  require(!blank_purpose);
  require(blank_purpose.error().code == ErrorCode::invalid_argument);

  const auto write = registry.execute(
      call("write-1", "write", R"({"path":"hello.txt","content":"你好"})"), {});
  assert(write);

  const auto read =
      registry.execute(call("read-1", "read", R"({"path":"hello.txt"})"), {});
  assert(read);
  assert(read.value().content == "你好");

  const auto edit = registry.execute(
      call(
          "edit-1", "edit",
          R"({"path":"hello.txt","old_text":"你好","new_text":"你好，zed","expected_replacements":1})"),
      {});
  assert(edit);

  const auto grep = registry.execute(
      call("grep-1", "grep", R"({"pattern":"zed","path":"."})"), {});
  assert(grep);
  assert(grep.value().content.find("hello.txt:1:") != std::string::npos);

  const auto empty_write = registry.execute(
      call("write-2", "write", R"({"path":"empty.txt","content":""})"), {});
  assert(empty_write);

  const auto traversal = registry.execute(
      call("read-2", "read", R"({"path":"../outside.txt"})"), {});
  assert(!traversal);

  const auto outside = root.parent_path() / "zed-modules-outside.txt";
  {
    std::ofstream outside_file(outside);
    outside_file << "secret";
  }
  std::error_code symlink_error;
  std::filesystem::create_symlink(outside, root / "outside-link.txt",
                                  symlink_error);
  assert(!symlink_error);
  const auto symlink_read = registry.execute(
      call("read-3", "read", R"({"path":"outside-link.txt"})"), {});
  assert(!symlink_read);
  std::filesystem::remove(outside, cleanup_error);

  const auto overwrite_rejected = registry.execute(
      call("write-3", "write", R"({"path":"hello.txt","content":"replace"})"),
      {});
  assert(!overwrite_rejected);
  assert(overwrite_rejected.error().code == ErrorCode::conflict);

  const auto bash = registry.execute(
      call("bash-1", "bash", R"({"command":"printf hello","timeout_ms":1000})"),
      {});
  assert(bash);
  assert(bash.value().content == "hello");
  assert(!bash.value().is_error);

  const auto truncated_bash = registry.execute(
      call("bash-2", "bash",
           R"({"command":"printf 123456789","max_output_bytes":4})"),
      {});
  assert(truncated_bash);
  assert(truncated_bash.value().content.find("[output truncated]") !=
         std::string::npos);

  zed::core::CancellationSource bash_cancellation;
  std::optional<zed::core::Result<zed::core::ToolResult>> cancelled_bash;
  std::thread cancellable_bash([&]() {
    cancelled_bash =
        registry.execute(call("bash-3", "bash", R"({"command":"sleep 5"})"),
                         bash_cancellation.token());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  bash_cancellation.cancel();
  cancellable_bash.join();
  assert(cancelled_bash.has_value());
  assert(!cancelled_bash.value());
  assert(cancelled_bash->error().code == ErrorCode::cancelled);

  const auto skill_root = root / ".zed" / "skills";
  const auto skill_directory = skill_root / "review";
  std::filesystem::create_directories(skill_directory);
  {
    std::ofstream skill_file(skill_directory / "SKILL.md");
    skill_file << "---\n"
                  "name: code-review\n"
                  "description: Review code carefully.\n"
                  "---\n"
                  "Check correctness before changing files.\n";
  }
  zed::skills::SkillRegistry skills;
  assert(skills.discover({skill_root}));
  const auto *review_skill = skills.find("code-review");
  assert(review_skill != nullptr);
  assert(review_skill->description == "Review code carefully.");
  assert(skills.prompt_context("code-review").find("Check correctness") !=
         std::string::npos);

  zed::extensions::ExtensionRegistry extensions;
  assert(extensions.register_command({
      "echo",
      "Echo arguments.",
      [](std::string_view arguments) {
        return zed::core::Result<std::string>::success(std::string(arguments));
      },
      {{"short", "Use short output."}, {"long", "Use long output."}},
  }));
  assert(extensions.commands().front().options.size() == 2);
  const auto echo = extensions.execute("echo", "hello");
  assert(echo);
  assert(echo.value() == "hello");
  assert(!extensions.register_command({
      "echo",
      "Duplicate.",
      [](std::string_view) {
        return zed::core::Result<std::string>::success(std::string{});
      },
  }));
  assert(!extensions.register_command({
      "invalid-options",
      "Duplicate options.",
      [](std::string_view) {
        return zed::core::Result<std::string>::success(std::string{});
      },
      {{"same", "First."}, {"same", "Second."}},
  }));

  std::stringstream terminal_output;
  zed::ui::TerminalRenderer renderer(terminal_output, {false});
  renderer.banner("/tmp/workspace", "muse-spark-1.2-contributor", "0.1.0",
                  "session-fixture", true);
  renderer.render({zed::core::AgentEventType::assistant_delta, "hello", {}});
  renderer.render({zed::core::AgentEventType::agent_end, "", {}});
  assert(terminal_output.str().find("zeda") != std::string::npos);
  assert(terminal_output.str().find("0.1.0") != std::string::npos);
  assert(terminal_output.str().find("session-fixture") != std::string::npos);
  assert(terminal_output.str().find("quick bash: on") != std::string::npos);
  assert(terminal_output.str().find("theme: light") != std::string::npos);
  assert(terminal_output.str().find("hello") != std::string::npos);

  std::stringstream terminal_input("first line\n");
  zed::ui::TerminalInput input(terminal_input);
  const auto line = input.read_line();
  assert(line);
  assert(line.value() == "first line");

  RecordingContextController controller;
  ApproximateTokenEstimator estimator;
  BasicContextManager context(estimator, &controller);
  const std::vector<Message> context_messages{
      {"system-1", Role::system, "system instructions", {}, std::nullopt},
      {"user-1", Role::user, std::string(120, 'x'), {}, std::nullopt},
  };
  const auto context_window =
      context.build(context_messages, {256, 32, 20}, {});
  assert(context_window);
  assert(controller.called);

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
  std::thread server([server_socket]() {
    const int client = accept(server_socket, nullptr, nullptr);
    require(client >= 0);
    std::string request_text;
    std::size_t expected_size = 0;
    while (expected_size == 0 || request_text.size() < expected_size) {
      char request_buffer[4096];
      const auto received =
          recv(client, request_buffer, sizeof(request_buffer), 0);
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        std::abort();
      }
      request_text.append(request_buffer, static_cast<std::size_t>(received));
      const auto header_end = request_text.find("\r\n\r\n");
      const auto length_header = request_text.find("Content-Length: ");
      if (header_end == std::string::npos ||
          length_header == std::string::npos) {
        continue;
      }
      const auto length_start =
          length_header + std::string_view("Content-Length: ").size();
      const auto length_end = request_text.find("\r\n", length_start);
      require(length_end != std::string::npos);
      const auto content_length = std::stoull(
          request_text.substr(length_start, length_end - length_start));
      expected_size = header_end + 4 + static_cast<std::size_t>(content_length);
    }
    require(request_text.find(R"("reasoning":{"effort":"low"})") !=
            std::string::npos);
    require(request_text.find(R"("purpose")") != std::string::npos);
    require(request_text.find(R"("max_output_tokens")") == std::string::npos);
    const std::string first_event =
        "data: "
        "{\"type\":\"response.output_text.delta\",\"delta\":\"hello\"}\n\n";
    const std::string remaining_events =
        "data: "
        "{\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
        "data: "
        "{\"type\":\"response.function_call_arguments.delta\",\"call_id\":"
        "\"fixture-call\",\"name\":\"read\",\"delta\":\"{\\\"purpose\\\":"
        "\\\"Read fixture\\\",\\\"path\\\":\\\"x.txt\\\"}\"}\n\n"
        "data: "
        "{\"type\":\"response.function_call_arguments.done\",\"call_id\":"
        "\"fixture-call\",\"name\":\"read\",\"arguments\":\"{\\\"purpose\\\":"
        "\\\"Read fixture\\\",\\\"path\\\":\\\"x.txt\\\"}\"}\n\n"
        "data: "
        "{\"type\":\"response.completed\",\"response\":{\"status\":"
        "\"completed\",\"usage\":{\"input_"
        "tokens\":12,\"output_tokens\":3,\"input_tokens_details\":{\"cached_"
        "tokens\":8}},\"output\":[]}}\n\n";
    const std::string body = first_event + remaining_events;
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: "
                                 "text/event-stream\r\nContent-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n" + first_event;
    const auto first_written =
        send(client, response.data(), response.size(), 0);
    require(first_written == static_cast<ssize_t>(response.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto remaining_written =
        send(client, remaining_events.data(), remaining_events.size(), 0);
    require(remaining_written == static_cast<ssize_t>(remaining_events.size()));
    close(client);
    close(server_socket);
  });

  zed::providers::OpenCodeGoModel fixture_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(port) + "/responses",
  });
  const auto capabilities = fixture_model.capabilities();
  assert(capabilities.streaming);
  assert(capabilities.function_calling);
  ModelRequest fixture_request;
  fixture_request.model = {"opencode-go", "muse-spark-1.2-contributor"};
  fixture_request.messages = {
      {"fixture-user", Role::user, "hello", {}, std::nullopt}};
  fixture_request.tools = registry.definitions();
  std::string streamed;
  std::vector<std::chrono::steady_clock::time_point> delta_times;
  const auto fixture_response = fixture_model.complete(
      fixture_request,
      [&](const ModelDelta &delta) {
        streamed += delta.text;
        delta_times.push_back(std::chrono::steady_clock::now());
      },
      {});
  server.join();
  assert(fixture_response);
  assert(streamed == "hello world");
  assert(delta_times.size() == 2);
  assert(delta_times[1] - delta_times[0] >= std::chrono::milliseconds(100));
  assert(fixture_response.value().tool_calls.size() == 1);
  assert(fixture_response.value().tool_calls[0].name == "read");
  assert(fixture_response.value().finish_reason == FinishReason::tool_calls);
  assert(fixture_response.value().usage.input_tokens == 12);
  assert(fixture_response.value().usage.cached_input_tokens == 8);

  auto incomplete_server = start_sse_fixture_server(
      "data: {\"type\":\"response.incomplete\",\"response\":{\"status\":"
      "\"incomplete\",\"incomplete_details\":{\"reason\":\"max_tokens\"},"
      "\"usage\":{\"input_tokens\":5,\"output_tokens\":4},\"output\":[]}}\n\n");
  zed::providers::OpenCodeGoModel incomplete_fixture_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(incomplete_server.port) +
          "/responses",
  });
  const auto incomplete_fixture_response =
      incomplete_fixture_model.complete(fixture_request, {}, {});
  incomplete_server.thread.join();
  assert(incomplete_fixture_response);
  assert(incomplete_fixture_response.value().finish_reason ==
         FinishReason::length);
  assert(incomplete_fixture_response.value().usage.output_tokens == 4);

  auto unterminated_server = start_sse_fixture_server(
      "data: "
      "{\"type\":\"response.output_text.delta\",\"delta\":\"partial\"}\n\n"
      "data: [DONE]\n\n");
  zed::providers::OpenCodeGoModel unterminated_fixture_model({
      "fixture-key",
      "http://127.0.0.1:" + std::to_string(unterminated_server.port) +
          "/responses",
  });
  const auto unterminated_fixture_response =
      unterminated_fixture_model.complete(fixture_request, {}, {});
  unterminated_server.thread.join();
  assert(!unterminated_fixture_response);
  assert(unterminated_fixture_response.error().message.find(
             "without a terminal response event") != std::string::npos);

  std::filesystem::remove_all(root, cleanup_error);
  return 0;
}
