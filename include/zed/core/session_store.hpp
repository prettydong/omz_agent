#pragma once

#include <mutex>
#include <vector>

#include "zed/core/message.hpp"
#include "zed/core/result.hpp"

namespace zed::core {

class SessionStore {
public:
    virtual ~SessionStore() = default;

    virtual Result<void> append(const Message& message) = 0;
    virtual Result<std::vector<Message>> load() const = 0;
};

class InMemorySessionStore final : public SessionStore {
public:
    Result<void> append(const Message& message) override;
    Result<std::vector<Message>> load() const override;

private:
    mutable std::mutex mutex_;
    std::vector<Message> messages_;
};

}  // namespace zed::core
