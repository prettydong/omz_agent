#pragma once

#include <cstddef>
#include <string>

namespace zed::core {

using TokenCount = std::size_t;
using MessageId = std::string;
using ToolCallId = std::string;

struct ModelRef {
    std::string provider;
    std::string model;
};

}  // namespace zed::core
