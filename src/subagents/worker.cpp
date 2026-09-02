#include "zed/subagents/worker.hpp"

#include "zed/app/config.hpp"
#include "zed/core/agent_loop.hpp"
#include "zed/core/context.hpp"
#include "zed/core/session_store.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/core/utf8.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/providers/opencode_go_catalog.hpp"
#include "zed/providers/opencode_go_model.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/subagents/worker_protocol.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/tools/clangd_tool.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <exception>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zed::subagents {

namespace {

constexpr std::size_t kMaximumOutstandingTasks = 64;
volatile std::sig_atomic_t worker_host_stop_requested = 0;

void handle_worker_host_signal(int) { worker_host_stop_requested = 1; }

class WorkerHostSignalScope {
public:
  WorkerHostSignalScope() {
    worker_host_stop_requested = 0;
    struct sigaction action{};
    action.sa_handler = handle_worker_host_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, &old_term_);
    sigaction(SIGINT, &action, &old_int_);
  }

  ~WorkerHostSignalScope() {
    sigaction(SIGTERM, &old_term_, nullptr);
    sigaction(SIGINT, &old_int_, nullptr);
  }

  WorkerHostSignalScope(const WorkerHostSignalScope &) = delete;
  WorkerHostSignalScope &operator=(const WorkerHostSignalScope &) = delete;

private:
  struct sigaction old_term_{};
  struct sigaction old_int_{};
};

core::Result<std::optional<std::string>>
read_command_line(std::istream &input) {
  std::string line;
  line.reserve(kMaximumTaskBytes + 1024);
  char character = '\0';
  while (input.get(character)) {
    if (character == '\n')
      return core::Result<std::optional<std::string>>::success(std::move(line));
    line.push_back(character);
    if (line.size() > kMaximumTaskBytes + 1024) {
      return core::Result<std::optional<std::string>>::failure(
          {core::ErrorCode::invalid_argument,
           "subagent worker host command exceeds its byte limit"});
    }
  }
  if (line.empty())
    return core::Result<std::optional<std::string>>::success(std::nullopt);
  return core::Result<std::optional<std::string>>::failure(
      {core::ErrorCode::invalid_argument,
       "subagent worker host received an unterminated command"});
}

class EventWriter {
public:
  explicit EventWriter(std::ostream &output) : output_(output) {}

  void write(const WorkerEvent &event) {
    std::scoped_lock lock(mutex_);
    output_ << serialize_worker_event(event) << '\n' << std::flush;
  }

private:
  std::ostream &output_;
  std::mutex mutex_;
};

std::string limit_final_output(std::string output) {
  auto sanitized = core::sanitize_utf8(output).text;
  constexpr std::string_view kMarker = "\n[worker output truncated]";
  if (sanitized.size() <= kMaximumFinalOutputBytes)
    return sanitized;
  sanitized.resize(kMaximumFinalOutputBytes - kMarker.size());
  while (!sanitized.empty() && !core::is_valid_utf8(sanitized))
    sanitized.pop_back();
  sanitized += kMarker;
  return sanitized;
}

struct HostTask {
  explicit HostTask(WorkerRequest worker_request)
      : request(std::move(worker_request)) {}

  WorkerRequest request;
  core::CancellationSource cancellation;
};

struct HostQueue {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::shared_ptr<HostTask>> queued;
  std::unordered_map<std::string, std::shared_ptr<HostTask>> outstanding;
  bool stopping{false};
};

void finish_task(HostQueue &queue, std::string_view request_id) {
  std::scoped_lock lock(queue.mutex);
  queue.outstanding.erase(std::string(request_id));
}

void emit_failure(EventWriter &events, std::string_view request_id,
                  std::string message, core::ModelUsage usage = {}) {
  WorkerEvent failed;
  failed.type = WorkerEventType::failed;
  failed.request_id = request_id;
  failed.error = std::move(message);
  failed.usage = usage;
  events.write(failed);
}

void execute_task(const std::shared_ptr<HostTask> &task,
                  const app::RuntimeConfig &runtime,
                  const std::vector<providers::OpenCodeGoModelInfo> &models,
                  const std::vector<AgentDefinition> &agents,
                  providers::OpenCodeGoModel &model, lsp::ClangdClient &clangd,
                  EventWriter &events) {
  const auto &request = task->request;
  const auto token = task->cancellation.token();
  try {
    if (token.is_cancelled()) {
      events.write({WorkerEventType::cancelled, request.request_id});
      return;
    }
    const auto *agent = find_agent(agents, request.agent);
    if (agent == nullptr) {
      emit_failure(events, request.request_id,
                   "unknown configured agent: " + request.agent);
      return;
    }
    if (!agent->available) {
      emit_failure(events, request.request_id,
                   "agent '" + agent->name +
                       "' is unavailable: " + agent->unavailable_reason);
      return;
    }
    const auto *model_info =
        providers::find_opencode_go_model(models, agent->model.model);
    if (model_info == nullptr) {
      emit_failure(events, request.request_id,
                   "Sub Agent configured model disappeared from the catalog");
      return;
    }

    core::ApproximateTokenEstimator estimator;
    core::BasicContextManager context(estimator);
    core::InMemorySessionStore session;
    core::ToolRegistry tools;
    const auto registered = register_explorer_tools(tools, runtime, clangd);
    if (!registered) {
      emit_failure(events, request.request_id,
                   "cannot register Sub Agent tools: " +
                       registered.error().message);
      return;
    }

    core::AgentLoopConfig config;
    config.model_request.model = agent->model;
    config.model_request.temperature = 0.0;
    config.model_request.reasoning_effort = agent->reasoning_effort;
    config.model_request.max_output_tokens = std::min<std::size_t>(
        agent->max_output_tokens, model_info->max_output_tokens == 0
                                      ? agent->max_output_tokens
                                      : model_info->max_output_tokens);
    config.context_limits = core::cap_context_limits(
        runtime.context_limits, model_info->max_context_tokens);
    config.context_limits.automatic_compaction = true;
    config.max_turns = agent->max_turns;
    config.system_prompt = agent->system_prompt;

    core::ModelUsage usage;
    core::AgentLoop loop(model, tools, session, context, config);
    const auto result =
        loop.run(request.task, token, [&](const core::AgentEvent &event) {
          if (event.type == core::AgentEventType::assistant_message &&
              event.model_usage.has_value()) {
            usage.input_tokens += event.model_usage->input_tokens;
            usage.cached_input_tokens += event.model_usage->cached_input_tokens;
            usage.output_tokens += event.model_usage->output_tokens;
          } else if (event.type == core::AgentEventType::tool_start) {
            events.write({WorkerEventType::tool_start,
                          request.request_id,
                          {},
                          event.tool_call.has_value() ? event.tool_call->name
                                                      : std::string("tool"),
                          event.text});
          }
        });
    if (result) {
      WorkerEvent completed;
      completed.type = WorkerEventType::completed;
      completed.request_id = request.request_id;
      completed.content = limit_final_output(result.value());
      completed.usage = usage;
      events.write(completed);
      return;
    }
    if (result.error().code == core::ErrorCode::cancelled) {
      events.write({WorkerEventType::cancelled, request.request_id});
      return;
    }
    emit_failure(events, request.request_id, result.error().message, usage);
  } catch (const std::exception &error) {
    emit_failure(events, request.request_id,
                 "subagent worker host task failed: " +
                     std::string(error.what()));
  } catch (...) {
    emit_failure(events, request.request_id,
                 "subagent worker host task failed: unknown error");
  }
}

void stop_queue(HostQueue &queue) {
  std::scoped_lock lock(queue.mutex);
  queue.stopping = true;
  for (auto &[request_id, task] : queue.outstanding) {
    static_cast<void>(request_id);
    task->cancellation.cancel();
  }
  queue.ready.notify_all();
}

} // namespace

core::Result<void> register_explorer_tools(core::ToolRegistry &registry,
                                           const app::RuntimeConfig &runtime,
                                           lsp::ClangdClient &clangd) {
  // Design invariant: Sub Agents never receive Shell-capable tools. Keep this
  // as an explicit read-only allowlist instead of reusing the main Agent's tool
  // registration. This deliberately avoids inheriting command execution,
  // environment filtering, process lifetime, and nested delegation semantics in
  // the worker host, which keeps the Sub Agent security model small enough to
  // reason about.
  const auto register_tool = [&](std::unique_ptr<core::Tool> tool) {
    return registry.register_tool(std::move(tool));
  };
  auto result = register_tool(std::make_unique<tools::ReadFileTool>(
      runtime.workspace, runtime.tool_limits));
  if (!result)
    return result;
  result = register_tool(std::make_unique<tools::GrepTool>(
      runtime.workspace, runtime.tool_limits));
  if (!result)
    return result;
  result = register_tool(std::make_unique<tools::FindFilesTool>(
      runtime.workspace, runtime.tool_limits));
  if (!result)
    return result;
  result = register_tool(std::make_unique<tools::ListDirectoryTool>(
      runtime.workspace, runtime.tool_limits));
  if (!result)
    return result;
  return register_tool(std::make_unique<tools::ClangdTool>(
      runtime.workspace, clangd, runtime.tool_limits));
}

int run_worker_host(std::istream &input, std::ostream &output,
                    std::ostream &diagnostics) {
  WorkerHostSignalScope signal_scope;
  const auto runtime =
      app::load_runtime_config({.load_workspace_system_prompt = false});
  if (!runtime) {
    diagnostics << "cannot initialize subagent worker host: "
                << runtime.error().message << '\n';
    return 2;
  }

  auto models = providers::default_opencode_go_models();
  const auto agents = configured_agents(models, runtime.value().subagents);
  providers::OpenCodeGoModel model({
      runtime.value().opencode_go_api_key,
      runtime.value().opencode_endpoint,
      runtime.value().opencode_request_timeout_ms,
      models,
  });
  lsp::ClangdConfig clangd_config;
  clangd_config.workspace_root = runtime.value().workspace;
  clangd_config.executable = runtime.value().clangd_path;
  clangd_config.compile_commands_directory =
      lsp::discover_compile_commands_directory(runtime.value().workspace);
  clangd_config.background_index = false;
  lsp::ClangdClient clangd(std::move(clangd_config));

  EventWriter events(output);
  HostQueue queue;
  const auto concurrency = std::max<std::size_t>(
      1, runtime.value().subagent_execution.max_concurrency);
  std::vector<std::jthread> workers;
  workers.reserve(concurrency);
  for (std::size_t index = 0; index < concurrency; ++index) {
    workers.emplace_back([&] {
      while (true) {
        std::shared_ptr<HostTask> task;
        {
          std::unique_lock lock(queue.mutex);
          queue.ready.wait(
              lock, [&] { return queue.stopping || !queue.queued.empty(); });
          if (queue.stopping && queue.queued.empty())
            return;
          task = std::move(queue.queued.front());
          queue.queued.pop_front();
        }
        execute_task(task, runtime.value(), models, agents, model, clangd,
                     events);
        finish_task(queue, task->request.request_id);
      }
    });
  }

  std::jthread signal_monitor([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (worker_host_stop_requested != 0) {
        stop_queue(queue);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  int exit_code = 0;
  while (worker_host_stop_requested == 0) {
    const auto line = read_command_line(input);
    if (!line) {
      diagnostics << line.error().message << '\n';
      exit_code = 2;
      break;
    }
    if (!line.value().has_value())
      break;
    if (line.value()->empty())
      continue;
    const auto command = parse_worker_command(*line.value());
    if (!command) {
      diagnostics << command.error().message << '\n';
      exit_code = 2;
      break;
    }
    if (command.value().type == WorkerCommandType::cancel) {
      std::scoped_lock lock(queue.mutex);
      const auto task =
          queue.outstanding.find(command.value().request.request_id);
      if (task != queue.outstanding.end())
        task->second->cancellation.cancel();
      continue;
    }

    auto task = std::make_shared<HostTask>(command.value().request);
    bool accepted = false;
    {
      std::scoped_lock lock(queue.mutex);
      const auto id = task->request.request_id;
      if (!queue.stopping &&
          queue.outstanding.size() < kMaximumOutstandingTasks &&
          !queue.outstanding.contains(id)) {
        queue.outstanding.emplace(id, task);
        queue.queued.push_back(task);
        accepted = true;
      }
    }
    events.write({WorkerEventType::started, task->request.request_id,
                  task->request.agent});
    if (!accepted) {
      emit_failure(events, task->request.request_id,
                   "subagent worker host rejected the task");
      continue;
    }
    queue.ready.notify_one();
  }

  signal_monitor.request_stop();
  stop_queue(queue);
  return exit_code;
}

} // namespace zed::subagents
