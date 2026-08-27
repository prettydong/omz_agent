#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zed/core/types.hpp"

namespace zed::core {

enum class Role {
    system,
    user,
    assistant,
    tool,
};

struct ToolCall {
    ToolCallId id;
    std::string name;
    std::string arguments_json;
};

struct Message {
    MessageId id;
    Role role{Role::user};
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::optional<ToolCallId> tool_call_id;
};

}  // namespace zed::core
