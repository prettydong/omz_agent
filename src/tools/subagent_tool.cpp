#include "zed/tools/subagent_tool.hpp"

#include "zed/core/utf8.hpp"
#include "zed/subagents/worker_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zed::tools {

namespace {

using Json = nlohmann::json;
using core::ErrorCode;

enum class InvocationMode {
  single,
  parallel,
  chain,
};

struct Invocation {
  InvocationMode mode{InvocationMode::single};
  std::vector<subagents::SubagentTask> tasks;
};

struct TaskOutcome {
  std::optional<subagents::SubagentRunResult> result;
  std::optional<core::Error> error;
};

core::ToolDefinition
subagent_definition(const std::vector<subagents::AgentDefinition> &agents) {
  Json agent_names = Json::array();
  std::string description =
      "Delegate independent, read-only workspace investigation to a "
      "configured Sub Agent. Available roles: ";
  for (std::size_t index = 0; index < agents.size(); ++index) {
    agent_names.push_back(agents[index].name);
    if (index > 0)
      description += ", ";
    description += agents[index].name;
  }
  description +=
      ". Use task+agent for one task, tasks for parallel work, or chain for "
      "ordered work whose later prompts may contain {previous}.";
  const Json agent_field{{"type", "string"}, {"enum", agent_names}};
  const Json task_field{{"type", "string"},
                        {"minLength", 1},
                        {"maxLength", subagents::kMaximumTaskBytes}};
  const Json task_item{
      {"type", "object"},
      {"additionalProperties", false},
      {"required", Json::array({"agent", "task"})},
      {"properties", {{"agent", agent_field}, {"task", task_field}}},
  };
  const Json task_list{{"type", "array"},
                       {"minItems", 2},
                       {"maxItems", 8},
                       {"items", task_item}};
  const Json schema{
      {"type", "object"},
      {"additionalProperties", false},
      {"properties",
       {{"purpose", {{"type", "string"}, {"minLength", 1}}},
        {"agent", agent_field},
        {"task", task_field},
        {"tasks", task_list},
        {"chain", task_list}}},
      {"required", Json::array({"purpose"})},
      {"oneOf", Json::array({Json{{"required", Json::array({"agent", "task"})}},
                             Json{{"required", Json::array({"tasks"})}},
                             Json{{"required", Json::array({"chain"})}}})},
  };
  return {"subagent", std::move(description), schema.dump()};
}

core::Result<void>
require_fields(const Json &value,
               std::initializer_list<std::string_view> fields,
               std::string_view context) {
  for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
    if (std::find(fields.begin(), fields.end(), iterator.key()) ==
        fields.end()) {
      return core::Result<void>::failure(
          {ErrorCode::invalid_argument,
           std::string(context) +
               " contains unexpected field: " + iterator.key()});
    }
  }
  return core::Result<void>::success();
}

core::Result<std::string> required_string(const Json &value,
                                          std::string_view name) {
  const auto iterator = value.find(std::string(name));
  if (iterator == value.end() || !iterator->is_string()) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "missing non-empty string argument: " + std::string(name)});
  }
  auto text = iterator->get<std::string>();
  if (text.empty() ||
      std::none_of(text.begin(), text.end(), [](char character) {
        return std::isspace(static_cast<unsigned char>(character)) == 0;
      })) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "missing non-empty string argument: " + std::string(name)});
  }
  return core::Result<std::string>::success(std::move(text));
}

core::Result<subagents::SubagentTask> parse_task(const Json &value,
                                                 std::size_t max_task_bytes) {
  if (!value.is_object()) {
    return core::Result<subagents::SubagentTask>::failure(
        {ErrorCode::invalid_argument, "subagent task must be an object"});
  }
  const auto fields = require_fields(value, {"agent", "task"}, "subagent task");
  if (!fields)
    return core::Result<subagents::SubagentTask>::failure(fields.error());
  const auto agent = required_string(value, "agent");
  const auto task = required_string(value, "task");
  if (!agent)
    return core::Result<subagents::SubagentTask>::failure(agent.error());
  if (!task)
    return core::Result<subagents::SubagentTask>::failure(task.error());
  if (task.value().size() > max_task_bytes) {
    return core::Result<subagents::SubagentTask>::failure(
        {ErrorCode::invalid_argument, "subagent task exceeds 32 KiB"});
  }
  return core::Result<subagents::SubagentTask>::success(
      {agent.value(), task.value()});
}

core::Result<Invocation> parse_invocation(const core::ToolCall &call,
                                          const SubagentToolConfig &config) {
  Json arguments;
  try {
    arguments = Json::parse(call.arguments_json);
  } catch (const Json::exception &error) {
    return core::Result<Invocation>::failure(
        {ErrorCode::invalid_argument,
         "invalid subagent arguments: " + std::string(error.what())});
  }
  if (!arguments.is_object()) {
    return core::Result<Invocation>::failure(
        {ErrorCode::invalid_argument, "subagent arguments must be an object"});
  }
  const auto purpose = required_string(arguments, "purpose");
  if (!purpose)
    return core::Result<Invocation>::failure(purpose.error());

  const bool has_task = arguments.contains("task");
  const bool has_tasks = arguments.contains("tasks");
  const bool has_chain = arguments.contains("chain");
  const auto mode_count = static_cast<int>(has_task) +
                          static_cast<int>(has_tasks) +
                          static_cast<int>(has_chain);
  if (mode_count != 1) {
    return core::Result<Invocation>::failure(
        {ErrorCode::invalid_argument,
         "subagent requires exactly one of task, tasks, or chain"});
  }

  Invocation invocation;
  if (has_task) {
    invocation.mode = InvocationMode::single;
    const auto fields = require_fields(arguments, {"purpose", "agent", "task"},
                                       "subagent invocation");
    if (!fields)
      return core::Result<Invocation>::failure(fields.error());
    const auto task =
        parse_task(Json{{"agent", arguments.value("agent", Json{})},
                        {"task", arguments.at("task")}},
                   config.max_task_bytes);
    if (!task)
      return core::Result<Invocation>::failure(task.error());
    invocation.tasks.push_back(task.value());
    return core::Result<Invocation>::success(std::move(invocation));
  }

  invocation.mode =
      has_tasks ? InvocationMode::parallel : InvocationMode::chain;
  const std::string field_name = has_tasks ? "tasks" : "chain";
  const auto fields =
      require_fields(arguments, {"purpose", field_name}, "subagent invocation");
  if (!fields)
    return core::Result<Invocation>::failure(fields.error());
  const auto &items = arguments.at(field_name);
  if (!items.is_array() || items.size() < 2 ||
      items.size() > config.max_tasks) {
    return core::Result<Invocation>::failure(
        {ErrorCode::invalid_argument,
         field_name + " must contain between 2 and " +
             std::to_string(config.max_tasks) + " tasks"});
  }
  invocation.tasks.reserve(items.size());
  for (const auto &item : items) {
    const auto task = parse_task(item, config.max_task_bytes);
    if (!task)
      return core::Result<Invocation>::failure(task.error());
    invocation.tasks.push_back(task.value());
  }
  return core::Result<Invocation>::success(std::move(invocation));
}

std::chrono::milliseconds
remaining_time(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline)
    return std::chrono::milliseconds::zero();
  return std::max(
      std::chrono::milliseconds(1),
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
}

void add_usage(core::ModelUsage &total, const core::ModelUsage &usage) {
  total.input_tokens += usage.input_tokens;
  total.cached_input_tokens += usage.cached_input_tokens;
  total.output_tokens += usage.output_tokens;
}

std::string task_label(std::size_t index, std::size_t total,
                       std::string_view agent) {
  return "[" + std::string(agent) + " " + std::to_string(index + 1) + "/" +
         std::to_string(total) + "]";
}

std::string outcome_text(const TaskOutcome &outcome) {
  if (outcome.result.has_value())
    return outcome.result->content;
  return outcome.error.has_value() ? outcome.error->message
                                   : "subagent task did not produce a result";
}

std::string truncate_aggregate(std::string output, std::size_t maximum) {
  constexpr std::string_view kMarker = "\n[aggregate output truncated]";
  output = core::sanitize_utf8(output).text;
  if (output.size() <= maximum)
    return output;
  if (maximum <= kMarker.size())
    return std::string(kMarker.substr(0, maximum));
  output.resize(maximum - kMarker.size());
  while (!output.empty() && !core::is_valid_utf8(output))
    output.pop_back();
  output += kMarker;
  return output;
}

std::string replace_previous(std::string task, std::string_view previous) {
  constexpr std::string_view kPlaceholder = "{previous}";
  std::size_t offset = 0;
  while ((offset = task.find(kPlaceholder, offset)) != std::string::npos) {
    task.replace(offset, kPlaceholder.size(), previous);
    offset += previous.size();
  }
  return task;
}

} // namespace

SubagentTool::SubagentTool(subagents::SubagentRunner &runner,
                           std::vector<subagents::AgentDefinition> agents,
                           SubagentToolConfig config)
    : runner_(runner), agents_(std::move(agents)), config_(config),
      definition_(subagent_definition(agents_)) {}

const core::ToolDefinition &SubagentTool::definition() const {
  return definition_;
}

void SubagentTool::set_agents(std::vector<subagents::AgentDefinition> agents) {
  agents_ = std::move(agents);
  definition_ = subagent_definition(agents_);
}

core::Result<core::ToolResult>
SubagentTool::execute(const core::ToolCall &call,
                      core::CancellationToken cancellation) {
  return execute_with_progress(call, cancellation, {});
}

core::Result<core::ToolResult> SubagentTool::execute_with_progress(
    const core::ToolCall &call, core::CancellationToken cancellation,
    const core::ToolProgressCallback &on_progress) {
  if (config_.max_tasks < 2 || config_.max_concurrency == 0 ||
      config_.max_task_bytes == 0 || config_.max_aggregate_output_bytes == 0 ||
      config_.total_timeout <= std::chrono::milliseconds::zero()) {
    return core::Result<core::ToolResult>::failure(
        {ErrorCode::invalid_argument, "subagent tool limits are invalid"});
  }
  const auto parsed = parse_invocation(call, config_);
  if (!parsed)
    return core::Result<core::ToolResult>::failure(parsed.error());
  const auto invocation = parsed.value();
  for (const auto &task : invocation.tasks) {
    const auto *agent = subagents::find_agent(agents_, task.agent);
    if (agent == nullptr) {
      return core::Result<core::ToolResult>::failure(
          {ErrorCode::not_found, "unknown configured agent: " + task.agent});
    }
    if (!agent->available) {
      return core::Result<core::ToolResult>::failure(
          {ErrorCode::not_found, "agent '" + task.agent + "' is unavailable: " +
                                     agent->unavailable_reason});
    }
  }

  std::mutex progress_mutex;
  const auto emit_progress = [&](std::string text) {
    if (!on_progress)
      return;
    std::scoped_lock lock(progress_mutex);
    on_progress({std::move(text)});
  };
  const auto deadline =
      std::chrono::steady_clock::now() + config_.total_timeout;
  const auto run_task = [&](const subagents::SubagentTask &task,
                            std::size_t index,
                            std::size_t total) -> TaskOutcome {
    const auto label = task_label(index, total, task.agent);
    emit_progress(label + " running");
    try {
      const auto result = runner_.run(
          task, cancellation, remaining_time(deadline),
          [&](std::string_view purpose) {
            emit_progress(label + " running — " + std::string(purpose));
          });
      if (result) {
        emit_progress(label +
                      (result.value().is_error ? " failed" : " completed"));
        return {result.value(), std::nullopt};
      }
      emit_progress(label + " failed — " + result.error().message);
      return {std::nullopt, result.error()};
    } catch (const std::exception &error) {
      core::Error failure{ErrorCode::internal,
                          "subagent runner threw an exception: " +
                              std::string(error.what())};
      emit_progress(label + " failed — " + failure.message);
      return {std::nullopt, std::move(failure)};
    }
  };

  for (std::size_t index = 0; index < invocation.tasks.size(); ++index) {
    emit_progress(task_label(index, invocation.tasks.size(),
                             invocation.tasks[index].agent) +
                  " queued");
  }

  std::vector<TaskOutcome> outcomes(invocation.tasks.size());
  if (invocation.mode == InvocationMode::single) {
    outcomes[0] = run_task(invocation.tasks[0], 0, 1);
  } else if (invocation.mode == InvocationMode::parallel) {
    std::atomic_size_t next_task{0};
    const auto worker_count =
        std::min(config_.max_concurrency, invocation.tasks.size());
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker_index = 0; worker_index < worker_count;
         ++worker_index) {
      workers.emplace_back([&] {
        while (!cancellation.is_cancelled()) {
          const auto index = next_task.fetch_add(1);
          if (index >= invocation.tasks.size())
            return;
          outcomes[index] =
              run_task(invocation.tasks[index], index, invocation.tasks.size());
        }
      });
    }
    for (auto &worker : workers)
      worker.join();
  } else {
    std::string previous;
    for (std::size_t index = 0; index < invocation.tasks.size(); ++index) {
      auto task = invocation.tasks[index];
      task.task = replace_previous(std::move(task.task), previous);
      if (task.task.size() > config_.max_task_bytes) {
        outcomes[index].error = core::Error{
            ErrorCode::invalid_argument,
            "chain task exceeds 32 KiB after {previous} substitution"};
      } else {
        outcomes[index] = run_task(task, index, invocation.tasks.size());
      }
      if (outcomes[index].error.has_value() ||
          (outcomes[index].result.has_value() &&
           outcomes[index].result->is_error))
        break;
      previous = outcomes[index].result->content;
    }
  }

  if (cancellation.is_cancelled()) {
    return core::Result<core::ToolResult>::failure(
        {ErrorCode::cancelled, "subagent invocation cancelled"});
  }
  for (const auto &outcome : outcomes) {
    if (outcome.error.has_value() &&
        outcome.error->code == ErrorCode::cancelled) {
      return core::Result<core::ToolResult>::failure(*outcome.error);
    }
  }

  bool failed = false;
  core::ModelUsage usage;
  std::string output;
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    const auto &outcome = outcomes[index];
    if (!outcome.result.has_value() && !outcome.error.has_value())
      break;
    const bool task_failed =
        outcome.error.has_value() ||
        (outcome.result.has_value() && outcome.result->is_error);
    failed = failed || task_failed;
    if (outcome.result.has_value())
      add_usage(usage, outcome.result->usage);
    if (invocation.mode == InvocationMode::single) {
      output = task_failed ? "Explorer failed:\n" : "Explorer completed:\n";
    } else {
      if (!output.empty())
        output += "\n\n";
      output +=
          "## " +
          std::string(invocation.mode == InvocationMode::parallel ? "Task "
                                                                  : "Step ") +
          std::to_string(index + 1) + " — " + invocation.tasks[index].agent +
          " — " + (task_failed ? "failed\n" : "completed\n");
    }
    output += outcome_text(outcome);
  }
  output =
      truncate_aggregate(std::move(output), config_.max_aggregate_output_bytes);
  return core::Result<core::ToolResult>::success(
      {call.id, std::move(output), failed, usage});
}

} // namespace zed::tools
