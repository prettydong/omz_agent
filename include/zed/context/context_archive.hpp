#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/cancellation.hpp"
#include "zed/core/message.hpp"
#include "zed/core/result.hpp"
#include "zed/core/session_store.hpp"

namespace zed::context {

struct WindowCheckpoint {
  std::string id;
  std::size_t history_size{};
  std::string last_message_id;
  std::vector<std::string> retained_ids;
  std::string reason;
};

struct ArchiveState {
  std::map<std::string, std::string> notes;
  std::vector<WindowCheckpoint> windows;
  bool reset_requested{false};
};

// Persists per-session archive metadata separately from SessionStore's JSONL
// schema. journal_path is evaluated for every operation so session switches
// never reuse stale archive state.
class ContextArchive {
public:
  using JournalPath = std::function<std::filesystem::path()>;

  ContextArchive(core::SessionStore &session, JournalPath journal_path);

  [[nodiscard]] core::Result<ArchiveState>
  snapshot(core::CancellationToken cancellation = {}) const;
  [[nodiscard]] core::Result<void>
  commit_window(WindowCheckpoint checkpoint,
                core::CancellationToken cancellation = {});
  [[nodiscard]] core::Result<void>
  request_new_window(core::CancellationToken cancellation = {});
  [[nodiscard]] core::Result<void>
  fork_to(const std::filesystem::path &destination_journal,
          core::CancellationToken cancellation = {}) const;
  [[nodiscard]] core::Result<void>
  write_note(std::string_view name, std::string_view text, bool append,
             core::CancellationToken cancellation = {});
  [[nodiscard]] core::Result<std::vector<core::Message>>
  read_history(core::CancellationToken cancellation = {}) const;

private:
  core::SessionStore &session_;
  JournalPath journal_path_;
};

} // namespace zed::context
