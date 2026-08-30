#pragma once

#include <chrono>
#include <mutex>
#include <sys/types.h>

namespace zed::support {

// Serializes pipe creation through fork on platforms where CLOEXEC cannot be
// applied atomically. Every fork site must participate so another child cannot
// inherit a pipe during the pipe/fcntl window.
[[nodiscard]] std::unique_lock<std::mutex> lock_process_spawn();

// Creates a pipe whose descriptors are closed by exec. Call while holding the
// process-spawn lock when a fork will follow.
bool create_cloexec_pipe(int descriptors[2]);

// Removes credential environment variables from the current process. Call in
// the child after fork and before exec.
void clear_sensitive_environment();

// Signals the child's process group and falls back to the child itself when
// process-group setup raced with startup.
void signal_process_group(pid_t child, int signal_number);

// Returns true once the child has been reaped or is no longer waitable.
bool try_reap_child(pid_t child, int &status);

// Sends SIGTERM, waits up to grace, then sends SIGKILL and reaps the child.
void terminate_process_group(pid_t child, std::chrono::milliseconds grace,
                             bool &reaped, int &status);

} // namespace zed::support
