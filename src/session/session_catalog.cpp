#include "zed/session/session_catalog.hpp"

#include "zed/session/jsonl_session_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <system_error>
#include <utility>

#include <unistd.h>

namespace zed::session {

namespace {

core::Error catalog_error(std::string operation,
                          const std::filesystem::path &path,
                          const std::error_code &error) {
  return {
      core::ErrorCode::session_error,
      std::move(operation) + " '" + path.string() + "': " + error.message(),
  };
}

core::Result<SessionEntry> selected_entry(const SessionEntry &entry) {
  if (!entry.valid) {
    return core::Result<SessionEntry>::failure({
        core::ErrorCode::session_error,
        "session is not a valid Session v2 file: " + entry.name + ": " +
            entry.error,
    });
  }
  return core::Result<SessionEntry>::success(entry);
}

} // namespace

std::filesystem::path new_session_path(const std::filesystem::path &directory) {
  static std::atomic_uint64_t sequence{0};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  const std::string filename = "session-" + std::to_string(now.count()) + "-" +
                               std::to_string(getpid()) + "-" +
                               std::to_string(++sequence) + ".jsonl";
  return directory / filename;
}

core::Result<std::vector<SessionEntry>>
list_sessions(const std::filesystem::path &directory) {
  std::error_code filesystem_error;
  const bool exists = std::filesystem::exists(directory, filesystem_error);
  if (filesystem_error) {
    return core::Result<std::vector<SessionEntry>>::failure(catalog_error(
        "cannot inspect session directory", directory, filesystem_error));
  }
  if (!exists)
    return core::Result<std::vector<SessionEntry>>::success({});

  const bool is_directory =
      std::filesystem::is_directory(directory, filesystem_error);
  if (filesystem_error) {
    return core::Result<std::vector<SessionEntry>>::failure(catalog_error(
        "cannot inspect session directory", directory, filesystem_error));
  }
  if (!is_directory) {
    return core::Result<std::vector<SessionEntry>>::failure({
        core::ErrorCode::session_error,
        "session directory is not a directory: " + directory.string(),
    });
  }

  std::vector<SessionEntry> sessions;
  std::filesystem::directory_iterator iterator(directory, filesystem_error);
  const std::filesystem::directory_iterator end;
  if (filesystem_error) {
    return core::Result<std::vector<SessionEntry>>::failure(catalog_error(
        "cannot list session directory", directory, filesystem_error));
  }
  while (iterator != end) {
    const auto &entry = *iterator;
    const bool is_regular_file = entry.is_regular_file(filesystem_error);
    if (filesystem_error) {
      return core::Result<std::vector<SessionEntry>>::failure(catalog_error(
          "cannot inspect session file", entry.path(), filesystem_error));
    }
    const bool is_symlink = entry.is_symlink(filesystem_error);
    if (filesystem_error) {
      return core::Result<std::vector<SessionEntry>>::failure(catalog_error(
          "cannot inspect session file", entry.path(), filesystem_error));
    }
    if (is_regular_file && !is_symlink &&
        entry.path().extension() == ".jsonl") {
      const auto modified_at = entry.last_write_time(filesystem_error);
      if (filesystem_error) {
        return core::Result<std::vector<SessionEntry>>::failure(
            catalog_error("cannot read session modification time", entry.path(),
                          filesystem_error));
      }
      SessionEntry session_entry{
          entry.path().stem().string(),
          entry.path().stem().string(),
          entry.path(),
          modified_at,
          0,
          0,
          false,
          true,
          {},
      };
      JsonlSessionStore store(entry.path());
      const auto inspection = store.inspect();
      if (inspection) {
        session_entry.title = inspection.value().metadata.title;
        session_entry.message_count = inspection.value().message_count;
        session_entry.turn_count = inspection.value().turn_count;
        session_entry.interrupted = inspection.value().has_interrupted_turn;
      } else {
        session_entry.valid = false;
        session_entry.error = inspection.error().message;
      }
      sessions.push_back(std::move(session_entry));
    }
    iterator.increment(filesystem_error);
    if (filesystem_error) {
      return core::Result<std::vector<SessionEntry>>::failure(catalog_error(
          "cannot list session directory", directory, filesystem_error));
    }
  }

  std::sort(sessions.begin(), sessions.end(),
            [](const SessionEntry &left, const SessionEntry &right) {
              if (left.modified_at != right.modified_at)
                return left.modified_at > right.modified_at;
              return left.name < right.name;
            });
  return core::Result<std::vector<SessionEntry>>::success(std::move(sessions));
}

core::Result<SessionEntry> find_session(const std::filesystem::path &directory,
                                        std::string_view identifier) {
  if (identifier.empty()) {
    return core::Result<SessionEntry>::failure({
        core::ErrorCode::invalid_argument,
        "session name cannot be empty",
    });
  }
  const auto sessions = list_sessions(directory);
  if (!sessions)
    return core::Result<SessionEntry>::failure(sessions.error());

  const auto exact =
      std::find_if(sessions.value().begin(), sessions.value().end(),
                   [&](const SessionEntry &entry) {
                     return entry.name == identifier ||
                            entry.path.filename().string() == identifier;
                   });
  if (exact != sessions.value().end())
    return selected_entry(*exact);

  const SessionEntry *title_match = nullptr;
  for (const auto &entry : sessions.value()) {
    if (entry.title != identifier)
      continue;
    if (title_match != nullptr) {
      return core::Result<SessionEntry>::failure({
          core::ErrorCode::conflict,
          "multiple sessions use the title: " + std::string(identifier) +
              "; open one by its session id",
      });
    }
    title_match = &entry;
  }
  if (title_match != nullptr)
    return selected_entry(*title_match);

  return core::Result<SessionEntry>::failure({
      core::ErrorCode::not_found,
      "session not found: " + std::string(identifier),
  });
}

} // namespace zed::session
