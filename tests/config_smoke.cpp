#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "zed/app/config.hpp"

namespace {

void set_environment(const char *name, const char *value) {
  assert(setenv(name, value, 1) == 0);
}

void clear_environment(const char *name) { assert(unsetenv(name) == 0); }

} // namespace

int main() {
  const auto config_workspace =
      std::filesystem::temp_directory_path() /
      ("zeda-config-smoke-workspace-" + std::to_string(getpid()));
  std::filesystem::remove_all(config_workspace);
  std::filesystem::create_directories(config_workspace);
  const auto auth_path =
      std::filesystem::temp_directory_path() /
      ("zeda-config-smoke-auth-" + std::to_string(getpid()) + ".json");
  std::filesystem::remove(auth_path);

  clear_environment("OPENCODE_GO_API_KEY");
  set_environment("ZED_OPENCODE_AUTH_PATH", auth_path.c_str());
  clear_environment("ZED_REASONING_EFFORT");
  clear_environment("ZED_OPENCODE_PATH");
  clear_environment("ZED_SESSION_PATH");
  clear_environment("ZED_QUICK_BASH");
  clear_environment("ZED_THEME");
  clear_environment("ZED_CLANGD_PATH");
  set_environment("ZED_WORKSPACE", config_workspace.c_str());
  const auto missing_key = zed::app::load_runtime_config();
  assert(!missing_key);

  {
    std::ofstream auth_file(auth_path);
    assert(auth_file);
    auth_file << R"({"opencode-go":{"type":"api","key":"stored-key"}})";
  }
  const auto stored_key = zed::app::load_runtime_config();
  assert(stored_key);
  assert(stored_key.value().opencode_go_api_key == "stored-key");

  {
    std::ofstream auth_file(auth_path);
    assert(auth_file);
    auth_file << "not-json";
  }
  const auto malformed_credentials = zed::app::load_runtime_config();
  assert(!malformed_credentials);

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
  assert(valid.value().opencode_path == "opencode");
  assert(valid.value().clangd_path == "clangd");
  assert(valid.value().context_limits.max_context_tokens == 2048);
  assert(valid.value().context_limits.reserved_output_tokens == 256);
  assert(valid.value().context_limits.compaction_trigger_tokens == 1500);
  assert(valid.value().reasoning_effort == zed::core::ReasoningEffort::low);
  assert(valid.value().quick_bash_enabled);
  assert(valid.value().terminal_theme == "light");
  assert(valid.value().session_path.parent_path() ==
         valid.value().workspace / ".zed" / "sessions");
  assert(valid.value().session_path.extension() == ".jsonl");
  assert(valid.value().system_prompt_path ==
         valid.value().workspace / ".zed" / "zed_system_propmt.md");
  assert(std::filesystem::is_regular_file(valid.value().system_prompt_path));
  assert(valid.value().system_prompt.find("你的名字是 zeda") !=
         std::string::npos);
  assert(valid.value().system_prompt.find("通用 agent") != std::string::npos);
  assert(valid.value().system_prompt.find("Zed Huang 独立开发") !=
         std::string::npos);
  assert(valid.value().system_prompt.find("默认使用中文") != std::string::npos);

  const auto system_prompt_path =
      config_workspace / ".zed" / "zed_system_propmt.md";
  {
    std::ofstream prompt_file(system_prompt_path, std::ios::binary);
    assert(prompt_file);
    prompt_file << "You are a custom agent.\n请保持冷静。\n";
  }
  const auto custom_prompt = zed::app::load_runtime_config();
  assert(custom_prompt);
  assert(custom_prompt.value().system_prompt ==
         "You are a custom agent.\n请保持冷静。\n");

  {
    std::ofstream prompt_file(system_prompt_path, std::ios::binary);
    assert(prompt_file);
    prompt_file << " \n\t";
  }
  const auto empty_prompt = zed::app::load_runtime_config();
  assert(!empty_prompt);
  assert(empty_prompt.error().message.find("cannot be empty") !=
         std::string::npos);

  {
    std::ofstream prompt_file(system_prompt_path, std::ios::binary);
    assert(prompt_file);
    prompt_file << "bad" << static_cast<char>(0xFF);
  }
  const auto invalid_utf8_prompt = zed::app::load_runtime_config();
  assert(!invalid_utf8_prompt);
  assert(invalid_utf8_prompt.error().message.find("valid UTF-8") !=
         std::string::npos);
  std::filesystem::remove(system_prompt_path);

  const auto worker_config =
      zed::app::load_runtime_config({.load_workspace_system_prompt = false});
  assert(worker_config);
  assert(worker_config.value().system_prompt.empty());
  assert(!std::filesystem::exists(system_prompt_path));

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

  for (const auto *value : {"auto", "minimal", "xhigh", "max", "thinking"}) {
    set_environment("ZED_REASONING_EFFORT", value);
    const auto extended_reasoning = zed::app::load_runtime_config();
    assert(extended_reasoning);
    assert(zed::core::reasoning_effort_name(
               extended_reasoning.value().reasoning_effort) == value);
  }
  clear_environment("ZED_REASONING_EFFORT");

  set_environment("ZED_OPENCODE_PATH", "");
  const auto invalid_opencode_path = zed::app::load_runtime_config();
  assert(!invalid_opencode_path);
  clear_environment("ZED_OPENCODE_PATH");

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
  clear_environment("ZED_CLANGD_PATH");
  clear_environment("ZED_QUICK_BASH");
  clear_environment("ZED_THEME");

  set_environment("ZED_MAX_CONTEXT_TOKENS", "128");
  set_environment("ZED_RESERVED_OUTPUT_TOKENS", "128");
  const auto invalid_budget = zed::app::load_runtime_config();
  assert(!invalid_budget);

  clear_environment("OPENCODE_GO_API_KEY");
  clear_environment("ZED_OPENCODE_AUTH_PATH");
  clear_environment("ZED_OPENCODE_ENDPOINT");
  clear_environment("ZED_OPENCODE_PATH");
  clear_environment("ZED_REQUEST_TIMEOUT_MS");
  clear_environment("ZED_MAX_CONTEXT_TOKENS");
  clear_environment("ZED_RESERVED_OUTPUT_TOKENS");
  clear_environment("ZED_CONTEXT_TRIGGER_TOKENS");
  clear_environment("ZED_REASONING_EFFORT");
  clear_environment("ZED_SESSION_PATH");
  clear_environment("ZED_WORKSPACE");
  std::filesystem::remove(auth_path);
  std::filesystem::remove_all(config_workspace);
  return 0;
}
