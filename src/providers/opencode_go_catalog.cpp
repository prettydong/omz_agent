#include "zed/providers/opencode_go_catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace zed::providers {

namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxCatalogBytes = 4 * 1024 * 1024;

void terminate_catalog_process(pid_t child, int *status) {
  int local_status = 0;
  int *target_status = status == nullptr ? &local_status : status;
  if (kill(-child, SIGTERM) != 0)
    static_cast<void>(kill(child, SIGTERM));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = waitpid(child, target_status, WNOHANG);
    if (waited == child || (waited < 0 && errno == ECHILD))
      return;
    if (waited < 0 && errno != EINTR)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (kill(-child, SIGKILL) != 0)
    static_cast<void>(kill(child, SIGKILL));
  while (waitpid(child, target_status, 0) < 0 && errno == EINTR) {
  }
}

std::string_view trim_ascii(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                            value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return value;
}

OpenCodeProtocol protocol_from_npm(std::string_view npm) {
  if (npm == "@ai-sdk/openai")
    return OpenCodeProtocol::responses;
  if (npm == "@ai-sdk/anthropic")
    return OpenCodeProtocol::messages;
  return OpenCodeProtocol::chat_completions;
}

std::optional<core::ReasoningEffort> variant_effort(std::string_view name) {
  return core::reasoning_effort_from_name(name);
}

core::Result<std::string>
capture_catalog(std::string_view executable, std::size_t timeout_ms,
                core::CancellationToken cancellation) {
  if (executable.empty()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "OpenCode executable path cannot be empty",
    });
  }
  if (cancellation.is_cancelled()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::cancelled,
        "OpenCode model discovery cancelled",
    });
  }

  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    return core::Result<std::string>::failure({
        core::ErrorCode::internal,
        "cannot create OpenCode model discovery pipe",
    });
  }

  const pid_t child = fork();
  if (child == -1) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    return core::Result<std::string>::failure({
        core::ErrorCode::internal,
        "cannot start OpenCode model discovery",
    });
  }
  if (child == 0) {
    close(output_pipe[0]);
    setpgid(0, 0);
    dup2(output_pipe[1], STDOUT_FILENO);
    close(output_pipe[1]);
    const int error_fd = open("/dev/null", O_WRONLY);
    if (error_fd >= 0) {
      dup2(error_fd, STDERR_FILENO);
      close(error_fd);
    }
    std::string executable_copy(executable);
    std::vector<std::string> arguments{executable_copy, "models", "opencode-go",
                                       "--verbose"};
    std::vector<char *> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (auto &argument : arguments)
      raw_arguments.push_back(argument.data());
    raw_arguments.push_back(nullptr);
    execvp(raw_arguments[0], raw_arguments.data());
    _exit(127);
  }

  setpgid(child, child);
  close(output_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);
  if (flags < 0 || fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
    terminate_catalog_process(child, nullptr);
    close(output_pipe[0]);
    return core::Result<std::string>::failure({
        core::ErrorCode::internal,
        "cannot configure OpenCode model discovery output",
    });
  }

  std::string output;
  bool pipe_closed = false;
  bool child_finished = false;
  int status = 0;
  const auto started_at = std::chrono::steady_clock::now();
  while (!pipe_closed || !child_finished) {
    if (cancellation.is_cancelled()) {
      terminate_catalog_process(child, &status);
      close(output_pipe[0]);
      return core::Result<std::string>::failure({
          core::ErrorCode::cancelled,
          "OpenCode model discovery cancelled",
      });
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();
    if (elapsed >= static_cast<long long>(timeout_ms)) {
      terminate_catalog_process(child, &status);
      close(output_pipe[0]);
      return core::Result<std::string>::failure({
          core::ErrorCode::timeout,
          "OpenCode model discovery timed out",
      });
    }

    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    const int poll_result = poll(&descriptor, 1, 50);
    if (poll_result < 0 && errno != EINTR) {
      terminate_catalog_process(child, &status);
      close(output_pipe[0]);
      return core::Result<std::string>::failure({
          core::ErrorCode::internal,
          "cannot read OpenCode model discovery output",
      });
    }
    if (poll_result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
      char buffer[8192];
      while (true) {
        const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
        if (count > 0) {
          output.append(buffer, static_cast<std::size_t>(count));
          if (output.size() > kMaxCatalogBytes) {
            terminate_catalog_process(child, &status);
            close(output_pipe[0]);
            return core::Result<std::string>::failure({
                core::ErrorCode::internal,
                "OpenCode model discovery output exceeded 4 MiB",
            });
          }
          continue;
        }
        if (count == 0) {
          pipe_closed = true;
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        pipe_closed = true;
        break;
      }
    }

    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      child_finished = true;
  }
  close(output_pipe[0]);

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return core::Result<std::string>::failure({
        core::ErrorCode::not_found,
        "cannot run 'opencode models opencode-go --verbose'",
    });
  }
  return core::Result<std::string>::success(std::move(output));
}

OpenCodeGoModelInfo model(std::string id, std::string name,
                          OpenCodeProtocol protocol,
                          std::initializer_list<core::ReasoningEffort> efforts,
                          core::TokenCount context = 1'000'000,
                          std::size_t output = 131'072,
                          bool temperature = true) {
  return {std::move(id),
          std::move(name),
          protocol,
          context,
          output,
          temperature,
          std::vector<core::ReasoningEffort>(efforts)};
}

} // namespace

core::Result<std::vector<OpenCodeGoModelInfo>>
parse_opencode_go_models(std::string_view output) {
  constexpr std::string_view kPrefix = "opencode-go/";
  std::vector<OpenCodeGoModelInfo> models;
  std::size_t position = 0;
  while (position < output.size()) {
    const auto header = output.find(kPrefix, position);
    if (header == std::string_view::npos)
      break;
    if (header != 0 && output[header - 1] != '\n') {
      position = header + kPrefix.size();
      continue;
    }
    const auto header_end = output.find('\n', header);
    if (header_end == std::string_view::npos)
      break;
    const auto next_header = output.find("\nopencode-go/", header_end + 1);
    const auto json_end =
        next_header == std::string_view::npos ? output.size() : next_header;
    const auto model_id = trim_ascii(output.substr(
        header + kPrefix.size(), header_end - header - kPrefix.size()));
    const auto json_text =
        trim_ascii(output.substr(header_end + 1, json_end - header_end - 1));
    position =
        next_header == std::string_view::npos ? output.size() : next_header + 1;
    if (model_id.empty() || json_text.empty())
      continue;

    Json value;
    try {
      value = Json::parse(json_text);
    } catch (const Json::parse_error &error) {
      return core::Result<std::vector<OpenCodeGoModelInfo>>::failure({
          core::ErrorCode::invalid_argument,
          "cannot parse OpenCode metadata for " + std::string(model_id) + ": " +
              error.what(),
      });
    }
    if (!value.is_object())
      continue;

    OpenCodeGoModelInfo info;
    info.id = value.value("id", std::string(model_id));
    info.name = value.value("name", info.id);
    if (const auto api = value.find("api");
        api != value.end() && api->is_object()) {
      info.protocol = protocol_from_npm(api->value("npm", std::string{}));
    } else {
      info.protocol = infer_open_code_protocol(info.id);
    }
    if (const auto limit = value.find("limit");
        limit != value.end() && limit->is_object()) {
      info.max_context_tokens = limit->value("context", core::TokenCount{});
      info.max_output_tokens = limit->value("output", std::size_t{});
    }
    if (const auto capabilities = value.find("capabilities");
        capabilities != value.end() && capabilities->is_object()) {
      info.supports_temperature = capabilities->value("temperature", true);
    }
    if (const auto variants = value.find("variants");
        variants != value.end() && variants->is_object()) {
      for (auto iterator = variants->begin(); iterator != variants->end();
           ++iterator) {
        if (const auto effort = variant_effort(iterator.key());
            effort.has_value()) {
          info.reasoning_efforts.push_back(*effort);
        }
      }
    }
    models.push_back(std::move(info));
  }

  if (models.empty()) {
    return core::Result<std::vector<OpenCodeGoModelInfo>>::failure({
        core::ErrorCode::not_found,
        "OpenCode returned no opencode-go model metadata",
    });
  }
  return core::Result<std::vector<OpenCodeGoModelInfo>>::success(
      std::move(models));
}

core::Result<std::vector<OpenCodeGoModelInfo>>
discover_opencode_go_models(std::string_view executable, std::size_t timeout_ms,
                            core::CancellationToken cancellation) {
  const auto output = capture_catalog(executable, timeout_ms, cancellation);
  if (!output)
    return core::Result<std::vector<OpenCodeGoModelInfo>>::failure(
        output.error());
  return parse_opencode_go_models(output.value());
}

std::vector<OpenCodeGoModelInfo> default_opencode_go_models() {
  using Effort = core::ReasoningEffort;
  using Protocol = OpenCodeProtocol;
  return {
      model("gpt-5.6-luna", "GPT-5.6 Luna", Protocol::responses,
            {Effort::none, Effort::low, Effort::medium, Effort::high,
             Effort::xhigh, Effort::max},
            1'050'000, 128'000, false),
      model("grok-4.6", "Grok 4.6", Protocol::responses,
            {Effort::low, Effort::medium, Effort::high, Effort::xhigh}, 500'000,
            500'000),
      model("muse-spark-1.2-contributor", "Muse Spark 1.2 Contributor",
            Protocol::responses,
            {Effort::minimal, Effort::low, Effort::medium, Effort::high,
             Effort::xhigh},
            1'048'576, 131'072),
      model("deepseek-v4-flash", "DeepSeek V4 Flash",
            Protocol::chat_completions,
            {Effort::low, Effort::high, Effort::max}),
      model("deepseek-v4-flash-vision-exp", "DeepSeek V4 Flash Vision Exp",
            Protocol::chat_completions,
            {Effort::low, Effort::high, Effort::max}),
      model("deepseek-v4-pro", "DeepSeek V4 Pro", Protocol::chat_completions,
            {Effort::high, Effort::max}),
      model("glm-5.1", "GLM-5.1", Protocol::chat_completions, {}, 202'752,
            32'768),
      model("glm-5.2", "GLM-5.2", Protocol::chat_completions,
            {Effort::high, Effort::max}),
      model("glm-5.3", "GLM-5.3", Protocol::chat_completions,
            {Effort::low, Effort::high, Effort::max}),
      model("glm-5.3-flash", "GLM-5.3 Flash", Protocol::chat_completions,
            {Effort::low, Effort::high, Effort::max}),
      model("hy3", "Hy3", Protocol::chat_completions,
            {Effort::none, Effort::low, Effort::high}, 256'000, 64'000),
      model("hy4-preview", "Hy4 Preview", Protocol::chat_completions,
            {Effort::none, Effort::high}, 1'024'000, 64'000),
      model("kimi-k2.6", "Kimi K2.6", Protocol::chat_completions, {}, 262'144,
            65'536),
      model("kimi-k2.7-code", "Kimi K2.7 Code", Protocol::chat_completions, {},
            262'144, 262'144, false),
      model("kimi-k3", "Kimi K3", Protocol::chat_completions, {Effort::max},
            1'048'576, 131'072, false),
      model("longcat-2.0", "LongCat 2.0", Protocol::chat_completions,
            {Effort::low, Effort::medium, Effort::high}),
      model("mimo-v2.5", "MiMo V2.5", Protocol::chat_completions, {}),
      model("mimo-v2.5-pro", "MiMo V2.5 Pro", Protocol::chat_completions, {},
            1'048'576, 128'000),
      model("minimax-m2.7", "MiniMax M2.7", Protocol::messages, {}, 204'800),
      model("minimax-m3", "MiniMax M3", Protocol::messages,
            {Effort::none, Effort::thinking}),
      model("qwen3.6-plus", "Qwen3.6 Plus", Protocol::messages, {}),
      model("qwen3.7-max", "Qwen3.7 Max", Protocol::messages, {}),
      model("qwen3.7-plus", "Qwen3.7 Plus", Protocol::messages, {}),
      model("qwen3.8-flash", "Qwen3.8 Flash", Protocol::messages,
            {Effort::low, Effort::medium, Effort::xhigh}),
      model("qwen3.8-max", "Qwen3.8 Max", Protocol::messages,
            {Effort::low, Effort::medium, Effort::xhigh}),
  };
}

const OpenCodeGoModelInfo *
find_opencode_go_model(const std::vector<OpenCodeGoModelInfo> &models,
                       std::string_view id) {
  const auto iterator =
      std::find_if(models.begin(), models.end(),
                   [&](const auto &model_info) { return model_info.id == id; });
  return iterator == models.end() ? nullptr : &*iterator;
}

OpenCodeProtocol infer_open_code_protocol(std::string_view id) {
  if (id.starts_with("gpt-") || id.starts_with("grok-") ||
      id.starts_with("muse-spark-")) {
    return OpenCodeProtocol::responses;
  }
  if (id.starts_with("minimax-") || id.starts_with("qwen3."))
    return OpenCodeProtocol::messages;
  return OpenCodeProtocol::chat_completions;
}

bool supports_reasoning_effort(const OpenCodeGoModelInfo &model,
                               core::ReasoningEffort effort) {
  if (effort == core::ReasoningEffort::automatic)
    return true;
  return std::find(model.reasoning_efforts.begin(),
                   model.reasoning_efforts.end(),
                   effort) != model.reasoning_efforts.end();
}

} // namespace zed::providers
