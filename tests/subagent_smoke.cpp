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

int run_fake_worker() {
  std::string line;
  if (!std::getline(std::cin, line))
    return 2;
  const auto request = zed::subagents::parse_worker_request(line);
  if (!request)
    return 2;
  const auto emit = [](const zed::subagents::WorkerEvent &event) {
    std::cout << zed::subagents::serialize_worker_event(event) << '\n'
              << std::flush;
  };
  emit({zed::subagents::WorkerEventType::started, request.value().agent});
  if (request.value().task == "runner-ok") {
    emit({zed::subagents::WorkerEventType::tool_start,
          {},
          "read",
          "inspect runner fixture"});
    zed::subagents::WorkerEvent completed;
    completed.type = zed::subagents::WorkerEventType::completed;
    completed.content = "runner result";
    completed.usage = {11, 2, 3};
    emit(completed);
    return 0;
  }
  if (request.value().task == "runner-failed") {
    zed::subagents::WorkerEvent failed;
    failed.type = zed::subagents::WorkerEventType::failed;
    failed.error = "fixture worker failure";
    failed.usage = {7, 1, 2};
    emit(failed);
    return 0;
  }
  if (request.value().task == "runner-no-terminal")
    return 0;
  if (request.value().task == "runner-nonzero") {
    std::cerr << "safe fixture diagnostic\n";
    return 7;
  }
  if (request.value().task == "runner-sensitive-stderr") {
    std::cerr << "Authorization: supersecret\n";
    return 7;
  }
  if (request.value().task == "runner-stderr-large") {
    std::cerr << std::string(1024, 'd');
    return 7;
  }
  if (request.value().task == "runner-malformed") {
    std::cout << "not-json\n" << std::flush;
    return 0;
  }
  if (request.value().task == "runner-oversized") {
    zed::subagents::WorkerEvent completed;
    completed.type = zed::subagents::WorkerEventType::completed;
    completed.content.assign(zed::subagents::kMaximumFinalOutputBytes + 1, 'x');
    emit(completed);
    return 0;
  }
  if (request.value().task == "runner-slow") {
    std::this_thread::sleep_for(2s);
    return 0;
  }
  emit({zed::subagents::WorkerEventType::failed,
        {},
        {},
        {},
        {},
        "unknown fake task"});
  return 0;
}

void test_protocol() {
  const auto encoded = zed::subagents::serialize_worker_request(
      {"explorer", "inspect the project"});
  const auto decoded = zed::subagents::parse_worker_request(encoded);
  assert(decoded);
  assert(decoded.value().agent == "explorer");
  assert(decoded.value().task == "inspect the project");
  assert(!zed::subagents::parse_worker_request(
      R"({"version":1,"agent":"explorer","task":"x","extra":true})"));
  assert(!zed::subagents::parse_worker_request(
      R"({"version":2,"agent":"explorer","task":"x"})"));
  assert(!zed::subagents::parse_worker_request(
      R"({"version":1,"agent":"explorer","task":"   "})"));

  zed::subagents::WorkerEvent completed;
  completed.type = zed::subagents::WorkerEventType::completed;
  completed.content = "done";
  completed.usage = {12, 3, 4};
  const auto parsed = zed::subagents::parse_worker_event(
      zed::subagents::serialize_worker_event(completed));
  assert(parsed);
  assert(parsed.value().content == "done");
  assert(parsed.value().usage.input_tokens == 12);
  assert(!zed::subagents::parse_worker_event(
      R"({"version":1,"type":"cancelled","extra":true})"));
  assert(!zed::subagents::parse_worker_event(
      R"({"version":1,"type":"completed","content":"x","usage":{"input_tokens":"1","cached_input_tokens":0,"output_tokens":1}})"));
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
  assert(std::find(names.begin(), names.end(), "bash") == names.end());
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
  setenv("ZED_TEST_CLANGD_ENV_MARKER", marker_path.c_str(), 1);

  zed::lsp::ClangdConfig config;
  config.workspace_root = root;
  config.executable = ZED_TEST_FAKE_CLANGD_PATH;
  config.initialize_timeout_ms = 500;
  config.request_timeout_ms = 500;
  config.background_index = false;
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
  zed::subagents::ProcessSubagentRunner runner({
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

  const auto failed = runner.run({"explorer", "runner-failed"}, {}, 2s, {});
  assert(failed);
  assert(failed.value().is_error);
  assert(failed.value().usage.input_tokens == 7);
  assert(failed.value().content.find("fixture worker failure") !=
         std::string::npos);

  const auto missing_terminal =
      runner.run({"explorer", "runner-no-terminal"}, {}, 2s, {});
  assert(!missing_terminal);
  assert(missing_terminal.error().message.find("terminal event") !=
         std::string::npos);

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
  assert(large_stderr.error().message.find("worker stderr truncated") !=
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
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--subagent-worker")
    return run_fake_worker();

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
