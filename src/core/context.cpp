#include "zed/core/context.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace zed::core {

ContextLimits cap_context_limits(ContextLimits configured,
                                 TokenCount model_max_context_tokens) {
  if (model_max_context_tokens == 0 ||
      model_max_context_tokens >= configured.max_context_tokens) {
    return configured;
  }
  configured.max_context_tokens = model_max_context_tokens;
  if (configured.reserved_output_tokens >= configured.max_context_tokens)
    configured.reserved_output_tokens = configured.max_context_tokens / 8;
  const auto available =
      configured.max_context_tokens - configured.reserved_output_tokens;
  if (configured.compaction_trigger_tokens >= available)
    configured.compaction_trigger_tokens = 0;
  return configured;
}

namespace {

constexpr long double kDefaultCompactionTriggerRatio = 0.8L;

Error invalid_limits() {
  return {
      ErrorCode::invalid_argument,
      "max_context_tokens must be greater than reserved_output_tokens",
  };
}

struct MessageGroup {
  std::vector<std::size_t> indices;
  bool required{false};
};

class MessageDisjointSet {
public:
  explicit MessageDisjointSet(std::size_t size) : parents_(size) {
    for (std::size_t index = 0; index < size; ++index)
      parents_[index] = index;
  }

  std::size_t find(std::size_t index) {
    if (parents_[index] != index)
      parents_[index] = find(parents_[index]);
    return parents_[index];
  }

  void join(std::size_t first, std::size_t second) {
    const auto first_root = find(first);
    const auto second_root = find(second);
    if (first_root != second_root)
      parents_[second_root] = first_root;
  }

private:
  std::vector<std::size_t> parents_;
};

std::vector<MessageGroup> group_messages(std::span<const Message> messages) {
  MessageDisjointSet sets(messages.size());
  std::unordered_map<ToolCallId, std::size_t> call_owners;
  for (std::size_t index = 0; index < messages.size(); ++index) {
    for (const auto &call : messages[index].tool_calls)
      call_owners.try_emplace(call.id, index);
  }
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (!messages[index].tool_call_id.has_value())
      continue;
    const auto owner = call_owners.find(*messages[index].tool_call_id);
    if (owner != call_owners.end())
      sets.join(owner->second, index);
  }

  std::vector<MessageGroup> groups;
  std::unordered_map<std::size_t, std::size_t> group_indices;
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto root = sets.find(index);
    const auto [iterator, inserted] =
        group_indices.try_emplace(root, groups.size());
    if (inserted)
      groups.push_back({});
    auto &group = groups[iterator->second];
    group.indices.push_back(index);
    group.required = group.required || messages[index].role == Role::system ||
                     index + 1 == messages.size();
  }
  return groups;
}

Result<TokenCount> estimate_group(const TokenEstimator &estimator,
                                  std::span<const Message> messages,
                                  const MessageGroup &group) {
  std::vector<Message> grouped_messages;
  grouped_messages.reserve(group.indices.size());
  for (const auto index : group.indices)
    grouped_messages.push_back(messages[index]);
  return estimator.estimate(grouped_messages);
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
          ? static_cast<TokenCount>(
                static_cast<long double>(available.value()) *
                kDefaultCompactionTriggerRatio)
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
          ? static_cast<TokenCount>(
                static_cast<long double>(available.value()) *
                kDefaultCompactionTriggerRatio)
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

    const auto groups = group_messages(messages);
    std::vector<bool> required(messages.size(), false);
    for (const auto &group : groups) {
      if (!group.required)
        continue;
      for (const auto index : group.indices)
        required[index] = true;
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
          required[index],
      });
    }

    const auto decision = controller_->decide(request, cancellation);
    if (!decision && decision.error().code == ErrorCode::cancelled) {
      return Result<ContextWindow>::failure(decision.error());
    }
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

      bool partial_group_selected = false;
      for (const auto &group : groups) {
        std::size_t selected_count = 0;
        for (const auto index : group.indices) {
          if (selected.contains(messages[index].id))
            ++selected_count;
        }
        if (selected_count != 0 && selected_count != group.indices.size()) {
          partial_group_selected = true;
          break;
        }
      }

      if (!required_missing && !unknown_selected && !partial_group_selected) {
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

  const auto groups = group_messages(messages);
  std::vector<bool> keep(groups.size(), false);
  TokenCount used = 0;

  for (std::size_t index = 0; index < groups.size(); ++index) {
    if (!groups[index].required)
      continue;
    const auto cost = estimate_group(estimator_, messages, groups[index]);
    if (!cost) {
      return Result<ContextWindow>::failure(cost.error());
    }
    if (used > available.value() || cost.value() > available.value() - used) {
      return Result<ContextWindow>::failure({
          ErrorCode::context_error,
          "required context messages exceed the context budget",
      });
    }
    keep[index] = true;
    used += cost.value();
  }

  for (std::size_t index = groups.size(); index > 0; --index) {
    const std::size_t current = index - 1;
    if (keep[current]) {
      continue;
    }
    const auto cost = estimate_group(estimator_, messages, groups[current]);
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
  std::vector<bool> keep_message(messages.size(), false);
  for (std::size_t group_index = 0; group_index < groups.size();
       ++group_index) {
    if (!keep[group_index])
      continue;
    for (const auto message_index : groups[group_index].indices)
      keep_message[message_index] = true;
  }
  for (std::size_t index = 0; index < messages.size(); ++index) {
    if (keep_message[index])
      result.push_back(messages[index]);
  }

  return Result<ContextWindow>::success({
      std::move(result),
      {used, available.value(), limits.reserved_output_tokens},
      true,
  });
}

} // namespace zed::core
