#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "zed/core/cancellation.hpp"
#include "zed/core/model.hpp"
#include "zed/core/result.hpp"

namespace zed::subagents {

struct SubagentTask {
  std::string agent;
  std::string task;
};

struct SubagentRunResult {
  std::string content;
  core::ModelUsage usage;
  bool is_error{false};
};

using SubagentProgressCallback = std::function<void(std::string_view)>;

class SubagentRunner {
public:
  virtual ~SubagentRunner() = default;

  virtual core::Result<SubagentRunResult>
  run(const SubagentTask &task, core::CancellationToken cancellation,
      std::chrono::milliseconds timeout,
      const SubagentProgressCallback &on_progress) = 0;
};

struct ProcessSubagentRunnerConfig {
  std::string executable;
  std::filesystem::path workspace_root;
  std::size_t max_protocol_output_bytes{256 * 1024};
  std::size_t max_stderr_bytes{4 * 1024};
  std::chrono::milliseconds termination_grace{2'000};
};

class ProcessSubagentRunner final : public SubagentRunner {
public:
  explicit ProcessSubagentRunner(ProcessSubagentRunnerConfig config);

  core::Result<SubagentRunResult>
  run(const SubagentTask &task, core::CancellationToken cancellation,
      std::chrono::milliseconds timeout,
      const SubagentProgressCallback &on_progress) override;

private:
  ProcessSubagentRunnerConfig config_;
};

} // namespace zed::subagents
