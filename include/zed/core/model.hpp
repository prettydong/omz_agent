#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/cancellation.hpp"
#include "zed/core/message.hpp"
#include "zed/core/result.hpp"
#include "zed/core/types.hpp"

namespace zed::core {

struct ToolDefinition {
  std::string name;
  std::string description;
  std::string input_schema_json;
};

enum class FinishReason {
  unknown,
  stop,
  tool_calls,
  length,
  content_filter,
  cancelled,
};

enum class ReasoningEffort {
  none,
  low,
  medium,
  high,
};

[[nodiscard]] constexpr std::string_view
reasoning_effort_name(ReasoningEffort effort) {
  switch (effort) {
  case ReasoningEffort::none:
    return "none";
  case ReasoningEffort::low:
    return "low";
  case ReasoningEffort::medium:
    return "medium";
  case ReasoningEffort::high:
    return "high";
  }
  return "low";
}

[[nodiscard]] constexpr std::optional<ReasoningEffort>
reasoning_effort_from_name(std::string_view name) {
  if (name == "none")
    return ReasoningEffort::none;
  if (name == "low")
    return ReasoningEffort::low;
  if (name == "medium")
    return ReasoningEffort::medium;
  if (name == "high")
    return ReasoningEffort::high;
  return std::nullopt;
}

struct ModelRequest {
  ModelRef model;
  std::vector<Message> messages;
  std::vector<ToolDefinition> tools;
  std::optional<std::size_t> max_output_tokens;
  double temperature{0.0};
  ReasoningEffort reasoning_effort{ReasoningEffort::low};
};

struct ModelCapabilities {
  TokenCount max_context_tokens{};
  bool streaming{false};
  bool structured_outputs{false};
  bool function_calling{false};
  bool prompt_caching{false};
};

struct ModelUsage {
  TokenCount input_tokens{};
  TokenCount cached_input_tokens{};
  TokenCount output_tokens{};
};

struct ModelDelta {
  std::string text;
};

struct AssistantResponse {
  std::string content;
  std::vector<ToolCall> tool_calls;
  FinishReason finish_reason{FinishReason::unknown};
  ModelUsage usage;
};

using StreamCallback = std::function<void(const ModelDelta &)>;

class Model {
public:
  virtual ~Model() = default;

  [[nodiscard]] virtual ModelCapabilities capabilities() const { return {}; }

  virtual Result<AssistantResponse>
  complete(const ModelRequest &request, const StreamCallback &on_delta,
           CancellationToken cancellation) = 0;
};

} // namespace zed::core
