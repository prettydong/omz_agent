#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

namespace zed::core {

class CancellationToken {
public:
  using Probe = std::function<bool()>;

  CancellationToken() = default;

  [[nodiscard]] static CancellationToken from_probe(Probe probe) {
    CancellationToken token;
    token.probe_ = std::make_shared<const Probe>(std::move(probe));
    return token;
  }

  [[nodiscard]] bool is_cancelled() const {
    if (state_ != nullptr && state_->load(std::memory_order_relaxed))
      return true;
    if (probe_ == nullptr)
      return false;
    try {
      return (*probe_)();
    } catch (...) {
      return true;
    }
  }

private:
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
      : state_(std::move(state)) {}

  std::shared_ptr<std::atomic_bool> state_;
  std::shared_ptr<const Probe> probe_;

  friend class CancellationSource;
};

class CancellationSource {
public:
  CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

  [[nodiscard]] CancellationToken token() const {
    return CancellationToken(state_);
  }

  void cancel() { state_->store(true, std::memory_order_relaxed); }

private:
  std::shared_ptr<std::atomic_bool> state_;
};

} // namespace zed::core
