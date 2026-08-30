#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "zed/app/config.hpp"
#include "zed/app/configure_web.hpp"
#include "zed/providers/opencode_go_catalog.hpp"
#include "zed/skills/skill_registry.hpp"

namespace {

using Json = nlohmann::json;

void set_environment(const char *name, const char *value) {
  assert(setenv(name, value, 1) == 0);
}

void clear_environment(const char *name) { assert(unsetenv(name) == 0); }

struct ServerAddress {
  int port{};
  std::string token;
};

ServerAddress parse_url(std::string_view url) {
  constexpr std::string_view prefix = "http://127.0.0.1:";
  assert(url.starts_with(prefix));
  url.remove_prefix(prefix.size());
  const auto path = url.find("/?token=");
  assert(path != std::string_view::npos);
  return {std::stoi(std::string(url.substr(0, path))),
          std::string(url.substr(path + std::string_view("/?token=").size()))};
}

httplib::Headers headers(const ServerAddress &address,
                         bool include_origin = false) {
  httplib::Headers result{{"X-Zeda-Config-Token", address.token}};
  if (include_origin) {
    result.emplace("Origin",
                   "http://127.0.0.1:" + std::to_string(address.port));
  }
  return result;
}

} // namespace

int main() {
  const auto workspace =
      std::filesystem::temp_directory_path() /
      ("zeda-configure-web-smoke-" + std::to_string(getpid()));
  std::filesystem::remove_all(workspace);
  std::filesystem::create_directories(workspace);

  for (const auto *name : {
           "ZED_MODEL",
           "ZED_CONTEXT_MODEL",
           "ZED_REASONING_EFFORT",
           "ZED_MAX_TURNS",
           "ZED_MAX_CONTEXT_TOKENS",
           "ZED_RESERVED_OUTPUT_TOKENS",
           "ZED_CONTEXT_TRIGGER_TOKENS",
           "ZED_SESSION_PATH",
           "ZED_THEME",
           "ZED_QUICK_BASH",
       }) {
    clear_environment(name);
  }
  set_environment("OPENCODE_GO_API_KEY", "web-fixture-secret");
  set_environment("ZED_WORKSPACE", workspace.c_str());
  set_environment("ZED_CONFIGURE_WEB_NO_BROWSER", "1");

  auto config = zed::app::default_workspace_config();
  config.agent.model.model = "gpt-5.6-luna";
  config.agent.reasoning_effort = zed::core::ReasoningEffort::high;
  config.agent.max_turns = 41;
  config.agent.max_output_tokens = 2'048;
  config.agent.temperature = 0.3;
  config.explorer.enabled = true;
  config.explorer.reasoning_effort = zed::core::ReasoningEffort::high;
  config.explorer.max_turns = 9;
  config.explorer.max_output_tokens = 4'096;
  config.subagent_execution = {3, 321'000, 192 * 1024};
  config.context.model.model = "qwen3.8-flash";
  config.context.limits = {700'000, 10'000, 500'000};
  config.context.max_output_tokens = 1'536;

  const zed::app::WorkspacePrompts initial_prompts{"main fixture prompt",
                                                   "explorer fixture prompt",
                                                   "context fixture prompt"};

  assert(zed::app::save_workspace_config(workspace, config));
  assert(zed::app::save_workspace_prompts(workspace, initial_prompts));
  const auto loaded = zed::app::load_workspace_config(workspace);
  assert(loaded);
  assert(loaded.value().agent.model.model == "gpt-5.6-luna");
  assert(loaded.value().agent.max_turns == 41);
  assert(loaded.value().agent.max_output_tokens == 2'048);
  assert(loaded.value().agent.temperature == 0.3);
  assert(loaded.value().explorer.max_turns == 9);
  assert(loaded.value().subagent_execution.max_concurrency == 3);
  assert(loaded.value().context.limits.max_context_tokens == 700'000);
  assert(loaded.value().context.max_output_tokens == 1'536);

  struct stat status{};
  assert(stat(zed::app::workspace_config_path(workspace).c_str(), &status) ==
         0);
  assert((status.st_mode & 0777) == 0600);

  const auto runtime = zed::app::load_runtime_config();
  assert(runtime);
  assert(runtime.value().main_model.model == "gpt-5.6-luna");
  assert(runtime.value().reasoning_effort == zed::core::ReasoningEffort::high);
  assert(runtime.value().max_turns == 41);
  assert(runtime.value().main_max_output_tokens == 2'048);
  assert(runtime.value().main_temperature == 0.3);
  assert(runtime.value().explorer.max_turns == 9);
  assert(runtime.value().explorer.system_prompt == "explorer fixture prompt");
  assert(runtime.value().subagent_execution.max_concurrency == 3);
  assert(runtime.value().subagent_execution.total_timeout_ms == 321'000);
  assert(runtime.value().context_model.model == "qwen3.8-flash");
  assert(runtime.value().context_limits.max_context_tokens == 700'000);
  assert(runtime.value().context_max_output_tokens == 1'536);
  assert(runtime.value().context_system_prompt == "context fixture prompt");
  assert(runtime.value().system_prompt == "main fixture prompt");
  assert(runtime.value().workspace_config_path ==
         std::filesystem::weakly_canonical(workspace) / ".zed" / "config.json");

  set_environment("ZED_MODEL", "muse-spark-1.2-contributor");
  set_environment("ZED_MAX_TURNS", "7");
  const auto overridden = zed::app::load_runtime_config();
  assert(overridden);
  assert(overridden.value().main_model.model == "muse-spark-1.2-contributor");
  assert(overridden.value().context_model.model == "qwen3.8-flash");
  assert(overridden.value().max_turns == 7);
  clear_environment("ZED_MODEL");
  clear_environment("ZED_MAX_TURNS");

  auto invalid_json = Json::parse(zed::app::serialize_workspace_config(config));
  invalid_json["unexpected"] = true;
  assert(!zed::app::parse_workspace_config(invalid_json.dump()));
  invalid_json.erase("unexpected");
  invalid_json["context"]["reserved_output_tokens"] = 700'000;
  assert(!zed::app::parse_workspace_config(invalid_json.dump()));
  assert(zed::app::parse_workspace_config(
      R"({"version":1,"agent":{"model":"gpt-5.6-luna","reasoning":"low","max_turns":32},"subagents":{"explorer":{"enabled":true,"model":"muse-spark-1.2-contributor","reasoning":"low","max_turns":12,"max_output_tokens":8192}},"context":{"model":"gpt-5.6-luna","max_tokens":1000000,"reserved_output_tokens":4096,"compaction_trigger_tokens":800000}})"));

  const auto catalog = zed::providers::default_opencode_go_models();
  zed::app::ConfigureWebServer server(workspace);
  const std::vector<zed::core::ToolDefinition> tool_catalog{
      {"read", "Read files.", R"({"type":"object"})"},
      {"subagent", "Delegate work.", R"({"type":"object"})"},
  };
  const auto opened = server.open(catalog, false, tool_catalog);
  assert(opened);
  const auto address = parse_url(opened.value());
  httplib::Client client("127.0.0.1", address.port);

  const auto forbidden = client.Get("/");
  assert(forbidden);
  assert(forbidden->status == 403);

  const auto page = client.Get(("/?token=" + address.token).c_str());
  assert(page);
  assert(page->status == 200);
  assert(page->body.find("zeda manager") != std::string::npos);
  assert(page->body.find("workspace control plane") == std::string::npos);
  assert(page->body.find("class=\"hint\"") == std::string::npos);
  assert(page->body.find("新增 Agent") != std::string::npos);
  assert(page->body.find("新增 Sub Agent") != std::string::npos);
  assert(page->body.find("新增 Skill") != std::string::npos);
  assert(page->get_header_value("Content-Security-Policy")
             .find("default-src 'none'") != std::string::npos);

  const auto current = client.Get("/api/config", headers(address));
  assert(current);
  assert(current->status == 200);
  assert(current->body.find("web-fixture-secret") == std::string::npos);
  const auto payload = Json::parse(current->body);
  assert(payload.at("workspace") == workspace.string());
  assert(payload.at("config").at("agent").at("max_turns") == 41);
  assert(payload.at("prompts").at("agent") == "main fixture prompt");
  assert(payload.at("prompts").at("explorer") == "explorer fixture prompt");
  assert(payload.at("management").at("active_agent") == "default");
  assert(payload.at("management").at("version") ==
         zed::app::kAgentManagementVersion);
  assert(payload.at("management").at("agents").size() == 1);
  assert(payload.at("management")
             .at("agents")
             .at(0)
             .at("automatic_context_compaction"));
  assert(payload.at("management").at("agents").at(0).at("tools") ==
         Json::array({"*"}));
  assert(payload.at("skills").empty());
  assert(payload.at("models").is_array());
  assert(!payload.at("models").empty());
  assert(payload.at("available_tools").size() == 2);

  auto posted = payload.at("config");
  posted["subagents"]["max_concurrency"] = 2;
  posted["context"]["max_output_tokens"] = 1'024;
  auto posted_prompts = payload.at("prompts");
  posted_prompts["explorer"] = "updated explorer prompt";
  posted_prompts["context"] = "updated context prompt";
  auto management = payload.at("management");
  auto business_agent = management.at("agents").at(0);
  business_agent["id"] = "business";
  business_agent["name"] = "Business Agent";
  business_agent["description"] = "Understand the business domain.";
  business_agent["max_turns"] = 17;
  business_agent["temperature"] = 0.7;
  business_agent["automatic_context_compaction"] = false;
  business_agent["compaction_trigger_tokens"] = 400'000;
  business_agent["tools"] = Json::array({"read", "subagent"});
  business_agent["system_prompt"] = "updated main prompt";
  management["agents"].push_back(business_agent);
  management["active_agent"] = "business";
  management["subagents"].push_back(
      {{"id", "reviewer"},
       {"description", "Review workspace evidence."},
       {"enabled", true},
       {"model", "muse-spark-1.2-contributor"},
       {"reasoning", "low"},
       {"max_turns", 8},
       {"max_output_tokens", 2'048},
       {"system_prompt", "reviewer system prompt"}});
  posted_prompts["agent"] = "updated main prompt";
  Json managed_skills =
      Json::array({{{"id", "domain"},
                    {"name", "domain-understanding"},
                    {"description", "Understand domain rules."},
                    {"instructions", "Read domain rules before coding."},
                    {"enabled", true}}});
  Json update{{"config", posted},
              {"prompts", posted_prompts},
              {"management", management},
              {"skills", managed_skills}};
  const auto saved = client.Post("/api/config", headers(address, true),
                                 update.dump(), "application/json");
  assert(saved);
  assert(saved->status == 200);
  assert(zed::app::load_workspace_config(workspace).value().agent.max_turns ==
         17);
  const auto saved_prompts = zed::app::load_workspace_prompts(workspace);
  assert(saved_prompts);
  assert(saved_prompts.value().agent == "updated main prompt");
  assert(saved_prompts.value().explorer == "updated explorer prompt");
  assert(saved_prompts.value().context == "updated context prompt");
  const auto saved_management = zed::app::load_agent_management(
      workspace, zed::app::load_workspace_config(workspace).value(),
      saved_prompts.value());
  assert(saved_management);
  assert(saved_management.value().active_agent == "business");
  assert(saved_management.value().agents.size() == 2);
  assert(!saved_management.value().agents[1].automatic_context_compaction);
  assert(saved_management.value().agents[1].tools ==
         (std::vector<std::string>{"read", "subagent"}));
  assert(saved_management.value().subagents.size() == 1);
  assert(saved_management.value().subagents[0].name == "reviewer");
  const auto saved_skills = zed::skills::load_workspace_skills(workspace);
  assert(saved_skills);
  assert(saved_skills.value().size() == 1);
  assert(saved_skills.value()[0].id == "domain");

  const auto managed_runtime = zed::app::load_runtime_config();
  assert(managed_runtime);
  assert(managed_runtime.value().max_turns == 17);
  assert(!managed_runtime.value().context_limits.automatic_compaction);
  assert(managed_runtime.value().context_limits.compaction_trigger_tokens ==
         400'000);
  assert(managed_runtime.value().agent_tools ==
         (std::vector<std::string>{"read", "subagent"}));
  assert(managed_runtime.value().system_prompt == "updated main prompt");
  assert(managed_runtime.value().subagents.size() == 2);
  assert(managed_runtime.value().subagents[1].name == "reviewer");
  assert(managed_runtime.value().subagents[1].system_prompt ==
         "reviewer system prompt");

  httplib::Headers wrong_origin = headers(address);
  wrong_origin.emplace("Origin", "https://example.invalid");
  const auto rejected_origin = client.Post("/api/config", wrong_origin,
                                           update.dump(), "application/json");
  assert(rejected_origin);
  assert(rejected_origin->status == 403);

  update["extra"] = true;
  const auto rejected_field = client.Post("/api/config", headers(address, true),
                                          update.dump(), "application/json");
  assert(rejected_field);
  assert(rejected_field->status == 400);
  update.erase("extra");
  update["prompts"]["context"] = "  \n";
  const auto rejected_prompt = client.Post(
      "/api/config", headers(address, true), update.dump(), "application/json");
  assert(rejected_prompt);
  assert(rejected_prompt->status == 400);

  const auto rejected_type =
      client.Post("/api/config", headers(address, true), "{}", "text/plain");
  assert(rejected_type);
  assert(rejected_type->status == 415);

  server.stop();

  const auto config_path = zed::app::workspace_config_path(workspace);
  const auto backup_path = config_path.string() + ".backup";
  std::filesystem::rename(config_path, backup_path);
  std::filesystem::create_symlink(backup_path, config_path);
  assert(!zed::app::save_workspace_config(workspace, config));
  std::filesystem::remove(config_path);
  std::filesystem::rename(backup_path, config_path);

  clear_environment("OPENCODE_GO_API_KEY");
  clear_environment("ZED_WORKSPACE");
  clear_environment("ZED_CONFIGURE_WEB_NO_BROWSER");
  std::filesystem::remove_all(workspace);
  return 0;
}
