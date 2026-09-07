#include "zed/context/context_archive.hpp"
#include "zed/core/utf8.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <utility>

namespace zed::context {
namespace {

using Json = nlohmann::json;
constexpr int kJournalVersion = 1;
constexpr std::size_t kMaxNoteBytes = 1024U * 1024U;
constexpr std::size_t kMaxNotesBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaxJournalLineBytes = 6U * kMaxNoteBytes + 8192U;
constexpr std::size_t kMaxRetainedIds = 10000U;

core::Error failure(core::ErrorCode code, std::string message) {
  return {code, "context archive: " + std::move(message)};
}

bool cancelled(const core::CancellationToken &token) {
  return token.is_cancelled();
}

core::Result<void> validate_note_name(std::string_view name) {
  if (name.empty() || name.size() > 512U || !core::is_valid_utf8(name) ||
      std::any_of(name.begin(), name.end(),
                  [](unsigned char c) { return c < 32U; }))
    return core::Result<void>::failure(
        failure(core::ErrorCode::invalid_argument, "invalid note name"));
  const std::filesystem::path path{name};
  if (path.is_absolute())
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "note name must be relative"));
  if (name.starts_with('/') || name.ends_with('/') ||
      name.find("//") != std::string_view::npos) {
    return core::Result<void>::failure(
        failure(core::ErrorCode::invalid_argument,
                "note name contains empty component"));
  }
  for (const auto &part : path) {
    const auto component = part.string();
    if (component.empty() || component == "." || component == "..") {
      return core::Result<void>::failure(failure(
          core::ErrorCode::invalid_argument, "note name contains traversal"));
    }
  }
  if (name.find('\\') != std::string_view::npos ||
      name.find('\0') != std::string_view::npos) {
    return core::Result<void>::failure(
        failure(core::ErrorCode::invalid_argument, "invalid note name"));
  }
  return core::Result<void>::success();
}

core::Result<void> validate_checkpoint(const WindowCheckpoint &checkpoint) {
  if (checkpoint.id.empty() || checkpoint.id.size() > 256U ||
      checkpoint.last_message_id.size() > 1024U ||
      checkpoint.reason.size() > 8192U ||
      checkpoint.retained_ids.size() > kMaxRetainedIds) {
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "invalid window checkpoint"));
  }
  std::unordered_set<std::string> identifiers;
  for (const auto &id : checkpoint.retained_ids) {
    if (id.empty() || id.size() > 1024U || !identifiers.insert(id).second) {
      return core::Result<void>::failure(failure(
          core::ErrorCode::invalid_argument, "invalid retained message id"));
    }
  }
  return core::Result<void>::success();
}

Json checkpoint_json(const WindowCheckpoint &checkpoint) {
  return {{"id", checkpoint.id},
          {"history_size", checkpoint.history_size},
          {"last_message_id", checkpoint.last_message_id},
          {"retained_ids", checkpoint.retained_ids},
          {"reason", checkpoint.reason}};
}

core::Result<WindowCheckpoint> parse_checkpoint(const Json &value) {
  if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
      !value.contains("history_size") ||
      !value["history_size"].is_number_unsigned() ||
      !value.contains("last_message_id") ||
      !value["last_message_id"].is_string() ||
      !value.contains("retained_ids") || !value["retained_ids"].is_array() ||
      !value.contains("reason") || !value["reason"].is_string()) {
    return core::Result<WindowCheckpoint>::failure(
        failure(core::ErrorCode::context_error, "malformed checkpoint record"));
  }
  WindowCheckpoint checkpoint;
  checkpoint.id = value["id"].get<std::string>();
  const auto size = value["history_size"].get<std::uint64_t>();
  if (size >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return core::Result<WindowCheckpoint>::failure(failure(
        core::ErrorCode::context_error, "checkpoint history size overflow"));
  }
  checkpoint.history_size = static_cast<std::size_t>(size);
  checkpoint.last_message_id = value["last_message_id"].get<std::string>();
  checkpoint.reason = value["reason"].get<std::string>();
  for (const auto &id : value["retained_ids"]) {
    if (!id.is_string()) {
      return core::Result<WindowCheckpoint>::failure(failure(
          core::ErrorCode::context_error, "malformed retained message id"));
    }
    checkpoint.retained_ids.push_back(id.get<std::string>());
  }
  const auto valid = validate_checkpoint(checkpoint);
  if (!valid)
    return core::Result<WindowCheckpoint>::failure(valid.error());
  return core::Result<WindowCheckpoint>::success(std::move(checkpoint));
}

core::Result<ArchiveState> read_state(const std::filesystem::path &path,
                                      core::CancellationToken cancellation) {
  ArchiveState state;
  state.windows.push_back({"window-1", 0U, {}, {}, "initial"});
  if (cancelled(cancellation)) {
    return core::Result<ArchiveState>::failure(
        failure(core::ErrorCode::cancelled, "read cancelled"));
  }
  support::UniqueFd descriptor(
      open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  const int fd = descriptor.get();
  if (fd < 0) {
    if (errno == ENOENT)
      return core::Result<ArchiveState>::success(std::move(state));
    return core::Result<ArchiveState>::failure(
        failure(core::ErrorCode::context_error, "cannot open journal"));
  }
  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
    return core::Result<ArchiveState>::failure(failure(
        core::ErrorCode::context_error, "journal is not a regular file"));
  }
  std::string content;
  char buffer[8192];
  while (true) {
    if (cancelled(cancellation)) {
      return core::Result<ArchiveState>::failure(
          failure(core::ErrorCode::cancelled, "read cancelled"));
    }
    const auto count = read(fd, buffer, sizeof(buffer));
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;

      return core::Result<ArchiveState>::failure(
          failure(core::ErrorCode::context_error, "cannot read journal"));
    }
    if (content.size() + static_cast<std::size_t>(count) >
        kMaxNotesBytes * 2U) {
      return core::Result<ArchiveState>::failure(failure(
          core::ErrorCode::context_error, "journal exceeds archive limit"));
    }
    content.append(buffer, static_cast<std::size_t>(count));
  }

  std::size_t offset = 0;
  while (offset < content.size()) {
    if (cancelled(cancellation))
      return core::Result<ArchiveState>::failure(
          failure(core::ErrorCode::cancelled, "journal parsing cancelled"));
    const auto end = content.find('\n', offset);
    if (end == std::string::npos)
      return core::Result<ArchiveState>::failure(
          failure(core::ErrorCode::context_error,
                  "journal has incomplete trailing record; previous transcript "
                  "remains intact"));
    const auto length =
        (end == std::string::npos ? content.size() : end) - offset;
    if (length > kMaxJournalLineBytes) {
      return core::Result<ArchiveState>::failure(failure(
          core::ErrorCode::context_error, "journal record exceeds limit"));
    }
    if (length != 0U) {
      try {
        const auto record = Json::parse(content.substr(offset, length));
        if (!record.is_object() ||
            record.value("version", 0) != kJournalVersion ||
            !record.contains("type") || !record["type"].is_string()) {
          return core::Result<ArchiveState>::failure(failure(
              core::ErrorCode::context_error, "invalid journal record"));
        }
        const auto type = record["type"].get<std::string>();
        if (type == "note") {
          if (!record.contains("name") || !record["name"].is_string() ||
              !record.contains("text") || !record["text"].is_string() ||
              !record.contains("append") || !record["append"].is_boolean()) {
            return core::Result<ArchiveState>::failure(failure(
                core::ErrorCode::context_error, "malformed note record"));
          }
          const auto name = record["name"].get<std::string>();
          const auto text = record["text"].get<std::string>();
          const auto valid_name = validate_note_name(name);
          if (!valid_name || text.size() > kMaxNoteBytes) {
            return core::Result<ArchiveState>::failure(
                failure(core::ErrorCode::context_error, "invalid note record"));
          }
          auto &note = state.notes[name];
          if (record["append"].get<bool>())
            note += text;
          else
            note = text;
          if (note.size() > kMaxNoteBytes)
            return core::Result<ArchiveState>::failure(
                failure(core::ErrorCode::context_error, "note exceeds limit"));
        } else if (type == "checkpoint") {
          if (!record.contains("checkpoint")) {
            return core::Result<ArchiveState>::failure(
                failure(core::ErrorCode::context_error, "missing checkpoint"));
          }
          const auto checkpoint = parse_checkpoint(record["checkpoint"]);
          if (!checkpoint)
            return core::Result<ArchiveState>::failure(checkpoint.error());
          if (checkpoint.value().history_size <
              state.windows.back().history_size) {
            return core::Result<ArchiveState>::failure(
                failure(core::ErrorCode::context_error,
                        "checkpoint history regresses"));
          }
          if (checkpoint.value().id !=
              "window-" + std::to_string(state.windows.size() + 1U))
            return core::Result<ArchiveState>::failure(
                failure(core::ErrorCode::context_error,
                        "checkpoint id is not sequential"));
          state.windows.push_back(checkpoint.value());
          state.reset_requested = false;
        } else if (type == "request_new_window") {
          state.reset_requested = true;
        } else {
          return core::Result<ArchiveState>::failure(failure(
              core::ErrorCode::context_error, "unknown journal record"));
        }
      } catch (const Json::exception &) {
        return core::Result<ArchiveState>::failure(failure(
            core::ErrorCode::context_error, "cannot parse journal record"));
      }
    }
    if (end == std::string::npos)
      break;
    offset = end + 1U;
  }
  std::size_t total = 0;
  for (const auto &[name, note] : state.notes) {
    static_cast<void>(name);
    total += note.size();
  }
  if (total > kMaxNotesBytes) {
    return core::Result<ArchiveState>::failure(failure(
        core::ErrorCode::context_error, "notes exceed aggregate limit"));
  }
  return core::Result<ArchiveState>::success(std::move(state));
}

core::Result<void> append_record(const std::filesystem::path &path,
                                 const Json &record,
                                 core::CancellationToken cancellation) {
  if (cancelled(cancellation))
    return core::Result<void>::failure(
        failure(core::ErrorCode::cancelled, "write cancelled"));
  const auto directory = path.parent_path();
  std::error_code error;
  if (!directory.empty()) {
    std::filesystem::create_directories(directory, error);
    const auto directory_status =
        std::filesystem::symlink_status(directory, error);
    if (error || std::filesystem::is_symlink(directory_status) ||
        !std::filesystem::is_directory(directory_status)) {
      return core::Result<void>::failure(failure(
          core::ErrorCode::context_error, "cannot secure journal directory"));
    }
  }
  support::UniqueFd descriptor(open(
      path.c_str(),
      O_RDWR | O_APPEND | O_CREAT | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW, 0600));
  const int fd = descriptor.get();
  if (fd < 0)
    return core::Result<void>::failure(failure(
        core::ErrorCode::context_error, "cannot open journal for writing"));
  struct stat status{};
  if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      flock(fd, LOCK_EX | LOCK_NB) != 0) {
    return core::Result<void>::failure(
        failure(errno == EWOULDBLOCK ? core::ErrorCode::conflict
                                     : core::ErrorCode::context_error,
                "cannot secure journal"));
  }
  if (status.st_size > 0) {
    char last = '\0';
    if (lseek(fd, -1, SEEK_END) < 0 || read(fd, &last, 1U) != 1 ||
        last != '\n') {
      return core::Result<void>::failure(
          failure(core::ErrorCode::context_error,
                  "journal has incomplete trailing record"));
    }
  }
  std::string line;
  try {
    line = record.dump() + "\n";
  } catch (const Json::exception &) {
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "cannot encode journal record"));
  }
  if (line.size() - 1U > kMaxJournalLineBytes)
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "journal record exceeds limit"));
  if (static_cast<std::uintmax_t>(status.st_size) + line.size() >
      kMaxNotesBytes * 2U) {
    return core::Result<void>::failure(failure(
        core::ErrorCode::context_error, "journal exceeds archive limit"));
  }
  std::size_t offset = 0;
  const auto rollback = [&](core::Error error) {
    if (ftruncate(fd, status.st_size) != 0 || fsync(fd) != 0)
      error.message += "; cannot roll back incomplete journal append";
    return core::Result<void>::failure(std::move(error));
  };
  while (offset < line.size()) {
    if (cancelled(cancellation))
      return rollback(failure(core::ErrorCode::cancelled, "write cancelled"));
    const auto written = write(fd, line.data() + offset, line.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      return rollback(failure(core::ErrorCode::context_error,
                              "cannot append journal: " +
                                  std::string(std::strerror(errno))));
    }
  }
  const bool synced = fsync(fd) == 0;

  if (!synced)
    return rollback(
        failure(core::ErrorCode::context_error, "cannot flush journal"));
  return core::Result<void>::success();
}

core::Result<void>
write_journal_exclusive(const std::filesystem::path &path,
                        std::string_view content,
                        core::CancellationToken cancellation) {
  if (cancelled(cancellation)) {
    return core::Result<void>::failure(
        failure(core::ErrorCode::cancelled, "fork cancelled"));
  }
  const auto directory = path.parent_path();
  std::error_code error;
  if (!directory.empty()) {
    std::filesystem::create_directories(directory, error);
    const auto status = std::filesystem::symlink_status(directory, error);
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      return core::Result<void>::failure(
          failure(core::ErrorCode::context_error,
                  "cannot secure fork journal directory"));
    }
  }
  support::UniqueFd descriptor(
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
           0600));
  const int fd = descriptor.get();
  if (fd < 0) {
    return core::Result<void>::failure(
        failure(errno == EEXIST ? core::ErrorCode::conflict
                                : core::ErrorCode::context_error,
                "cannot create fork journal"));
  }
  std::size_t offset = 0;
  while (offset < content.size()) {
    if (cancelled(cancellation)) {

      static_cast<void>(unlink(path.c_str()));
      return core::Result<void>::failure(
          failure(core::ErrorCode::cancelled, "fork cancelled"));
    }
    const auto written =
        write(fd, content.data() + offset, content.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {

      static_cast<void>(unlink(path.c_str()));
      return core::Result<void>::failure(
          failure(core::ErrorCode::context_error, "cannot write fork journal"));
    }
  }
  const bool synced = fsync(fd) == 0;

  if (!synced) {
    static_cast<void>(unlink(path.c_str()));
    return core::Result<void>::failure(
        failure(core::ErrorCode::context_error, "cannot flush fork journal"));
  }
  return core::Result<void>::success();
}

} // namespace

ContextArchive::ContextArchive(core::SessionStore &session,
                               JournalPath journal_path)
    : session_(session), journal_path_(std::move(journal_path)) {}

core::Result<ArchiveState>
ContextArchive::snapshot(core::CancellationToken cancellation) const {
  if (!journal_path_)
    return core::Result<ArchiveState>::failure(
        failure(core::ErrorCode::context_error, "journal path is unavailable"));
  return read_state(journal_path_(), cancellation);
}

core::Result<void>
ContextArchive::commit_window(WindowCheckpoint checkpoint,
                              core::CancellationToken cancellation) {
  const auto valid = validate_checkpoint(checkpoint);
  if (!valid)
    return valid;
  const auto state = snapshot(cancellation);
  if (!state)
    return core::Result<void>::failure(state.error());
  if (checkpoint.history_size < state.value().windows.back().history_size)
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "checkpoint history regresses"));
  const auto expected_id =
      "window-" + std::to_string(state.value().windows.size() + 1U);
  if (checkpoint.id != expected_id) {
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "checkpoint id is not sequential"));
  }
  const auto history = read_history(cancellation);
  if (!history)
    return core::Result<void>::failure(history.error());
  if (checkpoint.history_size > history.value().size()) {
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "checkpoint history is invalid"));
  }
  if (checkpoint.history_size == 0U
          ? !checkpoint.last_message_id.empty()
          : checkpoint.last_message_id !=
                history.value()[checkpoint.history_size - 1U].id) {
    return core::Result<void>::failure(
        failure(core::ErrorCode::invalid_argument,
                "checkpoint last message is invalid"));
  }
  std::unordered_set<std::string> prefix_ids;
  for (std::size_t i = 0; i < checkpoint.history_size; ++i)
    prefix_ids.insert(history.value()[i].id);
  std::unordered_set<std::string> retained_ids;
  for (const auto &retained : checkpoint.retained_ids) {
    if (!prefix_ids.contains(retained) ||
        !retained_ids.insert(retained).second) {
      return core::Result<void>::failure(
          failure(core::ErrorCode::invalid_argument,
                  "checkpoint retained message is invalid"));
    }
  }
  return append_record(journal_path_(),
                       {{"version", kJournalVersion},
                        {"type", "checkpoint"},
                        {"checkpoint", checkpoint_json(checkpoint)}},
                       cancellation);
}

core::Result<void>
ContextArchive::request_new_window(core::CancellationToken cancellation) {
  const auto state = snapshot(cancellation);
  if (!state)
    return core::Result<void>::failure(state.error());
  return append_record(
      journal_path_(),
      {{"version", kJournalVersion}, {"type", "request_new_window"}},
      cancellation);
}

core::Result<void>
ContextArchive::fork_to(const std::filesystem::path &destination_journal,
                        core::CancellationToken cancellation) const {
  if (!journal_path_) {
    return core::Result<void>::failure(
        failure(core::ErrorCode::context_error, "journal path is unavailable"));
  }
  const auto source = journal_path_();
  std::error_code filesystem_error;
  const auto source_status =
      std::filesystem::symlink_status(source, filesystem_error);
  if (filesystem_error &&
      filesystem_error != std::errc::no_such_file_or_directory) {
    return core::Result<void>::failure(failure(
        core::ErrorCode::context_error, "cannot inspect source journal"));
  }
  if (!std::filesystem::exists(source_status))
    return core::Result<void>::success();
  if (std::filesystem::is_symlink(source_status) ||
      !std::filesystem::is_regular_file(source_status)) {
    return core::Result<void>::failure(
        failure(core::ErrorCode::context_error,
                "source journal is not a regular file"));
  }
  const auto state = snapshot(cancellation);
  if (!state)
    return core::Result<void>::failure(state.error());
  std::string records;
  for (const auto &[name, text] : state.value().notes) {
    records += Json{
        {"version", kJournalVersion},
        {"type", "note"},
        {"name", name},
        {"text", text},
        {"append",
         false}}.dump();
    records += '\n';
  }
  for (std::size_t index = 1; index < state.value().windows.size(); ++index) {
    records +=
        Json{{"version", kJournalVersion},
             {"type", "checkpoint"},
             {"checkpoint", checkpoint_json(state.value().windows[index])}}
            .dump();
    records += '\n';
  }
  return write_journal_exclusive(destination_journal, records, cancellation);
}

core::Result<void>
ContextArchive::write_note(std::string_view name, std::string_view text,
                           bool append, core::CancellationToken cancellation) {
  const auto valid_name = validate_note_name(name);
  if (!valid_name)
    return valid_name;
  if (!core::is_valid_utf8(text))
    return core::Result<void>::failure(failure(
        core::ErrorCode::invalid_argument, "note text must be valid UTF-8"));
  if (text.size() > kMaxNoteBytes)
    return core::Result<void>::failure(
        failure(core::ErrorCode::invalid_argument, "note exceeds limit"));
  const auto state = snapshot(cancellation);
  if (!state)
    return core::Result<void>::failure(state.error());
  const auto existing = state.value().notes.find(std::string(name));
  const std::size_t old_bytes =
      existing == state.value().notes.end() ? 0U : existing->second.size();
  std::size_t total = text.size() + (append ? old_bytes : 0U);
  for (const auto &[key, value] : state.value().notes) {
    if (key != name)
      total += value.size();
  }
  if (total > kMaxNotesBytes ||
      (append && old_bytes + text.size() > kMaxNoteBytes))
    return core::Result<void>::failure(
        failure(core::ErrorCode::invalid_argument, "notes exceed limit"));
  return append_record(journal_path_(),
                       {{"version", kJournalVersion},
                        {"type", "note"},
                        {"name", std::string(name)},
                        {"text", std::string(text)},
                        {"append", append}},
                       cancellation);
}

core::Result<std::vector<core::Message>>
ContextArchive::read_history(core::CancellationToken cancellation) const {
  if (cancelled(cancellation))
    return core::Result<std::vector<core::Message>>::failure(
        failure(core::ErrorCode::cancelled, "history read cancelled"));
  const auto history = session_.load();
  if (!history)
    return core::Result<std::vector<core::Message>>::failure(
        {history.error().code,
         "context archive: history unavailable: " + history.error().message});
  if (cancelled(cancellation))
    return core::Result<std::vector<core::Message>>::failure(
        failure(core::ErrorCode::cancelled, "history read cancelled"));
  return history;
}

} // namespace zed::context
