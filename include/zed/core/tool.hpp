#pragma once

#include <string>

#include "zed/core/cancellation.hpp"
#include "zed/core/message.hpp"
#include "zed/core/model.hpp"
#include "zed/core/result.hpp"

namespace zed::core {

struct ToolResult {
    ToolCallId tool_call_id;
    std::string content;
    bool is_error{false};
};

class Tool {
public:
    virtual ~Tool() = default;

    [[nodiscard]] virtual const ToolDefinition& definition() const = 0;

    virtual Result<ToolResult> execute(
        const ToolCall& call,
        CancellationToken cancellation) = 0;
};

}  // namespace zed::core
