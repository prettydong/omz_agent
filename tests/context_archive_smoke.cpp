#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "zed/context/context_archive.hpp"
#include "zed/core/cancellation.hpp"
#include "zed/core/session_store.hpp"
#include "zed/tools/context_tools.hpp"

namespace {

class SwitchingStore final : public zed::core::SessionStore {
public:
  zed::core::Result<void> append(const zed::core::Message &message) override {
    return active_->append(message);
  }
  zed::core::Result<std::vector<zed::core::Message>> load() const override {
    return active_->load();
  }
  void select(zed::core::SessionStore &store) { active_ = &store; }

private:
  zed::core::SessionStore *active_{nullptr};
};

zed::core::ToolCall call(std::string id, std::string name,
                         nlohmann::json arguments) {
  return {std::move(id), std::move(name), arguments.dump()};
}

} // namespace

int main() {
  using zed::core::CancellationSource;
  using zed::core::InMemorySessionStore;
  using zed::core::Message;
  using zed::core::Role;

  const auto root =
      std::filesystem::temp_directory_path() / "zeda-context-archive-smoke";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);

  InMemorySessionStore first;
  InMemorySessionStore second;
  assert(first.append({"user-1", Role::user, "first", {}, std::nullopt}));
  assert(first.append({"assistant-1",
                       Role::assistant,
                       "",
                       {{"tool-1", "grep", R"({"pattern":"literal-needle"})"}},
                       std::nullopt}));
  assert(second.append({"user-2", Role::user, "second", {}, std::nullopt}));
  SwitchingStore switching;
  switching.select(first);
  auto journal = root / "first.context.jsonl";
  zed::context::ContextArchive archive(switching,
                                       [&journal] { return journal; });

  const auto initial = archive.snapshot();
  assert(initial && initial.value().windows.size() == 1U);
  assert(archive.write_note("plans/today", "keep this", false));
  assert(archive.write_note("plans/today", "\nnext", true));
  assert(!archive.write_note("../escape", "no", false));
  assert(!archive.write_note("/absolute", "no", false));
  assert(!archive.write_note("plans//escape", "no", false));
  assert(
      archive.commit_window({"window-2", 1U, "user-1", {"user-1"}, "manual"}));
  assert(archive.request_new_window());
  const auto persisted = archive.snapshot();
  assert(persisted &&
         persisted.value().notes.at("plans/today") == "keep this\nnext");
  assert(persisted.value().windows.size() == 2U &&
         persisted.value().reset_requested);
  const auto fork_journal = root / "fork.context.jsonl";
  assert(archive.fork_to(fork_journal));
  assert(!archive.fork_to(fork_journal));
  zed::context::ContextArchive forked(first,
                                      [fork_journal] { return fork_journal; });
  const auto forked_state = forked.snapshot();
  assert(forked_state && !forked_state.value().reset_requested);
  assert(forked_state.value().notes == persisted.value().notes);
  assert(forked_state.value().windows.size() ==
         persisted.value().windows.size());
  assert(forked.write_note("fork-only", "yes", false));
  assert(!archive.snapshot().value().notes.contains("fork-only"));

  zed::tools::ContextHistoryTool history_tool(archive);
  auto history_search =
      history_tool.execute(call("history-search", "context_history",
                                {{"purpose", "find prior tool"},
                                 {"action", "search"},
                                 {"query", "literal-needle"}}),
                           {});
  assert(history_search);
  assert(history_search.value().content.find("assistant-1") !=
         std::string::npos);
  auto window_list =
      history_tool.execute(call("history-window", "context_history",
                                {{"purpose", "read first window"},
                                 {"action", "list"},
                                 {"window_id", "window-1"}}),
                           {});
  assert(window_list &&
         window_list.value().content.find("user-1") != std::string::npos);
  assert(window_list.value().content.find("assistant-1") == std::string::npos);

  zed::tools::ContextNotesTool notes_tool(archive);
  const std::string large(512U * 1024U, 'x');
  assert(archive.write_note("large", large, false));
  const auto large_read =
      notes_tool.execute(call("notes-large", "context_notes",
                              {{"purpose", "inspect bounded output"},
                               {"action", "read"},
                               {"name", "large"}}),
                         {});
  assert(large_read && large_read.value().content.size() <= 16U * 1024U);
  const auto first_page = nlohmann::json::parse(large_read.value().content);
  assert(first_page.at("truncated") && first_page.at("next_byte_offset") > 0);
  const auto tail_read =
      notes_tool.execute(call("notes-tail", "context_notes",
                              {{"purpose", "read note tail"},
                               {"action", "read"},
                               {"name", "large"},
                               {"byte_offset", 512U * 1024U - 4U},
                               {"max_bytes", 4}}),
                         {});
  assert(tail_read);
  const auto tail_page = nlohmann::json::parse(tail_read.value().content);
  assert(tail_page.at("text") == "xxxx" && !tail_page.at("truncated"));

  zed::tools::ContextNotesTool timed_out(archive, std::chrono::milliseconds(0));
  assert(!timed_out.execute(
      call("deadline", "context_notes",
           {{"purpose", "test deadline"}, {"action", "list"}}),
      {}));

  zed::tools::NewContextTool new_context_tool(archive);
  const auto requested = new_context_tool.execute(
      call("new-context", "new_context", {{"purpose", "start a fresh window"}}),
      {});
  assert(requested && archive.snapshot().value().reset_requested);

  CancellationSource source;
  source.cancel();
  assert(!archive.snapshot(source.token()));
  assert(!archive.write_note("cancelled", "no", false, source.token()));

  journal = root / "second.context.jsonl";
  switching.select(second);
  assert(archive.write_note("isolated", "yes", false));
  const auto switched_history = archive.read_history();
  assert(switched_history && switched_history.value().size() == 1U);
  const auto switched_state = archive.snapshot();
  assert(switched_state && switched_state.value().notes.size() == 1U);
  journal = root / "first.context.jsonl";
  switching.select(first);
  assert(archive.snapshot().value().notes.contains("plans/today"));

  const auto directory_journal = root / "not-a-journal";
  std::filesystem::create_directories(directory_journal, error);
  zed::context::ContextArchive broken(
      first, [directory_journal] { return directory_journal; });
  assert(!broken.snapshot());
  zed::context::ContextArchive absent(
      first, [root] { return root / "absent.context.jsonl"; });
  assert(absent.fork_to(root / "absent-fork.context.jsonl"));
  assert(!std::filesystem::exists(root / "absent-fork.context.jsonl"));

  std::filesystem::remove_all(root, error);
  return 0;
}
