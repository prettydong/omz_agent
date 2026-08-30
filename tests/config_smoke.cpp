#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "zed/app/config.hpp"
#include "zed/support/atomic_file.hpp"

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
  const auto atomic_path = config_workspace / "atomic" / "settings.json";
  assert(zed::support::write_private_file_atomically(atomic_path, "first",
                                                     "test settings"));
  assert(zed::support::write_private_file_atomically(atomic_path, "second",
                                                     "test settings"));
  {
    std::ifstream saved(atomic_path, std::ios::binary);
    assert(saved);
    assert(std::string(std::istreambuf_iterator<char>(saved), {}) == "second");
  }
  const auto symlink_target = config_workspace / "atomic-target.json";
  {
    std::ofstream target(symlink_target);
    assert(target);
    target << "unchanged";
  }
  const auto symlink_path = config_workspace / "atomic-link.json";
  std::filesystem::create_symlink(symlink_target, symlink_path);
  assert(!zed::support::write_private_file_atomically(symlink_path, "replaced",
                                                      "test settings"));
  const auto auth_path =
      std::filesystem::temp_directory_path() /
      ("zeda-config-smoke-auth-" + std::to_string(getpid()) + ".json");
  std::filesystem::remove(auth_path);

  clear_environment("OPENCODE_GO_API_KEY");
  set_environment("ZED_OPENCODE_AUTH_PATH", auth_path.c_str());
  clear_environment("ZED_MODEL");
  clear_environment("ZED_CONTEXT_MODEL");
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

  set_environment("ZED_MODEL", "gpt-5.6-luna");
  const auto inherited_context_model = zed::app::load_runtime_config();
  assert(inherited_context_model);
  assert(inherited_context_model.value().main_model.model == "gpt-5.6-luna");
  assert(inherited_context_model.value().context_model.model == "gpt-5.6-luna");
  clear_environment("ZED_MODEL");

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
  assert(valid.value().explorer_system_prompt_path ==
         valid.value().workspace / ".zed" / "explorer_system_prompt.md");
  assert(valid.value().context_system_prompt_path ==
         valid.value().workspace / ".zed" / "context_system_prompt.md");
  assert(std::filesystem::is_regular_file(valid.value().system_prompt_path));
  assert(std::filesystem::is_regular_file(
      valid.value().explorer_system_prompt_path));
  assert(std::filesystem::is_regular_file(
      valid.value().context_system_prompt_path));
  assert(valid.value().system_prompt.find("你的名字是 zeda") !=
         std::string::npos);
  assert(valid.value().system_prompt.find("通用 agent") != std::string::npos);
  assert(valid.value().system_prompt.find("Zed Huang 独立开发") !=
         std::string::npos);
  assert(valid.value().system_prompt.find("默认使用中文") != std::string::npos);
  assert(valid.value().explorer.system_prompt.find("strictly read-only") !=
         std::string::npos);
  assert(valid.value().context_system_prompt.find("selected_ids") !=
         std::string::npos);

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

  auto prompts = zed::app::load_workspace_prompts(config_workspace);
  assert(prompts);
  prompts.value().explorer = "Custom Explorer prompt.";
  prompts.value().context = "Custom context prompt.";
  assert(zed::app::save_workspace_prompts(config_workspace, prompts.value()));
  const auto custom_all_prompts = zed::app::load_runtime_config();
  assert(custom_all_prompts);
  assert(custom_all_prompts.value().explorer.system_prompt ==
         "Custom Explorer prompt.");
  assert(custom_all_prompts.value().context_system_prompt ==
         "Custom context prompt.");
  prompts.value().context = " \n";
  assert(!zed::app::validate_workspace_prompts(prompts.value()));

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

  const auto workspace_settings =
      zed::app::load_workspace_config(config_workspace);
  assert(workspace_settings);
  zed::app::WorkspacePrompts management_prompts{"Default Agent prompt.",
                                                "Custom Explorer prompt.",
                                                "Custom context prompt."};
  auto management = zed::app::load_agent_management(
      config_workspace, workspace_settings.value(), management_prompts);
  assert(management);
  assert(management.value().version == zed::app::kAgentManagementVersion);
  assert(management.value().agents.front().automatic_context_compaction);
  assert(management.value().agents.front().tools ==
         std::vector<std::string>{"*"});
  auto business = management.value().agents.front();
  business.id = "business";
  business.name = "Business Agent";
  business.config.max_turns = 19;
  business.automatic_context_compaction = false;
  business.compaction_trigger_tokens = 1'200;
  business.tools = {"read", "grep"};
  business.system_prompt = "Business system prompt.";
  management.value().agents.push_back(business);
  management.value().active_agent = "business";
  zed::subagents::ExplorerAgentConfig reviewer;
  reviewer.name = "reviewer";
  reviewer.description = "Review evidence.";
  reviewer.system_prompt = "Reviewer system prompt.";
  management.value().subagents.push_back(reviewer);
  assert(zed::app::validate_agent_management(management.value()));
  const auto encoded_management =
      zed::app::serialize_agent_management_config(management.value());
  const auto decoded_management =
      zed::app::parse_agent_management_config(encoded_management);
  assert(decoded_management);
  assert(decoded_management.value().version ==
         zed::app::kAgentManagementVersion);
  assert(decoded_management.value().agents.size() == 2);
  assert(!decoded_management.value().agents[1].automatic_context_compaction);
  assert(decoded_management.value().agents[1].compaction_trigger_tokens ==
         1'200);
  assert(decoded_management.value().agents[1].tools ==
         (std::vector<std::string>{"read", "grep"}));
  assert(decoded_management.value().subagents.front().name == "reviewer");
  auto legacy_management_json = nlohmann::json::parse(encoded_management);
  legacy_management_json["version"] = 1;
  for (auto &agent : legacy_management_json["agents"]) {
    agent.erase("automatic_context_compaction");
    agent.erase("compaction_trigger_tokens");
    agent.erase("tools");
  }
  const auto migrated_management =
      zed::app::parse_agent_management_config(legacy_management_json.dump());
  assert(migrated_management);
  assert(migrated_management.value().version ==
         zed::app::kAgentManagementVersion);
  assert(migrated_management.value().agents.front().tools ==
         std::vector<std::string>{"*"});
  assert(zed::app::save_agent_management(config_workspace, management.value()));
  const auto managed_runtime = zed::app::load_runtime_config();
  assert(managed_runtime);
  assert(managed_runtime.value().max_turns == 19);
  assert(!managed_runtime.value().context_limits.automatic_compaction);
  assert(managed_runtime.value().agent_tools ==
         (std::vector<std::string>{"read", "grep"}));
  assert(managed_runtime.value().system_prompt == "Business system prompt.");
  assert(managed_runtime.value().subagents.size() == 2);
  assert(managed_runtime.value().subagents[1].name == "reviewer");

  auto invalid_permissions = management.value();
  invalid_permissions.agents.front().tools = {"*", "read"};
  assert(!zed::app::validate_agent_management(invalid_permissions));

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
  clear_environment("ZED_MODEL");
  clear_environment("ZED_CONTEXT_MODEL");
  clear_environment("ZED_REASONING_EFFORT");
  clear_environment("ZED_SESSION_PATH");
  clear_environment("ZED_WORKSPACE");
  std::filesystem::remove(auth_path);
  std::filesystem::remove_all(config_workspace);
  return 0;
}
