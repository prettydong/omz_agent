#include "zed/app/configure_web.hpp"

#include "zed/app/config.hpp"
#include "zed/app/configure_web_page.hpp"
#include "zed/skills/skill_registry.hpp"
#include "zed/support/child_process.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <string_view>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace zed::app {

namespace {

using Json = nlohmann::json;

struct ConfigurationUpdate {
  WorkspaceConfig config;
  WorkspacePrompts prompts;
  AgentManagementConfig management;
  std::vector<skills::ManagedSkill> skills;
};

core::Error invalid_request(std::string message) {
  return {core::ErrorCode::invalid_argument,
          "invalid configuration request: " + std::move(message)};
}

core::Result<void>
exact_object_fields(std::string_view name, const Json &object,
                    std::initializer_list<std::string_view> fields) {
  if (!object.is_object()) {
    return core::Result<void>::failure(
        invalid_request(std::string(name) + " must be an object"));
  }
  for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
    if (std::find(fields.begin(), fields.end(), iterator.key()) ==
        fields.end()) {
      return core::Result<void>::failure(
          invalid_request(std::string(name) + " contains unknown field '" +
                          iterator.key() + "'"));
    }
  }
  for (const auto field : fields) {
    if (!object.contains(field)) {
      return core::Result<void>::failure(
          invalid_request(std::string(name) + " is missing field '" +
                          std::string(field) + "'"));
    }
  }
  return core::Result<void>::success();
}

core::Result<ConfigurationUpdate>
parse_configuration_update(std::string_view body) {
  const auto root = Json::parse(body, nullptr, false);
  if (root.is_discarded()) {
    return core::Result<ConfigurationUpdate>::failure(
        invalid_request("body is not valid JSON"));
  }
  const auto root_shape = exact_object_fields(
      "root", root, {"config", "prompts", "management", "skills"});
  if (!root_shape)
    return core::Result<ConfigurationUpdate>::failure(root_shape.error());

  const auto config = parse_workspace_config(root.at("config").dump());
  if (!config)
    return core::Result<ConfigurationUpdate>::failure(config.error());

  const auto &prompt_json = root.at("prompts");
  const auto prompt_shape = exact_object_fields(
      "prompts", prompt_json, {"agent", "explorer", "context"});
  if (!prompt_shape)
    return core::Result<ConfigurationUpdate>::failure(prompt_shape.error());
  for (const auto field : {"agent", "explorer", "context"}) {
    if (!prompt_json.at(field).is_string()) {
      return core::Result<ConfigurationUpdate>::failure(invalid_request(
          "prompts." + std::string(field) + " must be a string"));
    }
  }
  WorkspacePrompts prompts{
      prompt_json.at("agent").get<std::string>(),
      prompt_json.at("explorer").get<std::string>(),
      prompt_json.at("context").get<std::string>(),
  };
  const auto valid_prompts = validate_workspace_prompts(prompts);
  if (!valid_prompts) {
    return core::Result<ConfigurationUpdate>::failure(valid_prompts.error());
  }
  const auto management =
      parse_agent_management_config(root.at("management").dump());
  if (!management) {
    return core::Result<ConfigurationUpdate>::failure(management.error());
  }
  if (!root.at("skills").is_array()) {
    return core::Result<ConfigurationUpdate>::failure(
        invalid_request("skills must be an array"));
  }
  std::vector<skills::ManagedSkill> managed_skills;
  managed_skills.reserve(root.at("skills").size());
  for (std::size_t index = 0; index < root.at("skills").size(); ++index) {
    const auto &value = root.at("skills").at(index);
    const auto context = "skills[" + std::to_string(index) + "]";
    const auto skill_shape = exact_object_fields(
        context, value,
        {"id", "name", "description", "instructions", "enabled"});
    if (!skill_shape)
      return core::Result<ConfigurationUpdate>::failure(skill_shape.error());
    for (const auto field : {"id", "name", "description", "instructions"}) {
      if (!value.at(field).is_string()) {
        return core::Result<ConfigurationUpdate>::failure(
            invalid_request(context + "." + field + " must be a string"));
      }
    }
    if (!value.at("enabled").is_boolean()) {
      return core::Result<ConfigurationUpdate>::failure(
          invalid_request(context + ".enabled must be a boolean"));
    }
    managed_skills.push_back({
        value.at("id").get<std::string>(),
        value.at("name").get<std::string>(),
        value.at("description").get<std::string>(),
        value.at("instructions").get<std::string>(),
        value.at("enabled").get<bool>(),
    });
  }
  const auto valid_skills = skills::validate_workspace_skills(managed_skills);
  if (!valid_skills) {
    return core::Result<ConfigurationUpdate>::failure(valid_skills.error());
  }

  auto final_config = config.value();
  auto final_prompts = std::move(prompts);
  const auto active = std::find_if(
      management.value().agents.begin(), management.value().agents.end(),
      [&](const AgentProfile &profile) {
        return profile.id == management.value().active_agent;
      });
  if (active == management.value().agents.end()) {
    return core::Result<ConfigurationUpdate>::failure(
        invalid_request("active Agent does not exist"));
  }
  final_config.agent = active->config;
  final_config.context.limits.compaction_trigger_tokens =
      active->compaction_trigger_tokens;
  const auto available_context_tokens =
      final_config.context.limits.max_context_tokens -
      final_config.context.limits.reserved_output_tokens;
  if (active->compaction_trigger_tokens > available_context_tokens) {
    return core::Result<ConfigurationUpdate>::failure(invalid_request(
        "active Agent compaction trigger exceeds its context budget"));
  }
  final_prompts.agent = active->system_prompt;
  return core::Result<ConfigurationUpdate>::success(
      {std::move(final_config), std::move(final_prompts), management.value(),
       std::move(managed_skills)});
}

std::string random_token() {
  std::random_device random;
  std::ostringstream token;
  token << std::hex << std::setfill('0');
  for (int index = 0; index < 4; ++index)
    token << std::setw(8) << random();
  return token.str();
}

void launch_url(std::string_view url) {
  const char *disabled = std::getenv("ZED_CONFIGURE_WEB_NO_BROWSER");
  if (disabled != nullptr && std::string_view(disabled) == "1")
    return;
#if defined(__APPLE__) || defined(__linux__)
  const std::string target(url);
  auto spawn_lock = zed::support::lock_process_spawn();
  zed::support::SpawnOptions spawn_options;
#if defined(__APPLE__)
  spawn_options.executable = "open";
#else
  spawn_options.executable = "xdg-open";
#endif
  spawn_options.arguments = {target};
  spawn_options.additional_environment_variables = {
      "DISPLAY",
      "WAYLAND_DISPLAY",
      "DBUS_SESSION_BUS_ADDRESS",
      "XAUTHORITY",
  };
  pid_t child = -1;
  if (zed::support::spawn_process(spawn_options, child) != 0)
    return;
  spawn_lock.unlock();
  std::thread([child] {
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
#else
  static_cast<void>(url);
#endif
}

void secure_headers(httplib::Response &response) {
  response.set_header("Cache-Control", "no-store");
  response.set_header("Content-Security-Policy",
                      "default-src 'none'; script-src 'unsafe-inline'; "
                      "style-src 'unsafe-inline'; connect-src 'self'; "
                      "img-src 'self' data:; base-uri 'none'; form-action "
                      "'none'; frame-ancestors 'none'");
  response.set_header("Referrer-Policy", "no-referrer");
  response.set_header("X-Content-Type-Options", "nosniff");
  response.set_header("X-Frame-Options", "DENY");
}

void json_response(httplib::Response &response, int status, const Json &body) {
  response.status = status;
  response.set_content(body.dump(), "application/json; charset=utf-8");
  secure_headers(response);
}

core::Result<void> validate_catalog_selection(
    const WorkspaceConfig &config,
    const std::vector<providers::OpenCodeGoModelInfo> &models) {
  const auto find = [&](std::string_view id) {
    return providers::find_opencode_go_model(models, id);
  };
  const auto *agent = find(config.agent.model.model);
  if (agent == nullptr) {
    return core::Result<void>::failure({
        core::ErrorCode::not_found,
        "main Agent model is not present in the current model catalog: " +
            config.agent.model.model,
    });
  }
  if (!providers::supports_reasoning_effort(*agent,
                                            config.agent.reasoning_effort)) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "main Agent model does not support reasoning '" +
            std::string(
                core::reasoning_effort_name(config.agent.reasoning_effort)) +
            "'",
    });
  }
  if (config.agent.max_output_tokens > 0 && agent->max_output_tokens > 0 &&
      config.agent.max_output_tokens > agent->max_output_tokens) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "main Agent output limit exceeds the selected model maximum of " +
            std::to_string(agent->max_output_tokens),
    });
  }
  if (config.explorer.enabled) {
    const auto *explorer = find(config.explorer.model.model);
    if (explorer == nullptr) {
      return core::Result<void>::failure({
          core::ErrorCode::not_found,
          "Explorer model is not present in the current model catalog: " +
              config.explorer.model.model,
      });
    }
    if (!providers::supports_reasoning_effort(
            *explorer, config.explorer.reasoning_effort)) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "Explorer model does not support reasoning '" +
              std::string(core::reasoning_effort_name(
                  config.explorer.reasoning_effort)) +
              "'",
      });
    }
    if (explorer->max_output_tokens > 0 &&
        config.explorer.max_output_tokens > explorer->max_output_tokens) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "Explorer output limit exceeds the selected model maximum of " +
              std::to_string(explorer->max_output_tokens),
      });
    }
  }
  const auto *context = find(config.context.model.model);
  if (context == nullptr) {
    return core::Result<void>::failure({
        core::ErrorCode::not_found,
        "context model is not present in the current model catalog: " +
            config.context.model.model,
    });
  }
  if (context->max_output_tokens > 0 &&
      config.context.max_output_tokens > context->max_output_tokens) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "context output limit exceeds the selected model maximum of " +
            std::to_string(context->max_output_tokens),
    });
  }
  return core::Result<void>::success();
}

core::Result<void> validate_management_catalog(
    const AgentManagementConfig &management,
    const std::vector<providers::OpenCodeGoModelInfo> &models) {
  const auto validate = [&](std::string_view role, const core::ModelRef &model,
                            core::ReasoningEffort reasoning,
                            std::size_t max_output_tokens) {
    const auto *info = providers::find_opencode_go_model(models, model.model);
    if (info == nullptr) {
      return core::Result<void>::failure({
          core::ErrorCode::not_found,
          std::string(role) +
              " model is not present in the current catalog: " + model.model,
      });
    }
    if (!providers::supports_reasoning_effort(*info, reasoning)) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          std::string(role) + " model does not support reasoning '" +
              std::string(core::reasoning_effort_name(reasoning)) + "'",
      });
    }
    if (max_output_tokens > 0 && info->max_output_tokens > 0 &&
        max_output_tokens > info->max_output_tokens) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          std::string(role) +
              " output limit exceeds the selected model "
              "maximum of " +
              std::to_string(info->max_output_tokens),
      });
    }
    return core::Result<void>::success();
  };
  for (const auto &agent : management.agents) {
    const auto valid =
        validate("Agent '" + agent.id + "'", agent.config.model,
                 agent.config.reasoning_effort, agent.config.max_output_tokens);
    if (!valid)
      return valid;
  }
  for (const auto &subagent : management.subagents) {
    if (!subagent.enabled)
      continue;
    const auto valid =
        validate("Sub Agent '" + subagent.name + "'", subagent.model,
                 subagent.reasoning_effort, subagent.max_output_tokens);
    if (!valid)
      return valid;
  }
  return core::Result<void>::success();
}

Json model_json(const providers::OpenCodeGoModelInfo &model) {
  Json reasoning = Json::array();
  for (const auto effort : model.reasoning_efforts)
    reasoning.push_back(core::reasoning_effort_name(effort));
  return {
      {"id", model.id},
      {"name", model.name},
      {"protocol", providers::open_code_protocol_name(model.protocol)},
      {"max_context_tokens", model.max_context_tokens},
      {"max_output_tokens", model.max_output_tokens},
      {"reasoning", std::move(reasoning)},
  };
}

} // namespace

class ConfigureWebServer::Impl {
public:
  explicit Impl(std::filesystem::path workspace)
      : workspace_(std::move(workspace)) {}

  ~Impl() { stop(); }

  core::Result<std::string>
  open(const std::vector<providers::OpenCodeGoModelInfo> &models,
       bool open_browser, const std::vector<core::ToolDefinition> &tools) {
    try {
      std::scoped_lock server_lock(server_mutex_);
      {
        std::scoped_lock models_lock(models_mutex_);
        models_ = models;
        tools_ = tools;
      }
      if (models.empty()) {
        return core::Result<std::string>::failure({
            core::ErrorCode::not_found,
            "cannot open configuration page without a model catalog",
        });
      }
      if (server_ == nullptr && !start_server()) {
        return core::Result<std::string>::failure({
            core::ErrorCode::internal,
            "cannot bind the configuration server to 127.0.0.1",
        });
      }
      const auto target = url();
      if (open_browser)
        launch_url(target);
      return core::Result<std::string>::success(target);
    } catch (const std::exception &error) {
      std::scoped_lock lock(server_mutex_);
      stop_locked();
      return core::Result<std::string>::failure({
          core::ErrorCode::internal,
          "cannot start the configuration server: " + std::string(error.what()),
      });
    } catch (...) {
      std::scoped_lock lock(server_mutex_);
      stop_locked();
      return core::Result<std::string>::failure({
          core::ErrorCode::internal,
          "cannot start the configuration server",
      });
    }
  }

  void stop() {
    std::scoped_lock lock(server_mutex_);
    stop_locked();
  }

private:
  void stop_locked() {
    if (server_ != nullptr)
      server_->stop();
    if (server_thread_.joinable())
      server_thread_.join();
    server_.reset();
    port_ = 0;
    token_.clear();
  }

  bool authorized(const httplib::Request &request) const {
    if (request.has_param("token") &&
        request.get_param_value("token") == token_) {
      return true;
    }
    return request.get_header_value("X-Zeda-Config-Token") == token_;
  }

  bool origin_allowed(const httplib::Request &request) const {
    const auto origin = request.get_header_value("Origin");
    if (origin.empty())
      return true;
    return origin == "http://127.0.0.1:" + std::to_string(port_) ||
           origin == "http://localhost:" + std::to_string(port_);
  }

  std::string url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/?token=" + token_;
  }

  std::vector<providers::OpenCodeGoModelInfo> models() const {
    std::scoped_lock lock(models_mutex_);
    return models_;
  }

  std::vector<core::ToolDefinition> tools() const {
    std::scoped_lock lock(models_mutex_);
    return tools_;
  }

  bool start_server() {
    token_ = random_token();
    server_ = std::make_unique<httplib::Server>();
    server_->set_payload_max_length(32 * 1024 * 1024);
    server_->Get("/", [this](const httplib::Request &request,
                             httplib::Response &response) {
      if (!authorized(request)) {
        json_response(response, 403, {{"error", "invalid access token"}});
        return;
      }
      response.set_content(std::string(kConfigureWebPage),
                           "text/html; charset=utf-8");
      secure_headers(response);
    });
    server_->Get("/api/config", [this](const httplib::Request &request,
                                       httplib::Response &response) {
      if (!authorized(request)) {
        json_response(response, 403, {{"error", "invalid access token"}});
        return;
      }
      const auto loaded = load_workspace_config(workspace_);
      if (!loaded) {
        json_response(response, 500, {{"error", loaded.error().message}});
        return;
      }
      const auto prompts = load_workspace_prompts(workspace_);
      if (!prompts) {
        json_response(response, 500, {{"error", prompts.error().message}});
        return;
      }
      const auto management =
          load_agent_management(workspace_, loaded.value(), prompts.value());
      if (!management) {
        json_response(response, 500, {{"error", management.error().message}});
        return;
      }
      const auto managed_skills = skills::load_workspace_skills(workspace_);
      if (!managed_skills) {
        json_response(response, 500,
                      {{"error", managed_skills.error().message}});
        return;
      }
      auto config_json = Json::parse(serialize_workspace_config(loaded.value()),
                                     nullptr, false);
      auto management_json =
          Json::parse(serialize_agent_management_config(management.value()),
                      nullptr, false);
      Json skills_json = Json::array();
      for (const auto &skill : managed_skills.value()) {
        skills_json.push_back({
            {"id", skill.id},
            {"name", skill.name},
            {"description", skill.description},
            {"instructions", skill.instructions},
            {"enabled", skill.enabled},
        });
      }
      Json catalog = Json::array();
      for (const auto &model : models())
        catalog.push_back(model_json(model));
      Json tool_catalog = Json::array();
      for (const auto &tool : tools()) {
        tool_catalog.push_back(
            {{"id", tool.name}, {"description", tool.description}});
      }
      json_response(
          response, 200,
          {{"workspace", workspace_.string()},
           {"config_path", workspace_config_path(workspace_).string()},
           {"restart_required", true},
           {"config", std::move(config_json)},
           {"management", std::move(management_json)},
           {"skills", std::move(skills_json)},
           {"prompts",
            {{"agent", prompts.value().agent},
             {"explorer", prompts.value().explorer},
             {"context", prompts.value().context}}},
           {"models", std::move(catalog)},
           {"available_tools", std::move(tool_catalog)}});
    });
    server_->Post("/api/config", [this](const httplib::Request &request,
                                        httplib::Response &response) {
      if (!authorized(request) || !origin_allowed(request)) {
        json_response(response, 403, {{"error", "request is not authorized"}});
        return;
      }
      const auto content_type = request.get_header_value("Content-Type");
      if (!std::string_view(content_type).starts_with("application/json")) {
        json_response(response, 415,
                      {{"error", "Content-Type must be application/json"}});
        return;
      }
      const auto update = parse_configuration_update(request.body);
      if (!update) {
        json_response(response, 400, {{"error", update.error().message}});
        return;
      }
      const auto catalog_valid =
          validate_catalog_selection(update.value().config, models());
      if (!catalog_valid) {
        json_response(response, 400,
                      {{"error", catalog_valid.error().message}});
        return;
      }
      const auto management_catalog_valid =
          validate_management_catalog(update.value().management, models());
      if (!management_catalog_valid) {
        json_response(response, 400,
                      {{"error", management_catalog_valid.error().message}});
        return;
      }
      const auto saved_prompts =
          save_workspace_prompts(workspace_, update.value().prompts);
      if (!saved_prompts) {
        json_response(response, 500,
                      {{"error", saved_prompts.error().message}});
        return;
      }
      const auto saved_config =
          save_workspace_config(workspace_, update.value().config);
      if (!saved_config) {
        json_response(response, 500, {{"error", saved_config.error().message}});
        return;
      }
      const auto saved_management =
          save_agent_management(workspace_, update.value().management);
      if (!saved_management) {
        json_response(response, 500,
                      {{"error", saved_management.error().message}});
        return;
      }
      const auto saved_skills =
          skills::save_workspace_skills(workspace_, update.value().skills);
      if (!saved_skills) {
        json_response(response, 500, {{"error", saved_skills.error().message}});
        return;
      }
      json_response(response, 200,
                    {{"saved", true}, {"restart_required", true}});
    });
    server_->set_error_handler(
        [](const httplib::Request &, httplib::Response &response) {
          if (response.status == 404)
            json_response(response, 404, {{"error", "not found"}});
          else
            secure_headers(response);
        });
    server_->set_exception_handler([](const httplib::Request &,
                                      httplib::Response &response,
                                      std::exception_ptr) {
      json_response(response, 500, {{"error", "configuration server error"}});
    });
    port_ = server_->bind_to_any_port("127.0.0.1");
    if (port_ <= 0) {
      server_.reset();
      return false;
    }
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    return true;
  }

  std::filesystem::path workspace_;
  mutable std::mutex models_mutex_;
  std::vector<providers::OpenCodeGoModelInfo> models_;
  std::vector<core::ToolDefinition> tools_;
  std::mutex server_mutex_;
  std::unique_ptr<httplib::Server> server_;
  std::thread server_thread_;
  int port_{};
  std::string token_;
};

ConfigureWebServer::ConfigureWebServer(std::filesystem::path workspace)
    : impl_(std::make_unique<Impl>(std::move(workspace))) {}

ConfigureWebServer::~ConfigureWebServer() = default;

core::Result<std::string> ConfigureWebServer::open(
    const std::vector<providers::OpenCodeGoModelInfo> &models,
    bool launch_browser, const std::vector<core::ToolDefinition> &tools) {
  return impl_->open(models, launch_browser, tools);
}

void ConfigureWebServer::stop() { impl_->stop(); }

} // namespace zed::app
