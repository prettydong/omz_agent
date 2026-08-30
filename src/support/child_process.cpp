#include "zed/support/child_process.hpp"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace zed::support {

namespace {

std::mutex process_spawn_mutex;

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

void clear_sensitive_environment() {
  static constexpr std::array<const char *, 9> kSensitiveVariables{
      "OPENAI_API_KEY",       "OPENCODE_GO_API_KEY",   "ANTHROPIC_API_KEY",
      "AZURE_OPENAI_API_KEY", "GOOGLE_API_KEY",        "GITHUB_TOKEN",
      "AWS_ACCESS_KEY_ID",    "AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN",
  };
  for (const auto *name : kSensitiveVariables)
    static_cast<void>(unsetenv(name));
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
  if (reaped || child <= 0)
    return;
  signal_process_group(child, SIGTERM);
  const auto deadline = std::chrono::steady_clock::now() + grace;
  while (std::chrono::steady_clock::now() < deadline) {
    if (try_reap_child(child, status)) {
      reaped = true;
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (try_reap_child(child, status)) {
    reaped = true;
    return;
  }
  signal_process_group(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  reaped = true;
}

} // namespace zed::support
