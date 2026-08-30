#include "zed/support/atomic_file.hpp"

#include "zed/support/unique_fd.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace zed::support {

namespace {

core::Result<void> failure(core::ErrorCode code, std::string message) {
  return core::Result<void>::failure({code, std::move(message)});
}

class TemporaryFile {
public:
  explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path)) {}

  TemporaryFile(const TemporaryFile &) = delete;
  TemporaryFile &operator=(const TemporaryFile &) = delete;

  ~TemporaryFile() {
    if (path_.empty())
      return;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }
  void release() { path_.clear(); }

private:
  std::filesystem::path path_;
};

struct DestinationStatus {
  bool exists{false};
  mode_t mode{0600};
};

core::Result<DestinationStatus>
inspect_destination(const std::filesystem::path &path,
                    std::string_view description, bool private_permissions) {
  struct stat status{};
  if (lstat(path.c_str(), &status) == 0) {
    if (!S_ISREG(status.st_mode)) {
      return core::Result<DestinationStatus>::failure({
          core::ErrorCode::invalid_argument,
          std::string(description) +
              " path must be a regular file, not a symlink: " + path.string(),
      });
    }
    return core::Result<DestinationStatus>::success(
        {true, private_permissions
                   ? static_cast<mode_t>(0600)
                   : static_cast<mode_t>(status.st_mode & 0777)});
  }
  if (errno != ENOENT) {
    return core::Result<DestinationStatus>::failure({
        core::ErrorCode::invalid_argument,
        "cannot inspect " + std::string(description) + " " + path.string() +
            ": " + std::string(std::strerror(errno)),
    });
  }
  return core::Result<DestinationStatus>::success(
      {false, private_permissions ? static_cast<mode_t>(0600)
                                  : static_cast<mode_t>(0666)});
}

core::Result<void> write_atomically(const std::filesystem::path &path,
                                    std::string_view content,
                                    std::string_view description,
                                    bool replace_existing,
                                    bool private_permissions,
                                    core::CancellationToken cancellation) {
  if (cancellation.is_cancelled()) {
    return failure(core::ErrorCode::cancelled,
                   std::string(description) + " write cancelled");
  }

  const auto directory = path.parent_path();
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    return failure(core::ErrorCode::invalid_argument,
                   "cannot create " + std::string(description) + " directory " +
                       directory.string() + ": " + filesystem_error.message());
  }

  const auto directory_status =
      std::filesystem::symlink_status(directory, filesystem_error);
  if (filesystem_error || std::filesystem::is_symlink(directory_status) ||
      !std::filesystem::is_directory(directory_status)) {
    return failure(core::ErrorCode::invalid_argument,
                   std::string(description) +
                       " directory must be a regular directory, not a "
                       "symlink: " +
                       directory.string());
  }

  const auto destination =
      inspect_destination(path, description, private_permissions);
  if (!destination)
    return core::Result<void>::failure(destination.error());
  if (!replace_existing && destination.value().exists) {
    return failure(core::ErrorCode::conflict,
                   std::string(description) +
                       " already exists: " + path.string());
  }

  static std::atomic<unsigned long> sequence{0};
  TemporaryFile temporary(path.string() + ".tmp." + std::to_string(getpid()) +
                          "." + std::to_string(sequence.fetch_add(1)));
  UniqueFd descriptor(open(temporary.path().c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                           destination.value().mode));
  if (!descriptor.valid()) {
    return failure(core::ErrorCode::internal,
                   "cannot create temporary " + std::string(description) +
                       ": " + std::string(std::strerror(errno)));
  }
  if ((private_permissions || destination.value().exists) &&
      fchmod(descriptor.get(), destination.value().mode) != 0) {
    return failure(core::ErrorCode::internal,
                   "cannot set temporary " + std::string(description) +
                       " permissions: " + std::string(std::strerror(errno)));
  }

  std::size_t offset = 0;
  while (offset < content.size()) {
    if (cancellation.is_cancelled()) {
      return failure(core::ErrorCode::cancelled,
                     std::string(description) + " write cancelled");
    }
    const auto written = write(descriptor.get(), content.data() + offset,
                               content.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    return failure(core::ErrorCode::internal,
                   "cannot write temporary " + std::string(description) + ": " +
                       std::string(std::strerror(errno)));
  }

  if (fsync(descriptor.get()) != 0) {
    return failure(core::ErrorCode::internal,
                   "cannot flush temporary " + std::string(description) + ": " +
                       std::string(std::strerror(errno)));
  }
  const int raw_descriptor = descriptor.release();
  if (close(raw_descriptor) != 0) {
    return failure(core::ErrorCode::internal,
                   "cannot close temporary " + std::string(description) + ": " +
                       std::string(std::strerror(errno)));
  }
  if (cancellation.is_cancelled()) {
    return failure(core::ErrorCode::cancelled,
                   std::string(description) + " write cancelled");
  }

  if (replace_existing) {
    if (rename(temporary.path().c_str(), path.c_str()) != 0) {
      return failure(core::ErrorCode::internal,
                     "cannot replace " + std::string(description) + ": " +
                         std::string(std::strerror(errno)));
    }
  } else {
    if (link(temporary.path().c_str(), path.c_str()) != 0) {
      const auto code = errno == EEXIST ? core::ErrorCode::conflict
                                        : core::ErrorCode::internal;
      return failure(code, "cannot install " + std::string(description) + ": " +
                               std::string(std::strerror(errno)));
    }
    static_cast<void>(unlink(temporary.path().c_str()));
  }
  temporary.release();

  UniqueFd directory_descriptor(open(directory.c_str(), O_RDONLY | O_CLOEXEC));
  if (directory_descriptor.valid())
    static_cast<void>(fsync(directory_descriptor.get()));
  return core::Result<void>::success();
}

} // namespace

core::Result<void> write_file_atomically(const std::filesystem::path &path,
                                         std::string_view content,
                                         std::string_view description,
                                         bool replace_existing,
                                         core::CancellationToken cancellation) {
  return write_atomically(path, content, description, replace_existing, false,
                          cancellation);
}

core::Result<void>
write_private_file_atomically(const std::filesystem::path &path,
                              std::string_view content,
                              std::string_view description) {
  return write_atomically(path, content, description, true, true, {});
}

} // namespace zed::support
