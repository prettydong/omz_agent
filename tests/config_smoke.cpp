#include <cassert>
#include <cstdlib>
#include <string>

#include "zed/app/config.hpp"

namespace {

void set_environment(const char *name, const char *value) {
  assert(setenv(name, value, 1) == 0);
}

void clear_environment(const char *name) { assert(unsetenv(name) == 0); }

} // namespace

int main() {
  clear_environment("OPENCODE_GO_API_KEY");
  clear_environment("ZED_REASONING_EFFORT");
  clear_environment("ZED_SESSION_PATH");
  clear_environment("ZED_QUICK_BASH");
  clear_environment("ZED_THEME");
  const auto missing_key = zed::app::load_runtime_config();
  assert(!missing_key);

  set_environment("OPENCODE_GO_API_KEY", "fixture-key");
  set_environment("ZED_OPENCODE_ENDPOINT", "http://127.0.0.1:9999/responses");
  set_environment("ZED_MAX_TURNS", "0");
  const auto invalid_turns = zed::app::load_runtime_config();
  assert(!invalid_turns);
  clear_environment("ZED_MAX_TURNS");

  set_environment("ZED_REQUEST_TIMEOUT_MS", "2500");
  set_environment("ZED_MAX_CONTEXT_TOKENS", "2048");
  set_environment("ZED_RESERVED_OUTPUT_TOKENS", "256");
  set_environment("ZED_CONTEXT_TRIGGER_TOKENS", "1500");
  const auto valid = zed::app::load_runtime_config();
  assert(valid);
  assert(valid.value().opencode_go_api_key == "fixture-key");
  assert(valid.value().opencode_endpoint == "http://127.0.0.1:9999/responses");
  assert(valid.value().opencode_request_timeout_ms == 2500);
  assert(valid.value().context_limits.max_context_tokens == 2048);
  assert(valid.value().context_limits.reserved_output_tokens == 256);
  assert(valid.value().context_limits.compaction_trigger_tokens == 1500);
  assert(valid.value().reasoning_effort == zed::core::ReasoningEffort::low);
  assert(valid.value().quick_bash_enabled);
  assert(valid.value().terminal_theme == "light");
  assert(valid.value().session_path.parent_path() ==
         valid.value().workspace / ".zed" / "sessions");
  assert(valid.value().session_path.extension() == ".jsonl");

  set_environment("ZED_REASONING_EFFORT", "medium");
  const auto medium_reasoning = zed::app::load_runtime_config();
  assert(medium_reasoning);
  assert(medium_reasoning.value().reasoning_effort ==
         zed::core::ReasoningEffort::medium);
  assert(medium_reasoning.value().session_path != valid.value().session_path);

  set_environment("ZED_REASONING_EFFORT", "maximum");
  const auto invalid_reasoning = zed::app::load_runtime_config();
  assert(!invalid_reasoning);
  clear_environment("ZED_REASONING_EFFORT");

  for (const auto *value : {"on", "true", "1"}) {
    set_environment("ZED_QUICK_BASH", value);
    const auto quick_bash_on = zed::app::load_runtime_config();
    assert(quick_bash_on);
    assert(quick_bash_on.value().quick_bash_enabled);
  }
  for (const auto *value : {"off", "false", "0"}) {
    set_environment("ZED_QUICK_BASH", value);
    const auto quick_bash_off = zed::app::load_runtime_config();
    assert(quick_bash_off);
    assert(!quick_bash_off.value().quick_bash_enabled);
  }
  set_environment("ZED_QUICK_BASH", "sometimes");
  const auto invalid_quick_bash = zed::app::load_runtime_config();
  assert(!invalid_quick_bash);
  clear_environment("ZED_QUICK_BASH");

  set_environment("ZED_THEME", "monaka");
  const auto monaka_theme = zed::app::load_runtime_config();
  assert(monaka_theme);
  assert(monaka_theme.value().terminal_theme == "monaka");
  set_environment("ZED_THEME", "custom");
  const auto invalid_theme = zed::app::load_runtime_config();
  assert(!invalid_theme);
  clear_environment("ZED_THEME");

  set_environment("ZED_SESSION_PATH", "/tmp/zed-resumed-session.jsonl");
  const auto resumed_session = zed::app::load_runtime_config();
  assert(resumed_session);
  assert(resumed_session.value().session_path ==
         "/tmp/zed-resumed-session.jsonl");
  clear_environment("ZED_SESSION_PATH");
  clear_environment("ZED_QUICK_BASH");
  clear_environment("ZED_THEME");

  set_environment("ZED_MAX_CONTEXT_TOKENS", "128");
  set_environment("ZED_RESERVED_OUTPUT_TOKENS", "128");
  const auto invalid_budget = zed::app::load_runtime_config();
  assert(!invalid_budget);

  clear_environment("OPENCODE_GO_API_KEY");
  clear_environment("ZED_OPENCODE_ENDPOINT");
  clear_environment("ZED_REQUEST_TIMEOUT_MS");
  clear_environment("ZED_MAX_CONTEXT_TOKENS");
  clear_environment("ZED_RESERVED_OUTPUT_TOKENS");
  clear_environment("ZED_CONTEXT_TRIGGER_TOKENS");
  clear_environment("ZED_REASONING_EFFORT");
  clear_environment("ZED_SESSION_PATH");
  return 0;
}
