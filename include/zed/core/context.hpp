#pragma once

#include <span>
#include <string>
#include <vector>

#include "zed/core/cancellation.hpp"
#include "zed/core/message.hpp"
#include "zed/core/result.hpp"
#include "zed/core/types.hpp"

namespace zed::core {

struct ContextLimits {
  TokenCount max_context_tokens{};
  TokenCount reserved_output_tokens{};
  TokenCount compaction_trigger_tokens{};
  bool automatic_compaction{true};
};

[[nodiscard]] ContextLimits
cap_context_limits(ContextLimits configured,
                   TokenCount model_max_context_tokens);

struct ContextUsage {
  TokenCount input_tokens{};
  TokenCount available_tokens{};
  TokenCount reserved_output_tokens{};
};

struct ContextWindow {
  std::vector<Message> messages;
  ContextUsage usage;
  bool was_compacted{false};
};

struct ContextCandidate {
  MessageId id;
  Message message;
  TokenCount estimated_tokens{};
  bool required{false};
};

struct ContextRequest {
  std::string current_task;
  std::vector<ContextCandidate> candidates;
  ContextLimits limits;
};

struct ContextDecision {
  std::vector<MessageId> selected_ids;
  std::vector<MessageId> summarized_ids;
  std::string summary;
};

class TokenEstimator {
public:
  virtual ~TokenEstimator() = default;

  virtual Result<TokenCount>
  estimate(std::span<const Message> messages) const = 0;
};

class ApproximateTokenEstimator final : public TokenEstimator {
public:
  Result<TokenCount> estimate(std::span<const Message> messages) const override;
};

class ContextController {
public:
  virtual ~ContextController() = default;

  virtual Result<ContextDecision> decide(const ContextRequest &request,
                                         CancellationToken cancellation) = 0;
};

class ContextManager {
public:
  virtual ~ContextManager() = default;

  virtual Result<bool> needs_compaction(std::span<const Message> messages,
                                        const ContextLimits &limits) const = 0;

  virtual Result<ContextWindow> build(std::span<const Message> messages,
                                      const ContextLimits &limits,
                                      CancellationToken cancellation) = 0;
};

class BasicContextManager final : public ContextManager {
public:
  explicit BasicContextManager(TokenEstimator &estimator,
                               ContextController *controller = nullptr);

  Result<bool> needs_compaction(std::span<const Message> messages,
                                const ContextLimits &limits) const override;

  Result<ContextWindow> build(std::span<const Message> messages,
                              const ContextLimits &limits,
                              CancellationToken cancellation) override;

private:
  Result<ContextWindow>
  build_deterministically(std::span<const Message> messages,
                          const ContextLimits &limits) const;

  TokenEstimator &estimator_;
  ContextController *controller_;
};

} // namespace zed::core
