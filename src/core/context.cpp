#include "zed/core/context.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace zed::core {

namespace {

Error invalid_limits() {
  return {
      ErrorCode::invalid_argument,
      "max_context_tokens must be greater than reserved_output_tokens",
  };
}

Result<TokenCount> available_tokens(const ContextLimits &limits) {
  if (limits.max_context_tokens == 0 ||
      limits.max_context_tokens <= limits.reserved_output_tokens) {
    return Result<TokenCount>::failure(invalid_limits());
  }
  return Result<TokenCount>::success(limits.max_context_tokens -
                                     limits.reserved_output_tokens);
}

} // namespace

Result<TokenCount>
ApproximateTokenEstimator::estimate(std::span<const Message> messages) const {
  TokenCount total = 0;
  for (const auto &message : messages) {
    std::size_t bytes = message.content.size() + message.id.size() + 8;
    for (const auto &call : message.tool_calls) {
      bytes += call.id.size() + call.name.size() + call.arguments_json.size();
    }
    total += static_cast<TokenCount>((bytes + 3) / 4);
  }
  return Result<TokenCount>::success(total);
}

BasicContextManager::BasicContextManager(TokenEstimator &estimator,
                                         ContextController *controller)
    : estimator_(estimator), controller_(controller) {}

Result<bool>
BasicContextManager::needs_compaction(std::span<const Message> messages,
                                      const ContextLimits &limits) const {
  const auto available = available_tokens(limits);
  if (!available) {
    return Result<bool>::failure(available.error());
  }

  const auto estimated = estimator_.estimate(messages);
  if (!estimated) {
    return Result<bool>::failure(estimated.error());
  }

  if (!limits.automatic_compaction) {
    return Result<bool>::success(estimated.value() > available.value());
  }

  const TokenCount trigger =
      limits.compaction_trigger_tokens == 0
          ? static_cast<TokenCount>(available.value() * 0.8)
          : limits.compaction_trigger_tokens;
  return Result<bool>::success(estimated.value() >= trigger);
}

Result<ContextWindow>
BasicContextManager::build(std::span<const Message> messages,
                           const ContextLimits &limits,
                           CancellationToken cancellation) {
  if (cancellation.is_cancelled()) {
    return Result<ContextWindow>::failure({
        ErrorCode::cancelled,
        "context construction cancelled",
    });
  }

  const auto available = available_tokens(limits);
  if (!available) {
    return Result<ContextWindow>::failure(available.error());
  }

  const auto estimated = estimator_.estimate(messages);
  if (!estimated) {
    return Result<ContextWindow>::failure(estimated.error());
  }

  const TokenCount trigger =
      limits.compaction_trigger_tokens == 0
          ? static_cast<TokenCount>(available.value() * 0.8)
          : limits.compaction_trigger_tokens;

  if ((!limits.automatic_compaction &&
       estimated.value() <= available.value()) ||
      (limits.automatic_compaction && estimated.value() < trigger &&
       estimated.value() <= available.value())) {
    return Result<ContextWindow>::success({
        std::vector<Message>(messages.begin(), messages.end()),
        {estimated.value(), available.value(), limits.reserved_output_tokens},
        false,
    });
  }

  if (limits.automatic_compaction && controller_ != nullptr) {
    ContextRequest request;
    request.limits = limits;
    if (!messages.empty()) {
      request.current_task = messages.back().content;
    }

    request.candidates.reserve(messages.size());
    for (std::size_t index = 0; index < messages.size(); ++index) {
      std::vector<Message> single_message{messages[index]};
      const auto message_tokens = estimator_.estimate(single_message);
      if (!message_tokens) {
        return Result<ContextWindow>::failure(message_tokens.error());
      }
      request.candidates.push_back({
          messages[index].id,
          messages[index],
          message_tokens.value(),
          messages[index].role == Role::system || index + 1 == messages.size(),
      });
    }

    const auto decision = controller_->decide(request, cancellation);
    if (decision) {
      std::unordered_set<MessageId> selected(
          decision.value().selected_ids.begin(),
          decision.value().selected_ids.end());
      std::unordered_set<MessageId> known_ids;
      for (const auto &candidate : request.candidates)
        known_ids.insert(candidate.id);
      bool unknown_selected = false;
      for (const auto &id : selected) {
        if (!known_ids.contains(id)) {
          unknown_selected = true;
          break;
        }
      }
      bool required_missing = false;
      for (const auto &candidate : request.candidates) {
        if (candidate.required && !selected.contains(candidate.id)) {
          required_missing = true;
          break;
        }
      }

      if (!required_missing && !unknown_selected) {
        std::vector<Message> selected_messages;
        selected_messages.reserve(selected.size() + 1);
        if (!decision.value().summary.empty()) {
          selected_messages.push_back({
              "context-summary",
              Role::system,
              decision.value().summary,
              {},
              std::nullopt,
          });
        }
        for (const auto &message : messages) {
          if (selected.contains(message.id)) {
            selected_messages.push_back(message);
          }
        }
        const auto selected_tokens = estimator_.estimate(selected_messages);
        if (selected_tokens && selected_tokens.value() <= available.value()) {
          return Result<ContextWindow>::success({
              std::move(selected_messages),
              {selected_tokens.value(), available.value(),
               limits.reserved_output_tokens},
              true,
          });
        }
      }
    }
  }

  if (estimated.value() <= available.value()) {
    return Result<ContextWindow>::success({
        std::vector<Message>(messages.begin(), messages.end()),
        {estimated.value(), available.value(), limits.reserved_output_tokens},
        false,
    });
  }

  return build_deterministically(messages, limits);
}

Result<ContextWindow> BasicContextManager::build_deterministically(
    std::span<const Message> messages, const ContextLimits &limits) const {
  const auto available = available_tokens(limits);
  if (!available) {
    return Result<ContextWindow>::failure(available.error());
  }

  std::vector<bool> keep(messages.size(), false);
  TokenCount used = 0;

  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (messages[index].role != Role::system) {
      continue;
    }
    std::vector<Message> single_message{messages[index]};
    const auto cost = estimator_.estimate(single_message);
    if (!cost) {
      return Result<ContextWindow>::failure(cost.error());
    }
    if (used > available.value() || cost.value() > available.value() - used) {
      return Result<ContextWindow>::failure({
          ErrorCode::context_error,
          "required system messages exceed the context budget",
      });
    }
    keep[index] = true;
    used += cost.value();
  }

  if (!messages.empty()) {
    const std::size_t last = messages.size() - 1;
    if (!keep[last]) {
      std::vector<Message> single_message{messages[last]};
      const auto cost = estimator_.estimate(single_message);
      if (!cost) {
        return Result<ContextWindow>::failure(cost.error());
      }
      if (cost.value() > available.value() - used) {
        return Result<ContextWindow>::failure({
            ErrorCode::context_error,
            "the current message exceeds the context budget",
        });
      }
      keep[last] = true;
      used += cost.value();
    }
  }

  for (std::size_t index = messages.size(); index > 0; --index) {
    const std::size_t current = index - 1;
    if (keep[current]) {
      continue;
    }
    std::vector<Message> single_message{messages[current]};
    const auto cost = estimator_.estimate(single_message);
    if (!cost) {
      return Result<ContextWindow>::failure(cost.error());
    }
    if (cost.value() <= available.value() - used) {
      keep[current] = true;
      used += cost.value();
    }
  }

  std::vector<Message> result;
  result.reserve(messages.size());
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (keep[index]) {
      result.push_back(messages[index]);
    }
  }

  return Result<ContextWindow>::success({
      std::move(result),
      {used, available.value(), limits.reserved_output_tokens},
      true,
  });
}

} // namespace zed::core
