#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "zed/app/config.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/providers/opencode_go_catalog.hpp"
#include "zed/subagents/agent_registry.hpp"
#include "zed/subagents/subagent_runner.hpp"
#include "zed/subagents/worker.hpp"
#include "zed/subagents/worker_protocol.hpp"
#include "zed/tools/subagent_tool.hpp"

#ifndef ZED_TEST_FAKE_CLANGD_PATH
#error "ZED_TEST_FAKE_CLANGD_PATH must point to the fake clangd executable"
#endif

#ifndef ZED_TEST_FAKE_OPENCODE_PATH
#error "ZED_TEST_FAKE_OPENCODE_PATH must point to the fake opencode executable"
#endif

namespace {

using namespace std::chrono_literals;
using zed::core::ErrorCode;
using zed::core::Result;
using zed::subagents::SubagentRunResult;
using zed::subagents::SubagentTask;

zed::core::ToolCall subagent_call(const nlohmann::json &arguments) {
  return {"subagent-call", "subagent", arguments.dump()};
}

class FakeRunner final : public zed::subagents::SubagentRunner {
public:
  Result<SubagentRunResult>
  run(const SubagentTask &task, zed::core::CancellationToken cancellation,
      std::chrono::milliseconds,
      const zed::subagents::SubagentProgressCallback &on_progress) override {
    const auto active = active_.fetch_add(1) + 1;
    auto observed = max_active_.load();
    while (active > observed &&
           !max_active_.compare_exchange_weak(observed, active)) {
    }
    struct ActiveGuard {
      std::atomic_int &active;
      ~ActiveGuard() { active.fetch_sub(1); }
    } guard{active_};

    {
      std::scoped_lock lock(mutex_);
      tasks_.push_back(task.task);
    }
    if (on_progress)
      on_progress("read: inspect fixture");
    for (int step = 0; step < 4; ++step) {
      if (cancellation.is_cancelled()) {
        return Result<SubagentRunResult>::failure(
            {ErrorCode::cancelled, "fake runner cancelled"});
      }
      std::this_thread::sleep_for(10ms);
    }
    if (task.task.find("fail") != std::string::npos) {
      return Result<SubagentRunResult>::success(
          {"fixture failure: " + task.task, {10, 2, 3}, true});
    }
    std::string content = "result:" + task.task;
    if (task.task == "huge")
      content.assign(512, 'x');
    return Result<SubagentRunResult>::success(
        {std::move(content), {10, 2, 3}, false});
  }

  [[nodiscard]] int max_active() const { return max_active_.load(); }

  [[nodiscard]] std::vector<std::string> tasks() const {
    std::scoped_lock lock(mutex_);
    return tasks_;
  }

private:
  std::atomic_int active_{0};
  std::atomic_int max_active_{0};
  mutable std::mutex mutex_;
  std::vector<std::string> tasks_;
};

int run_fake_worker_host() {
  const auto emit = [](const zed::subagents::WorkerEvent &event) {
    std::cout << zed::subagents::serialize_worker_event(event) << '\n'
              << std::flush;
  };
  std::vector<zed::subagents::WorkerRequest> paired_requests;
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto command = zed::subagents::parse_worker_command(line);
    if (!command)
      return 2;
    const auto &request = command.value().request;
    if (command.value().type == zed::subagents::WorkerCommandType::cancel) {
      emit({zed::subagents::WorkerEventType::cancelled, request.request_id});
      continue;
    }
    emit({zed::subagents::WorkerEventType::started, request.request_id,
          request.agent});
    if (request.task == "runner-ok") {
      emit({zed::subagents::WorkerEventType::tool_start,
            request.request_id,
            {},
            "read",
            "inspect runner fixture"});
      zed::subagents::WorkerEvent completed;
      completed.type = zed::subagents::WorkerEventType::completed;
      completed.request_id = request.request_id;
      completed.content = "runner result";
      completed.usage = {11, 2, 3};
      emit(completed);
      continue;
    }
    if (request.task == "runner-pid") {
      zed::subagents::WorkerEvent completed;
      completed.type = zed::subagents::WorkerEventType::completed;
      completed.request_id = request.request_id;
      completed.content = std::to_string(getpid());
      emit(completed);
      continue;
    }
    if (request.task == "runner-pair-a" || request.task == "runner-pair-b") {
      paired_requests.push_back(request);
      if (paired_requests.size() == 2) {
        for (auto iterator = paired_requests.rbegin();
             iterator != paired_requests.rend(); ++iterator) {
          zed::subagents::WorkerEvent completed;
          completed.type = zed::subagents::WorkerEventType::completed;
          completed.request_id = iterator->request_id;
          completed.content = iterator->task;
          emit(completed);
        }
        paired_requests.clear();
      }
      continue;
    }
    if (request.task == "runner-failed") {
      zed::subagents::WorkerEvent failed;
      failed.type = zed::subagents::WorkerEventType::failed;
      failed.request_id = request.request_id;
      failed.error = "fixture worker failure";
      failed.usage = {7, 1, 2};
      emit(failed);
      continue;
    }
    if (request.task == "runner-no-terminal" || request.task == "runner-slow")
      continue;
    if (request.task == "runner-nonzero") {
      std::cerr << "safe fixture diagnostic\n" << std::flush;
      _exit(7);
    }
    if (request.task == "runner-sensitive-stderr") {
      std::cerr << "Authorization: supersecret\n" << std::flush;
      _exit(7);
    }
    if (request.task == "runner-stderr-large") {
      std::cerr << std::string(1024, 'd') << std::flush;
      _exit(7);
    }
    if (request.task == "runner-malformed") {
      std::cout << "not-json\n" << std::flush;
      continue;
    }
    if (request.task == "runner-oversized") {
      zed::subagents::WorkerEvent completed;
      completed.type = zed::subagents::WorkerEventType::completed;
      completed.request_id = request.request_id;
      completed.content.assign(zed::subagents::kMaximumFinalOutputBytes + 1,
                               'x');
      emit(completed);
      continue;
    }
    emit({zed::subagents::WorkerEventType::failed,
          request.request_id,
          {},
          {},
          {},
          {},
          "unknown fake task"});
  }
  return 0;
}

void test_protocol() {
  const auto encoded = zed::subagents::serialize_worker_request(
      {"request-1", "explorer", "inspect the project"});
  const auto decoded = zed::subagents::parse_worker_command(encoded);
  assert(decoded);
  assert(decoded.value().type == zed::subagents::WorkerCommandType::run);
  assert(decoded.value().request.request_id == "request-1");
  assert(decoded.value().request.agent == "explorer");
  assert(decoded.value().request.task == "inspect the project");
  assert(!zed::subagents::parse_worker_command(
      R"({"version":2,"type":"run","id":"1","agent":"explorer","task":"x","extra":true})"));
  assert(!zed::subagents::parse_worker_command(
      R"({"version":1,"type":"run","id":"1","agent":"explorer","task":"x"})"));
  assert(!zed::subagents::parse_worker_command(
      R"({"version":2,"type":"run","id":"1","agent":"explorer","task":"   "})"));
  const auto cancellation = zed::subagents::parse_worker_command(
      zed::subagents::serialize_worker_cancellation("request-1"));
  assert(cancellation);
  assert(cancellation.value().type ==
         zed::subagents::WorkerCommandType::cancel);

  zed::subagents::WorkerEvent completed;
  completed.type = zed::subagents::WorkerEventType::completed;
  completed.request_id = "request-1";
  completed.content = "done";
  completed.usage = {12, 3, 4};
  const auto parsed = zed::subagents::parse_worker_event(
      zed::subagents::serialize_worker_event(completed));
  assert(parsed);
  assert(parsed.value().request_id == "request-1");
  assert(parsed.value().content == "done");
  assert(parsed.value().usage.input_tokens == 12);
  assert(!zed::subagents::parse_worker_event(
      R"({"version":2,"type":"cancelled","id":"1","extra":true})"));
  assert(!zed::subagents::parse_worker_event(
      R"({"version":2,"type":"completed","id":"1","content":"x","usage":{"input_tokens":"1","cached_input_tokens":0,"output_tokens":1}})"));
}

void test_registry_and_read_only_tools(const std::filesystem::path &root) {
  const auto agents = zed::subagents::built_in_agents(
      zed::providers::default_opencode_go_models());
  assert(agents.size() == 1);
  assert(agents[0].name == "explorer");
  assert(agents[0].available);
  assert(agents[0].model.model == "muse-spark-1.2-contributor");
  assert(agents[0].reasoning_effort == zed::core::ReasoningEffort::low);
  assert((agents[0].tools ==
          std::vector<std::string>{"read", "grep", "find", "ls", "lsp"}));
  const auto report = zed::subagents::format_agents(agents);
  assert(report.find("status: available") != std::string::npos);
  const auto unavailable = zed::subagents::built_in_agents({});
  assert(!unavailable[0].available);
  assert(zed::subagents::format_agents(unavailable).find("unavailable") !=
         std::string::npos);

  zed::subagents::ExplorerAgentConfig custom;
  custom.enabled = false;
  custom.model.model = "gpt-5.6-luna";
  custom.reasoning_effort = zed::core::ReasoningEffort::high;
  custom.max_turns = 7;
  custom.max_output_tokens = 2'048;
  custom.system_prompt = "Custom Explorer role.";
  const auto configured = zed::subagents::built_in_agents(
      zed::providers::default_opencode_go_models(), custom);
  assert(!configured[0].available);
  assert(configured[0].model.model == "gpt-5.6-luna");
  assert(configured[0].reasoning_effort == zed::core::ReasoningEffort::high);
  assert(configured[0].max_turns == 7);
  assert(configured[0].max_output_tokens == 2'048);
  assert(configured[0].system_prompt == "Custom Explorer role.");
  assert(configured[0].unavailable_reason.find("disabled") !=
         std::string::npos);

  zed::subagents::ExplorerAgentConfig reviewer;
  reviewer.name = "reviewer";
  reviewer.description = "Review workspace evidence.";
  reviewer.system_prompt = "Custom reviewer prompt.";
  const auto multiple = zed::subagents::configured_agents(
      zed::providers::default_opencode_go_models(),
      {zed::subagents::ExplorerAgentConfig{}, reviewer});
  assert(multiple.size() == 2);
  assert(multiple[1].name == "reviewer");
  assert(multiple[1].system_prompt == "Custom reviewer prompt.");

  zed::app::RuntimeConfig runtime;
  runtime.workspace = root;
  runtime.clangd_path = "clangd-does-not-run-in-this-test";
  zed::lsp::ClangdClient clangd({runtime.workspace, runtime.clangd_path, {}});
  zed::core::ToolRegistry registry;
  assert(zed::subagents::register_explorer_tools(registry, runtime, clangd));
  const auto definitions = registry.definitions();
  std::vector<std::string> names;
  for (const auto &definition : definitions)
    names.push_back(definition.name);
  assert(
      (names == std::vector<std::string>{"read", "grep", "find", "ls", "lsp"}));
  // Shell exclusion is a Sub Agent design invariant, not a configurable
  // default.
  assert(std::find(names.begin(), names.end(), "bash") == names.end());
  assert(std::find(names.begin(), names.end(), "multi_bash") == names.end());
  assert(std::find(names.begin(), names.end(), "write") == names.end());
  assert(std::find(names.begin(), names.end(), "subagent") == names.end());
  assert(!std::filesystem::exists(root / ".zed" / "sessions"));
}

void test_clangd_credential_boundary(const std::filesystem::path &root) {
  const auto source_path = root / "credential_fixture.cpp";
  const auto marker_path = root / "clangd-environment.txt";
  {
    std::ofstream source(source_path);
    source << "int credential_fixture = 0;\n";
  }
  setenv("OPENAI_API_KEY", "openai-secret", 1);
  setenv("OPENCODE_GO_API_KEY", "opencode-secret", 1);
  setenv("ANTHROPIC_API_KEY", "anthropic-secret", 1);
  setenv("AWS_SECRET_ACCESS_KEY", "aws-secret", 1);
  setenv("NPM_TOKEN", "npm-secret", 1);
  setenv("DATABASE_URL", "postgres://secret", 1);
  setenv("ZED_TEST_CLANGD_ENV_MARKER", marker_path.c_str(), 1);

  zed::lsp::ClangdConfig config;
  config.workspace_root = root;
  config.executable = ZED_TEST_FAKE_CLANGD_PATH;
  config.initialize_timeout_ms = 500;
  config.request_timeout_ms = 500;
  config.background_index = false;
  config.environment_allowlist = {"ZED_TEST_CLANGD_ENV_MARKER"};
  zed::lsp::ClangdClient client(std::move(config));
  const auto result = client.diagnostics(source_path);
  assert(!result);
  std::ifstream marker(marker_path);
  std::string status;
  marker >> status;
  assert(status == "clean");

  unsetenv("OPENAI_API_KEY");
  unsetenv("OPENCODE_GO_API_KEY");
  unsetenv("ANTHROPIC_API_KEY");
  unsetenv("AWS_SECRET_ACCESS_KEY");
  unsetenv("NPM_TOKEN");
  unsetenv("DATABASE_URL");
  unsetenv("ZED_TEST_CLANGD_ENV_MARKER");
}

void test_catalog_cancellation() {
  zed::core::CancellationSource cancellation;
  const auto started_at = std::chrono::steady_clock::now();
  auto future = std::async(std::launch::async, [&] {
    return zed::providers::discover_opencode_go_models(
        ZED_TEST_FAKE_OPENCODE_PATH, 5'000, cancellation.token());
  });
  std::this_thread::sleep_for(50ms);
  cancellation.cancel();
  const auto result = future.get();
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  assert(!result);
  assert(result.error().code == ErrorCode::cancelled);
  assert(elapsed < 1s);
}

void test_tool_orchestration() {
  const auto agents = zed::subagents::built_in_agents(
      zed::providers::default_opencode_go_models());

  FakeRunner single_runner;
  zed::tools::SubagentTool single(single_runner, agents);
  auto reviewer_config = zed::subagents::ExplorerAgentConfig{};
  reviewer_config.name = "reviewer";
  reviewer_config.description = "Review evidence.";
  reviewer_config.system_prompt = "Reviewer prompt.";
  const auto multiple_agents = zed::subagents::configured_agents(
      zed::providers::default_opencode_go_models(),
      {zed::subagents::ExplorerAgentConfig{}, reviewer_config});
  zed::tools::SubagentTool dynamic(single_runner, multiple_agents);
  assert(dynamic.definition().input_schema_json.find("reviewer") !=
         std::string::npos);
  const auto reviewer_result =
      dynamic.execute(subagent_call({{"purpose", "Review one area"},
                                     {"agent", "reviewer"},
                                     {"task", "review-one"}}),
                      {});
  assert(reviewer_result);
  assert(reviewer_result.value().content.find("result:review-one") !=
         std::string::npos);
  std::vector<std::string> progress;
  const auto single_result = single.execute_with_progress(
      subagent_call({{"purpose", "Inspect one area"},
                     {"agent", "explorer"},
                     {"task", "single"}}),
      {}, [&](const zed::core::ToolProgress &update) {
        progress.push_back(update.text);
      });
  assert(single_result);
  assert(!single_result.value().is_error);
  assert(single_result.value().content.find("result:single") !=
         std::string::npos);
  assert(single_result.value().model_usage.has_value());
  assert(single_result.value().model_usage->input_tokens == 10);
  assert(progress.size() >= 4);
  assert(progress.front().find("queued") != std::string::npos);
  assert(progress.back().find("completed") != std::string::npos);

  FakeRunner parallel_runner;
  zed::tools::SubagentTool parallel(parallel_runner, agents);
  nlohmann::json tasks = nlohmann::json::array();
  for (const auto &task : {"p1", "p2", "p3-fail", "p4", "p5", "p6"})
    tasks.push_back({{"agent", "explorer"}, {"task", task}});
  const auto parallel_result = parallel.execute(
      subagent_call({{"purpose", "Inspect in parallel"}, {"tasks", tasks}}),
      {});
  assert(parallel_result);
  assert(parallel_result.value().is_error);
  assert(parallel_runner.max_active() <= 4);
  assert(parallel_runner.max_active() >= 2);
  const auto &parallel_output = parallel_result.value().content;
  const auto p1 = parallel_output.find("result:p1");
  const auto p2 = parallel_output.find("result:p2");
  const auto p3 = parallel_output.find("fixture failure: p3-fail");
  const auto p4 = parallel_output.find("result:p4");
  assert(p1 < p2 && p2 < p3 && p3 < p4);
  assert(parallel_result.value().model_usage->input_tokens == 60);

  FakeRunner chain_runner;
  zed::tools::SubagentTool chain(chain_runner, agents);
  const auto chain_result = chain.execute(
      subagent_call(
          {{"purpose", "Inspect in order"},
           {"chain",
            nlohmann::json::array(
                {{{"agent", "explorer"}, {"task", "first"}},
                 {{"agent", "explorer"}, {"task", "use {previous}"}},
                 {{"agent", "explorer"}, {"task", "fail-third"}},
                 {{"agent", "explorer"}, {"task", "must-not-run"}}})}}),
      {});
  assert(chain_result);
  assert(chain_result.value().is_error);
  const auto chain_tasks = chain_runner.tasks();
  assert(chain_tasks.size() == 3);
  assert(chain_tasks[1] == "use result:first");
  assert(chain_result.value().content.find("Step 4") == std::string::npos);
  assert(chain_result.value().model_usage->input_tokens == 30);

  assert(!single.execute(subagent_call({{"purpose", "Bad mode"},
                                        {"agent", "explorer"},
                                        {"task", "x"},
                                        {"tasks", tasks}}),
                         {}));
  assert(!single.execute(subagent_call({{"purpose", "Extra field"},
                                        {"agent", "explorer"},
                                        {"task", "x"},
                                        {"cwd", "/tmp"}}),
                         {}));
  assert(!single.execute(
      subagent_call(
          {{"purpose", "Unknown role"}, {"agent", "reviewer"}, {"task", "x"}}),
      {}));
  assert(!single.execute(
      subagent_call({{"purpose", "Too few tasks"},
                     {"tasks", nlohmann::json::array(
                                   {{{"agent", "explorer"}, {"task", "x"}}})}}),
      {}));
  assert(!single.execute(
      subagent_call({{"purpose", "Too large"},
                     {"agent", "explorer"},
                     {"task", std::string(32 * 1024 + 1, 'x')}}),
      {}));
  assert(!single.execute(subagent_call({{"purpose", "Blank task"},
                                        {"agent", "explorer"},
                                        {"task", "   \n"}}),
                         {}));

  FakeRunner missing_runner;
  zed::tools::SubagentTool missing(missing_runner,
                                   zed::subagents::built_in_agents({}));
  const auto unavailable =
      missing.execute(subagent_call({{"purpose", "Unavailable role"},
                                     {"agent", "explorer"},
                                     {"task", "x"}}),
                      {});
  assert(!unavailable);
  assert(unavailable.error().message.find("unavailable") != std::string::npos);

  FakeRunner truncating_runner;
  zed::tools::SubagentTool truncating(truncating_runner, agents,
                                      {.max_tasks = 8,
                                       .max_concurrency = 4,
                                       .max_task_bytes = 32 * 1024,
                                       .max_aggregate_output_bytes = 96,
                                       .total_timeout = 1s});
  const auto truncated =
      truncating.execute(subagent_call({{"purpose", "Limit output"},
                                        {"agent", "explorer"},
                                        {"task", "huge"}}),
                         {});
  assert(truncated);
  assert(truncated.value().content.size() <= 96);
  assert(truncated.value().content.find("aggregate output truncated") !=
         std::string::npos);
}

void test_process_runner(const std::filesystem::path &executable,
                         const std::filesystem::path &root) {
  zed::subagents::WorkerHostRunner runner({
      executable.string(),
      root,
      256 * 1024,
      128,
      50ms,
  });
  std::vector<std::string> progress;
  const auto completed = runner.run(
      {"explorer", "runner-ok"}, {}, 2s,
      [&](std::string_view update) { progress.emplace_back(update); });
  assert(completed);
  assert(completed.value().content == "runner result");
  assert(completed.value().usage.input_tokens == 11);
  assert(progress.size() == 1);
  assert(progress[0].find("inspect runner fixture") != std::string::npos);

  const auto first_pid = runner.run({"explorer", "runner-pid"}, {}, 2s, {});
  const auto second_pid = runner.run({"explorer", "runner-pid"}, {}, 2s, {});
  assert(first_pid && second_pid);
  assert(first_pid.value().content == second_pid.value().content);

  auto pair_a = std::async(std::launch::async, [&] {
    return runner.run({"explorer", "runner-pair-a"}, {}, 2s, {});
  });
  auto pair_b = std::async(std::launch::async, [&] {
    return runner.run({"explorer", "runner-pair-b"}, {}, 2s, {});
  });
  const auto paired_a = pair_a.get();
  const auto paired_b = pair_b.get();
  if (!paired_a)
    std::cerr << "paired_a failed: " << paired_a.error().message << '\n';
  if (!paired_b)
    std::cerr << "paired_b failed: " << paired_b.error().message << '\n';
  assert(paired_a && paired_b);
  assert(paired_a.value().content == "runner-pair-a");
  assert(paired_b.value().content == "runner-pair-b");

  const auto failed = runner.run({"explorer", "runner-failed"}, {}, 2s, {});
  assert(failed);
  assert(failed.value().is_error);
  assert(failed.value().usage.input_tokens == 7);
  assert(failed.value().content.find("fixture worker failure") !=
         std::string::npos);

  const auto missing_terminal =
      runner.run({"explorer", "runner-no-terminal"}, {}, 75ms, {});
  assert(!missing_terminal);
  assert(missing_terminal.error().code == ErrorCode::timeout);

  const auto nonzero = runner.run({"explorer", "runner-nonzero"}, {}, 2s, {});
  assert(!nonzero);
  assert(nonzero.error().message.find("exit status 7") != std::string::npos);
  assert(nonzero.error().message.find("safe fixture diagnostic") !=
         std::string::npos);

  const auto sensitive =
      runner.run({"explorer", "runner-sensitive-stderr"}, {}, 2s, {});
  assert(!sensitive);
  assert(sensitive.error().message.find("supersecret") == std::string::npos);
  assert(sensitive.error().message.find("redacted") != std::string::npos);

  const auto large_stderr =
      runner.run({"explorer", "runner-stderr-large"}, {}, 2s, {});
  assert(!large_stderr);
  assert(large_stderr.error().message.find("worker host stderr truncated") !=
         std::string::npos);
  assert(large_stderr.error().message.size() < 512);

  const auto malformed =
      runner.run({"explorer", "runner-malformed"}, {}, 2s, {});
  assert(!malformed);
  assert(malformed.error().message.find("invalid subagent worker event JSON") !=
         std::string::npos);

  const auto oversized =
      runner.run({"explorer", "runner-oversized"}, {}, 2s, {});
  assert(!oversized);
  assert(oversized.error().message.find("exceeds 32 KiB") != std::string::npos);

  const auto timed_out = runner.run({"explorer", "runner-slow"}, {}, 75ms, {});
  assert(!timed_out);
  assert(timed_out.error().code == ErrorCode::timeout);
  const auto pid_before_cancel =
      runner.run({"explorer", "runner-pid"}, {}, 2s, {});
  assert(pid_before_cancel);

  zed::core::CancellationSource cancellation;
  auto future = std::async(std::launch::async, [&] {
    return runner.run({"explorer", "runner-slow"}, cancellation.token(), 2s,
                      {});
  });
  std::this_thread::sleep_for(50ms);
  cancellation.cancel();
  const auto cancelled = future.get();
  assert(!cancelled);
  assert(cancelled.error().code == ErrorCode::cancelled);
  const auto pid_after_cancel =
      runner.run({"explorer", "runner-pid"}, {}, 2s, {});
  assert(pid_after_cancel);
  assert(pid_before_cancel.value().content == pid_after_cancel.value().content);
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--subagent-worker-host")
    return run_fake_worker_host();

  const auto root =
      std::filesystem::temp_directory_path() / "zed-subagent-smoke";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root);
  test_protocol();
  test_registry_and_read_only_tools(root);
  test_clangd_credential_boundary(root);
  test_catalog_cancellation();
  test_tool_orchestration();
  test_process_runner(std::filesystem::canonical(argv[0]), root);
  std::filesystem::remove_all(root, error);
  return 0;
}
