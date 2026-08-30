#pragma once

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <vector>

namespace zed::support {

// Serializes pipe creation through spawn on platforms where CLOEXEC cannot be
// applied atomically. Every spawn site must participate so another child cannot
// inherit a pipe during the pipe/fcntl window.
[[nodiscard]] std::unique_lock<std::mutex> lock_process_spawn();

// Creates a pipe whose descriptors are closed by exec. Call while holding the
// process-spawn lock when a spawn will follow.
bool create_cloexec_pipe(int descriptors[2]);

struct SpawnFileAction {
  int source{-1};
  int target{-1};
};

struct SpawnOptions {
  std::string executable;
  std::vector<std::string> arguments;
  std::filesystem::path working_directory;
  std::vector<SpawnFileAction> duplicate_descriptors;
  std::vector<int> close_descriptors;
  std::vector<std::string> additional_environment_variables;
};

// Starts a child with a new process group and a minimal environment. Call while
// holding the process-spawn lock if pipes were created under that lock. Only
// the named additional variables are copied from the parent environment.
// Returns zero on success or a POSIX error number on failure.
[[nodiscard]] int spawn_process(const SpawnOptions &options, pid_t &child);

// Signals the child's process group and falls back to the child itself when
// process-group setup raced with startup.
void signal_process_group(pid_t child, int signal_number);

// Returns true once the child has been reaped or is no longer waitable.
bool try_reap_child(pid_t child, int &status);

// Sends SIGTERM to the process group, waits up to grace, then sends SIGKILL.
// Descendants are signalled even when the process-group leader was already
// reaped. The leader is reaped before this function returns when necessary.
void terminate_process_group(pid_t child, std::chrono::milliseconds grace,
                             bool &reaped, int &status);

} // namespace zed::support
