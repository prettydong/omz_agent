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
  std::string opencode_path{"opencode"};
  std::string clangd_path{"clangd"};
  std::size_t opencode_request_timeout_ms{120'000};
  std::filesystem::path workspace;
  std::filesystem::path session_path;
  std::filesystem::path system_prompt_path;
  std::string system_prompt;
  core::ModelRef main_model;
  core::ModelRef context_model;
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::string terminal_theme{"light"};
  bool quick_bash_enabled{true};
  core::ContextLimits context_limits;
  tools::ToolLimits tool_limits;
  std::size_t max_turns{32};
};

struct RuntimeConfigLoadOptions {
  bool load_workspace_system_prompt{true};
};

core::Result<RuntimeConfig>
load_runtime_config(RuntimeConfigLoadOptions options = {});

} // namespace zed::app
