#pragma once

#include <atomic>
#include <memory>

namespace zed::core {

class CancellationToken {
public:
    CancellationToken() = default;

    [[nodiscard]] bool is_cancelled() const {
        return state_ != nullptr && state_->load(std::memory_order_relaxed);
    }

private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
        : state_(std::move(state)) {}

    std::shared_ptr<std::atomic_bool> state_;

    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource()
        : state_(std::make_shared<std::atomic_bool>(false)) {}

    [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }

    void cancel() { state_->store(true, std::memory_order_relaxed); }

private:
    std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace zed::core
