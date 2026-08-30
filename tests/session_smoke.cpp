#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "zed/core/message.hpp"
#include "zed/session/jsonl_session_store.hpp"

namespace {

zed::session::SessionMetadata
metadata_for(const std::filesystem::path &path,
             const std::filesystem::path &workspace) {
  const auto id = path.stem().string();
  return {id, id, workspace.string(), "fixture", "fixture-model"};
}

} // namespace

int main() {
  using zed::core::Message;
  using zed::core::Role;
  using zed::core::SessionTurnOutcome;

  const auto root =
      std::filesystem::temp_directory_path() / "zeda-session-v2-smoke";
  std::error_code filesystem_error;
  std::filesystem::remove_all(root, filesystem_error);
  std::filesystem::create_directories(root);

  const auto path = root / "primary.jsonl";
  zed::session::JsonlSessionStore session(path);
  assert(session.initialize(metadata_for(path, root)));
  zed::session::JsonlSessionStore competing_writer(path);
  const auto rejected_writer =
      competing_writer.initialize(metadata_for(path, root));
  assert(!rejected_writer);
  assert(rejected_writer.error().code == zed::core::ErrorCode::conflict);
  assert(session.begin_turn(
      "turn-1", {"user-1", Role::user, "run tools", {}, std::nullopt}));
  assert(session.append({
      "assistant-1",
      Role::assistant,
      {},
      {
          {"call-1", "read", R"({"path":"one.txt"})"},
          {"call-2", "read", R"({"path":"two.txt"})"},
      },
      std::nullopt,
  }));
  assert(session.append(
      {"tool-1", Role::tool, "one", {}, std::string("call-1"), false}));

  {
    std::ofstream partial(path, std::ios::app);
    partial << R"({"version":2,"type":"message")";
  }
  const auto before_recovery = session.inspect();
  assert(before_recovery);
  assert(before_recovery.value().has_interrupted_turn);
  assert(before_recovery.value().unresolved_tool_calls == 1);

  const auto recovery = session.recover_interrupted_turn();
  assert(recovery);
  assert(recovery.value().recovered);
  assert(recovery.value().synthesized_tool_results == 1);
  const auto recovered_history = session.load();
  assert(recovered_history);
  assert(recovered_history.value().size() == 4);
  assert(recovered_history.value()[3].role == Role::tool);
  assert(recovered_history.value()[3].tool_call_id == "call-2");
  assert(recovered_history.value()[3].is_error);

  assert(session.begin_turn(
      "turn-2", {"user-2", Role::user, "continue", {}, std::nullopt}));
  assert(session.append(
      {"assistant-2", Role::assistant, "done", {}, std::nullopt, false}));
  assert(session.finish_turn("turn-2", SessionTurnOutcome::completed));
  const auto completed = session.inspect();
  assert(completed);
  assert(completed.value().turn_count == 2);
  assert(!completed.value().has_interrupted_turn);

  std::ifstream records(path);
  std::string line;
  std::size_t header_count = 0;
  std::size_t interrupted_count = 0;
  while (std::getline(records, line)) {
    const auto record = nlohmann::json::parse(line);
    assert(record.at("version") == 2);
    if (record.at("type") == "session")
      ++header_count;
    if (record.at("type") == "turn_end" &&
        record.at("outcome") == "interrupted") {
      ++interrupted_count;
    }
  }
  assert(header_count == 1);
  assert(interrupted_count == 1);

  const auto fork_path = root / "fork.jsonl";
  assert(session.fork_to(fork_path, "fork title"));
  zed::session::JsonlSessionStore forked(fork_path);
  const auto forked_info = forked.inspect();
  assert(forked_info);
  assert(forked_info.value().metadata.parent_id ==
         completed.value().metadata.id);
  assert(forked_info.value().metadata.title == "fork title");
  const auto forked_history = forked.load();
  const auto source_history = session.load();
  assert(forked_history);
  assert(source_history);
  assert(forked_history.value().size() == source_history.value().size());

  const auto old_path = root / "old.jsonl";
  {
    std::ofstream old(old_path);
    old << R"({"version":1,"type":"message"})" << '\n';
  }
  zed::session::JsonlSessionStore old(old_path);
  assert(!old.load());

  std::filesystem::remove_all(root, filesystem_error);
  return 0;
}
