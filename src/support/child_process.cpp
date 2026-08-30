#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "zed/support/child_process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace zed::support {

namespace {

std::mutex process_spawn_mutex;

constexpr std::array<std::string_view, 20> kSafeEnvironmentVariables{
    "PATH",
    "HOME",
    "USER",
    "LOGNAME",
    "SHELL",
    "TMPDIR",
    "LANG",
    "LC_ALL",
    "LC_CTYPE",
    "TERM",
    "COLORTERM",
    "NO_COLOR",
    "XDG_CONFIG_HOME",
    "XDG_CACHE_HOME",
    "XDG_DATA_HOME",
    "DEVELOPER_DIR",
    "SDKROOT",
    "CPATH",
    "CPLUS_INCLUDE_PATH",
    "MACOSX_DEPLOYMENT_TARGET",
};

class SpawnActions {
public:
  SpawnActions() : error_(posix_spawn_file_actions_init(&actions_)) {}
  ~SpawnActions() {
    if (error_ == 0)
      static_cast<void>(posix_spawn_file_actions_destroy(&actions_));
  }

  SpawnActions(const SpawnActions &) = delete;
  SpawnActions &operator=(const SpawnActions &) = delete;

  [[nodiscard]] int error() const { return error_; }
  [[nodiscard]] posix_spawn_file_actions_t *get() { return &actions_; }

private:
  posix_spawn_file_actions_t actions_{};
  int error_{};
};

class SpawnAttributes {
public:
  SpawnAttributes() : error_(posix_spawnattr_init(&attributes_)) {}
  ~SpawnAttributes() {
    if (error_ == 0)
      static_cast<void>(posix_spawnattr_destroy(&attributes_));
  }

  SpawnAttributes(const SpawnAttributes &) = delete;
  SpawnAttributes &operator=(const SpawnAttributes &) = delete;

  [[nodiscard]] int error() const { return error_; }
  [[nodiscard]] posix_spawnattr_t *get() { return &attributes_; }

private:
  posix_spawnattr_t attributes_{};
  int error_{};
};

void add_environment_variable(std::vector<std::string> &environment,
                              std::string_view name) {
  if (name.empty() || name.find('=') != std::string_view::npos)
    return;
  const auto duplicate = std::any_of(
      environment.begin(), environment.end(), [&](const std::string &entry) {
        return entry.starts_with(std::string(name) + '=');
      });
  if (duplicate)
    return;
  const std::string owned_name(name);
  const char *value = std::getenv(owned_name.c_str());
  if (value != nullptr)
    environment.push_back(owned_name + '=' + value);
}

bool process_group_exists(pid_t child) {
  if (child <= 0)
    return false;
  if (kill(-child, 0) == 0)
    return true;
  return errno == EPERM;
}

void signal_process_group_for_termination(pid_t child, int signal_number,
                                          bool leader_reaped) {
  if (child <= 0)
    return;
  if (kill(-child, signal_number) != 0 && !leader_reaped)
    static_cast<void>(kill(child, signal_number));
}

} // namespace

std::unique_lock<std::mutex> lock_process_spawn() {
  return std::unique_lock(process_spawn_mutex);
}

bool create_cloexec_pipe(int descriptors[2]) {
  if (pipe(descriptors) != 0)
    return false;
  if (fcntl(descriptors[0], F_SETFD, FD_CLOEXEC) == 0 &&
      fcntl(descriptors[1], F_SETFD, FD_CLOEXEC) == 0) {
    return true;
  }
  const int saved_errno = errno;
  static_cast<void>(close(descriptors[0]));
  static_cast<void>(close(descriptors[1]));
  descriptors[0] = -1;
  descriptors[1] = -1;
  errno = saved_errno;
  return false;
}

int spawn_process(const SpawnOptions &options, pid_t &child) {
  child = -1;
  if (options.executable.empty())
    return EINVAL;

  SpawnActions actions;
  if (actions.error() != 0)
    return actions.error();
  for (const auto &action : options.duplicate_descriptors) {
    if (action.source < 0 || action.target < 0)
      return EINVAL;
    const int error = posix_spawn_file_actions_adddup2(
        actions.get(), action.source, action.target);
    if (error != 0)
      return error;
  }
  for (const int descriptor : options.close_descriptors) {
    if (descriptor < 0)
      continue;
    const int error =
        posix_spawn_file_actions_addclose(actions.get(), descriptor);
    if (error != 0)
      return error;
  }
  if (!options.working_directory.empty()) {
#if defined(__APPLE__) || defined(__linux__)
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    const int error = posix_spawn_file_actions_addchdir_np(
        actions.get(), options.working_directory.c_str());
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif
    if (error != 0)
      return error;
#else
    return ENOTSUP;
#endif
  }

  SpawnAttributes attributes;
  if (attributes.error() != 0)
    return attributes.error();
  sigset_t default_signals;
  sigemptyset(&default_signals);
  sigaddset(&default_signals, SIGINT);
  sigaddset(&default_signals, SIGTERM);
  sigaddset(&default_signals, SIGPIPE);
  int error = posix_spawnattr_setsigdefault(attributes.get(), &default_signals);
  if (error != 0)
    return error;
  error = posix_spawnattr_setpgroup(attributes.get(), 0);
  if (error != 0)
    return error;
  const auto flags =
      static_cast<short>(POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
  error = posix_spawnattr_setflags(attributes.get(), flags);
  if (error != 0)
    return error;

  std::vector<std::string> argument_storage;
  argument_storage.reserve(options.arguments.size() + 1);
  argument_storage.push_back(options.executable);
  argument_storage.insert(argument_storage.end(), options.arguments.begin(),
                          options.arguments.end());
  std::vector<char *> arguments;
  arguments.reserve(argument_storage.size() + 1);
  for (auto &argument : argument_storage)
    arguments.push_back(argument.data());
  arguments.push_back(nullptr);

  std::vector<std::string> environment_storage;
  environment_storage.reserve(kSafeEnvironmentVariables.size() +
                              options.additional_environment_variables.size());
  for (const auto name : kSafeEnvironmentVariables)
    add_environment_variable(environment_storage, name);
  for (const auto &name : options.additional_environment_variables)
    add_environment_variable(environment_storage, name);
  std::vector<char *> environment;
  environment.reserve(environment_storage.size() + 1);
  for (auto &entry : environment_storage)
    environment.push_back(entry.data());
  environment.push_back(nullptr);

  return posix_spawnp(&child, options.executable.c_str(), actions.get(),
                      attributes.get(), arguments.data(), environment.data());
}

void signal_process_group(pid_t child, int signal_number) {
  if (child <= 0)
    return;
  if (kill(-child, signal_number) != 0)
    static_cast<void>(kill(child, signal_number));
}

bool try_reap_child(pid_t child, int &status) {
  while (true) {
    const auto waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      return true;
    if (waited == 0)
      return false;
    if (waited < 0 && errno == EINTR)
      continue;
    return waited < 0 && errno == ECHILD;
  }
}

void terminate_process_group(pid_t child, std::chrono::milliseconds grace,
                             bool &reaped, int &status) {
  if (child <= 0)
    return;
  signal_process_group_for_termination(child, SIGTERM, reaped);
  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!reaped && try_reap_child(child, status))
      reaped = true;
    if (reaped && !process_group_exists(child))
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!reaped && try_reap_child(child, status))
    reaped = true;
  if (process_group_exists(child))
    signal_process_group_for_termination(child, SIGKILL, reaped);
  if (!reaped) {
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    reaped = true;
  }
}

} // namespace zed::support
