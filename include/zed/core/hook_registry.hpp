#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/cancellation.hpp"
#include "zed/core/result.hpp"

namespace zed::core {

enum class HookPoint {
  user_message_submit,
  before_model_request,
  after_model_response,
  before_tool_call,
  after_tool_result,
  session_write,
  agent_turn_start,
  agent_turn_end,
};

[[nodiscard]] std::string_view hook_point_name(HookPoint point);

enum class HookAction {
  continue_execution,
  replace_payload,
  reject,
};

struct HookCallbackResult {
  HookAction action{HookAction::continue_execution};
  std::string payload_json;
  std::string detail;
};

using HookCallback = std::function<Result<HookCallbackResult>(
    std::string_view payload_json, CancellationToken cancellation)>;
using HookSubscriptionId = std::uint64_t;

struct HookDefinition {
  std::string name;
  HookPoint point{HookPoint::agent_turn_start};
  std::int32_t priority{};
  HookCallback callback;
};

class HookRegistry {
public:
  Result<HookSubscriptionId> register_hook(HookDefinition hook);
  bool unregister_hook(HookSubscriptionId id);

  Result<std::string> dispatch(HookPoint point, std::string payload_json,
                               CancellationToken cancellation = {}) const;

  [[nodiscard]] std::size_t subscription_count() const;

private:
  struct Subscription;

  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Subscription>> subscriptions_;
  HookSubscriptionId next_id_{1};
};

} // namespace zed::core
