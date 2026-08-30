#pragma once

#include <dlfcn.h>

#include <utility>

namespace zed::support {

class UniqueLibrary {
public:
  UniqueLibrary() = default;
  explicit UniqueLibrary(void *handle) : handle_(handle) {}
  ~UniqueLibrary() { reset(); }

  UniqueLibrary(const UniqueLibrary &) = delete;
  UniqueLibrary &operator=(const UniqueLibrary &) = delete;

  UniqueLibrary(UniqueLibrary &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}

  UniqueLibrary &operator=(UniqueLibrary &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] void *get() const { return handle_; }
  [[nodiscard]] explicit operator bool() const { return handle_ != nullptr; }

  void reset(void *handle = nullptr) {
    if (handle_ != nullptr)
      static_cast<void>(dlclose(handle_));
    handle_ = handle;
  }

private:
  void *handle_{nullptr};
};

} // namespace zed::support
