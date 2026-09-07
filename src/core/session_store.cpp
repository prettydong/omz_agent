#include "zed/core/session_store.hpp"

#include <random>

namespace zed::core {

std::string new_conversation_id() {
  std::random_device random;
  constexpr char digits[] = "0123456789abcdef";
  std::string id = "zeda-";
  for (int i = 0; i < 4; ++i) {
    const auto value = random();
    for (unsigned shift = 0; shift < 32; shift += 4)
      id.push_back(digits[(value >> shift) & 15U]);
  }
  return id;
}

Result<void> InMemorySessionStore::append(const Message &message) {
  std::scoped_lock lock(mutex_);
  messages_.push_back(message);
  return Result<void>::success();
}

Result<std::vector<Message>> InMemorySessionStore::load() const {
  std::scoped_lock lock(mutex_);
  return Result<std::vector<Message>>::success(messages_);
}

} // namespace zed::core
