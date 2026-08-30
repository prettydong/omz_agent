#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
#include "zed/session/session_catalog.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/tools/multi_bash_tool.hpp"
#include "zed/ui/terminal.hpp"

namespace {

zed::ui::TerminalStartupTiming startup_timing() {
  using namespace std::chrono_literals;
  return {2ms, 1ms, 3ms, 5ms, 10ms, 2ms};
}

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

class InvalidUtf8Tool final : public Tool {
public:
  [[nodiscard]] const ToolDefinition &definition() const override {
    static const ToolDefinition definition{
        "invalid_utf8",
        "Return invalid UTF-8 for boundary testing.",
        R"({"type":"object","properties":{}})",
    };
    return definition;
  }

  Result<ToolResult> execute(const ToolCall &call, CancellationToken) override {
    std::string content = "before";
    content.push_back(static_cast<char>(0xFF));
    content += "after";
    return Result<ToolResult>::success({call.id, std::move(content), false});
  }
};

} // namespace

int main() {
  const auto root =
      std::filesystem::temp_directory_path() / "zed-modules-smoke";
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  std::filesystem::create_directories(root);

  const auto metadata = [&](const std::filesystem::path &path,
                            std::string title = {}) {
    const auto id = path.stem().string();
    return zed::session::SessionMetadata{
        id,
        title.empty() ? id : std::move(title),
        root.string(),
        "test-provider",
        "test-model",
    };
  };

  zed::session::JsonlSessionStore session(root / "session.jsonl");
  assert(session.initialize(metadata(session.path(), "Primary session")));
  const Message original_user{
      "user-1", Role::user, "你好，zed", {}, std::nullopt};
  const Message original_assistant{
      "assistant-1",
      Role::assistant,
      {},
      {call("call-1", "echo", R"({"text":"hello"})")},
      std::nullopt,
  };
  const Message original_tool{"tool-1", Role::tool, "hello", {}, "call-1"};
  const Message original_final{
      "assistant-2", Role::assistant, "done", {}, std::nullopt};
  assert(session.begin_turn("turn-1", original_user));
  assert(session.append(original_assistant));
  assert(session.append(original_tool));
  assert(session.append(original_final));
  assert(session.finish_turn("turn-1", SessionTurnOutcome::completed));
  const auto loaded = session.load();
  assert(loaded);
  assert(loaded.value().size() == 4);
  assert(loaded.value()[0].content == original_user.content);
  assert(loaded.value()[1].tool_calls[0].arguments_json ==
         original_assistant.tool_calls[0].arguments_json);
  const auto initial_inspection = session.inspect();
  assert(initial_inspection);
  assert(initial_inspection.value().metadata.title == "Primary session");
  assert(initial_inspection.value().turn_count == 1);
  assert(initial_inspection.value().message_count == 4);
  assert(!initial_inspection.value().has_interrupted_turn);

  const auto invalid_utf8_session_path = root / "invalid-utf8.jsonl";
  zed::session::JsonlSessionStore invalid_utf8_session(
      invalid_utf8_session_path);
  assert(invalid_utf8_session.initialize(metadata(invalid_utf8_session_path)));
  std::string invalid_utf8_content = "invalid";
  invalid_utf8_content.push_back(static_cast<char>(0xFF));
  const auto rejected_invalid_utf8 = invalid_utf8_session.begin_turn(
      "invalid-utf8-turn",
      {"invalid-utf8", Role::user, invalid_utf8_content, {}, std::nullopt});
  assert(!rejected_invalid_utf8);
  assert(rejected_invalid_utf8.error().code == ErrorCode::session_error);
  assert(rejected_invalid_utf8.error().message.find("serialize") !=
         std::string::npos);
  const auto invalid_utf8_history = invalid_utf8_session.load();
  assert(invalid_utf8_history);
  assert(invalid_utf8_history.value().empty());

  const auto interrupted_session_path = root / "interrupted.jsonl";
  zed::session::JsonlSessionStore interrupted_session(interrupted_session_path);
  assert(interrupted_session.initialize(metadata(interrupted_session_path)));
  assert(interrupted_session.begin_turn(
      "interrupted-turn",
      {"interrupted-user", Role::user, "run tool", {}, std::nullopt}));
  assert(interrupted_session.append({
      "interrupted-assistant",
      Role::assistant,
      {},
      {call("interrupted-call", "echo", R"({"text":"hello"})")},
      std::nullopt,
  }));
  {
    std::ofstream partial(interrupted_session.path(), std::ios::app);
    partial << R"({"version":2,"type":"message")";
  }
  const auto interrupted_inspection = interrupted_session.inspect();
  assert(interrupted_inspection);
  assert(interrupted_inspection.value().has_interrupted_turn);
  assert(interrupted_inspection.value().unresolved_tool_calls == 1);
  const auto recovered = interrupted_session.recover_interrupted_turn();
  assert(recovered);
  assert(recovered.value().recovered);
  assert(recovered.value().turn_id == "interrupted-turn");
  assert(recovered.value().synthesized_tool_results == 1);
  const auto recovered_history = interrupted_session.load();
  assert(recovered_history);
  assert(recovered_history.value().size() == 3);
  assert(recovered_history.value()[2].role == Role::tool);
  assert(recovered_history.value()[2].is_error);
  assert(recovered_history.value()[2].content.find("may or may not") !=
         std::string::npos);

  const auto unfinished_session_path = root / "unfinished.jsonl";
  zed::session::JsonlSessionStore unfinished_session(unfinished_session_path);
  assert(unfinished_session.initialize(metadata(unfinished_session_path)));
  assert(unfinished_session.begin_turn(
      "unfinished-turn",
      {"unfinished-user", Role::user, "no response yet", {}, std::nullopt}));
  const auto rejected_completion = unfinished_session.finish_turn(
      "unfinished-turn", SessionTurnOutcome::completed);
  assert(!rejected_completion);
  assert(rejected_completion.error().message.find("terminal assistant") !=
         std::string::npos);
  const auto recovered_unfinished =
      unfinished_session.recover_interrupted_turn();
  assert(recovered_unfinished);
  assert(recovered_unfinished.value().recovered);
  const auto unfinished_history = unfinished_session.load();
  assert(unfinished_history);
  assert(unfinished_history.value().size() == 1);

  const auto strict_v1_path = root / "strict-v1.jsonl";
  {
    std::ofstream strict_v1(strict_v1_path);
    strict_v1 << R"({"version":1,"type":"message","id":"old"})" << '\n';
  }
  zed::session::JsonlSessionStore strict_v1(strict_v1_path);
  const auto rejected_v1 = strict_v1.load();
  assert(!rejected_v1);
  assert(rejected_v1.error().message.find("Session v2") != std::string::npos ||
         rejected_v1.error().message.find("version") != std::string::npos);

  const auto sessions_root = root / "sessions";
  const auto older_path = sessions_root / "session-older.jsonl";
  const auto newer_path = sessions_root / "session-newer.jsonl";
  {
    zed::session::JsonlSessionStore older_session(older_path);
    zed::session::JsonlSessionStore newer_session(newer_path);
    assert(older_session.initialize(metadata(older_path, "Older")));
    assert(newer_session.initialize(metadata(newer_path, "Newer")));
    assert(older_session.begin_turn(
        "older-turn",
        {"older-user", Role::user, "older request", {}, std::nullopt}));
    assert(older_session.finish_turn("older-turn", SessionTurnOutcome::failed,
                                     "fixture failure"));
    assert(newer_session.begin_turn(
        "newer-turn",
        {"newer-user", Role::user, "newer request", {}, std::nullopt}));
    assert(newer_session.append(
        {"newer-assistant", Role::assistant, "done", {}, std::nullopt}));
    assert(
        newer_session.finish_turn("newer-turn", SessionTurnOutcome::completed));
  }
  const auto catalog_time = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(older_path,
                                   catalog_time - std::chrono::hours(1));
  std::filesystem::last_write_time(newer_path, catalog_time);
  const auto saved_sessions = zed::session::list_sessions(sessions_root);
  assert(saved_sessions);
  assert(saved_sessions.value().size() == 2);
  assert(saved_sessions.value()[0].name == "session-newer");
  assert(saved_sessions.value()[0].title == "Newer");
  assert(saved_sessions.value()[0].turn_count == 1);
  const auto selected_session =
      zed::session::find_session(sessions_root, "session-older");
  assert(selected_session);
  assert(selected_session.value().path == older_path);
  assert(zed::session::find_session(sessions_root, "Newer"));
  assert(zed::session::find_session(sessions_root, "session-newer.jsonl"));
  assert(!zed::session::find_session(sessions_root, "../session.jsonl"));

  const auto switched = session.switch_to(selected_session.value().path);
  assert(switched);
  const auto switched_history = session.load();
  assert(switched_history);
  assert(switched_history.value().size() == 1);
  assert(switched_history.value()[0].content == "older request");
  assert(session.set_title("Renamed older"));
  const auto renamed_info = session.inspect();
  assert(renamed_info);
  assert(renamed_info.value().metadata.title == "Renamed older");

  const auto fork_path = zed::session::new_session_path(sessions_root);
  assert(session.fork_to(fork_path, "Forked session"));
  zed::session::JsonlSessionStore forked_session(fork_path);
  const auto forked_info = forked_session.inspect();
  assert(forked_info);
  assert(forked_info.value().metadata.title == "Forked session");
  assert(forked_info.value().metadata.parent_id ==
         renamed_info.value().metadata.id);
  const auto forked_history = forked_session.load();
  assert(forked_history);
  assert(forked_history.value().size() == switched_history.value().size());

  const auto invalid_session_path = sessions_root / "invalid.jsonl";
  {
    std::ofstream invalid_session(invalid_session_path);
    invalid_session << "{invalid json}\n";
  }
  const auto rejected_invalid = session.switch_to(invalid_session_path);
  assert(!rejected_invalid);
  assert(session.path() == selected_session.value().path);
  const auto catalog_with_invalid = zed::session::list_sessions(sessions_root);
  assert(catalog_with_invalid);
  const auto invalid_entry = std::find_if(
      catalog_with_invalid.value().begin(), catalog_with_invalid.value().end(),
      [](const zed::session::SessionEntry &entry) {
        return entry.name == "invalid";
      });
  assert(invalid_entry != catalog_with_invalid.value().end());
  assert(!invalid_entry->valid);

  zed::core::ToolRegistry registry;
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::ReadFileTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::WriteFileTool>(root))));
  require(static_cast<bool>(
      registry.register_tool(std::make_unique<zed::tools::BashTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::MultiBashTool>(root))));
  require(static_cast<bool>(
      registry.register_tool(std::make_unique<zed::tools::GrepTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::FindFilesTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::ListDirectoryTool>(root))));
  require(static_cast<bool>(registry.register_tool(
      std::make_unique<zed::tools::EditFileTool>(root))));
  require(static_cast<bool>(
      registry.register_tool(std::make_unique<InvalidUtf8Tool>())));

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

  std::filesystem::create_directories(root / "tree" / "docs");
  std::filesystem::create_directories(root / "tree" / "src");
  {
    std::ofstream source(root / "tree" / "src" / "main.cpp");
    source << "int main() {}\n";
    std::ofstream header(root / "tree" / "src" / "helper.hpp");
    header << "#pragma once\n";
    std::ofstream documentation(root / "tree" / "docs" / "guide.md");
    documentation << "guide\n";
  }
  const auto listed =
      registry.execute(call("ls-1", "ls", R"({"path":"tree"})"), {});
  assert(listed);
  assert(listed.value().content == "docs/\nsrc/\n");

  const auto found_source = registry.execute(
      call("find-1", "find", R"({"pattern":"*.cpp","path":"tree"})"), {});
  assert(found_source);
  assert(found_source.value().content == "tree/src/main.cpp\n");
  const auto found_relative = registry.execute(
      call("find-2", "find", R"({"pattern":"src/*.hpp","path":"tree"})"), {});
  assert(found_relative);
  assert(found_relative.value().content == "tree/src/helper.hpp\n");

  const auto truncated_find = registry.execute(
      call("find-3", "find",
           R"({"pattern":"*","path":"tree","max_output_bytes":4})"),
      {});
  assert(truncated_find);
  assert(truncated_find.value().content.find("[results truncated]") !=
         std::string::npos);
  const auto truncated_list = registry.execute(
      call("ls-2", "ls", R"({"path":"tree","max_entries":1})"), {});
  assert(truncated_list);
  assert(truncated_list.value().content.find("[results truncated]") !=
         std::string::npos);
  const auto invalid_list_limit = registry.execute(
      call("ls-3", "ls", R"({"path":"tree","max_entries":0})"), {});
  assert(!invalid_list_limit);
  assert(invalid_list_limit.error().code == ErrorCode::invalid_argument);
  const auto list_file =
      registry.execute(call("ls-4", "ls", R"({"path":"hello.txt"})"), {});
  assert(!list_file);
  assert(list_file.error().code == ErrorCode::tool_error);
  const auto find_traversal = registry.execute(
      call("find-4", "find", R"({"pattern":"*","path":".."})"), {});
  assert(!find_traversal);

  zed::core::CancellationSource discovery_cancellation;
  discovery_cancellation.cancel();
  const auto cancelled_find = registry.execute(
      call("find-5", "find", R"({"pattern":"*","path":"tree"})"),
      discovery_cancellation.token());
  assert(!cancelled_find);
  assert(cancelled_find.error().code == ErrorCode::cancelled);

  const auto grep = registry.execute(
      call("grep-1", "grep", R"({"pattern":"zed","path":"."})"), {});
  assert(grep);
  assert(grep.value().content.find("hello.txt:1:") != std::string::npos);

  std::filesystem::create_directories(root / ".git");
  {
    std::ofstream binary_index(root / ".git" / "index", std::ios::binary);
    binary_index << "clangd";
    binary_index.put(static_cast<char>(0xFF));
  }
  {
    std::ofstream binary_file(root / "binary.dat", std::ios::binary);
    binary_file << "clangd";
    binary_file.put(static_cast<char>(0xFF));
  }
  {
    std::ofstream source_file(root / "source.txt");
    source_file << "clangd is configured\n";
  }
  const auto safe_grep = registry.execute(
      call("grep-2", "grep", R"({"pattern":"clangd","path":"."})"), {});
  assert(safe_grep);
  assert(safe_grep.value().content.find("source.txt:1:") != std::string::npos);
  assert(safe_grep.value().content.find(".git") == std::string::npos);
  assert(safe_grep.value().content.find("binary.dat") == std::string::npos);

  const auto sanitized_tool =
      registry.execute(call("invalid-utf8-1", "invalid_utf8", R"({})"), {});
  assert(sanitized_tool);
  assert(sanitized_tool.value().content.find("before\xEF\xBF\xBD"
                                             "after") != std::string::npos);
  assert(sanitized_tool.value().content.find("replaced 1 invalid UTF-8 byte") !=
         std::string::npos);

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

  const auto find_symlink = registry.execute(
      call("find-6", "find", R"({"pattern":"outside-link.txt","path":"."})"),
      {});
  assert(find_symlink);
  assert(find_symlink.value().content.empty());
  const auto find_git = registry.execute(
      call("find-7", "find", R"({"pattern":"index","path":"."})"), {});
  assert(find_git);
  assert(find_git.value().content.empty());
  const auto listed_symlink =
      registry.execute(call("ls-5", "ls", R"({"path":"."})"), {});
  assert(listed_symlink);
  assert(listed_symlink.value().content.find("outside-link.txt@") !=
         std::string::npos);
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

  const auto multi_bash = registry.execute(
      call(
          "multi-bash-1", "multi_bash",
          R"({"commands":[{"command":"touch first-ready; while [ ! -f second-ready ]; do sleep 0.01; done; printf first"},{"command":"touch second-ready; while [ ! -f first-ready ]; do sleep 0.01; done; printf second"}],"max_concurrency":2})"),
      {});
  assert(multi_bash);
  assert(!multi_bash.value().is_error);
  assert(multi_bash.value().content.starts_with("summary: 1=ok 2=ok"));
  assert(multi_bash.value().content.find("[1]\nfirst") != std::string::npos);
  assert(multi_bash.value().content.find("[2]\nsecond") != std::string::npos);

  const auto partly_failed_multi_bash = registry.execute(
      call("multi-bash-2", "multi_bash",
           R"({"commands":[{"command":"printf okay"},{"command":"exit 7"}]})"),
      {});
  assert(partly_failed_multi_bash);
  assert(partly_failed_multi_bash.value().is_error);
  assert(partly_failed_multi_bash.value().content.starts_with(
      "summary: 1=ok 2=error"));
  assert(partly_failed_multi_bash.value().content.find("[exit code 7]") !=
         std::string::npos);

  const auto invalid_multi_bash =
      registry.execute(call("multi-bash-3", "multi_bash",
                            R"({"commands":[{"command":"printf only-one"}]})"),
                       {});
  assert(!invalid_multi_bash);
  assert(invalid_multi_bash.error().code == ErrorCode::invalid_argument);

  const auto truncated_multi_bash = registry.execute(
      call(
          "multi-bash-4", "multi_bash",
          R"({"commands":[{"command":"printf 123456789"},{"command":"printf abcdefghi"}],"max_output_bytes":24})"),
      {});
  assert(truncated_multi_bash);
  assert(truncated_multi_bash.value().content.find(
             "[multi_bash output truncated]") != std::string::npos);

  const auto timed_out_multi_bash = registry.execute(
      call(
          "multi-bash-5", "multi_bash",
          R"({"commands":[{"command":"sleep 1","timeout_ms":50},{"command":"printf companion"}]})"),
      {});
  assert(timed_out_multi_bash);
  assert(timed_out_multi_bash.value().is_error);
  assert(timed_out_multi_bash.value().content.starts_with(
      "summary: 1=error 2=ok"));
  assert(timed_out_multi_bash.value().content.find("[command timed out]") !=
         std::string::npos);

  {
    std::ofstream blocked_command(root / "not-executable");
    blocked_command << "#!/bin/sh\nprintf should-not-run";
  }
  std::filesystem::permissions(root / "not-executable",
                               std::filesystem::perms::owner_exec |
                                   std::filesystem::perms::group_exec |
                                   std::filesystem::perms::others_exec,
                               std::filesystem::perm_options::remove);
  const auto permission_failed_multi_bash = registry.execute(
      call(
          "multi-bash-6", "multi_bash",
          R"({"commands":[{"command":"./not-executable"},{"command":"printf companion"}]})"),
      {});
  assert(permission_failed_multi_bash);
  assert(permission_failed_multi_bash.value().is_error);
  assert(permission_failed_multi_bash.value().content.starts_with(
      "summary: 1=error 2=ok"));

  const auto traversal_multi_bash = registry.execute(
      call(
          "multi-bash-7", "multi_bash",
          R"({"commands":[{"command":"pwd","working_dir":".."},{"command":"pwd"}]})"),
      {});
  assert(!traversal_multi_bash);
  assert(traversal_multi_bash.error().code == ErrorCode::invalid_argument);

  zed::core::CancellationSource multi_bash_cancellation;
  std::optional<zed::core::Result<zed::core::ToolResult>> cancelled_multi_bash;
  std::thread cancellable_multi_bash([&]() {
    cancelled_multi_bash = registry.execute(
        call("multi-bash-8", "multi_bash",
             R"({"commands":[{"command":"sleep 5"},{"command":"sleep 5"}]})"),
        multi_bash_cancellation.token());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  multi_bash_cancellation.cancel();
  cancellable_multi_bash.join();
  assert(cancelled_multi_bash.has_value());
  assert(!cancelled_multi_bash.value());
  assert(cancelled_multi_bash->error().code == ErrorCode::cancelled);

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

  auto managed_skills = zed::skills::load_workspace_skills(root);
  assert(managed_skills);
  assert(managed_skills.value().size() == 1);
  assert(managed_skills.value()[0].id == "review");
  managed_skills.value()[0].enabled = false;
  managed_skills.value().push_back({"domain", "domain", "Domain rules",
                                    "Read the domain model first.", true});
  assert(zed::skills::save_workspace_skills(root, managed_skills.value()));
  assert(std::filesystem::is_regular_file(root / ".zed" / "skills-disabled" /
                                          "review" / "SKILL.md"));
  assert(std::filesystem::is_regular_file(root / ".zed" / "skills" / "domain" /
                                          "SKILL.md"));
  managed_skills.value().erase(managed_skills.value().begin() + 1);
  assert(zed::skills::save_workspace_skills(root, managed_skills.value()));
  std::error_code archive_error;
  const auto archive_count = static_cast<std::size_t>(
      std::distance(std::filesystem::directory_iterator(
                        root / ".zed" / "skill-archive", archive_error),
                    std::filesystem::directory_iterator()));
  assert(!archive_error);
  assert(archive_count == 1);

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
                  startup_timing(), zed::core::ReasoningEffort::low);
  renderer.render({zed::core::AgentEventType::assistant_delta, "hello", {}});
  renderer.render({zed::core::AgentEventType::agent_end, "", {}});
  renderer.render(
      {zed::core::AgentEventType::tool_start, "delegate exploration", {}});
  renderer.render(
      {zed::core::AgentEventType::tool_update, "explorer running", {}});
  renderer.render(
      {zed::core::AgentEventType::tool_update, "explorer running", {}});
  const auto startup_summary =
      zed::ui::terminal_startup_summary(startup_timing());
  assert(startup_summary.find('\n') == std::string::npos);
  assert(terminal_output.str().find("workspace: /tmp/workspace\n" +
                                    startup_summary + "\n") !=
         std::string::npos);
  assert(terminal_output.str().find("zeda 0.1.0") != std::string::npos);
  assert(terminal_output.str().find("session 3.000") != std::string::npos);
  assert(terminal_output.str().find("core") == std::string::npos);
  assert(terminal_output.str().find("other 1.000") != std::string::npos);
  assert(terminal_output.str().find("quick bash") == std::string::npos);
  assert(terminal_output.str().find("theme") == std::string::npos);
  assert(terminal_output.str().find("hello") != std::string::npos);
  const auto first_update = terminal_output.str().find("explorer running");
  assert(first_update != std::string::npos);
  assert(terminal_output.str().find("explorer running", first_update + 1) ==
         std::string::npos);

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
    require(request_text.find(R"("effort":"low")") != std::string::npos);
    require(request_text.find(R"("summary":"auto")") != std::string::npos);
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
