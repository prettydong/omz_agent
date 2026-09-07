#include "zed/core/hook_registry.hpp"

#include <algorithm>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

namespace zed::core {

namespace {

using Json = nlohmann::json;

Result<void> validate_payload(std::string_view payload,
                              std::string_view operation) {
  try {
    const auto parsed = Json::parse(payload);
    if (!parsed.is_object()) {
      return Result<void>::failure(
          {ErrorCode::hook_error,
           std::string(operation) + " hook payload must be a JSON object"});
    }
    return Result<void>::success();
  } catch (const Json::exception &error) {
    return Result<void>::failure(
        {ErrorCode::hook_error,
         std::string(operation) +
             " hook payload is invalid JSON: " + error.what()});
  } catch (const std::exception &error) {
    return Result<void>::failure(
        {ErrorCode::hook_error,
         std::string(operation) +
             " hook payload validation failed: " + error.what()});
  } catch (...) {
    return Result<void>::failure(
        {ErrorCode::hook_error,
         std::string(operation) +
             " hook payload validation failed with an unknown error"});
  }
}

} // namespace

struct HookRegistry::Subscription {
  HookSubscriptionId id{};
  HookDefinition definition;
};

std::string_view hook_point_name(HookPoint point) {
  switch (point) {
  case HookPoint::user_message_submit:
    return "user_message_submit";
  case HookPoint::before_model_request:
    return "before_model_request";
  case HookPoint::after_model_response:
    return "after_model_response";
  case HookPoint::before_tool_call:
    return "before_tool_call";
  case HookPoint::after_tool_result:
    return "after_tool_result";
  case HookPoint::session_write:
    return "session_write";
  case HookPoint::agent_turn_start:
    return "agent_turn_start";
  case HookPoint::agent_turn_end:
    return "agent_turn_end";
  }
  return "unknown";
}

Result<HookSubscriptionId> HookRegistry::register_hook(HookDefinition hook) {
  if (hook.name.empty()) {
    return Result<HookSubscriptionId>::failure(
        {ErrorCode::invalid_argument, "hook name cannot be empty"});
  }
  if (!hook.callback) {
    return Result<HookSubscriptionId>::failure(
        {ErrorCode::invalid_argument,
         "hook callback cannot be empty: " + hook.name});
  }

  std::scoped_lock lock(mutex_);
  const auto duplicate = std::find_if(
      subscriptions_.begin(), subscriptions_.end(), [&](const auto &existing) {
        return existing->definition.name == hook.name;
      });
  if (duplicate != subscriptions_.end()) {
    return Result<HookSubscriptionId>::failure(
        {ErrorCode::conflict, "hook already registered: " + hook.name});
  }
  if (next_id_ == 0) {
    return Result<HookSubscriptionId>::failure(
        {ErrorCode::internal, "hook subscription id space is exhausted"});
  }

  try {
    const auto id = next_id_++;
    subscriptions_.push_back(
        std::make_shared<Subscription>(Subscription{id, std::move(hook)}));
    return Result<HookSubscriptionId>::success(id);
  } catch (const std::exception &error) {
    return Result<HookSubscriptionId>::failure(
        {ErrorCode::internal,
         "cannot register hook: " + std::string(error.what())});
  }
}

bool HookRegistry::unregister_hook(HookSubscriptionId id) {
  std::scoped_lock lock(mutex_);
  const auto iterator = std::find_if(
      subscriptions_.begin(), subscriptions_.end(),
      [&](const auto &subscription) { return subscription->id == id; });
  if (iterator == subscriptions_.end())
    return false;
  subscriptions_.erase(iterator);
  return true;
}

Result<std::string>
HookRegistry::dispatch(HookPoint point, std::string payload_json,
                       CancellationToken cancellation) const {
  try {
    const auto initial_validation =
        validate_payload(payload_json, hook_point_name(point));
    if (!initial_validation)
      return Result<std::string>::failure(initial_validation.error());

    std::vector<std::shared_ptr<Subscription>> snapshot;
    {
      std::scoped_lock lock(mutex_);
      snapshot.reserve(subscriptions_.size());
      for (const auto &subscription : subscriptions_) {
        if (subscription->definition.point == point)
          snapshot.push_back(subscription);
      }
    }
    std::stable_sort(
        snapshot.begin(), snapshot.end(),
        [](const auto &left, const auto &right) {
          if (left->definition.priority != right->definition.priority)
            return left->definition.priority < right->definition.priority;
          return left->id < right->id;
        });

    for (const auto &subscription : snapshot) {
      if (cancellation.is_cancelled()) {
        return Result<std::string>::failure(
            {ErrorCode::cancelled, "hook dispatch cancelled: " +
                                       std::string(hook_point_name(point))});
      }

      Result<HookCallbackResult> callback_result =
          Result<HookCallbackResult>::failure(
              {ErrorCode::internal, "hook callback did not run"});
      try {
        callback_result =
            subscription->definition.callback(payload_json, cancellation);
      } catch (const std::exception &error) {
        return Result<std::string>::failure(
            {ErrorCode::hook_error,
             "hook threw an exception: " + subscription->definition.name +
                 ": " + error.what()});
      } catch (...) {
        return Result<std::string>::failure(
            {ErrorCode::hook_error, "hook threw an unknown exception: " +
                                        subscription->definition.name});
      }
      if (!callback_result) {
        const auto code = callback_result.error().code == ErrorCode::cancelled
                              ? ErrorCode::cancelled
                              : ErrorCode::hook_error;
        return Result<std::string>::failure(
            {code, "hook failed: " + subscription->definition.name + ": " +
                       callback_result.error().message});
      }

      auto result = std::move(callback_result.value());
      switch (result.action) {
      case HookAction::continue_execution:
        break;
      case HookAction::replace_payload: {
        const auto replacement_validation = validate_payload(
            result.payload_json, subscription->definition.name);
        if (!replacement_validation) {
          return Result<std::string>::failure(replacement_validation.error());
        }
        payload_json = std::move(result.payload_json);
        break;
      }
      case HookAction::reject:
        return Result<std::string>::failure(
            {ErrorCode::hook_error,
             "hook rejected " + std::string(hook_point_name(point)) + ": " +
                 subscription->definition.name +
                 (result.detail.empty() ? std::string{}
                                        : ": " + std::move(result.detail))});
      }
    }
    return Result<std::string>::success(std::move(payload_json));
  } catch (const std::exception &error) {
    return Result<std::string>::failure(
        {ErrorCode::hook_error, "hook dispatch failed for " +
                                    std::string(hook_point_name(point)) + ": " +
                                    error.what()});
  } catch (...) {
    return Result<std::string>::failure(
        {ErrorCode::hook_error, "hook dispatch failed for " +
                                    std::string(hook_point_name(point)) +
                                    " with an unknown error"});
  }
}

std::size_t HookRegistry::subscription_count() const {
  std::scoped_lock lock(mutex_);
  return subscriptions_.size();
}

} // namespace zed::core
