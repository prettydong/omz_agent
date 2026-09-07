#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "zed/context/context_archive.hpp"
#include "zed/core/cancellation.hpp"
#include "zed/core/session_store.hpp"
#include "zed/tools/context_tools.hpp"

namespace {

using Json = nlohmann::json;
using zed::core::Message;
using zed::core::Role;

class TempDirectory final {
public:
  TempDirectory() {
    char pattern[] = "/tmp/zeda-context-tools-edges-XXXXXX";
    const char *created = mkdtemp(pattern);
    assert(created != nullptr);
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

class SleepingStore final : public zed::core::SessionStore {
public:
  zed::core::Result<void> append(const Message &message) override {
    messages_.push_back(message);
    return zed::core::Result<void>::success();
  }

  zed::core::Result<std::vector<Message>> load() const override {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return zed::core::Result<std::vector<Message>>::success(messages_);
  }

private:
  std::vector<Message> messages_;
};

zed::core::ToolCall call(std::string id, std::string name, Json arguments) {
  return {std::move(id), std::move(name), arguments.dump()};
}

Json execute(zed::core::Tool &tool, std::string id, std::string name,
             Json arguments) {
  const auto result = tool.execute(
      call(std::move(id), std::move(name), std::move(arguments)), {});
  assert(result);
  return Json::parse(result.value().content);
}

std::string note_pages(zed::tools::ContextNotesTool &tool,
                       std::string_view name, std::size_t max_bytes) {
  std::string recovered;
  std::size_t offset = 0;
  while (true) {
    const auto page =
        execute(tool, "note-page-" + std::to_string(offset), "context_notes",
                {{"purpose", "recover complete note"},
                 {"action", "read"},
                 {"name", name},
                 {"byte_offset", offset},
                 {"max_bytes", max_bytes}});
    assert(page.at("byte_offset") == offset);
    recovered += page.at("text").get<std::string>();
    if (!page.at("truncated"))
      return recovered;
    const auto next = page.at("next_byte_offset").get<std::size_t>();
    assert(next > offset);
    offset = next;
  }
}

std::string message_pages(zed::tools::ContextHistoryTool &tool,
                          std::string_view id, std::size_t max_bytes) {
  std::string recovered;
  std::size_t offset = 0;
  while (true) {
    const auto page = execute(tool, "message-page-" + std::to_string(offset),
                              "context_history",
                              {{"purpose", "recover tool arguments"},
                               {"action", "read"},
                               {"id", id},
                               {"byte_offset", offset},
                               {"max_bytes", max_bytes}});
    assert(page.at("byte_offset") == offset);
    recovered += page.at("message_json").get<std::string>();
    if (!page.at("truncated"))
      return recovered;
    const auto next = page.at("next_byte_offset").get<std::size_t>();
    assert(next > offset);
    offset = next;
  }
}

template <typename ReadPage>
std::vector<std::string> collect_message_ids(ReadPage read_page,
                                             std::string_view key) {
  std::vector<std::string> ids;
  std::size_t offset = 0;
  while (true) {
    const auto page = read_page(offset);
    assert(page.at("offset") == offset);
    for (const auto &message : page.at(key))
      ids.push_back(message.at("id").template get<std::string>());
    if (!page.at("truncated"))
      return ids;
    const auto next = page.at("next_offset").template get<std::size_t>();
    assert(next > offset);
    offset = next;
  }
}

void assert_failure(zed::core::Result<zed::core::ToolResult> result,
                    zed::core::ErrorCode code) {
  assert(!result);
  assert(result.error().code == code);
}

} // namespace

int main() {
  TempDirectory temporary;
  zed::core::InMemorySessionStore store;
  const auto journal = temporary.path() / "session.context.jsonl";
  zed::context::ContextArchive archive(store, [journal] { return journal; });
  zed::tools::ContextNotesTool notes(archive);
  zed::tools::ContextHistoryTool history(archive);

  const std::string utf8_note =
      "start \"quoted\"\n\tcontrol \xE2\x82\xAC \xF0\x9F\x98\x80 end";
  assert(archive.write_note("utf8", utf8_note, false));
  assert(note_pages(notes, "utf8", 4U) == utf8_note);

  const auto euro = utf8_note.find("\xE2\x82\xAC");
  assert(euro != std::string::npos);
  assert_failure(notes.execute(call("mid-codepoint", "context_notes",
                                    {{"purpose", "reject split UTF-8 offset"},
                                     {"action", "read"},
                                     {"name", "utf8"},
                                     {"byte_offset", euro + 1U},
                                     {"max_bytes", 4U}}),
                               {}),
                 zed::core::ErrorCode::invalid_argument);
  assert_failure(
      notes.execute(call("tiny-page", "context_notes",
                         {{"purpose", "reject non-progressing UTF-8 page"},
                          {"action", "read"},
                          {"name", "utf8"},
                          {"byte_offset", euro},
                          {"max_bytes", 1U}}),
                    {}),
      zed::core::ErrorCode::invalid_argument);

  const std::string tail(20U * 1024U, 'z');
  std::vector<zed::core::ToolCall> tool_calls{
      {"tool-many", "write", "{\"tail\":\"" + tail + "\"}"}};
  for (std::size_t index = 0; index < 15U; ++index) {
    tool_calls.push_back({"tool-" + std::to_string(index), "read",
                          "{\"needle\":\"" + std::string(1024U, 'q') + "\"}"});
  }
  assert(store.append({"assistant-tools", Role::assistant, "",
                       std::move(tool_calls), std::nullopt}));
  const auto recovered_message =
      message_pages(history, "assistant-tools", 8192U);
  const auto decoded_message = Json::parse(recovered_message);
  assert(decoded_message.at("tool_calls").at(0).at("arguments_json") ==
         "{\"tail\":\"" + tail + "\"}");

  for (std::size_t index = 0; index < 100U; ++index) {
    const auto id = "message-" + std::to_string(index);
    assert(store.append({id,
                         Role::user,
                         "needle " + std::string(1200U, 'a'),
                         {},
                         std::nullopt}));
  }
  const auto expected_ids = [&] {
    std::vector<std::string> ids{"assistant-tools"};
    for (std::size_t index = 0; index < 100U; ++index)
      ids.push_back("message-" + std::to_string(index));
    return ids;
  }();
  const auto list_ids = collect_message_ids(
      [&](std::size_t offset) {
        return execute(history, "history-list-" + std::to_string(offset),
                       "context_history",
                       {{"purpose", "page durable history"},
                        {"action", "list"},
                        {"offset", offset},
                        {"limit", 100U}});
      },
      "messages");
  assert(list_ids == expected_ids);
  const auto search_ids = collect_message_ids(
      [&](std::size_t offset) {
        return execute(history, "history-search-" + std::to_string(offset),
                       "context_history",
                       {{"purpose", "page matching history"},
                        {"action", "search"},
                        {"query", "needle"},
                        {"offset", offset},
                        {"limit", 100U}});
      },
      "messages");
  // The assistant's tool arguments also contain the literal query.
  assert(search_ids == expected_ids);

  for (std::size_t index = 0; index < 100U; ++index) {
    assert(archive.write_note("needle-note-" + std::to_string(index),
                              std::string(1024U, 'b'), false));
  }
  std::vector<std::string> note_names;
  std::size_t note_offset = 0;
  while (true) {
    const auto page = execute(
        notes, "notes-search-" + std::to_string(note_offset), "context_notes",
        {{"purpose", "page matching notes"},
         {"action", "search"},
         {"query", "needle-note"},
         {"offset", note_offset},
         {"limit", 100U}});
    for (const auto &match : page.at("matches"))
      note_names.push_back(match.at("name").get<std::string>());
    if (!page.at("truncated"))
      break;
    const auto next = page.at("next_offset").get<std::size_t>();
    assert(next > note_offset);
    note_offset = next;
  }
  assert(note_names.size() == 100U);

  for (std::size_t index = 1; index <= 100U; ++index) {
    const auto id = "message-" + std::to_string(index - 1U);
    assert(archive.commit_window({"window-" + std::to_string(index + 1U),
                                  index + 1U,
                                  id,
                                  {},
                                  "page windows"}));
  }
  std::vector<std::string> window_ids;
  std::size_t window_offset = 0;
  while (true) {
    const auto page = execute(
        history, "windows-" + std::to_string(window_offset), "context_history",
        {{"purpose", "page context windows"},
         {"action", "windows"},
         {"offset", window_offset},
         {"limit", 100U}});
    for (const auto &window : page.at("windows"))
      window_ids.push_back(window.at("id").get<std::string>());
    if (!page.at("truncated"))
      break;
    const auto next = page.at("next_offset").get<std::size_t>();
    assert(next > window_offset);
    window_offset = next;
  }
  assert(window_ids.size() == 101U);
  for (std::size_t index = 0; index < window_ids.size(); ++index)
    assert(window_ids[index] == "window-" + std::to_string(index + 1U));

  assert(!archive.commit_window({"window-102",
                                 101U,
                                 "message-99",
                                 {"message-1", "message-1"},
                                 "duplicate retained ids"}));
  assert(store.append({"future-user",
                       Role::user,
                       "After the checkpoint boundary",
                       {},
                       std::nullopt}));
  assert(!archive.commit_window({"window-102",
                                 101U,
                                 "message-99",
                                 {"future-user"},
                                 "non-prefix retained ids"}));

  const std::string invalid_utf8("bad\xFF", 4U);
  assert(!archive.write_note("invalid-utf8", invalid_utf8, false));
  zed::context::ContextArchive no_journal(store, {});
  assert(!no_journal.snapshot());
  zed::tools::ContextNotesTool unavailable_notes(no_journal);
  assert_failure(
      unavailable_notes.execute(
          call("missing-callback", "context_notes",
               {{"purpose", "report unavailable archive"}, {"action", "list"}}),
          {}),
      zed::core::ErrorCode::context_error);

  SleepingStore slow_store;
  const auto slow_journal = temporary.path() / "slow.context.jsonl";
  zed::context::ContextArchive slow_archive(
      slow_store, [slow_journal] { return slow_journal; });
  zed::tools::ContextHistoryTool timed_history(slow_archive,
                                               std::chrono::milliseconds(1));
  assert_failure(
      timed_history.execute(
          call("timeout", "context_history",
               {{"purpose", "enforce execution budget"}, {"action", "list"}}),
          {}),
      zed::core::ErrorCode::timeout);
  zed::core::CancellationSource cancelled;
  cancelled.cancel();
  assert_failure(
      history.execute(call("cancelled", "context_history",
                           {{"purpose", "preserve caller cancellation"},
                            {"action", "list"}}),
                      cancelled.token()),
      zed::core::ErrorCode::cancelled);

  const auto symlink_path = temporary.path() / "symlink.context.jsonl";
  assert(symlink(journal.c_str(), symlink_path.c_str()) == 0);
  zed::context::ContextArchive symlink_archive(
      store, [symlink_path] { return symlink_path; });
  assert(!symlink_archive.snapshot());
  const auto fifo = temporary.path() / "fifo.context.jsonl";
  assert(mkfifo(fifo.c_str(), 0600) == 0);
  zed::context::ContextArchive fifo_archive(store, [fifo] { return fifo; });
  assert(!fifo_archive.snapshot());

  const auto locked = temporary.path() / "locked.context.jsonl";
  zed::context::ContextArchive locked_archive(store,
                                              [locked] { return locked; });
  assert(locked_archive.write_note("first", "value", false));
  const int lock_fd = open(locked.c_str(), O_RDWR | O_CLOEXEC);
  assert(lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) == 0);
  const auto contested = locked_archive.write_note("second", "value", false);
  assert(!contested &&
         contested.error().code == zed::core::ErrorCode::conflict);
  assert(flock(lock_fd, LOCK_UN) == 0 && close(lock_fd) == 0);

  const auto protected_directory = temporary.path() / "unreadable";
  assert(std::filesystem::create_directory(protected_directory));
  assert(chmod(protected_directory.c_str(), 0000) == 0);
  const auto protected_journal = protected_directory / "context.jsonl";
  zed::context::ContextArchive protected_archive(
      store, [protected_journal] { return protected_journal; });
  assert(!protected_archive.write_note("cannot-write", "value", false));
  assert(chmod(protected_directory.c_str(), 0700) == 0);

  return 0;
}
