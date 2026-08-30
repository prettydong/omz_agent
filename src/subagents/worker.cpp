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
#include <csignal>
#include <exception>
#include <istream>
#include <memory>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

namespace zed::subagents {

namespace {

volatile std::sig_atomic_t worker_cancel_requested = 0;

void handle_worker_signal(int) { worker_cancel_requested = 1; }

class WorkerSignalScope {
public:
  WorkerSignalScope() {
    worker_cancel_requested = 0;
    struct sigaction action{};
    action.sa_handler = handle_worker_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGTERM, &action, &old_term_);
    sigaction(SIGINT, &action, &old_int_);
  }

  ~WorkerSignalScope() {
    sigaction(SIGTERM, &old_term_, nullptr);
    sigaction(SIGINT, &old_int_, nullptr);
  }

  WorkerSignalScope(const WorkerSignalScope &) = delete;
  WorkerSignalScope &operator=(const WorkerSignalScope &) = delete;

private:
  struct sigaction old_term_{};
  struct sigaction old_int_{};
};

core::Result<std::string> read_request_line(std::istream &input) {
  std::string line;
  line.reserve(kMaximumTaskBytes + 1024);
  char character = '\0';
  while (input.get(character)) {
    if (character == '\n')
      return core::Result<std::string>::success(std::move(line));
    line.push_back(character);
    if (line.size() > kMaximumTaskBytes + 1024) {
      return core::Result<std::string>::failure(
          {core::ErrorCode::invalid_argument,
           "subagent worker request exceeds its byte limit"});
    }
  }
  if (!line.empty())
    return core::Result<std::string>::success(std::move(line));
  return core::Result<std::string>::failure(
      {core::ErrorCode::invalid_argument,
       "subagent worker did not receive a request"});
}

void write_event(std::ostream &output, const WorkerEvent &event) {
  output << serialize_worker_event(event) << '\n' << std::flush;
}

core::ContextLimits
model_context_limits(core::ContextLimits configured,
                     const providers::OpenCodeGoModelInfo &model) {
  if (model.max_context_tokens == 0 ||
      model.max_context_tokens >= configured.max_context_tokens) {
    return configured;
  }
  configured.max_context_tokens = model.max_context_tokens;
  if (configured.reserved_output_tokens >= configured.max_context_tokens)
    configured.reserved_output_tokens = configured.max_context_tokens / 8;
  const auto available =
      configured.max_context_tokens - configured.reserved_output_tokens;
  if (configured.compaction_trigger_tokens >= available)
    configured.compaction_trigger_tokens = 0;
  return configured;
}

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

} // namespace

core::Result<void> register_explorer_tools(core::ToolRegistry &registry,
                                           const app::RuntimeConfig &runtime,
                                           lsp::ClangdClient &clangd) {
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

int run_explorer_worker(std::istream &input, std::ostream &output,
                        std::ostream &diagnostics) {
  static_cast<void>(diagnostics);
  const auto line = read_request_line(input);
  if (!line) {
    write_event(
        output,
        {WorkerEventType::failed, {}, {}, {}, {}, line.error().message});
    return 2;
  }
  const auto request = parse_worker_request(line.value());
  if (!request) {
    write_event(
        output,
        {WorkerEventType::failed, {}, {}, {}, {}, request.error().message});
    return 2;
  }
  write_event(output, {WorkerEventType::started, request.value().agent});

  WorkerSignalScope signal_scope;
  core::CancellationSource cancellation;
  std::jthread signal_monitor([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (worker_cancel_requested != 0) {
        cancellation.cancel();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  try {
    const auto runtime =
        app::load_runtime_config({.load_workspace_system_prompt = false});
    if (!runtime) {
      write_event(
          output,
          {WorkerEventType::failed, {}, {}, {}, {}, runtime.error().message});
      return 0;
    }
    if (cancellation.token().is_cancelled()) {
      write_event(output, {WorkerEventType::cancelled});
      return 0;
    }
    auto models = providers::default_opencode_go_models();
    const auto agents = configured_agents(models, runtime.value().subagents);
    const auto *agent = find_agent(agents, request.value().agent);
    if (agent == nullptr) {
      write_event(output, {WorkerEventType::failed,
                           {},
                           {},
                           {},
                           {},
                           "unknown built-in agent: " + request.value().agent});
      return 0;
    }
    if (!agent->available) {
      write_event(output, {WorkerEventType::failed,
                           {},
                           {},
                           {},
                           {},
                           "agent '" + agent->name + "' is unavailable: " +
                               agent->unavailable_reason});
      return 0;
    }
    const auto *model_info =
        providers::find_opencode_go_model(models, agent->model.model);
    if (model_info == nullptr) {
      write_event(output,
                  {WorkerEventType::failed,
                   {},
                   {},
                   {},
                   {},
                   "Explorer configured model disappeared from the catalog"});
      return 0;
    }

    lsp::ClangdConfig clangd_config;
    clangd_config.workspace_root = runtime.value().workspace;
    clangd_config.executable = runtime.value().clangd_path;
    clangd_config.compile_commands_directory =
        lsp::discover_compile_commands_directory(runtime.value().workspace);
    clangd_config.background_index = false;
    lsp::ClangdClient clangd(std::move(clangd_config));
    providers::OpenCodeGoModel model({
        runtime.value().opencode_go_api_key,
        runtime.value().opencode_endpoint,
        runtime.value().opencode_request_timeout_ms,
        models,
    });
    core::ApproximateTokenEstimator estimator;
    core::BasicContextManager context(estimator);
    core::InMemorySessionStore session;
    core::ToolRegistry tools;
    const auto registered =
        register_explorer_tools(tools, runtime.value(), clangd);
    if (!registered) {
      write_event(output, {WorkerEventType::failed,
                           {},
                           {},
                           {},
                           {},
                           "cannot register Explorer tools: " +
                               registered.error().message});
      return 0;
    }

    core::AgentLoopConfig config;
    config.model_request.model = agent->model;
    config.model_request.temperature = 0.0;
    config.model_request.reasoning_effort = agent->reasoning_effort;
    config.model_request.max_output_tokens = std::min<std::size_t>(
        agent->max_output_tokens, model_info->max_output_tokens == 0
                                      ? agent->max_output_tokens
                                      : model_info->max_output_tokens);
    config.context_limits =
        model_context_limits(runtime.value().context_limits, *model_info);
    config.context_limits.automatic_compaction = true;
    config.max_turns = agent->max_turns;
    config.system_prompt = agent->system_prompt;

    core::ModelUsage usage;
    core::AgentLoop loop(model, tools, session, context, config);
    const auto result = loop.run(
        request.value().task, cancellation.token(),
        [&](const core::AgentEvent &event) {
          if (event.type == core::AgentEventType::assistant_message &&
              event.model_usage.has_value()) {
            usage.input_tokens += event.model_usage->input_tokens;
            usage.cached_input_tokens += event.model_usage->cached_input_tokens;
            usage.output_tokens += event.model_usage->output_tokens;
          } else if (event.type == core::AgentEventType::tool_start) {
            write_event(output,
                        {WorkerEventType::tool_start,
                         {},
                         event.tool_call.has_value() ? event.tool_call->name
                                                     : std::string("tool"),
                         event.text});
          }
        });
    signal_monitor.request_stop();
    if (result) {
      WorkerEvent completed;
      completed.type = WorkerEventType::completed;
      completed.content = limit_final_output(result.value());
      completed.usage = usage;
      write_event(output, completed);
      return 0;
    }
    if (result.error().code == core::ErrorCode::cancelled) {
      write_event(output, {WorkerEventType::cancelled});
      return 0;
    }
    write_event(output, {WorkerEventType::failed,
                         {},
                         {},
                         {},
                         {},
                         result.error().message,
                         usage});
    return 0;
  } catch (const std::exception &error) {
    write_event(output,
                {WorkerEventType::failed,
                 {},
                 {},
                 {},
                 {},
                 "subagent worker failed: " + std::string(error.what())});
    return 0;
  }
}

} // namespace zed::subagents
