#include "zed/core/cancellation.hpp"
#include "zed/support/atomic_file.hpp"
#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <cassert>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace {

pid_t start_waiting_child(bool ignore_sigterm) {
  auto spawn_lock = zed::support::lock_process_spawn();
  int ready_pipe[2];
  assert(zed::support::create_cloexec_pipe(ready_pipe));
  const pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    close(ready_pipe[0]);
    setpgid(0, 0);
    if (ignore_sigterm)
      std::signal(SIGTERM, SIG_IGN);
    const char ready = '1';
    static_cast<void>(write(ready_pipe[1], &ready, 1));
    close(ready_pipe[1]);
    while (true)
      pause();
  }
  spawn_lock.unlock();
  close(ready_pipe[1]);
  setpgid(child, child);
  char ready = 0;
  assert(read(ready_pipe[0], &ready, 1) == 1);
  close(ready_pipe[0]);
  return child;
}

} // namespace

int main() {
  int descriptors[2] = {-1, -1};
  assert(zed::support::create_cloexec_pipe(descriptors));
  zed::support::UniqueFd read_end(descriptors[0]);
  zed::support::UniqueFd write_end(descriptors[1]);
  assert(read_end.valid());
  assert(write_end.valid());
  const int read_flags = fcntl(read_end.get(), F_GETFD);
  const int write_flags = fcntl(write_end.get(), F_GETFD);
  assert(read_flags >= 0);
  assert(write_flags >= 0);
  assert((read_flags & FD_CLOEXEC) != 0);
  assert((write_flags & FD_CLOEXEC) != 0);

  zed::support::UniqueFd moved = std::move(read_end);
  assert(moved.valid());
  assert(!read_end.valid());
  moved.reset();
  write_end.reset();
  assert(!moved.valid());

  int status = 0;
  bool reaped = false;
  const pid_t cooperative = start_waiting_child(false);
  zed::support::terminate_process_group(
      cooperative, std::chrono::milliseconds(100), reaped, status);
  assert(reaped);
  assert(WIFSIGNALED(status));
  assert(WTERMSIG(status) == SIGTERM);

  status = 0;
  reaped = false;
  const pid_t stubborn = start_waiting_child(true);
  zed::support::terminate_process_group(stubborn, std::chrono::milliseconds(30),
                                        reaped, status);
  assert(reaped);
  assert(WIFSIGNALED(status));
  assert(WTERMSIG(status) == SIGKILL);

  const auto workspace = std::filesystem::temp_directory_path() /
                         ("zeda-support-smoke-" + std::to_string(getpid()));
  std::filesystem::remove_all(workspace);
  std::filesystem::create_directories(workspace);
  const auto target = workspace / "file.txt";
  const auto outside = workspace / "outside.txt";
  {
    std::ofstream output(outside);
    output << "secret";
  }
  const auto link = workspace / "file-link.txt";
  std::filesystem::create_symlink(outside, link);
  assert(!zed::support::write_file_atomically(link, "replaced",
                                              "workspace file", true));
  {
    std::ifstream saved(outside);
    assert(std::string(std::istreambuf_iterator<char>(saved), {}) == "secret");
  }

  const auto planted_temporary = std::filesystem::path(
      target.string() + ".tmp." + std::to_string(getpid()) + ".0");
  std::filesystem::create_symlink(outside, planted_temporary);
  assert(!zed::support::write_file_atomically(target, "first", "workspace file",
                                              false));
  {
    std::ifstream saved(outside);
    assert(std::string(std::istreambuf_iterator<char>(saved), {}) == "secret");
  }
  std::filesystem::remove(planted_temporary);

  assert(zed::support::write_file_atomically(target, "first", "workspace file",
                                             false));
  assert(!zed::support::write_file_atomically(target, "second",
                                              "workspace file", false));
  {
    std::ifstream saved(target);
    assert(std::string(std::istreambuf_iterator<char>(saved), {}) == "first");
  }
  assert(zed::support::write_file_atomically(target, "second", "workspace file",
                                             true));
  {
    std::ifstream saved(target);
    assert(std::string(std::istreambuf_iterator<char>(saved), {}) == "second");
  }

  zed::core::CancellationSource cancelled;
  cancelled.cancel();
  assert(!zed::support::write_file_atomically(target, "third", "workspace file",
                                              true, cancelled.token()));
  {
    std::ifstream saved(target);
    assert(std::string(std::istreambuf_iterator<char>(saved), {}) == "second");
  }

  std::filesystem::remove_all(workspace);
  return 0;
}
