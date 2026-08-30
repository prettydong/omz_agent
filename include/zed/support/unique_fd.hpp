#pragma once

#include <unistd.h>

#include <utility>

namespace zed::support {

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int descriptor) : descriptor_(descriptor) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset();
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const { return descriptor_; }
  [[nodiscard]] bool valid() const { return descriptor_ >= 0; }
  [[nodiscard]] int release() { return std::exchange(descriptor_, -1); }

  void reset(int descriptor = -1) {
    if (descriptor_ >= 0)
      static_cast<void>(close(descriptor_));
    descriptor_ = descriptor;
  }

private:
  int descriptor_{-1};
};

} // namespace zed::support
