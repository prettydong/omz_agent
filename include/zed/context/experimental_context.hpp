#pragma once

#include <span>

#include "zed/context/context_archive.hpp"
#include "zed/core/context.hpp"

namespace zed::context {

// Projects a durable transcript into independently recoverable context windows.
// It never asks a model to repeatedly summarize the previous summary.
class ExperimentalContextManager final : public core::ContextManager {
public:
  ExperimentalContextManager(core::TokenEstimator &estimator,
                             ContextArchive &archive);

  core::Result<bool>
  needs_compaction(std::span<const core::Message> messages,
                   const core::ContextLimits &limits) const override;
  core::Result<core::ContextWindow>
  build(std::span<const core::Message> messages,
        const core::ContextLimits &limits,
        core::CancellationToken cancellation) override;
  core::Result<core::ContextWindow>
  build_request(std::span<const core::Message> messages,
                std::span<const core::ToolDefinition> tools,
                const core::ContextLimits &limits,
                core::CancellationToken cancellation) override;

private:
  core::Result<core::ContextWindow>
  build_window(std::span<const core::Message> messages,
               const core::ContextLimits &limits,
               core::TokenCount schema_tokens,
               core::CancellationToken cancellation);

  core::TokenEstimator &estimator;
  ContextArchive &archive;
};

} // namespace zed::context
