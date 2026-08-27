#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

#include "zed/core/context.hpp"
#include "zed/core/model.hpp"
#include "zed/core/types.hpp"
#include "zed/tools/basic_tools.hpp"

namespace zed::app {

struct RuntimeConfig {
  std::string opencode_go_api_key;
  std::string opencode_endpoint;
  std::size_t opencode_request_timeout_ms{120'000};
  std::filesystem::path workspace;
  std::filesystem::path session_path;
  core::ModelRef main_model;
  core::ModelRef context_model;
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::string terminal_theme{"light"};
  bool quick_bash_enabled{true};
  core::ContextLimits context_limits;
  tools::ToolLimits tool_limits;
  std::size_t max_turns{32};
};

core::Result<RuntimeConfig> load_runtime_config();

} // namespace zed::app
