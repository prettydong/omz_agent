#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/model.hpp"
#include "zed/core/result.hpp"

namespace zed::providers {

enum class OpenCodeProtocol {
  responses,
  chat_completions,
  messages,
};

[[nodiscard]] constexpr std::string_view
open_code_protocol_name(OpenCodeProtocol protocol) {
  switch (protocol) {
  case OpenCodeProtocol::responses:
    return "responses";
  case OpenCodeProtocol::chat_completions:
    return "chat-completions";
  case OpenCodeProtocol::messages:
    return "messages";
  }
  return "chat-completions";
}

struct OpenCodeGoModelInfo {
  std::string id;
  std::string name;
  OpenCodeProtocol protocol{OpenCodeProtocol::chat_completions};
  core::TokenCount max_context_tokens{};
  std::size_t max_output_tokens{};
  bool supports_temperature{true};
  std::vector<core::ReasoningEffort> reasoning_efforts;
};

[[nodiscard]] core::Result<std::vector<OpenCodeGoModelInfo>>
parse_opencode_go_models(std::string_view output);

[[nodiscard]] core::Result<std::vector<OpenCodeGoModelInfo>>
discover_opencode_go_models(std::string_view executable = "opencode",
                            std::size_t timeout_ms = 5'000,
                            core::CancellationToken cancellation = {});

[[nodiscard]] std::vector<OpenCodeGoModelInfo> default_opencode_go_models();

[[nodiscard]] const OpenCodeGoModelInfo *
find_opencode_go_model(const std::vector<OpenCodeGoModelInfo> &models,
                       std::string_view id);

[[nodiscard]] OpenCodeProtocol infer_open_code_protocol(std::string_view id);

[[nodiscard]] bool supports_reasoning_effort(const OpenCodeGoModelInfo &model,
                                             core::ReasoningEffort effort);

} // namespace zed::providers
