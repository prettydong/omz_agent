#include "zed/core/session_store.hpp"

namespace zed::core {

Result<void> InMemorySessionStore::append(const Message& message) {
    std::scoped_lock lock(mutex_);
    messages_.push_back(message);
    return Result<void>::success();
}

Result<std::vector<Message>> InMemorySessionStore::load() const {
    std::scoped_lock lock(mutex_);
    return Result<std::vector<Message>>::success(messages_);
}

}  // namespace zed::core
