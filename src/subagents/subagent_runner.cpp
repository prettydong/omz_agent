#include "zed/subagents/subagent_runner.hpp"

#include "zed/subagents/worker_protocol.hpp"
#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

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
    result += "\n[worker host stderr truncated]";
  return result;
}

struct SpawnedWorkerHost {
  pid_t pid{-1};
  UniqueFd input;
  UniqueFd output;
  UniqueFd error;
  bool output_eof{false};
  bool error_eof{false};
  bool reaped{false};
  int status{0};
};

core::Result<SpawnedWorkerHost>
spawn_worker_host(const WorkerHostRunnerConfig &config) {
  if (config.executable.empty()) {
    return core::Result<SpawnedWorkerHost>::failure(
        {ErrorCode::invalid_argument,
         "subagent worker host executable is empty"});
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
    return core::Result<SpawnedWorkerHost>::failure(
        {ErrorCode::tool_error, "cannot create subagent worker host pipes: " +
                                    std::string(std::strerror(errno))});
  }

  support::SpawnOptions spawn_options;
  spawn_options.executable = config.executable;
  spawn_options.arguments = {"--subagent-worker-host"};
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
  // The host is a trusted zeda process and needs only provider/configuration
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
    return core::Result<SpawnedWorkerHost>::failure(
        {ErrorCode::tool_error, "cannot start subagent worker host: " +
                                    std::string(std::strerror(spawn_error))});
  }

  spawn_lock.unlock();
  close(input_pipe[0]);
  close(output_pipe[1]);
  close(error_pipe[1]);
  set_nonblocking(output_pipe[0]);
  set_nonblocking(error_pipe[0]);
  return core::Result<SpawnedWorkerHost>::success(
      {child, UniqueFd(input_pipe[1]), UniqueFd(output_pipe[0]),
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

class WorkerHostRunner::Impl {
public:
  explicit Impl(WorkerHostRunnerConfig config) : config_(std::move(config)) {}

  ~Impl() { stop_host(); }

  core::Result<SubagentRunResult>
  run(const SubagentTask &task, core::CancellationToken cancellation,
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
          {ErrorCode::timeout,
           "subagent worker host timed out before starting"});
    }
    if (cancellation.is_cancelled()) {
      return core::Result<SubagentRunResult>::failure(
          {ErrorCode::cancelled,
           "subagent worker host cancelled before starting"});
    }

    static std::once_flag ignore_sigpipe_once;
    std::call_once(ignore_sigpipe_once,
                   [] { static_cast<void>(std::signal(SIGPIPE, SIG_IGN)); });

    const auto request_id = std::to_string(next_request_id_.fetch_add(1));
    auto pending = std::make_shared<PendingTask>();
    pending->agent = task.agent;
    {
      std::scoped_lock lock(state_mutex_);
      const auto started = ensure_host_locked();
      if (!started)
        return core::Result<SubagentRunResult>::failure(started.error());
      pending_tasks_.emplace(request_id, pending);
    }

    auto request =
        serialize_worker_request({request_id, task.agent, task.task});
    request.push_back('\n');
    if (!send_command(request)) {
      fail_host(
          {ErrorCode::tool_error, "cannot write subagent worker host request"});
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      std::vector<std::string> progress;
      std::optional<WorkerEvent> terminal_event;
      std::optional<core::Error> error;
      {
        std::scoped_lock lock(state_mutex_);
        progress.assign(std::make_move_iterator(pending->progress.begin()),
                        std::make_move_iterator(pending->progress.end()));
        pending->progress.clear();
        terminal_event = pending->terminal_event;
        error = pending->error;
      }
      for (const auto &update : progress) {
        if (on_progress)
          on_progress(update);
      }
      if (error.has_value()) {
        auto command = serialize_worker_cancellation(request_id);
        command.push_back('\n');
        static_cast<void>(send_command(command));
        remove_pending(request_id);
        return core::Result<SubagentRunResult>::failure(std::move(*error));
      }
      if (terminal_event.has_value()) {
        remove_pending(request_id);
        return terminal_result(std::move(*terminal_event));
      }

      const bool cancelled = cancellation.is_cancelled();
      const bool timed_out = std::chrono::steady_clock::now() >= deadline;
      if (cancelled || timed_out) {
        auto command = serialize_worker_cancellation(request_id);
        command.push_back('\n');
        static_cast<void>(send_command(command));
        remove_pending(request_id);
        return core::Result<SubagentRunResult>::failure(
            {cancelled ? ErrorCode::cancelled : ErrorCode::timeout,
             cancelled ? "subagent worker host task cancelled"
                       : "subagent worker host task exceeded its timeout"});
      }

      pump_io(std::chrono::milliseconds(20));
    }
  }

private:
  struct PendingTask {
    std::string agent;
    bool started{false};
    std::size_t protocol_bytes{0};
    std::deque<std::string> progress;
    std::optional<WorkerEvent> terminal_event;
    std::optional<core::Error> error;
  };

  core::Result<void> ensure_host_locked() {
    if (host_.has_value())
      return core::Result<void>::success();
    auto spawned = spawn_worker_host(config_);
    if (!spawned)
      return core::Result<void>::failure(spawned.error());
    host_ = std::move(spawned.value());
    protocol_buffer_.clear();
    diagnostics_.clear();
    diagnostics_truncated_ = false;
    return core::Result<void>::success();
  }

  bool send_command(std::string_view command) {
    std::scoped_lock input_lock(input_mutex_);
    std::scoped_lock state_lock(state_mutex_);
    return host_.has_value() && host_->input.valid() &&
           write_all(host_->input.get(), command);
  }

  void remove_pending(std::string_view request_id) {
    std::scoped_lock lock(state_mutex_);
    pending_tasks_.erase(std::string(request_id));
  }

  core::Result<SubagentRunResult> terminal_result(WorkerEvent event) const {
    switch (event.type) {
    case WorkerEventType::completed:
      if (event.content.size() > kMaximumFinalOutputBytes) {
        return core::Result<SubagentRunResult>::failure(
            {ErrorCode::tool_error,
             "subagent worker host final output exceeds 32 KiB"});
      }
      return core::Result<SubagentRunResult>::success(
          {std::move(event.content), event.usage, false});
    case WorkerEventType::failed:
      return core::Result<SubagentRunResult>::success(
          {std::move(event.error), event.usage, true});
    case WorkerEventType::cancelled:
      return core::Result<SubagentRunResult>::failure(
          {ErrorCode::cancelled, "subagent worker host task cancelled"});
    case WorkerEventType::started:
    case WorkerEventType::tool_start:
      break;
    }
    return core::Result<SubagentRunResult>::failure(
        {ErrorCode::tool_error,
         "subagent worker host returned an invalid terminal event"});
  }

  void handle_event_locked(WorkerEvent event, std::size_t line_bytes) {
    const auto iterator = pending_tasks_.find(event.request_id);
    if (iterator == pending_tasks_.end())
      return;
    auto &pending = *iterator->second;
    pending.protocol_bytes += line_bytes;
    if (pending.protocol_bytes > config_.max_protocol_output_bytes) {
      pending.error = core::Error{
          ErrorCode::tool_error,
          "subagent worker host task output exceeded its byte limit"};
      return;
    }
    if (pending.terminal_event.has_value()) {
      pending.error = core::Error{
          ErrorCode::tool_error,
          "subagent worker host emitted an event after its terminal event"};
      return;
    }
    if (event.type == WorkerEventType::started) {
      if (pending.started || event.agent != pending.agent) {
        pending.error = core::Error{
            ErrorCode::tool_error,
            "subagent worker host emitted an invalid started event"};
        return;
      }
      pending.started = true;
      return;
    }
    if (!pending.started) {
      pending.error = core::Error{
          ErrorCode::tool_error,
          "subagent worker host emitted output before its started event"};
      return;
    }
    if (event.type == WorkerEventType::tool_start) {
      pending.progress.push_back(event.tool + ": " + event.purpose);
      return;
    }
    pending.terminal_event = std::move(event);
  }

  void read_stream_locked(UniqueFd &descriptor, std::string &target,
                          std::size_t limit, bool &truncated, bool &eof) {
    if (!descriptor.valid())
      return;
    std::array<char, 4096> buffer{};
    while (true) {
      const auto count = read(descriptor.get(), buffer.data(), buffer.size());
      if (count > 0) {
        const auto chunk_size = static_cast<std::size_t>(count);
        const auto remaining =
            target.size() >= limit ? 0 : limit - target.size();
        const auto accepted = std::min(remaining, chunk_size);
        target.append(buffer.data(), accepted);
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
  }

  std::optional<core::Error> read_protocol_locked() {
    if (!host_->output.valid())
      return std::nullopt;
    std::array<char, 4096> buffer{};
    while (true) {
      const auto count =
          read(host_->output.get(), buffer.data(), buffer.size());
      if (count > 0) {
        protocol_buffer_.append(buffer.data(), static_cast<std::size_t>(count));
        std::size_t newline = 0;
        while ((newline = protocol_buffer_.find('\n')) != std::string::npos) {
          auto line = protocol_buffer_.substr(0, newline);
          protocol_buffer_.erase(0, newline + 1);
          if (line.empty())
            continue;
          const auto parsed = parse_worker_event(line);
          if (!parsed)
            return parsed.error();
          handle_event_locked(parsed.value(), line.size() + 1);
        }
        if (protocol_buffer_.size() > config_.max_protocol_output_bytes) {
          return core::Error{
              ErrorCode::tool_error,
              "subagent worker host emitted an unterminated oversized event"};
        }
        continue;
      }
      if (count == 0) {
        host_->output.reset();
        host_->output_eof = true;
        return std::nullopt;
      }
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return std::nullopt;
      host_->output.reset();
      host_->output_eof = true;
      return std::nullopt;
    }
  }

  void pump_io(std::chrono::milliseconds wait) {
    std::scoped_lock io_lock(io_mutex_);
    std::array<pollfd, 2> descriptors{};
    {
      std::scoped_lock state_lock(state_mutex_);
      if (!host_.has_value())
        return;
      descriptors = {{
          {host_->output.valid() ? host_->output.get() : -1, POLLIN, 0},
          {host_->error.valid() ? host_->error.get() : -1, POLLIN, 0},
      }};
    }
    const int polled = poll(descriptors.data(), descriptors.size(),
                            static_cast<int>(wait.count()));
    std::scoped_lock state_lock(state_mutex_);
    if (!host_.has_value())
      return;
    if (polled < 0 && errno != EINTR) {
      fail_host_locked(
          {ErrorCode::tool_error, "cannot poll subagent worker host: " +
                                      std::string(std::strerror(errno))});
      return;
    }

    const auto protocol_error = read_protocol_locked();
    read_stream_locked(host_->error, diagnostics_, config_.max_stderr_bytes,
                       diagnostics_truncated_, host_->error_eof);
    if (protocol_error.has_value()) {
      fail_host_locked(*protocol_error);
      return;
    }

    if (!host_->reaped && support::try_reap_child(host_->pid, host_->status))
      host_->reaped = true;
    if (host_->reaped && host_->output_eof && host_->error_eof) {
      std::string message =
          "subagent worker host exited with " + exit_description(host_->status);
      const auto safe = safe_diagnostics(diagnostics_, diagnostics_truncated_);
      if (!safe.empty())
        message += ": " + safe;
      fail_host_locked({ErrorCode::tool_error, std::move(message)});
    }
  }

  void fail_host(core::Error error) {
    std::scoped_lock io_lock(io_mutex_);
    std::scoped_lock state_lock(state_mutex_);
    fail_host_locked(std::move(error));
  }

  void fail_host_locked(core::Error error) {
    for (auto &[request_id, pending] : pending_tasks_) {
      static_cast<void>(request_id);
      if (!pending->terminal_event.has_value() && !pending->error.has_value())
        pending->error = error;
    }
    if (host_.has_value()) {
      host_->input.reset();
      support::terminate_process_group(host_->pid, config_.termination_grace,
                                       host_->reaped, host_->status);
      host_.reset();
    }
    protocol_buffer_.clear();
    diagnostics_.clear();
    diagnostics_truncated_ = false;
  }

  void stop_host() {
    std::scoped_lock input_lock(input_mutex_);
    std::scoped_lock io_lock(io_mutex_);
    std::scoped_lock state_lock(state_mutex_);
    if (!host_.has_value())
      return;
    host_->input.reset();
    support::terminate_process_group(host_->pid, config_.termination_grace,
                                     host_->reaped, host_->status);
    host_.reset();
  }

  WorkerHostRunnerConfig config_;
  std::atomic_uint64_t next_request_id_{1};
  std::mutex input_mutex_;
  std::mutex io_mutex_;
  std::mutex state_mutex_;
  std::optional<SpawnedWorkerHost> host_;
  std::unordered_map<std::string, std::shared_ptr<PendingTask>> pending_tasks_;
  std::string protocol_buffer_;
  std::string diagnostics_;
  bool diagnostics_truncated_{false};
};

WorkerHostRunner::WorkerHostRunner(WorkerHostRunnerConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

WorkerHostRunner::~WorkerHostRunner() = default;

core::Result<SubagentRunResult>
WorkerHostRunner::run(const SubagentTask &task,
                      core::CancellationToken cancellation,
                      std::chrono::milliseconds timeout,
                      const SubagentProgressCallback &on_progress) {
  return impl_->run(task, std::move(cancellation), timeout, on_progress);
}

} // namespace zed::subagents
