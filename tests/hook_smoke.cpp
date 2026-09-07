#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zed/core/agent_loop.hpp"
#include "zed/core/hook_registry.hpp"

namespace {

using Json = nlohmann::json;
using namespace zed::core;

class HookTool final : public Tool {
public:
  HookTool()
      : definition_{
            "hook_tool", "Exercise hook interception.",
            R"({"type":"object","properties":{"text":{"type":"string"}}})"} {}

  [[nodiscard]] const ToolDefinition &definition() const override {
    return definition_;
  }

  Result<ToolResult> execute(const ToolCall &call, CancellationToken) override {
    const auto arguments = Json::parse(call.arguments_json);
    assert(arguments.at("text") == "intercepted");
    return Result<ToolResult>::success({call.id, "raw-tool-result"});
  }

private:
  ToolDefinition definition_;
};

class HookModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    assert(request.temperature == 0.25);
    ++calls_;
    if (calls_ == 1) {
      assert(request.messages.back().role == Role::user);
      assert(request.messages.back().content ==
             "input:start-low:start-high:submit:session");
      return Result<AssistantResponse>::success(
          {"first",
           {{"hook-call", "hook_tool",
             R"({"purpose":"exercise hooks","text":"original"})"}},
           FinishReason::tool_calls,
           {10, 0, 2}});
    }
    assert(request.messages.size() >= 4);
    assert(request.messages[request.messages.size() - 2].role ==
           Role::assistant);
    assert(request.messages[request.messages.size() - 2].content ==
           "first:response:session");
    assert(request.messages.back().role == Role::tool);
    assert(request.messages.back().content == "raw-tool-result:result:session");
    return Result<AssistantResponse>::success(
        {"final", {}, FinishReason::stop, {20, 0, 4}});
  }

  [[nodiscard]] std::size_t calls() const { return calls_; }

private:
  std::size_t calls_{};
};

class FinalModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &,
                                     CancellationToken) override {
    return Result<AssistantResponse>::success(
        {"final", {}, FinishReason::stop, {}});
  }
};

class TrackingSessionStore final : public SessionStore {
public:
  Result<void> append(const Message &message) override {
    messages_.push_back(message);
    return Result<void>::success();
  }

  Result<std::vector<Message>> load() const override {
    return Result<std::vector<Message>>::success(messages_);
  }

  Result<void> begin_turn(std::string_view, const Message &message) override {
    assert(!active_);
    active_ = true;
    messages_.push_back(message);
    return Result<void>::success();
  }

  Result<void> finish_turn(std::string_view, SessionTurnOutcome outcome,
                           std::string_view detail) override {
    assert(active_);
    active_ = false;
    ++finish_count_;
    finish_outcome_ = outcome;
    finish_detail_ = detail;
    return Result<void>::success();
  }

  [[nodiscard]] bool active() const { return active_; }
  [[nodiscard]] std::size_t finish_count() const { return finish_count_; }
  [[nodiscard]] SessionTurnOutcome finish_outcome() const {
    return finish_outcome_;
  }
  [[nodiscard]] const std::string &finish_detail() const {
    return finish_detail_;
  }

private:
  std::vector<Message> messages_;
  bool active_{false};
  std::size_t finish_count_{};
  SessionTurnOutcome finish_outcome_{SessionTurnOutcome::completed};
  std::string finish_detail_;
};

HookCallback replacing(std::function<void(Json &)> mutate, std::size_t &count) {
  return [mutate = std::move(mutate),
          &count](std::string_view payload,
                  CancellationToken) -> Result<HookCallbackResult> {
    ++count;
    auto document = Json::parse(payload);
    mutate(document);
    return Result<HookCallbackResult>::success(
        {HookAction::replace_payload, document.dump(), {}});
  };
}

void test_registry_and_agent_interception() {
  HookRegistry hooks;
  std::size_t start_low_count = 0;
  std::size_t start_high_count = 0;
  std::size_t submit_count = 0;
  std::size_t before_model_count = 0;
  std::size_t after_model_count = 0;
  std::size_t before_tool_count = 0;
  std::size_t after_result_count = 0;
  std::size_t session_write_count = 0;
  std::size_t turn_end_count = 0;

  assert(hooks.register_hook(
      {"start-high", HookPoint::agent_turn_start, 10,
       replacing(
           [](Json &payload) {
             payload["user_input"] =
                 payload.at("user_input").get<std::string>() + ":start-high";
           },
           start_high_count)}));
  assert(hooks.register_hook(
      {"start-low", HookPoint::agent_turn_start, -10,
       replacing(
           [](Json &payload) {
             payload["user_input"] =
                 payload.at("user_input").get<std::string>() + ":start-low";
           },
           start_low_count)}));
  assert(hooks.register_hook({"submit", HookPoint::user_message_submit, 0,
                              replacing(
                                  [](Json &payload) {
                                    auto &content =
                                        payload["message"]["content"];
                                    content =
                                        content.get<std::string>() + ":submit";
                                  },
                                  submit_count)}));
  assert(hooks.register_hook(
      {"before-model", HookPoint::before_model_request, 0,
       replacing(
           [](Json &payload) { payload["request"]["temperature"] = 0.25; },
           before_model_count)}));
  assert(hooks.register_hook({"after-model", HookPoint::after_model_response, 0,
                              replacing(
                                  [](Json &payload) {
                                    auto &content =
                                        payload["response"]["content"];
                                    content = content.get<std::string>() +
                                              ":response";
                                  },
                                  after_model_count)}));
  assert(hooks.register_hook(
      {"before-tool", HookPoint::before_tool_call, 0,
       replacing(
           [](Json &payload) {
             auto arguments = Json::parse(
                 payload["call"]["arguments_json"].get<std::string>());
             arguments["text"] = "intercepted";
             payload["call"]["arguments_json"] = arguments.dump();
           },
           before_tool_count)}));
  assert(hooks.register_hook({"after-result", HookPoint::after_tool_result, 0,
                              replacing(
                                  [](Json &payload) {
                                    auto &content =
                                        payload["result"]["content"];
                                    content =
                                        content.get<std::string>() + ":result";
                                  },
                                  after_result_count)}));
  assert(hooks.register_hook(
      {"session-write", HookPoint::session_write, 0,
       replacing(
           [](Json &payload) {
             if (payload.at("operation") == "finish_turn")
               return;
             auto &content = payload["message"]["content"];
             content = content.get<std::string>() + ":session";
           },
           session_write_count)}));
  assert(hooks.register_hook({"turn-end", HookPoint::agent_turn_end, 0,
                              replacing([](Json &) {}, turn_end_count)}));
  assert(hooks.subscription_count() == 9);

  HookModel model;
  ToolRegistry tools;
  assert(tools.register_tool(std::make_unique<HookTool>()));
  InMemorySessionStore session;
  ApproximateTokenEstimator estimator;
  BasicContextManager context(estimator);
  AgentLoopConfig config;
  config.model_request.model = {"fixture", "hooks"};
  config.context_limits = {4096, 512, 3000};
  AgentLoop loop(model, tools, session, context, config, &hooks);

  const auto result = loop.run("input");
  assert(result);
  assert(result.value() == "final:response:session");
  assert(model.calls() == 2);
  assert(start_low_count == 1);
  assert(start_high_count == 1);
  assert(submit_count == 1);
  assert(before_model_count == 2);
  assert(after_model_count == 2);
  assert(before_tool_count == 1);
  assert(after_result_count == 1);
  assert(session_write_count == 5);
  assert(turn_end_count == 1);

  const auto history = session.load();
  assert(history);
  assert(history.value().size() == 4);
  assert(history.value()[0].content ==
         "input:start-low:start-high:submit:session");
  assert(history.value()[1].content == "first:response:session");
  assert(history.value()[2].content == "raw-tool-result:result:session");
  assert(history.value()[3].content == "final:response:session");
}

void test_rejection_validation_cancellation_and_unregister() {
  HookRegistry rejected_hooks;
  std::size_t rejection_calls = 0;
  const auto rejection = rejected_hooks.register_hook(
      {"reject-user", HookPoint::user_message_submit, 0,
       [&rejection_calls](std::string_view,
                          CancellationToken) -> Result<HookCallbackResult> {
         ++rejection_calls;
         return Result<HookCallbackResult>::success(
             {HookAction::reject, {}, "policy denied the message"});
       }});
  assert(rejection);

  HookModel model;
  ToolRegistry tools;
  InMemorySessionStore session;
  ApproximateTokenEstimator estimator;
  BasicContextManager context(estimator);
  AgentLoopConfig config;
  config.model_request.model = {"fixture", "hooks"};
  config.context_limits = {4096, 512, 3000};
  AgentLoop loop(model, tools, session, context, config, &rejected_hooks);
  const auto denied = loop.run("denied");
  assert(!denied);
  assert(denied.error().code == ErrorCode::hook_error);
  assert(denied.error().message.find("policy denied") != std::string::npos);
  assert(rejection_calls == 1);
  assert(model.calls() == 0);
  assert(session.load().value().empty());

  HookRegistry invalid_hooks;
  assert(invalid_hooks.register_hook(
      {"invalid-json", HookPoint::agent_turn_start, 0,
       [](std::string_view, CancellationToken) -> Result<HookCallbackResult> {
         return Result<HookCallbackResult>::success(
             {HookAction::replace_payload, "[]", {}});
       }}));
  const auto invalid = invalid_hooks.dispatch(
      HookPoint::agent_turn_start,
      R"({"schema_version":1,"hook":"agent_turn_start","turn_id":"t"})");
  assert(!invalid);
  assert(invalid.error().code == ErrorCode::hook_error);

  HookRegistry cancelled_hooks;
  std::size_t cancelled_calls = 0;
  const auto cancelled_id = cancelled_hooks.register_hook(
      {"cancelled", HookPoint::agent_turn_start, 0,
       [&cancelled_calls](std::string_view,
                          CancellationToken) -> Result<HookCallbackResult> {
         ++cancelled_calls;
         return Result<HookCallbackResult>::success({});
       }});
  assert(cancelled_id);
  CancellationSource source;
  source.cancel();
  const auto cancelled = cancelled_hooks.dispatch(
      HookPoint::agent_turn_start, R"({"value":1})", source.token());
  assert(!cancelled);
  assert(cancelled.error().code == ErrorCode::cancelled);
  assert(cancelled_calls == 0);
  assert(cancelled_hooks.unregister_hook(cancelled_id.value()));
  assert(!cancelled_hooks.unregister_hook(cancelled_id.value()));
  assert(cancelled_hooks.subscription_count() == 0);
}

void test_rejected_finish_is_closed_as_failed() {
  HookRegistry hooks;
  assert(hooks.register_hook(
      {"reject-finish", HookPoint::session_write, 0,
       [](std::string_view payload,
          CancellationToken) -> Result<HookCallbackResult> {
         const auto document = Json::parse(payload);
         if (document.at("operation") == "finish_turn") {
           return Result<HookCallbackResult>::success(
               {HookAction::reject, {}, "deny original finish"});
         }
         return Result<HookCallbackResult>::success({});
       }}));

  FinalModel model;
  ToolRegistry tools;
  TrackingSessionStore session;
  ApproximateTokenEstimator estimator;
  BasicContextManager context(estimator);
  AgentLoopConfig config;
  config.model_request.model = {"fixture", "final"};
  config.context_limits = {4096, 512, 3000};
  AgentLoop loop(model, tools, session, context, config, &hooks);
  const auto result = loop.run("input");
  assert(!result);
  assert(result.error().code == ErrorCode::hook_error);
  assert(!session.active());
  assert(session.finish_count() == 1);
  assert(session.finish_outcome() == SessionTurnOutcome::failed);
  assert(session.finish_detail().find("deny original finish") !=
         std::string::npos);
}

} // namespace

int main() {
  test_registry_and_agent_interception();
  test_rejection_validation_cancellation_and_unregister();
  test_rejected_finish_is_closed_as_failed();
  return 0;
}
