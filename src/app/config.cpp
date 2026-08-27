#include "zed/app/config.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <unistd.h>

namespace zed::app {

namespace {

std::string environment_or(const char *name, std::string fallback = {}) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::move(fallback) : std::string(value);
}

core::Result<std::size_t> size_environment(const char *name,
                                           std::size_t fallback,
                                           bool allow_zero = true) {
  const std::string value = environment_or(name);
  if (value.empty())
    return core::Result<std::size_t>::success(fallback);
  std::size_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
    return core::Result<std::size_t>::failure({
        core::ErrorCode::invalid_argument,
        std::string(name) + " must be a positive integer",
    });
  }
  if (!allow_zero && parsed == 0) {
    return core::Result<std::size_t>::failure({
        core::ErrorCode::invalid_argument,
        std::string(name) + " must be greater than zero",
    });
  }
  return core::Result<std::size_t>::success(parsed);
}

core::Result<core::ReasoningEffort> reasoning_effort_environment() {
  const std::string value = environment_or("ZED_REASONING_EFFORT", "low");
  const auto effort = core::reasoning_effort_from_name(value);
  if (effort.has_value())
    return core::Result<core::ReasoningEffort>::success(*effort);
  return core::Result<core::ReasoningEffort>::failure({
      core::ErrorCode::invalid_argument,
      "ZED_REASONING_EFFORT must be one of: none, low, medium, high",
  });
}

core::Result<bool> boolean_environment(const char *name, bool fallback) {
  const std::string value = environment_or(name);
  if (value.empty())
    return core::Result<bool>::success(fallback);
  if (value == "on" || value == "true" || value == "1")
    return core::Result<bool>::success(true);
  if (value == "off" || value == "false" || value == "0")
    return core::Result<bool>::success(false);
  return core::Result<bool>::failure({
      core::ErrorCode::invalid_argument,
      std::string(name) + " must be one of: on, off, true, false, 1, 0",
  });
}

std::filesystem::path new_session_path(const std::filesystem::path &workspace) {
  static std::atomic_uint64_t sequence{0};
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  const std::string filename = "session-" + std::to_string(now.count()) + "-" +
                               std::to_string(getpid()) + "-" +
                               std::to_string(++sequence) + ".jsonl";
  return workspace / ".zed" / "sessions" / filename;
}

} // namespace

core::Result<RuntimeConfig> load_runtime_config() {
  RuntimeConfig config;
  config.opencode_go_api_key = environment_or("OPENCODE_GO_API_KEY");
  if (config.opencode_go_api_key.empty()) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "OPENCODE_GO_API_KEY is not set",
    });
  }

  config.opencode_endpoint = environment_or(
      "ZED_OPENCODE_ENDPOINT", "https://opencode.ai/zen/go/v1/responses");

  config.workspace =
      environment_or("ZED_WORKSPACE", std::filesystem::current_path().string());
  config.workspace = std::filesystem::weakly_canonical(config.workspace);
  const auto configured_session = environment_or("ZED_SESSION_PATH");
  config.session_path = configured_session.empty()
                            ? new_session_path(config.workspace)
                            : std::filesystem::path(configured_session);

  const std::string model =
      environment_or("ZED_MODEL", "muse-spark-1.2-contributor");
  const std::string context_model = environment_or("ZED_CONTEXT_MODEL", model);
  config.main_model = {"opencode-go", model};
  config.context_model = {"opencode-go", context_model};

  const auto reasoning_effort = reasoning_effort_environment();
  if (!reasoning_effort) {
    return core::Result<RuntimeConfig>::failure(reasoning_effort.error());
  }
  config.reasoning_effort = reasoning_effort.value();

  config.terminal_theme = environment_or("ZED_THEME", "light");
  if (config.terminal_theme != "light" && config.terminal_theme != "monaka") {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_THEME must be one of: light, monaka",
    });
  }

  const auto quick_bash = boolean_environment("ZED_QUICK_BASH", true);
  if (!quick_bash)
    return core::Result<RuntimeConfig>::failure(quick_bash.error());
  config.quick_bash_enabled = quick_bash.value();

  const auto max_context =
      size_environment("ZED_MAX_CONTEXT_TOKENS", 1'000'000, false);
  const auto reserved = size_environment("ZED_RESERVED_OUTPUT_TOKENS", 4096);
  const auto trigger = size_environment("ZED_CONTEXT_TRIGGER_TOKENS", 800'000);
  const auto max_turns = size_environment("ZED_MAX_TURNS", 32, false);
  const auto request_timeout =
      size_environment("ZED_REQUEST_TIMEOUT_MS", 120'000, false);
  if (!max_context || !reserved || !trigger || !max_turns || !request_timeout) {
    const auto *error = !max_context ? &max_context.error()
                        : !reserved  ? &reserved.error()
                        : !trigger   ? &trigger.error()
                        : !max_turns ? &max_turns.error()
                                     : &request_timeout.error();
    return core::Result<RuntimeConfig>::failure(*error);
  }
  config.context_limits = {max_context.value(), reserved.value(),
                           trigger.value()};
  config.max_turns = max_turns.value();
  config.opencode_request_timeout_ms = request_timeout.value();
  if (config.context_limits.max_context_tokens <=
      config.context_limits.reserved_output_tokens) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_MAX_CONTEXT_TOKENS must exceed ZED_RESERVED_OUTPUT_TOKENS",
    });
  }
  return core::Result<RuntimeConfig>::success(std::move(config));
}

} // namespace zed::app
