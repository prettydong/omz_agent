#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "zed/core/message.hpp"
#include "zed/core/model.hpp"
#include "zed/core/tool.hpp"

namespace zed::core {

enum class AgentEventType {
  agent_start,
  user_message,
  assistant_delta,
  assistant_message,
  tool_start,
  tool_result,
  agent_end,
  error,
};

struct AgentEvent {
  AgentEvent() = default;

  AgentEvent(AgentEventType event_type, std::string event_text = {},
             std::optional<ToolCall> event_tool_call = std::nullopt,
             std::optional<ToolResult> event_tool_result = std::nullopt,
             std::optional<ModelUsage> event_model_usage = std::nullopt)
      : type(event_type), text(std::move(event_text)),
        tool_call(std::move(event_tool_call)),
        tool_result(std::move(event_tool_result)),
        model_usage(std::move(event_model_usage)) {}

  AgentEventType type{AgentEventType::agent_start};
  std::string text;
  std::optional<ToolCall> tool_call;
  std::optional<ToolResult> tool_result;
  std::optional<ModelUsage> model_usage;
};

using AgentEventCallback = std::function<void(const AgentEvent &)>;

} // namespace zed::core
