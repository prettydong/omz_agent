#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "zed/core/cancellation.hpp"
#include "zed/core/message.hpp"
#include "zed/core/model.hpp"
#include "zed/core/result.hpp"

namespace zed::core {

struct ToolResult {
  ToolResult() = default;
  ToolResult(ToolCallId call_id, std::string result_content,
             bool result_is_error = false,
             std::optional<ModelUsage> usage = std::nullopt)
      : tool_call_id(std::move(call_id)), content(std::move(result_content)),
        is_error(result_is_error), model_usage(std::move(usage)) {}

  ToolCallId tool_call_id;
  std::string content;
  bool is_error{false};
  std::optional<ModelUsage> model_usage;
};

struct ToolProgress {
  std::string text;
};

using ToolProgressCallback = std::function<void(const ToolProgress &)>;

class Tool {
public:
  virtual ~Tool() = default;

  [[nodiscard]] virtual const ToolDefinition &definition() const = 0;

  virtual Result<ToolResult> execute(const ToolCall &call,
                                     CancellationToken cancellation) = 0;

  virtual Result<ToolResult>
  execute_with_progress(const ToolCall &call, CancellationToken cancellation,
                        const ToolProgressCallback &on_progress) {
    static_cast<void>(on_progress);
    return execute(call, cancellation);
  }
};

} // namespace zed::core
