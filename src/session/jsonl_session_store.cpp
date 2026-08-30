#include "zed/session/jsonl_session_store.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <sys/file.h>
#include <unistd.h>

namespace zed::session {

namespace {

using core::ErrorCode;
using core::Message;
using core::Role;
using core::SessionTurnOutcome;
using core::ToolCall;
using Json = nlohmann::json;

constexpr int kSessionVersion = 2;
constexpr std::size_t kMaxTurnDetailBytes = 2048;

struct ActiveTurn {
  core::TurnId id;
  bool has_user_message{false};
  bool has_terminal_assistant_message{false};
  std::vector<ToolCall> pending_tool_calls;
};

struct ParsedSession {
  SessionMetadata metadata;
  std::vector<Message> messages;
  std::unordered_set<core::MessageId> message_ids;
  std::optional<ActiveTurn> active_turn;
  std::size_t turn_count{};
  bool last_turn_interrupted{false};
  bool trailing_partial_record{false};
  std::uintmax_t valid_file_bytes{};
};

const Json *field(const Json &object, std::string_view name) {
  if (!object.is_object())
    return nullptr;
  const auto iterator = object.find(std::string(name));
  return iterator == object.end() ? nullptr : &*iterator;
}

core::Error session_error(std::string operation,
                          const std::filesystem::path &path,
                          std::string detail) {
  return {
      ErrorCode::session_error,
      std::move(operation) + " '" + path.string() + "': " + std::move(detail),
  };
}

std::int64_t unix_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool has_visible_character(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isspace(character) == 0;
  });
}

const char *role_name(Role role) {
  switch (role) {
  case Role::system:
    return "system";
  case Role::user:
    return "user";
  case Role::assistant:
    return "assistant";
  case Role::tool:
    return "tool";
  }
  return "user";
}

std::optional<Role> parse_role(const Json *value) {
  if (value == nullptr || !value->is_string())
    return std::nullopt;
  const auto role = value->get<std::string>();
  if (role == "user")
    return Role::user;
  if (role == "assistant")
    return Role::assistant;
  if (role == "tool")
    return Role::tool;
  return std::nullopt;
}

const char *outcome_name(SessionTurnOutcome outcome) {
  switch (outcome) {
  case SessionTurnOutcome::completed:
    return "completed";
  case SessionTurnOutcome::failed:
    return "failed";
  case SessionTurnOutcome::cancelled:
    return "cancelled";
  case SessionTurnOutcome::interrupted:
    return "interrupted";
  }
  return "failed";
}

std::optional<SessionTurnOutcome> parse_outcome(const Json *value) {
  if (value == nullptr || !value->is_string())
    return std::nullopt;
  const auto outcome = value->get<std::string>();
  if (outcome == "completed")
    return SessionTurnOutcome::completed;
  if (outcome == "failed")
    return SessionTurnOutcome::failed;
  if (outcome == "cancelled")
    return SessionTurnOutcome::cancelled;
  if (outcome == "interrupted")
    return SessionTurnOutcome::interrupted;
  return std::nullopt;
}

core::Result<std::string> required_string(const Json &object,
                                          std::string_view name,
                                          bool allow_empty = false) {
  const auto *value = field(object, name);
  if (value == nullptr || !value->is_string()) {
    return core::Result<std::string>::failure(
        {ErrorCode::session_error,
         "missing string field: " + std::string(name)});
  }
  auto text = value->get<std::string>();
  if (!allow_empty && text.empty()) {
    return core::Result<std::string>::failure(
        {ErrorCode::session_error, "empty string field: " + std::string(name)});
  }
  return core::Result<std::string>::success(std::move(text));
}

core::Result<std::int64_t> required_integer(const Json &object,
                                            std::string_view name) {
  const auto *value = field(object, name);
  if (value == nullptr || !value->is_number_integer()) {
    return core::Result<std::int64_t>::failure(
        {ErrorCode::session_error,
         "missing integer field: " + std::string(name)});
  }
  return core::Result<std::int64_t>::success(value->get<std::int64_t>());
}

core::Result<void> validate_record_envelope(const Json &record) {
  if (!record.is_object()) {
    return core::Result<void>::failure(
        {ErrorCode::session_error, "record is not an object"});
  }
  const auto *version = field(record, "version");
  const auto *type = field(record, "type");
  if (version == nullptr || !version->is_number_integer() ||
      version->get<int>() != kSessionVersion) {
    return core::Result<void>::failure(
        {ErrorCode::session_error, "unsupported session record version"});
  }
  if (type == nullptr || !type->is_string() ||
      type->get<std::string>().empty()) {
    return core::Result<void>::failure(
        {ErrorCode::session_error, "session record type is missing"});
  }
  return core::Result<void>::success();
}

Json message_json(const Message &message, std::string_view turn_id) {
  Json record = Json::object();
  record["version"] = kSessionVersion;
  record["type"] = "message";
  record["turn_id"] = turn_id;
  record["id"] = message.id;
  record["role"] = role_name(message.role);
  record["content"] = message.content;
  record["is_error"] = message.is_error;
  record["tool_calls"] = Json::array();
  for (const auto &call : message.tool_calls) {
    record["tool_calls"].push_back({
        {"id", call.id},
        {"name", call.name},
        {"arguments_json", call.arguments_json},
    });
  }
  if (message.tool_call_id.has_value())
    record["tool_call_id"] = *message.tool_call_id;
  return record;
}

core::Result<Message> parse_message(const Json &record) {
  const auto id = required_string(record, "id");
  const auto content = required_string(record, "content", true);
  const auto role = parse_role(field(record, "role"));
  if (!id)
    return core::Result<Message>::failure(id.error());
  if (!content)
    return core::Result<Message>::failure(content.error());
  if (!role.has_value()) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error, "invalid persisted message role"});
  }

  Message message{id.value(), *role, content.value(), {}, std::nullopt};
  const auto *is_error = field(record, "is_error");
  if (is_error == nullptr || !is_error->is_boolean()) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error, "message is_error must be a boolean"});
  }
  message.is_error = is_error->get<bool>();
  const auto *calls = field(record, "tool_calls");
  if (calls == nullptr || !calls->is_array()) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error, "message tool_calls must be an array"});
  }
  std::unordered_set<core::ToolCallId> call_ids;
  for (const auto &call_record : *calls) {
    const auto call_id = required_string(call_record, "id");
    const auto name = required_string(call_record, "name");
    const auto arguments = required_string(call_record, "arguments_json");
    if (!call_id)
      return core::Result<Message>::failure(call_id.error());
    if (!name)
      return core::Result<Message>::failure(name.error());
    if (!arguments)
      return core::Result<Message>::failure(arguments.error());
    if (!call_ids.insert(call_id.value()).second) {
      return core::Result<Message>::failure(
          {ErrorCode::session_error,
           "duplicate tool call id in assistant message: " + call_id.value()});
    }
    message.tool_calls.push_back(
        {call_id.value(), name.value(), arguments.value()});
  }
  if (const auto *tool_call_id = field(record, "tool_call_id");
      tool_call_id != nullptr) {
    if (!tool_call_id->is_string() ||
        tool_call_id->get<std::string>().empty()) {
      return core::Result<Message>::failure(
          {ErrorCode::session_error, "invalid message tool_call_id"});
    }
    message.tool_call_id = tool_call_id->get<std::string>();
  }

  if (message.role == Role::user &&
      (!message.tool_calls.empty() || message.tool_call_id.has_value())) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error,
         "user message cannot contain tool call fields"});
  }
  if (message.role == Role::assistant && message.tool_call_id.has_value()) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error,
         "assistant message cannot contain tool_call_id"});
  }
  if (message.role == Role::tool &&
      (!message.tool_calls.empty() || !message.tool_call_id.has_value())) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error,
         "tool message must identify exactly one tool call"});
  }
  if (message.role != Role::tool && message.is_error) {
    return core::Result<Message>::failure(
        {ErrorCode::session_error, "only tool messages can set is_error"});
  }
  return core::Result<Message>::success(std::move(message));
}

core::Result<void> apply_message(ParsedSession &session,
                                 const Message &message) {
  if (!session.active_turn.has_value()) {
    return core::Result<void>::failure(
        {ErrorCode::session_error, "message appears outside an active turn"});
  }
  auto &turn = *session.active_turn;
  if (!session.message_ids.insert(message.id).second) {
    return core::Result<void>::failure(
        {ErrorCode::session_error, "duplicate message id: " + message.id});
  }

  if (message.role == Role::user) {
    if (turn.has_user_message) {
      return core::Result<void>::failure(
          {ErrorCode::session_error,
           "user message is not the first message in its turn"});
    }
    turn.has_user_message = true;
  } else if (!turn.has_user_message) {
    return core::Result<void>::failure(
        {ErrorCode::session_error,
         "turn contains a response before its user message"});
  }

  if (message.role == Role::assistant) {
    if (!turn.pending_tool_calls.empty()) {
      return core::Result<void>::failure(
          {ErrorCode::session_error,
           "assistant response appears before prior tool calls finished"});
    }
    turn.pending_tool_calls = message.tool_calls;
    turn.has_terminal_assistant_message = message.tool_calls.empty();
  } else if (message.role == Role::tool) {
    turn.has_terminal_assistant_message = false;
    const auto pending = std::find_if(
        turn.pending_tool_calls.begin(), turn.pending_tool_calls.end(),
        [&](const ToolCall &call) { return call.id == *message.tool_call_id; });
    if (pending == turn.pending_tool_calls.end()) {
      return core::Result<void>::failure(
          {ErrorCode::session_error,
           "tool result does not match a pending tool call: " +
               *message.tool_call_id});
    }
    turn.pending_tool_calls.erase(pending);
  }

  session.messages.push_back(message);
  return core::Result<void>::success();
}

core::Result<SessionMetadata> parse_metadata(const Json &record) {
  const auto id = required_string(record, "id");
  const auto title = required_string(record, "title");
  const auto workspace = required_string(record, "workspace");
  const auto provider = required_string(record, "provider");
  const auto model = required_string(record, "model");
  const auto created_at = required_integer(record, "created_at_unix_ms");
  const auto updated_at = required_integer(record, "updated_at_unix_ms");
  if (!id)
    return core::Result<SessionMetadata>::failure(id.error());
  if (!title)
    return core::Result<SessionMetadata>::failure(title.error());
  if (!workspace)
    return core::Result<SessionMetadata>::failure(workspace.error());
  if (!provider)
    return core::Result<SessionMetadata>::failure(provider.error());
  if (!model)
    return core::Result<SessionMetadata>::failure(model.error());
  if (!created_at)
    return core::Result<SessionMetadata>::failure(created_at.error());
  if (!updated_at)
    return core::Result<SessionMetadata>::failure(updated_at.error());
  if (!has_visible_character(title.value()) || title.value().size() > 160) {
    return core::Result<SessionMetadata>::failure({
        ErrorCode::session_error,
        "session title must contain 1 to 160 bytes",
    });
  }

  SessionMetadata metadata{
      id.value(),    title.value(),      workspace.value(),  provider.value(),
      model.value(), created_at.value(), updated_at.value(), {}};
  if (const auto *parent_id = field(record, "parent_id");
      parent_id != nullptr) {
    if (!parent_id->is_string()) {
      return core::Result<SessionMetadata>::failure(
          {ErrorCode::session_error, "session parent_id must be a string"});
    }
    metadata.parent_id = parent_id->get<std::string>();
  }
  return core::Result<SessionMetadata>::success(std::move(metadata));
}

Json metadata_json(const SessionMetadata &metadata) {
  Json record = {
      {"version", kSessionVersion},
      {"type", "session"},
      {"id", metadata.id},
      {"title", metadata.title},
      {"workspace", metadata.workspace},
      {"provider", metadata.provider},
      {"model", metadata.model},
      {"created_at_unix_ms", metadata.created_at_unix_ms},
      {"updated_at_unix_ms", metadata.updated_at_unix_ms},
  };
  if (!metadata.parent_id.empty())
    record["parent_id"] = metadata.parent_id;
  return record;
}

core::Result<ParsedSession>
parse_session_file(const std::filesystem::path &path,
                   bool allow_trailing_partial_record) {
  std::error_code filesystem_error;
  const bool exists = std::filesystem::exists(path, filesystem_error);
  if (filesystem_error) {
    return core::Result<ParsedSession>::failure(session_error(
        "cannot inspect session", path, filesystem_error.message()));
  }
  if (!exists) {
    return core::Result<ParsedSession>::failure(
        {ErrorCode::not_found,
         "session file does not exist: " + path.string()});
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return core::Result<ParsedSession>::failure(
        session_error("cannot open session", path, "open failed"));
  }

  ParsedSession session;
  bool header_seen = false;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const bool terminated = !input.eof();
    if (line.empty()) {
      return core::Result<ParsedSession>::failure(
          session_error("cannot parse session", path,
                        "empty record at line " + std::to_string(line_number)));
    }

    Json record;
    try {
      record = Json::parse(line);
    } catch (const Json::parse_error &error) {
      if (allow_trailing_partial_record && !terminated) {
        session.trailing_partial_record = true;
        break;
      }
      return core::Result<ParsedSession>::failure(
          session_error("cannot parse session", path,
                        "invalid JSON at line " + std::to_string(line_number) +
                            ": " + error.what()));
    }
    const auto envelope = validate_record_envelope(record);
    if (!envelope) {
      return core::Result<ParsedSession>::failure(
          session_error("cannot parse session", path,
                        "line " + std::to_string(line_number) + ": " +
                            envelope.error().message));
    }
    const auto type = record.at("type").get<std::string>();
    if (!header_seen && type != "session") {
      return core::Result<ParsedSession>::failure(
          session_error("cannot parse session", path,
                        "first record must be a Session v2 header"));
    }

    if (type == "session") {
      if (header_seen || line_number != 1) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "duplicate Session v2 header at line " +
                              std::to_string(line_number)));
      }
      const auto metadata = parse_metadata(record);
      if (!metadata) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "invalid header: " + metadata.error().message));
      }
      session.metadata = metadata.value();
      header_seen = true;
    } else if (type == "session_metadata") {
      const auto title = required_string(record, "title");
      const auto updated_at = required_integer(record, "updated_at_unix_ms");
      if (!title || !updated_at) {
        const auto &error = !title ? title.error() : updated_at.error();
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "invalid metadata record: " + error.message));
      }
      session.metadata.title = title.value();
      session.metadata.updated_at_unix_ms = updated_at.value();
    } else if (type == "turn_start") {
      const auto turn_id = required_string(record, "turn_id");
      const auto started_at = required_integer(record, "started_at_unix_ms");
      if (!turn_id || !started_at) {
        const auto &error = !turn_id ? turn_id.error() : started_at.error();
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "invalid turn_start record: " + error.message));
      }
      if (session.active_turn.has_value()) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "new turn started before the previous turn ended"));
      }
      session.active_turn = ActiveTurn{turn_id.value(), false, false, {}};
      ++session.turn_count;
      session.last_turn_interrupted = false;
    } else if (type == "message") {
      const auto turn_id = required_string(record, "turn_id");
      if (!turn_id)
        return core::Result<ParsedSession>::failure(session_error(
            "cannot parse session", path, turn_id.error().message));
      if (!session.active_turn.has_value() ||
          session.active_turn->id != turn_id.value()) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "message references a turn that is not active"));
      }
      const auto message = parse_message(record);
      if (!message)
        return core::Result<ParsedSession>::failure(session_error(
            "cannot parse session", path, message.error().message));
      const auto applied = apply_message(session, message.value());
      if (!applied)
        return core::Result<ParsedSession>::failure(session_error(
            "cannot parse session", path, applied.error().message));
    } else if (type == "turn_end") {
      const auto turn_id = required_string(record, "turn_id");
      const auto ended_at = required_integer(record, "ended_at_unix_ms");
      const auto outcome = parse_outcome(field(record, "outcome"));
      if (!turn_id || !ended_at || !outcome.has_value()) {
        return core::Result<ParsedSession>::failure(session_error(
            "cannot parse session", path, "invalid turn_end record"));
      }
      if (!session.active_turn.has_value() ||
          session.active_turn->id != turn_id.value()) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "turn_end does not match the active turn"));
      }
      if (!session.active_turn->pending_tool_calls.empty()) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "turn ended with unresolved tool calls"));
      }
      if (*outcome == SessionTurnOutcome::completed &&
          !session.active_turn->has_terminal_assistant_message) {
        return core::Result<ParsedSession>::failure(
            session_error("cannot parse session", path,
                          "completed turn has no terminal assistant response"));
      }
      if (!session.active_turn->has_user_message &&
          *outcome != SessionTurnOutcome::interrupted) {
        return core::Result<ParsedSession>::failure(session_error(
            "cannot parse session", path, "turn ended without a user message"));
      }
      session.last_turn_interrupted =
          *outcome == SessionTurnOutcome::interrupted;
      session.active_turn.reset();
    } else {
      return core::Result<ParsedSession>::failure(session_error(
          "cannot parse session", path, "unsupported record type: " + type));
    }

    if (terminated)
      session.valid_file_bytes += line.size() + 1;
    else
      session.valid_file_bytes += line.size();
  }

  if (!header_seen) {
    return core::Result<ParsedSession>::failure(session_error(
        "cannot parse session", path, "Session v2 header is missing"));
  }
  return core::Result<ParsedSession>::success(std::move(session));
}

core::Result<void> append_records(const std::filesystem::path &path,
                                  const std::vector<Json> &records) {
  std::vector<std::string> serialized;
  serialized.reserve(records.size());
  try {
    for (const auto &record : records)
      serialized.push_back(record.dump());
  } catch (const Json::exception &error) {
    return core::Result<void>::failure(
        session_error("cannot serialize session record", path, error.what()));
  }

  std::error_code filesystem_error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
      return core::Result<void>::failure(session_error(
          "cannot create session directory", path, filesystem_error.message()));
    }
  }
  bool needs_record_separator = false;
  const bool exists = std::filesystem::exists(path, filesystem_error);
  if (filesystem_error) {
    return core::Result<void>::failure(
        session_error("cannot inspect session before append", path,
                      filesystem_error.message()));
  }
  if (exists) {
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
      return core::Result<void>::failure(
          session_error("cannot inspect session before append", path,
                        filesystem_error.message()));
    }
    if (size > 0) {
      std::ifstream tail(path, std::ios::binary);
      tail.seekg(-1, std::ios::end);
      char final_character = '\0';
      tail.get(final_character);
      if (!tail) {
        return core::Result<void>::failure(session_error(
            "cannot inspect session before append", path, "read failed"));
      }
      needs_record_separator = final_character != '\n';
    }
  }
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return core::Result<void>::failure(
        session_error("cannot append session", path, "open failed"));
  }
  if (needs_record_separator)
    output << '\n';
  for (const auto &record : serialized)
    output << record << '\n';
  if (!output) {
    return core::Result<void>::failure(
        session_error("cannot append session", path, "write failed"));
  }
  output.flush();
  if (!output) {
    return core::Result<void>::failure(
        session_error("cannot append session", path, "flush failed"));
  }
  return core::Result<void>::success();
}

Message interrupted_tool_result(std::string_view turn_id,
                                const ToolCall &call) {
  return {
      "recovery-" + std::string(turn_id) + "-" + call.id,
      Role::tool,
      "[session recovery] The turn ended before this tool result was saved. "
      "The tool may or may not have completed; do not repeat it without "
      "inspecting the workspace first.",
      {},
      call.id,
      true,
  };
}

core::Result<void> trim_partial_record(const std::filesystem::path &path,
                                       std::uintmax_t valid_bytes) {
  std::error_code filesystem_error;
  std::filesystem::resize_file(path, valid_bytes, filesystem_error);
  if (filesystem_error) {
    return core::Result<void>::failure(
        session_error("cannot remove incomplete trailing record", path,
                      filesystem_error.message()));
  }
  return core::Result<void>::success();
}

} // namespace

struct JsonlSessionStore::CachedSession {
  ParsedSession parsed;
  std::uintmax_t file_size{};
  std::filesystem::file_time_type write_time{};
};

JsonlSessionStore::JsonlSessionStore(std::filesystem::path path)
    : path_(std::move(path)) {}

JsonlSessionStore::~JsonlSessionStore() { release_write_lock(); }

core::Result<void> JsonlSessionStore::acquire_write_lock() {
  if (lock_fd_.valid())
    return core::Result<void>::success();

  std::error_code filesystem_error;
  if (!path_.parent_path().empty()) {
    std::filesystem::create_directories(path_.parent_path(), filesystem_error);
    if (filesystem_error) {
      return core::Result<void>::failure(
          session_error("cannot create session lock directory", path_,
                        filesystem_error.message()));
    }
  }
  const auto lock_path = path_.string() + ".lock";
  support::UniqueFd descriptor(
      open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
  if (!descriptor.valid()) {
    return core::Result<void>::failure(
        session_error("cannot open session lock", path_, std::strerror(errno)));
  }
  if (flock(descriptor.get(), LOCK_EX | LOCK_NB) != 0) {
    const int lock_error = errno;
    return core::Result<void>::failure({
        lock_error == EWOULDBLOCK ? ErrorCode::conflict
                                  : ErrorCode::session_error,
        "cannot lock session '" + path_.string() + "': " +
            (lock_error == EWOULDBLOCK
                 ? "the Session is already open in another zeda process"
                 : std::string(std::strerror(lock_error))),
    });
  }
  lock_fd_ = std::move(descriptor);
  return core::Result<void>::success();
}

core::Result<void>
JsonlSessionStore::refresh_cache(bool allow_trailing_partial_record) const {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path_, filesystem_error);
  if (filesystem_error) {
    return core::Result<void>::failure(session_error(
        "cannot inspect session", path_, filesystem_error.message()));
  }
  const auto write_time =
      std::filesystem::last_write_time(path_, filesystem_error);
  if (filesystem_error) {
    return core::Result<void>::failure(session_error(
        "cannot inspect session", path_, filesystem_error.message()));
  }
  if (cache_ != nullptr && cache_->file_size == size &&
      cache_->write_time == write_time &&
      (allow_trailing_partial_record ||
       !cache_->parsed.trailing_partial_record)) {
    return core::Result<void>::success();
  }

  const auto parsed = parse_session_file(path_, allow_trailing_partial_record);
  if (!parsed)
    return core::Result<void>::failure(parsed.error());
  cache_ = std::make_unique<CachedSession>(
      CachedSession{parsed.value(), size, write_time});
  return core::Result<void>::success();
}

void JsonlSessionStore::mark_cache_current() const {
  if (cache_ == nullptr)
    return;
  std::error_code filesystem_error;
  cache_->file_size = std::filesystem::file_size(path_, filesystem_error);
  if (filesystem_error) {
    cache_.reset();
    return;
  }
  cache_->write_time =
      std::filesystem::last_write_time(path_, filesystem_error);
  if (filesystem_error) {
    cache_.reset();
    return;
  }
  cache_->parsed.valid_file_bytes = cache_->file_size;
  cache_->parsed.trailing_partial_record = false;
}

void JsonlSessionStore::release_write_lock() {
  if (!lock_fd_.valid())
    return;
  static_cast<void>(flock(lock_fd_.get(), LOCK_UN));
  lock_fd_.reset();
}

core::Result<void> JsonlSessionStore::initialize(SessionMetadata metadata) {
  const auto locked = acquire_write_lock();
  if (!locked)
    return locked;
  std::error_code filesystem_error;
  const bool exists = std::filesystem::exists(path_, filesystem_error);
  if (filesystem_error) {
    return core::Result<void>::failure(session_error(
        "cannot initialize session", path_, filesystem_error.message()));
  }
  if (exists) {
    const auto size = std::filesystem::file_size(path_, filesystem_error);
    if (filesystem_error) {
      return core::Result<void>::failure(session_error(
          "cannot initialize session", path_, filesystem_error.message()));
    }
    if (size != 0) {
      return refresh_cache(true);
    }
  }

  if (metadata.id.empty())
    metadata.id = path_.stem().string();
  if (metadata.title.empty())
    metadata.title = metadata.id;
  if (metadata.created_at_unix_ms == 0)
    metadata.created_at_unix_ms = unix_time_ms();
  if (metadata.updated_at_unix_ms == 0)
    metadata.updated_at_unix_ms = metadata.created_at_unix_ms;
  if (metadata.workspace.empty() || metadata.provider.empty() ||
      metadata.model.empty()) {
    return core::Result<void>::failure({
        ErrorCode::invalid_argument,
        "session metadata requires workspace, provider, and model",
    });
  }
  if (!has_visible_character(metadata.title) || metadata.title.size() > 160) {
    return core::Result<void>::failure({
        ErrorCode::invalid_argument,
        "session title must contain 1 to 160 bytes",
    });
  }
  const auto written = append_records(path_, {metadata_json(metadata)});
  if (!written)
    return written;
  cache_ = std::make_unique<CachedSession>();
  cache_->parsed.metadata = std::move(metadata);
  mark_cache_current();
  return core::Result<void>::success();
}

core::Result<void> JsonlSessionStore::append(const Message &message) {
  if (!lock_fd_.valid()) {
    return core::Result<void>::failure(session_error(
        "cannot append message", path_, "Session is not open for writing"));
  }
  const auto refreshed = refresh_cache(false);
  if (!refreshed)
    return refreshed;
  if (!cache_->parsed.active_turn.has_value()) {
    return core::Result<void>::failure(session_error(
        "cannot append message", path_, "there is no active turn"));
  }
  auto candidate = cache_->parsed;
  const auto turn_id = candidate.active_turn->id;
  const auto applied = apply_message(candidate, message);
  if (!applied)
    return core::Result<void>::failure(
        session_error("cannot append message", path_, applied.error().message));
  const auto written = append_records(path_, {message_json(message, turn_id)});
  if (!written)
    return written;
  cache_->parsed = std::move(candidate);
  mark_cache_current();
  return core::Result<void>::success();
}

core::Result<std::vector<Message>> JsonlSessionStore::load() const {
  const auto refreshed = refresh_cache(false);
  if (!refreshed)
    return core::Result<std::vector<Message>>::failure(refreshed.error());
  return core::Result<std::vector<Message>>::success(cache_->parsed.messages);
}

core::Result<void> JsonlSessionStore::begin_turn(std::string_view turn_id,
                                                 const Message &user_message) {
  if (!lock_fd_.valid()) {
    return core::Result<void>::failure(session_error(
        "cannot begin turn", path_, "Session is not open for writing"));
  }
  if (turn_id.empty()) {
    return core::Result<void>::failure(
        {ErrorCode::invalid_argument, "turn id cannot be empty"});
  }
  if (user_message.role != Role::user) {
    return core::Result<void>::failure(
        {ErrorCode::invalid_argument, "turn must begin with a user message"});
  }
  const auto recovered = recover_interrupted_turn();
  if (!recovered)
    return core::Result<void>::failure(recovered.error());
  const auto refreshed = refresh_cache(false);
  if (!refreshed)
    return refreshed;
  if (cache_->parsed.active_turn.has_value()) {
    return core::Result<void>::failure(session_error(
        "cannot begin turn", path_, "another turn is already active"));
  }
  auto candidate = cache_->parsed;
  candidate.active_turn = ActiveTurn{std::string(turn_id), false, false, {}};
  ++candidate.turn_count;
  candidate.last_turn_interrupted = false;
  const auto applied = apply_message(candidate, user_message);
  if (!applied) {
    return core::Result<void>::failure(
        session_error("cannot begin turn", path_, applied.error().message));
  }

  Json turn_start = {
      {"version", kSessionVersion},
      {"type", "turn_start"},
      {"turn_id", turn_id},
      {"started_at_unix_ms", unix_time_ms()},
  };
  const auto written = append_records(
      path_, {std::move(turn_start), message_json(user_message, turn_id)});
  if (!written)
    return written;
  cache_->parsed = std::move(candidate);
  mark_cache_current();
  return core::Result<void>::success();
}

core::Result<void> JsonlSessionStore::finish_turn(std::string_view turn_id,
                                                  SessionTurnOutcome outcome,
                                                  std::string_view detail) {
  if (!lock_fd_.valid()) {
    return core::Result<void>::failure(session_error(
        "cannot finish turn", path_, "Session is not open for writing"));
  }
  const auto refreshed = refresh_cache(false);
  if (!refreshed)
    return refreshed;
  if (!cache_->parsed.active_turn.has_value() ||
      cache_->parsed.active_turn->id != turn_id) {
    return core::Result<void>::failure(
        session_error("cannot finish turn", path_, "turn is not active"));
  }

  const auto &turn = *cache_->parsed.active_turn;
  if (outcome == SessionTurnOutcome::completed &&
      !turn.pending_tool_calls.empty()) {
    return core::Result<void>::failure(
        session_error("cannot finish turn", path_,
                      "completed turn still has unresolved tool calls"));
  }
  if (outcome == SessionTurnOutcome::completed &&
      !turn.has_terminal_assistant_message) {
    return core::Result<void>::failure(
        session_error("cannot finish turn", path_,
                      "completed turn has no terminal assistant response"));
  }

  auto candidate = cache_->parsed;
  std::vector<Json> records;
  if (outcome != SessionTurnOutcome::completed) {
    records.reserve(turn.pending_tool_calls.size() + 1);
    for (const auto &call : turn.pending_tool_calls) {
      const auto result = interrupted_tool_result(turn.id, call);
      records.push_back(message_json(result, turn.id));
      const auto applied = apply_message(candidate, result);
      if (!applied) {
        return core::Result<void>::failure(session_error(
            "cannot finish turn", path_, applied.error().message));
      }
    }
  }
  std::string bounded_detail(detail.substr(0, kMaxTurnDetailBytes));
  records.push_back({
      {"version", kSessionVersion},
      {"type", "turn_end"},
      {"turn_id", turn.id},
      {"outcome", outcome_name(outcome)},
      {"detail", std::move(bounded_detail)},
      {"ended_at_unix_ms", unix_time_ms()},
  });
  const auto written = append_records(path_, records);
  if (!written)
    return written;
  candidate.last_turn_interrupted = outcome == SessionTurnOutcome::interrupted;
  candidate.active_turn.reset();
  cache_->parsed = std::move(candidate);
  mark_cache_current();
  return core::Result<void>::success();
}

core::Result<void> JsonlSessionStore::set_title(std::string_view title) {
  if (!lock_fd_.valid()) {
    return core::Result<void>::failure(session_error(
        "cannot rename session", path_, "Session is not open for writing"));
  }
  if (!has_visible_character(title) || title.size() > 160) {
    return core::Result<void>::failure({
        ErrorCode::invalid_argument,
        "session title must contain 1 to 160 bytes",
    });
  }
  const auto refreshed = refresh_cache(false);
  if (!refreshed)
    return refreshed;
  const auto updated_at = unix_time_ms();
  const auto written =
      append_records(path_, {{
                                {"version", kSessionVersion},
                                {"type", "session_metadata"},
                                {"title", title},
                                {"updated_at_unix_ms", updated_at},
                            }});
  if (!written)
    return written;
  cache_->parsed.metadata.title = title;
  cache_->parsed.metadata.updated_at_unix_ms = updated_at;
  mark_cache_current();
  return core::Result<void>::success();
}

core::Result<void> JsonlSessionStore::fork_to(const std::filesystem::path &path,
                                              std::string_view title) {
  if (!lock_fd_.valid()) {
    return core::Result<void>::failure(session_error(
        "cannot fork session", path_, "Session is not open for writing"));
  }
  if ((!title.empty() && !has_visible_character(title)) || title.size() > 160) {
    return core::Result<void>::failure({
        ErrorCode::invalid_argument,
        "session title must contain 1 to 160 bytes",
    });
  }
  const auto refreshed = refresh_cache(false);
  if (!refreshed)
    return refreshed;
  if (cache_->parsed.active_turn.has_value()) {
    return core::Result<void>::failure(session_error(
        "cannot fork session", path_, "the active turn has not ended"));
  }

  std::error_code filesystem_error;
  const bool destination_exists =
      std::filesystem::exists(path, filesystem_error);
  if (filesystem_error) {
    return core::Result<void>::failure(session_error(
        "cannot inspect fork destination", path, filesystem_error.message()));
  }
  if (destination_exists) {
    return core::Result<void>::failure(
        {ErrorCode::conflict,
         "session fork destination already exists: " + path.string()});
  }

  auto metadata = cache_->parsed.metadata;
  metadata.parent_id = metadata.id;
  metadata.id = path.stem().string();
  metadata.title =
      title.empty() ? metadata.title + " (fork)" : std::string(title);
  if (metadata.title.size() > 160)
    metadata.title = "Fork of " + metadata.id;
  metadata.created_at_unix_ms = unix_time_ms();
  metadata.updated_at_unix_ms = metadata.created_at_unix_ms;

  std::vector<Json> records{metadata_json(metadata)};
  std::ifstream input(path_, std::ios::binary);
  if (!input) {
    return core::Result<void>::failure(
        session_error("cannot fork session", path_, "open failed"));
  }
  std::string line;
  while (std::getline(input, line)) {
    Json record;
    try {
      record = Json::parse(line);
    } catch (const Json::parse_error &error) {
      return core::Result<void>::failure(
          session_error("cannot fork session", path_, error.what()));
    }
    const auto type = record.at("type").get<std::string>();
    if (type != "session" && type != "session_metadata")
      records.push_back(std::move(record));
  }

  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
      return core::Result<void>::failure(session_error(
          "cannot create fork destination", path, filesystem_error.message()));
    }
  }
  support::UniqueFd reservation(
      open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600));
  if (!reservation.valid()) {
    return core::Result<void>::failure({
        errno == EEXIST ? ErrorCode::conflict : ErrorCode::session_error,
        "cannot reserve session fork destination '" + path.string() +
            "': " + std::strerror(errno),
    });
  }
  reservation.reset();
  const auto written = append_records(path, records);
  if (!written) {
    std::filesystem::remove(path, filesystem_error);
    return written;
  }
  return core::Result<void>::success();
}

core::Result<SessionInspection> JsonlSessionStore::inspect() const {
  const auto refreshed = refresh_cache(true);
  if (!refreshed)
    return core::Result<SessionInspection>::failure(refreshed.error());
  const auto &parsed = cache_->parsed;
  const bool interrupted = parsed.active_turn.has_value() ||
                           parsed.trailing_partial_record ||
                           parsed.last_turn_interrupted;
  const std::size_t unresolved =
      parsed.active_turn.has_value()
          ? parsed.active_turn->pending_tool_calls.size()
          : 0;
  return core::Result<SessionInspection>::success({
      parsed.metadata,
      parsed.messages.size(),
      parsed.turn_count,
      interrupted,
      unresolved,
  });
}

core::Result<SessionRecovery> JsonlSessionStore::recover_interrupted_turn() {
  if (!lock_fd_.valid()) {
    return core::Result<SessionRecovery>::failure(session_error(
        "cannot recover session", path_, "Session is not open for writing"));
  }
  auto refreshed = refresh_cache(true);
  if (!refreshed)
    return core::Result<SessionRecovery>::failure(refreshed.error());
  if (cache_->parsed.trailing_partial_record) {
    const auto trimmed =
        trim_partial_record(path_, cache_->parsed.valid_file_bytes);
    if (!trimmed)
      return core::Result<SessionRecovery>::failure(trimmed.error());
    cache_.reset();
    refreshed = refresh_cache(false);
    if (!refreshed)
      return core::Result<SessionRecovery>::failure(refreshed.error());
  }
  if (!cache_->parsed.active_turn.has_value())
    return core::Result<SessionRecovery>::success({});

  const auto turn = *cache_->parsed.active_turn;
  const auto pending_count = turn.pending_tool_calls.size();
  const auto finished =
      finish_turn(turn.id, SessionTurnOutcome::interrupted,
                  "process stopped before the turn reached a terminal state");
  if (!finished)
    return core::Result<SessionRecovery>::failure(finished.error());
  return core::Result<SessionRecovery>::success({true, turn.id, pending_count});
}

core::Result<SessionRecovery>
JsonlSessionStore::switch_to(std::filesystem::path path) {
  if (path.lexically_normal() == path_.lexically_normal()) {
    const auto recovered = recover_interrupted_turn();
    if (!recovered)
      return core::Result<SessionRecovery>::failure(recovered.error());
    return recovered;
  }
  JsonlSessionStore candidate(path);
  const auto locked = candidate.acquire_write_lock();
  if (!locked)
    return core::Result<SessionRecovery>::failure(locked.error());
  const auto recovered = candidate.recover_interrupted_turn();
  if (!recovered) {
    return core::Result<SessionRecovery>::failure({
        ErrorCode::session_error,
        "cannot open session '" + path.string() +
            "': " + recovered.error().message,
    });
  }
  const auto loaded = candidate.load();
  if (!loaded) {
    return core::Result<SessionRecovery>::failure({
        ErrorCode::session_error,
        "cannot open session '" + path.string() +
            "': " + loaded.error().message,
    });
  }
  release_write_lock();
  path_ = std::move(path);
  lock_fd_ = std::move(candidate.lock_fd_);
  cache_ = std::move(candidate.cache_);
  return recovered;
}

} // namespace zed::session
