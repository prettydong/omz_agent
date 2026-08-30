#include "zed/app/config.hpp"

#include "zed/core/default_system_prompt.hpp"
#include "zed/core/utf8.hpp"
#include "zed/session/session_catalog.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

namespace zed::app {

namespace {

constexpr std::string_view kSystemPromptRelativePath =
    ".zed/zed_system_propmt.md";
constexpr std::uintmax_t kMaxSystemPromptBytes = 1024 * 1024;

std::string environment_or(const char *name, std::string fallback = {}) {
  const char *value = std::getenv(name);
  return value == nullptr ? std::move(fallback) : std::string(value);
}

std::filesystem::path opencode_auth_path() {
  const std::string configured = environment_or("ZED_OPENCODE_AUTH_PATH");
  if (!configured.empty())
    return configured;

  const std::string data_home = environment_or("XDG_DATA_HOME");
  if (!data_home.empty())
    return std::filesystem::path(data_home) / "opencode" / "auth.json";

  const std::string home = environment_or("HOME");
  if (!home.empty())
    return std::filesystem::path(home) / ".local" / "share" / "opencode" /
           "auth.json";

  return {};
}

core::Result<std::string> load_opencode_go_api_key() {
  const std::string environment_key = environment_or("OPENCODE_GO_API_KEY");
  if (!environment_key.empty())
    return core::Result<std::string>::success(environment_key);

  const std::filesystem::path auth_path = opencode_auth_path();
  if (auth_path.empty()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OPENCODE_GO_API_KEY is not set and the OpenCode credential path "
        "cannot be resolved",
    });
  }

  std::ifstream input(auth_path);
  if (!input) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OPENCODE_GO_API_KEY is not set and the OpenCode Go credential "
        "cannot be read from " +
            auth_path.string(),
    });
  }

  const nlohmann::json credentials =
      nlohmann::json::parse(input, nullptr, false);
  if (credentials.is_discarded() || !credentials.is_object()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot parse OpenCode credentials from " + auth_path.string(),
    });
  }

  const auto provider = credentials.find("opencode-go");
  if (provider == credentials.end() || !provider->is_object()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OpenCode Go credential is missing from " + auth_path.string(),
    });
  }

  const auto key = provider->find("key");
  if (key == provider->end() || !key->is_string() || key->empty()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OpenCode Go credential has no API key in " + auth_path.string(),
    });
  }

  return core::Result<std::string>::success(key->get<std::string>());
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
      "ZED_REASONING_EFFORT must be one of: auto, none, minimal, low, medium, "
      "high, xhigh, max, thinking",
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

core::Result<std::string>
load_workspace_system_prompt(const std::filesystem::path &path) {
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error == std::errc::no_such_file_or_directory ||
      (!status_error && !std::filesystem::exists(status))) {
    const auto parent = path.parent_path();
    std::error_code create_error;
    std::filesystem::create_directories(parent, create_error);
    if (create_error) {
      return core::Result<std::string>::failure({
          core::ErrorCode::invalid_argument,
          "cannot create system prompt directory " + parent.string() + ": " +
              create_error.message(),
      });
    }

    std::error_code parent_status_error;
    const auto parent_status =
        std::filesystem::symlink_status(parent, parent_status_error);
    if (parent_status_error || std::filesystem::is_symlink(parent_status) ||
        !std::filesystem::is_directory(parent_status)) {
      return core::Result<std::string>::failure({
          core::ErrorCode::invalid_argument,
          "system prompt directory must be a regular directory, not a "
          "symlink: " +
              parent.string(),
      });
    }

    std::ofstream output(path, std::ios::binary);
    if (!output) {
      return core::Result<std::string>::failure({
          core::ErrorCode::invalid_argument,
          "cannot install default system prompt file " + path.string(),
      });
    }
    output.write(
        core::kDefaultSystemPrompt.data(),
        static_cast<std::streamsize>(core::kDefaultSystemPrompt.size()));
    output.close();
    if (!output) {
      std::error_code remove_error;
      std::filesystem::remove(path, remove_error);
      return core::Result<std::string>::failure({
          core::ErrorCode::invalid_argument,
          "cannot finish installing default system prompt file " +
              path.string(),
      });
    }
    return core::Result<std::string>::success(
        std::string(core::kDefaultSystemPrompt));
  }
  if (status_error) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot inspect system prompt file " + path.string() + ": " +
            status_error.message(),
    });
  }
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "system prompt path must be a regular file, not a symlink: " +
            path.string(),
    });
  }

  std::error_code size_error;
  const auto size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot inspect system prompt size " + path.string() + ": " +
            size_error.message(),
    });
  }
  if (size > kMaxSystemPromptBytes) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "system prompt file exceeds 1 MiB: " + path.string(),
    });
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot read system prompt file " + path.string(),
    });
  }
  std::string prompt{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
  if (input.bad()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot finish reading system prompt file " + path.string(),
    });
  }
  if (prompt.starts_with("\xEF\xBB\xBF"))
    prompt.erase(0, 3);
  if (!core::is_valid_utf8(prompt)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "system prompt file is not valid UTF-8: " + path.string(),
    });
  }
  if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "system prompt file cannot be empty: " + path.string(),
    });
  }
  return core::Result<std::string>::success(std::move(prompt));
}

} // namespace

core::Result<RuntimeConfig>
load_runtime_config(RuntimeConfigLoadOptions options) {
  RuntimeConfig config;
  const auto api_key = load_opencode_go_api_key();
  if (!api_key)
    return core::Result<RuntimeConfig>::failure(api_key.error());
  config.opencode_go_api_key = api_key.value();

  config.opencode_endpoint =
      environment_or("ZED_OPENCODE_ENDPOINT", "https://opencode.ai/zen/go/v1");
  config.opencode_path = environment_or("ZED_OPENCODE_PATH", "opencode");
  if (config.opencode_path.empty()) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_OPENCODE_PATH cannot be empty",
    });
  }
  config.clangd_path = environment_or("ZED_CLANGD_PATH", "clangd");
  if (config.clangd_path.empty()) {
    return core::Result<RuntimeConfig>::failure({
        core::ErrorCode::invalid_argument,
        "ZED_CLANGD_PATH cannot be empty",
    });
  }

  config.workspace =
      environment_or("ZED_WORKSPACE", std::filesystem::current_path().string());
  config.workspace = std::filesystem::weakly_canonical(config.workspace);
  config.system_prompt_path = config.workspace / kSystemPromptRelativePath;
  if (options.load_workspace_system_prompt) {
    const auto system_prompt =
        load_workspace_system_prompt(config.system_prompt_path);
    if (!system_prompt)
      return core::Result<RuntimeConfig>::failure(system_prompt.error());
    config.system_prompt = system_prompt.value();
  }
  const auto configured_session = environment_or("ZED_SESSION_PATH");
  config.session_path = configured_session.empty()
                            ? zed::session::new_session_path(
                                  config.workspace / ".zed" / "sessions")
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
