#pragma once

#include <mutex>
#include <string_view>
#include <vector>

#include "zed/core/message.hpp"
#include "zed/core/result.hpp"

namespace zed::core {

enum class SessionTurnOutcome {
  completed,
  failed,
  cancelled,
  interrupted,
};

class SessionStore {
public:
  virtual ~SessionStore() = default;

  virtual Result<void> append(const Message &message) = 0;
  virtual Result<std::vector<Message>> load() const = 0;

  virtual Result<void> begin_turn(std::string_view turn_id,
                                  const Message &user_message) {
    static_cast<void>(turn_id);
    return append(user_message);
  }

  virtual Result<void> finish_turn(std::string_view turn_id,
                                   SessionTurnOutcome outcome,
                                   std::string_view detail = {}) {
    static_cast<void>(turn_id);
    static_cast<void>(outcome);
    static_cast<void>(detail);
    return Result<void>::success();
  }
};

class InMemorySessionStore final : public SessionStore {
public:
  Result<void> append(const Message &message) override;
  Result<std::vector<Message>> load() const override;

private:
  mutable std::mutex mutex_;
  std::vector<Message> messages_;
};

} // namespace zed::core
