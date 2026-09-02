#pragma once

#include <iosfwd>

#include "zed/core/result.hpp"

namespace zed::app {
struct RuntimeConfig;
}

namespace zed::core {
class ToolRegistry;
}

namespace zed::lsp {
class ClangdClient;
}

namespace zed::subagents {

core::Result<void> register_explorer_tools(core::ToolRegistry &registry,
                                           const app::RuntimeConfig &runtime,
                                           lsp::ClangdClient &clangd);

int run_worker_host(std::istream &input, std::ostream &output,
                    std::ostream &diagnostics);

} // namespace zed::subagents
