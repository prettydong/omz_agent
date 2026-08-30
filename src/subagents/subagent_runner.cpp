#include "zed/subagents/subagent_runner.hpp"

#include "zed/subagents/worker_protocol.hpp"
#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace zed::subagents {

namespace {

using core::ErrorCode;
using support::UniqueFd;

void close_pair(int (&descriptors)[2]) {
  for (int &descriptor : descriptors) {
    if (descriptor >= 0) {
      close(descriptor);
      descriptor = -1;
    }
  }
}

void set_nonblocking(int descriptor) {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  if (flags >= 0)
    static_cast<void>(fcntl(descriptor, F_SETFL, flags | O_NONBLOCK));
}

bool write_all(int descriptor, std::string_view content) {
  std::size_t written = 0;
  while (written < content.size()) {
    const auto count =
        write(descriptor, content.data() + written, content.size() - written);
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::string safe_diagnostics(std::string_view diagnostics, bool truncated) {
  std::string result;
  std::size_t offset = 0;
  while (offset < diagnostics.size()) {
    const auto end = diagnostics.find('\n', offset);
    const auto line = diagnostics.substr(
        offset, end == std::string_view::npos ? diagnostics.size() - offset
                                              : end - offset);
    const auto normalized = lowercase_ascii(std::string(line));
    const bool sensitive =
        normalized.find("authorization") != std::string::npos ||
        normalized.find("api_key") != std::string::npos ||
        normalized.find("api key") != std::string::npos ||
        normalized.find("token") != std::string::npos ||
        normalized.find("credential") != std::string::npos;
    if (!result.empty())
      result += '\n';
    result += sensitive ? "[redacted sensitive diagnostic]" : std::string(line);
    if (end == std::string_view::npos)
      break;
    offset = end + 1;
  }
  if (truncated)
    result += "\n[worker stderr truncated]";
  return result;
}

struct SpawnedWorker {
  pid_t pid{-1};
  UniqueFd input;
  UniqueFd output;
  UniqueFd error;
};

core::Result<SpawnedWorker>
spawn_worker(const ProcessSubagentRunnerConfig &config) {
  if (config.executable.empty()) {
    return core::Result<SpawnedWorker>::failure(
        {ErrorCode::invalid_argument, "subagent worker executable is empty"});
  }
  int input_pipe[2]{-1, -1};
  int output_pipe[2]{-1, -1};
  int error_pipe[2]{-1, -1};
  auto spawn_lock = support::lock_process_spawn();
  if (!support::create_cloexec_pipe(input_pipe) ||
      !support::create_cloexec_pipe(output_pipe) ||
      !support::create_cloexec_pipe(error_pipe)) {
    close_pair(input_pipe);
    close_pair(output_pipe);
    close_pair(error_pipe);
    return core::Result<SpawnedWorker>::failure(
        {ErrorCode::tool_error, "cannot create subagent worker pipes: " +
                                    std::string(std::strerror(errno))});
  }

  support::SpawnOptions spawn_options;
  spawn_options.executable = config.executable;
  spawn_options.arguments = {"--subagent-worker"};
  spawn_options.working_directory = config.workspace_root;
  spawn_options.duplicate_descriptors = {
      {input_pipe[0], STDIN_FILENO},
      {output_pipe[1], STDOUT_FILENO},
      {error_pipe[1], STDERR_FILENO},
  };
  spawn_options.close_descriptors = {
      input_pipe[0],  input_pipe[1], output_pipe[0],
      output_pipe[1], error_pipe[0], error_pipe[1],
  };
  // The worker is a trusted zeda process and needs only provider/configuration
  // inputs. Arbitrary host credentials are intentionally not inherited.
  spawn_options.additional_environment_variables = {
      "OPENCODE_GO_API_KEY",
      "ZED_OPENCODE_AUTH_PATH",
      "ZED_OPENCODE_ENDPOINT",
      "ZED_CLANGD_PATH",
      "ZED_MAX_CONTEXT_TOKENS",
      "ZED_RESERVED_OUTPUT_TOKENS",
      "ZED_CONTEXT_TRIGGER_TOKENS",
      "ZED_REQUEST_TIMEOUT_MS",
      "ZED_WORKSPACE",
  };
  pid_t child = -1;
  const int spawn_error = support::spawn_process(spawn_options, child);
  if (spawn_error != 0) {
    close_pair(input_pipe);
    close_pair(output_pipe);
    close_pair(error_pipe);
    return core::Result<SpawnedWorker>::failure(
        {ErrorCode::tool_error, "cannot start subagent worker: " +
                                    std::string(std::strerror(spawn_error))});
  }

  spawn_lock.unlock();
  close(input_pipe[0]);
  close(output_pipe[1]);
  close(error_pipe[1]);
  set_nonblocking(output_pipe[0]);
  set_nonblocking(error_pipe[0]);
  return core::Result<SpawnedWorker>::success({child, UniqueFd(input_pipe[1]),
                                               UniqueFd(output_pipe[0]),
                                               UniqueFd(error_pipe[0])});
}

std::string exit_description(int status) {
  if (WIFEXITED(status))
    return "exit status " + std::to_string(WEXITSTATUS(status));
  if (WIFSIGNALED(status))
    return "signal " + std::to_string(WTERMSIG(status));
  return "unknown process status";
}

} // namespace

ProcessSubagentRunner::ProcessSubagentRunner(ProcessSubagentRunnerConfig config)
    : config_(std::move(config)) {}

core::Result<SubagentRunResult>
ProcessSubagentRunner::run(const SubagentTask &task,
                           core::CancellationToken cancellation,
                           std::chrono::milliseconds timeout,
                           const SubagentProgressCallback &on_progress) {
  if (task.agent.empty() || task.task.empty()) {
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::invalid_argument,
         "subagent runner requires a non-empty agent and task"});
  }
  if (task.task.size() > kMaximumTaskBytes) {
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::invalid_argument, "subagent task exceeds 32 KiB"});
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::timeout, "subagent worker timed out before starting"});
  }
  if (cancellation.is_cancelled()) {
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::cancelled, "subagent worker cancelled before starting"});
  }

  static std::once_flag ignore_sigpipe_once;
  std::call_once(ignore_sigpipe_once,
                 [] { static_cast<void>(std::signal(SIGPIPE, SIG_IGN)); });
  auto spawned = spawn_worker(config_);
  if (!spawned)
    return core::Result<SubagentRunResult>::failure(spawned.error());
  auto worker = std::move(spawned.value());
  bool reaped = false;
  int status = 0;
  const auto cleanup = [&] {
    support::terminate_process_group(worker.pid, config_.termination_grace,
                                     reaped, status);
  };

  auto request = serialize_worker_request({task.agent, task.task});
  request.push_back('\n');
  if (!write_all(worker.input.get(), request)) {
    cleanup();
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::tool_error, "cannot write subagent worker request"});
  }
  worker.input.reset();

  std::string protocol_buffer;
  std::string diagnostics;
  std::size_t protocol_bytes = 0;
  std::size_t diagnostic_bytes = 0;
  bool protocol_truncated = false;
  bool diagnostics_truncated = false;
  bool output_eof = false;
  bool error_eof = false;
  bool started = false;
  std::optional<WorkerEvent> terminal_event;
  std::optional<core::Error> protocol_error;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  const auto handle_line = [&](std::string_view line) {
    if (line.empty() || protocol_error.has_value())
      return;
    const auto parsed = parse_worker_event(line);
    if (!parsed) {
      protocol_error = parsed.error();
      return;
    }
    const auto &event = parsed.value();
    if (terminal_event.has_value()) {
      protocol_error = core::Error{
          ErrorCode::tool_error,
          "subagent worker emitted an event after its terminal event"};
      return;
    }
    if (event.type == WorkerEventType::started) {
      if (started || event.agent != task.agent) {
        protocol_error =
            core::Error{ErrorCode::tool_error,
                        "subagent worker emitted an invalid started event"};
        return;
      }
      started = true;
      return;
    }
    if (!started) {
      protocol_error = core::Error{
          ErrorCode::tool_error,
          "subagent worker emitted output before its started event"};
      return;
    }
    if (event.type == WorkerEventType::tool_start) {
      if (on_progress)
        on_progress(event.tool + ": " + event.purpose);
      return;
    }
    terminal_event = event;
  };

  while (true) {
    if (cancellation.is_cancelled()) {
      cleanup();
      return core::Result<SubagentRunResult>::failure(
          {ErrorCode::cancelled, "subagent worker cancelled"});
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      cleanup();
      return core::Result<SubagentRunResult>::failure(
          {ErrorCode::timeout, "subagent worker exceeded its timeout"});
    }
    if (protocol_error.has_value() || protocol_truncated) {
      cleanup();
      return core::Result<SubagentRunResult>::failure(
          protocol_error.value_or(core::Error{
              ErrorCode::tool_error,
              "subagent worker protocol output exceeded its byte limit"}));
    }

    std::array<pollfd, 2> descriptors{{
        {worker.output.valid() ? worker.output.get() : -1, POLLIN, 0},
        {worker.error.valid() ? worker.error.get() : -1, POLLIN, 0},
    }};
    const int polled = poll(descriptors.data(), descriptors.size(), 20);
    if (polled < 0 && errno != EINTR) {
      cleanup();
      return core::Result<SubagentRunResult>::failure(
          {ErrorCode::tool_error, "cannot poll subagent worker: " +
                                      std::string(std::strerror(errno))});
    }

    const auto read_stream = [&](UniqueFd &descriptor, std::string &target,
                                 std::size_t &total_bytes, std::size_t limit,
                                 bool &truncated, bool &eof) {
      if (!descriptor.valid())
        return;
      std::array<char, 4096> buffer{};
      while (true) {
        const auto count = read(descriptor.get(), buffer.data(), buffer.size());
        if (count > 0) {
          const auto chunk_size = static_cast<std::size_t>(count);
          const auto remaining = total_bytes >= limit ? 0 : limit - total_bytes;
          const auto accepted = std::min(remaining, chunk_size);
          target.append(buffer.data(), accepted);
          total_bytes += accepted;
          if (accepted < chunk_size)
            truncated = true;
          continue;
        }
        if (count == 0) {
          descriptor.reset();
          eof = true;
          return;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          return;
        descriptor.reset();
        eof = true;
        return;
      }
    };

    read_stream(worker.output, protocol_buffer, protocol_bytes,
                config_.max_protocol_output_bytes, protocol_truncated,
                output_eof);
    read_stream(worker.error, diagnostics, diagnostic_bytes,
                config_.max_stderr_bytes, diagnostics_truncated, error_eof);

    std::size_t newline = 0;
    while ((newline = protocol_buffer.find('\n')) != std::string::npos) {
      const auto line = protocol_buffer.substr(0, newline);
      protocol_buffer.erase(0, newline + 1);
      handle_line(line);
    }

    if (!reaped && support::try_reap_child(worker.pid, status))
      reaped = true;
    if (reaped && output_eof && error_eof)
      break;
  }

  if (!protocol_buffer.empty()) {
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::tool_error,
         "subagent worker ended with an unterminated JSONL event"});
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::string message =
        "subagent worker exited with " + exit_description(status);
    const auto safe = safe_diagnostics(diagnostics, diagnostics_truncated);
    if (!safe.empty())
      message += ": " + safe;
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::tool_error, std::move(message)});
  }
  if (!started || !terminal_event.has_value()) {
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::tool_error,
         "subagent worker exited without a terminal event"});
  }
  switch (terminal_event->type) {
  case WorkerEventType::completed:
    if (terminal_event->content.size() > kMaximumFinalOutputBytes) {
      return core::Result<SubagentRunResult>::failure(
          {ErrorCode::tool_error,
           "subagent worker final output exceeds 32 KiB"});
    }
    return core::Result<SubagentRunResult>::success(
        {std::move(terminal_event->content), terminal_event->usage, false});
  case WorkerEventType::failed:
    return core::Result<SubagentRunResult>::success(
        {std::move(terminal_event->error), terminal_event->usage, true});
  case WorkerEventType::cancelled:
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::cancelled, "subagent worker cancelled"});
  case WorkerEventType::started:
  case WorkerEventType::tool_start:
    break;
  }
  return core::Result<SubagentRunResult>::failure(
      {ErrorCode::tool_error,
       "subagent worker returned an invalid terminal event"});
}

} // namespace zed::subagents
