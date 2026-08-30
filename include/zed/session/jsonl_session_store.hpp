#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "zed/core/session_store.hpp"
#include "zed/support/unique_fd.hpp"

namespace zed::session {

struct SessionMetadata {
  std::string id;
  std::string title;
  std::string workspace;
  std::string provider;
  std::string model;
  std::int64_t created_at_unix_ms{};
  std::int64_t updated_at_unix_ms{};
  std::string parent_id;
};

struct SessionInspection {
  SessionMetadata metadata;
  std::size_t message_count{};
  std::size_t turn_count{};
  bool has_interrupted_turn{false};
  std::size_t unresolved_tool_calls{};
};

struct SessionRecovery {
  bool recovered{false};
  core::TurnId turn_id;
  std::size_t synthesized_tool_results{};
};

class JsonlSessionStore final : public core::SessionStore {
public:
  explicit JsonlSessionStore(std::filesystem::path path);
  ~JsonlSessionStore() override;

  JsonlSessionStore(const JsonlSessionStore &) = delete;
  JsonlSessionStore &operator=(const JsonlSessionStore &) = delete;

  core::Result<void> append(const core::Message &message) override;
  core::Result<std::vector<core::Message>> load() const override;
  core::Result<void> begin_turn(std::string_view turn_id,
                                const core::Message &user_message) override;
  core::Result<void> finish_turn(std::string_view turn_id,
                                 core::SessionTurnOutcome outcome,
                                 std::string_view detail = {}) override;

  core::Result<void> initialize(SessionMetadata metadata);
  core::Result<void> set_title(std::string_view title);
  core::Result<void> fork_to(const std::filesystem::path &path,
                             std::string_view title = {});
  core::Result<SessionInspection> inspect() const;
  core::Result<SessionRecovery> recover_interrupted_turn();
  core::Result<SessionRecovery> switch_to(std::filesystem::path path);

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  struct CachedSession;

  core::Result<void> acquire_write_lock();
  core::Result<void> refresh_cache(bool allow_trailing_partial_record) const;
  void mark_cache_current() const;
  void release_write_lock();

  std::filesystem::path path_;
  support::UniqueFd lock_fd_;
  mutable std::unique_ptr<CachedSession> cache_;
};

} // namespace zed::session
