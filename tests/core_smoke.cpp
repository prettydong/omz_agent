#include <algorithm>
#include <cassert>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "zed/core/agent_loop.hpp"
#include "zed/core/model_context_controller.hpp"
#include "zed/core/utf8.hpp"

namespace {

using namespace zed::core;

class EchoTool final : public Tool {
public:
  EchoTool()
      : definition_{
            "echo",
            "Returns the text argument.",
            R"({"type":"object","properties":{"text":{"type":"string"}}})",
        } {}

  [[nodiscard]] const ToolDefinition &definition() const override {
    return definition_;
  }

  Result<ToolResult> execute(const ToolCall &call,
                             CancellationToken cancellation) override {
    if (cancellation.is_cancelled()) {
      return Result<ToolResult>::failure({
          ErrorCode::cancelled,
          "echo cancelled",
      });
    }
    return Result<ToolResult>::success(
        {call.id, "echo-result", false, ModelUsage{3, 1, 2}});
  }

  Result<ToolResult>
  execute_with_progress(const ToolCall &call, CancellationToken cancellation,
                        const ToolProgressCallback &on_progress) override {
    if (on_progress)
      on_progress({"echo is running"});
    return execute(call, cancellation);
  }

private:
  ToolDefinition definition_;
};

class FakeModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &on_delta,
                                     CancellationToken cancellation) override {
    assert(!request.messages.empty());
    assert(request.messages.front().role == Role::system);
    assert(request.messages.front().content.find("不要只回复进度") !=
           std::string::npos);
    assert(request.model.provider == "test");
    assert(request.model.model == "fake");
    assert(!request.tools.empty());
    observed_efforts_.push_back(request.reasoning_effort);
    if (cancellation.is_cancelled()) {
      return Result<AssistantResponse>::failure({
          ErrorCode::cancelled,
          "fake model cancelled",
      });
    }

    ++calls_;
    if (calls_ == 1) {
      const ToolCall call{
          "call-1", "echo",
          R"({"purpose":"Verify the echo workflow","text":"hello"})"};
      return Result<AssistantResponse>::success({
          {},
          {call},
          FinishReason::tool_calls,
          {10, 0, 2},
      });
    }

    if (on_delta) {
      on_delta({"done"});
    }
    return Result<AssistantResponse>::success({
        "done",
        {},
        FinishReason::stop,
        {20, 0, 4},
    });
  }

  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] const std::vector<ReasoningEffort> &observed_efforts() const {
    return observed_efforts_;
  }

private:
  int calls_{0};
  std::vector<ReasoningEffort> observed_efforts_;
};

class SessionIdModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    observed_session_id_ = request.session_id;
    return Result<AssistantResponse>::success(
        {"session id observed", {}, FinishReason::stop, {1, 0, 1}});
  }

  [[nodiscard]] const std::string &observed_session_id() const {
    return observed_session_id_;
  }

private:
  std::string observed_session_id_;
};

class IdentifiedSessionStore final : public SessionStore {
public:
  explicit IdentifiedSessionStore(std::string id) : id_(std::move(id)) {}

  Result<void> append(const Message &message) override {
    messages_.push_back(message);
    return Result<void>::success();
  }

  Result<std::vector<Message>> load() const override {
    return Result<std::vector<Message>>::success(messages_);
  }

  [[nodiscard]] std::string session_id() const override { return id_; }

private:
  std::string id_;
  std::vector<Message> messages_;
};

class RetrySequenceModel final : public Model {
public:
  explicit RetrySequenceModel(std::vector<AssistantResponse> responses)
      : responses_(std::move(responses)) {}

  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    assert(calls_ < static_cast<int>(responses_.size()));
    if (calls_ > 0) {
      bool has_retry_feedback = false;
      for (const auto &message : request.messages) {
        has_retry_feedback =
            has_retry_feedback ||
            (message.role == Role::system &&
             message.content.find("Validation diagnostic:") !=
                 std::string::npos &&
             message.content.find("purpose") != std::string::npos &&
             message.content.find("tool") != std::string::npos);
      }
      saw_retry_feedback_ = saw_retry_feedback_ || has_retry_feedback;
    }
    return Result<AssistantResponse>::success(
        responses_[static_cast<std::size_t>(calls_++)]);
  }

  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] bool saw_retry_feedback() const { return saw_retry_feedback_; }

private:
  std::vector<AssistantResponse> responses_;
  int calls_{0};
  bool saw_retry_feedback_{false};
};

class CancelledResponseModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &,
                                     CancellationToken) override {
    ++calls_;
    return Result<AssistantResponse>::failure(
        {ErrorCode::cancelled, "model cancelled"});
  }

  [[nodiscard]] int calls() const { return calls_; }

private:
  int calls_{0};
};

class RetryStatusDeltaModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &on_delta,
                                     CancellationToken) override {
    ++calls_;
    if (on_delta) {
      on_delta({"searching for a source", ModelDeltaKind::retry});
      on_delta({"final answer", ModelDeltaKind::text});
    }
    return Result<AssistantResponse>::success(
        {"final answer", {}, FinishReason::stop, {5, 0, 2}});
  }

  [[nodiscard]] int calls() const { return calls_; }

private:
  int calls_{0};
};

class DeferredActionModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    assert(!request.messages.empty());
    assert(request.messages.front().role == Role::system);
    ++calls_;
    if (calls_ == 1) {
      return Result<AssistantResponse>::success({
          "正在构建……马上就好",
          {},
          FinishReason::stop,
          {10, 0, 2},
      });
    }
    if (calls_ == 2) {
      saw_correction_ = request.messages.front().content.find(
                            "previous attempt stopped") != std::string::npos;
      return Result<AssistantResponse>::success({
          {},
          {{"deferred-call", "echo",
            R"({"purpose":"Complete the deferred task","text":"done"})"}},
          FinishReason::tool_calls,
          {12, 0, 3},
      });
    }
    return Result<AssistantResponse>::success({
        "implemented and verified",
        {},
        FinishReason::stop,
        {14, 0, 4},
    });
  }

  [[nodiscard]] int calls() const { return calls_; }
  [[nodiscard]] bool saw_correction() const { return saw_correction_; }

private:
  int calls_{0};
  bool saw_correction_{false};
};

class RepeatedDeferredActionModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &,
                                     CancellationToken) override {
    ++calls_;
    return Result<AssistantResponse>::success({
        "正在生成，请稍等",
        {},
        FinishReason::stop,
        {},
    });
  }

  [[nodiscard]] int calls() const { return calls_; }

private:
  int calls_{0};
};

class IncompleteModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &,
                                     CancellationToken) override {
    return Result<AssistantResponse>::success({
        "partial",
        {},
        FinishReason::length,
        {},
    });
  }
};

class AdditionalSystemContextModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    assert(request.messages.size() == 2);
    assert(request.messages[0].role == Role::system);
    assert(request.messages[0].content.find("custom base prompt") !=
           std::string::npos);
    assert(request.messages[0].content.find("active skill instructions") !=
           std::string::npos);
    assert(request.messages[1].role == Role::user);
    assert(request.messages[1].content == "exact user input");
    return Result<AssistantResponse>::success({
        "done",
        {},
        FinishReason::stop,
        {},
    });
  }
};

class CancellingContextController final : public ContextController {
public:
  Result<ContextDecision> decide(const ContextRequest &,
                                 CancellationToken) override {
    return Result<ContextDecision>::failure({
        ErrorCode::cancelled,
        "context controller cancelled",
    });
  }
};

class SlowCancelTool final : public Tool {
public:
  SlowCancelTool()
      : definition_{
            "slow_cancel",
            "Blocks until cancelled.",
            R"({"type":"object","properties":{}})",
        } {}

  [[nodiscard]] const ToolDefinition &definition() const override {
    return definition_;
  }

  Result<ToolResult> execute(const ToolCall &call,
                             CancellationToken cancellation) override {
    for (int attempt = 0; attempt < 200 && !cancellation.is_cancelled();
         ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (cancellation.is_cancelled()) {
      return Result<ToolResult>::failure({
          ErrorCode::cancelled,
          "slow_cancel cancelled",
      });
    }
    return Result<ToolResult>::success({call.id, "finished", false});
  }

private:
  ToolDefinition definition_;
};

class SlowCancelModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &,
                                     CancellationToken cancellation) override {
    if (cancellation.is_cancelled()) {
      return Result<AssistantResponse>::failure({
          ErrorCode::cancelled,
          "slow cancel model cancelled",
      });
    }
    ++calls_;
    if (calls_ == 1) {
      return Result<AssistantResponse>::success({
          {},
          {{"slow-1", "slow_cancel",
            R"({"purpose":"Wait until the caller cancels"})"}},
          FinishReason::tool_calls,
          {},
      });
    }
    return Result<AssistantResponse>::success({
        "should not happen",
        {},
        FinishReason::stop,
        {},
    });
  }

  [[nodiscard]] int calls() const { return calls_; }

private:
  int calls_{0};
};

class ConfiguredContextModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    assert(request.model.model == "context-fixture");
    assert(request.messages.front().content == "configured context prompt");
    assert(request.max_output_tokens == 77);
    assert(request.temperature == 0.0);
    assert(request.reasoning_effort == ReasoningEffort::none);
    return Result<AssistantResponse>::success({
        R"({"selected_ids":["required"],"summarized_ids":[],"summary":"ok"})",
        {},
        FinishReason::stop,
        {},
    });
  }
};

class PartialToolContextController final : public ContextController {
public:
  Result<ContextDecision> decide(const ContextRequest &request,
                                 CancellationToken) override {
    ContextDecision decision;
    for (const auto &candidate : request.candidates) {
      if (candidate.required || candidate.id == "partial-tool")
        decision.selected_ids.push_back(candidate.id);
    }
    return Result<ContextDecision>::success(std::move(decision));
  }
};

} // namespace

int main() {
  const ContextLimits configured_limits{1'000, 200, 700};
  assert(cap_context_limits(configured_limits, 0).max_context_tokens == 1'000);
  assert(cap_context_limits(configured_limits, 2'000).max_context_tokens ==
         1'000);
  const auto capped_limits = cap_context_limits(configured_limits, 800);
  assert(capped_limits.max_context_tokens == 800);
  assert(capped_limits.reserved_output_tokens == 200);
  assert(capped_limits.compaction_trigger_tokens == 0);
  const auto tightly_capped_limits = cap_context_limits(configured_limits, 100);
  assert(tightly_capped_limits.max_context_tokens == 100);
  assert(tightly_capped_limits.reserved_output_tokens == 12);
  assert(tightly_capped_limits.compaction_trigger_tokens == 0);

  ConfiguredContextModel configured_context_model;
  ModelBackedContextController configured_controller(
      configured_context_model, {"test", "context-fixture"},
      "configured context prompt", 77);
  const auto context_decision = configured_controller.decide(
      {"task",
       {{"required",
         {"message", Role::user, "content", {}, std::nullopt},
         5,
         true}},
       {1'000, 100, 700}},
      {});
  assert(context_decision);
  assert(context_decision.value().selected_ids ==
         std::vector<MessageId>{"required"});

  ApproximateTokenEstimator compaction_estimator;
  BasicContextManager compaction_manager(compaction_estimator);
  const std::vector<Message> compactable_messages{
      {"context-item", Role::user, std::string(200, 'x'), {}, std::nullopt}};
  const auto automatic_needed = compaction_manager.needs_compaction(
      compactable_messages, {100, 10, 10, true});
  assert(automatic_needed && automatic_needed.value());
  const auto manual_needed = compaction_manager.needs_compaction(
      compactable_messages, {100, 10, 10, false});
  assert(manual_needed && !manual_needed.value());
  const auto manual_window =
      compaction_manager.build(compactable_messages, {100, 10, 10, false}, {});
  assert(manual_window);
  assert(!manual_window.value().was_compacted);

  ToolRegistry denied_tools({"read"});
  assert(denied_tools.register_tool(std::make_unique<EchoTool>()));
  assert(denied_tools.definitions().empty());
  assert(denied_tools.registered_definitions().size() == 1);
  assert(!denied_tools.execute(
      {"denied", "echo", R"({"purpose":"test permission"})"}, {}));
  ToolRegistry permitted_tools({"echo"});
  assert(permitted_tools.register_tool(std::make_unique<EchoTool>()));
  assert(permitted_tools.definitions().size() == 1);
  assert(permitted_tools.execute(
      {"allowed", "echo", R"({"purpose":"test permission"})"}, {}));

  FakeModel model;
  ToolRegistry tools;
  assert(tools.register_tool(std::make_unique<EchoTool>()));

  InMemorySessionStore session;
  ApproximateTokenEstimator estimator;
  BasicContextManager context(estimator);

  AgentLoopConfig config;
  config.model_request.model = {"test", "fake"};
  config.model_request.max_output_tokens = 128;
  config.context_limits = {4096, 512, 3000};

  AgentLoop loop(model, tools, session, context, config);
  loop.set_model({"test", "switched"});
  assert(loop.model().provider == "test");
  assert(loop.model().model == "switched");
  loop.set_model({"test", "fake"});
  loop.set_context_limits({512, 64, 400});
  assert(loop.context_limits().max_context_tokens == 512);
  loop.set_context_limits(config.context_limits);
  loop.set_reasoning_effort(ReasoningEffort::high);
  assert(loop.reasoning_effort() == ReasoningEffort::high);
  std::vector<ModelUsage> observed_usage;
  std::vector<std::string> observed_tool_purposes;
  std::vector<std::string> observed_tool_updates;
  std::optional<ModelUsage> observed_tool_usage;
  const auto result = loop.run("run echo", {}, [&](const AgentEvent &event) {
    if (event.type == AgentEventType::assistant_message &&
        event.model_usage.has_value()) {
      observed_usage.push_back(*event.model_usage);
    }
    if (event.type == AgentEventType::tool_start) {
      observed_tool_purposes.push_back(event.text);
    }
    if (event.type == AgentEventType::tool_update)
      observed_tool_updates.push_back(event.text);
    if (event.type == AgentEventType::tool_result &&
        event.model_usage.has_value()) {
      observed_tool_usage = event.model_usage;
    }
  });
  assert(result);
  assert(result.value() == "done");
  assert(model.calls() == 2);
  assert(model.observed_efforts().size() == 2);
  assert(model.observed_efforts()[0] == ReasoningEffort::high);
  assert(model.observed_efforts()[1] == ReasoningEffort::high);
  assert(observed_usage.size() == 2);
  assert(observed_usage[0].input_tokens == 10);
  assert(observed_usage[1].output_tokens == 4);
  assert(observed_usage[0].output_tokens_per_second > 0.0);
  assert(observed_usage[1].output_tokens_per_second > 0.0);
  assert(observed_usage[0].context_breakdown.has_value());
  assert(observed_usage[0].context_breakdown->total_tokens() ==
         observed_usage[0].input_tokens);
  assert(observed_usage[0].context_breakdown->system_tokens > 0);
  assert(observed_usage[0].context_breakdown->tool_definition_tokens > 0);
  assert(observed_usage[1].context_breakdown.has_value());
  assert(observed_usage[1].context_breakdown->total_tokens() ==
         observed_usage[1].input_tokens);
  assert(observed_usage[1].context_breakdown->tool_tokens > 0);
  assert(observed_tool_purposes.size() == 1);
  assert(observed_tool_purposes[0] == "Verify the echo workflow");
  assert(observed_tool_updates.size() == 1);
  assert(observed_tool_updates[0] == "echo is running");
  assert(observed_tool_usage.has_value());
  assert(observed_tool_usage->input_tokens == 3);

  const auto history = session.load();
  assert(history);
  assert(history.value().size() == 4);
  assert(history.value()[1].role == Role::assistant);
  assert(history.value()[2].role == Role::tool);
  assert(history.value()[3].role == Role::assistant);

  SessionIdModel session_id_model;
  IdentifiedSessionStore identified_session("active-session-id");
  BasicContextManager session_id_context(estimator);
  AgentLoop session_id_loop(session_id_model, tools, identified_session,
                            session_id_context, config);
  const auto session_id_result = session_id_loop.run("observe the session");
  assert(session_id_result);
  assert(session_id_model.observed_session_id() == "active-session-id");

  const auto valid_echo_response = [] {
    return AssistantResponse{
        {},
        {{"corrected-call", "echo",
          R"({"purpose":"Retry with a valid call","text":"hello"})"}},
        FinishReason::tool_calls,
        {21, 2, 5}};
  };
  const auto completed_response = [] {
    return AssistantResponse{"completed", {}, FinishReason::stop, {23, 3, 7}};
  };
  const auto missing_purpose_response = [] {
    return AssistantResponse{
        {},
        {{"missing-purpose", "echo", R"({"text":"hello"})"}},
        FinishReason::tool_calls,
        {11, 1, 2}};
  };

  RetrySequenceModel corrected_model({missing_purpose_response(),
                                      valid_echo_response(),
                                      completed_response()});
  ToolRegistry corrected_tools;
  assert(corrected_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore corrected_session;
  BasicContextManager corrected_context(estimator);
  AgentLoop corrected_loop(corrected_model, corrected_tools, corrected_session,
                           corrected_context, config);
  std::size_t corrected_tool_starts = 0;
  std::vector<AgentEvent> retry_events;
  const auto corrected =
      corrected_loop.run("run invalid echo", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::tool_start)
          ++corrected_tool_starts;
        if (event.type == AgentEventType::model_retry)
          retry_events.push_back(event);
      });
  assert(corrected);
  assert(corrected.value() == "completed");
  assert(corrected_model.calls() == 3);
  assert(corrected_model.saw_retry_feedback());
  assert(corrected_tool_starts == 1);
  assert(retry_events.size() == 1);
  assert(retry_events[0].text.find("retrying (1/2)") != std::string::npos);
  assert(retry_events[0].text.find("purpose") != std::string::npos);
  assert(retry_events[0].model_usage.has_value());
  assert(retry_events[0].model_usage->input_tokens == 11);
  const auto corrected_history = corrected_session.load();
  assert(corrected_history);
  assert(corrected_history.value().size() == 4);
  assert(corrected_history.value()[0].role == Role::user);
  assert(corrected_history.value()[1].role == Role::assistant);
  assert(corrected_history.value()[1].tool_calls.size() == 1);
  assert(corrected_history.value()[1].tool_calls[0].id == "corrected-call");
  assert(corrected_history.value()[2].role == Role::tool);
  assert(corrected_history.value()[3].content == "completed");

  RetrySequenceModel empty_calls_model(
      {AssistantResponse{"", {}, FinishReason::tool_calls, {6, 0, 1}},
       valid_echo_response(), completed_response()});
  ToolRegistry empty_calls_tools;
  assert(empty_calls_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore empty_calls_session;
  BasicContextManager empty_calls_context(estimator);
  AgentLoop empty_calls_loop(empty_calls_model, empty_calls_tools,
                             empty_calls_session, empty_calls_context, config);
  const auto empty_calls_result = empty_calls_loop.run("retry empty calls");
  assert(empty_calls_result);
  assert(empty_calls_model.calls() == 3);
  const auto empty_calls_history = empty_calls_session.load();
  assert(empty_calls_history);
  assert(empty_calls_history.value().size() == 4);
  assert(empty_calls_history.value()[1].tool_calls[0].id == "corrected-call");

  for (const std::string empty_content :
       {std::string{}, std::string{" \t\r\n"}}) {
    RetrySequenceModel empty_answer_model(
        {AssistantResponse{empty_content, {}, FinishReason::stop, {3, 0, 1}},
         AssistantResponse{"valid answer", {}, FinishReason::stop, {4, 0, 2}}});
    ToolRegistry empty_answer_tools;
    InMemorySessionStore empty_answer_session;
    BasicContextManager empty_answer_context(estimator);
    AgentLoop empty_answer_loop(empty_answer_model, empty_answer_tools,
                                empty_answer_session, empty_answer_context,
                                config);
    const auto empty_answer_result =
        empty_answer_loop.run("retry empty answer");
    assert(empty_answer_result);
    assert(empty_answer_result.value() == "valid answer");
    assert(empty_answer_model.calls() == 2);
    const auto empty_answer_history = empty_answer_session.load();
    assert(empty_answer_history);
    assert(empty_answer_history.value().size() == 2);
    assert(empty_answer_history.value()[1].content == "valid answer");
  }

  RetrySequenceModel repeated_empty_model(
      {AssistantResponse{"", {}, FinishReason::stop, {7, 0, 1}},
       AssistantResponse{" \t", {}, FinishReason::stop, {8, 0, 1}},
       AssistantResponse{"", {}, FinishReason::stop, {9, 0, 1}}});
  ToolRegistry repeated_empty_tools;
  InMemorySessionStore repeated_empty_session;
  BasicContextManager repeated_empty_context(estimator);
  AgentLoop repeated_empty_loop(repeated_empty_model, repeated_empty_tools,
                                repeated_empty_session, repeated_empty_context,
                                config);
  std::vector<AgentEvent> repeated_empty_events;
  const auto repeated_empty = repeated_empty_loop.run(
      "do not accept empty answers", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::model_retry ||
            event.type == AgentEventType::error) {
          repeated_empty_events.push_back(event);
        }
      });
  assert(!repeated_empty);
  assert(repeated_empty.error().code == ErrorCode::model_error);
  assert(repeated_empty_model.calls() == 3);
  assert(repeated_empty_events.size() == 3);
  assert(repeated_empty_events[0].text.find("retrying (1/2)") !=
         std::string::npos);
  assert(repeated_empty_events[1].text.find("retrying (2/2)") !=
         std::string::npos);
  assert(repeated_empty_events[2].model_usage.has_value());
  assert(repeated_empty_events[2].model_usage->input_tokens == 9);
  const auto repeated_empty_history = repeated_empty_session.load();
  assert(repeated_empty_history);
  assert(repeated_empty_history.value().size() == 1);

  RetrySequenceModel shared_correction_budget_model(
      {missing_purpose_response(),
       AssistantResponse{"", {}, FinishReason::tool_calls, {12, 0, 1}},
       missing_purpose_response()});
  ToolRegistry shared_correction_budget_tools;
  assert(shared_correction_budget_tools.register_tool(
      std::make_unique<EchoTool>()));
  InMemorySessionStore shared_correction_budget_session;
  BasicContextManager shared_correction_budget_context(estimator);
  AgentLoop shared_correction_budget_loop(
      shared_correction_budget_model, shared_correction_budget_tools,
      shared_correction_budget_session, shared_correction_budget_context,
      config);
  std::size_t shared_correction_retries = 0;
  const auto shared_correction_budget = shared_correction_budget_loop.run(
      "share correction budget", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::model_retry)
          ++shared_correction_retries;
      });
  assert(!shared_correction_budget);
  assert(shared_correction_budget.error().code == ErrorCode::model_error);
  assert(shared_correction_budget_model.calls() == 3);
  assert(shared_correction_retries == 2);

  for (const std::string response : {
           std::string{"“正在构建”表示编译过程尚未结束，并不表示构建成功。"},
           std::string{"The phrase \"working on it\" is not a result."},
           std::string{"已完成：正在构建阶段已结束，测试已通过。"},
           std::string{"```text\n正在构建\n```"},
       }) {
    RetrySequenceModel explanatory_model(
        {AssistantResponse{response, {}, FinishReason::stop, {2, 0, 1}}});
    ToolRegistry explanatory_tools;
    InMemorySessionStore explanatory_session;
    BasicContextManager explanatory_context(estimator);
    AgentLoop explanatory_loop(explanatory_model, explanatory_tools,
                               explanatory_session, explanatory_context,
                               config);
    std::size_t explanatory_retries = 0;
    const auto explanatory_result = explanatory_loop.run(
        "accept explanatory reply", {}, [&](const AgentEvent &event) {
          if (event.type == AgentEventType::model_retry)
            ++explanatory_retries;
        });
    assert(explanatory_result);
    assert(explanatory_model.calls() == 1);
    assert(explanatory_retries == 0);
  }

  RetryStatusDeltaModel retry_status_model;
  ToolRegistry retry_status_tools;
  InMemorySessionStore retry_status_session;
  BasicContextManager retry_status_context(estimator);
  AgentLoop retry_status_loop(retry_status_model, retry_status_tools,
                              retry_status_session, retry_status_context,
                              config);
  std::vector<AgentEvent> retry_status_events;
  const auto retry_status_result = retry_status_loop.run(
      "stream retry status", {},
      [&](const AgentEvent &event) { retry_status_events.push_back(event); });
  assert(retry_status_result);
  assert(retry_status_model.calls() == 1);
  assert(std::count_if(retry_status_events.begin(), retry_status_events.end(),
                       [](const AgentEvent &event) {
                         return event.type == AgentEventType::model_retry &&
                                event.text == "searching for a source";
                       }) == 1);
  assert(std::count_if(retry_status_events.begin(), retry_status_events.end(),
                       [](const AgentEvent &event) {
                         return event.type == AgentEventType::assistant_delta &&
                                event.text == "searching for a source";
                       }) == 0);
  const auto retry_status_history = retry_status_session.load();
  assert(retry_status_history);
  assert(retry_status_history.value().size() == 2);
  assert(retry_status_history.value()[1].content == "final answer");

  for (const std::string arguments : {R"({"purpose":"","text":"hello"})",
                                      R"({"purpose":"   \t","text":"hello"})",
                                      R"({"purpose":42,"text":"hello"})"}) {
    RetrySequenceModel purpose_model(
        {AssistantResponse{{},
                           {{"invalid-purpose", "echo", arguments}},
                           FinishReason::tool_calls,
                           {1, 0, 1}},
         valid_echo_response(), completed_response()});
    ToolRegistry purpose_tools;
    assert(purpose_tools.register_tool(std::make_unique<EchoTool>()));
    InMemorySessionStore purpose_session;
    BasicContextManager purpose_context(estimator);
    AgentLoop purpose_loop(purpose_model, purpose_tools, purpose_session,
                           purpose_context, config);
    const auto purpose_result = purpose_loop.run("validate purpose");
    assert(purpose_result);
    assert(purpose_model.calls() == 3);
  }

  const std::vector<AssistantResponse> structural_invalid_responses{
      {{},
       {{"", "echo", R"({"purpose":"missing id"})"}},
       FinishReason::tool_calls,
       {1, 0, 1}},
      {{},
       {{"missing-name", "", R"({"purpose":"missing name"})"}},
       FinishReason::tool_calls,
       {1, 0, 1}},
      {{},
       {{"invalid-json", "echo", "{"}},
       FinishReason::tool_calls,
       {1, 0, 1}},
      {{},
       {{"duplicate", "echo", R"({"purpose":"first"})"},
        {"duplicate", "echo", R"({"purpose":"second"})"}},
       FinishReason::tool_calls,
       {1, 0, 1}},
  };
  for (const auto &invalid_response : structural_invalid_responses) {
    RetrySequenceModel structural_model(
        {invalid_response, valid_echo_response(), completed_response()});
    ToolRegistry structural_tools;
    assert(structural_tools.register_tool(std::make_unique<EchoTool>()));
    InMemorySessionStore structural_session;
    BasicContextManager structural_context(estimator);
    AgentLoop structural_loop(structural_model, structural_tools,
                              structural_session, structural_context, config);
    const auto structural_result = structural_loop.run("validate tool call");
    assert(structural_result);
    assert(structural_model.calls() == 3);
  }

  RetrySequenceModel repeated_invalid_model({missing_purpose_response(),
                                             missing_purpose_response(),
                                             missing_purpose_response()});
  ToolRegistry repeated_invalid_tools;
  assert(repeated_invalid_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore repeated_invalid_session;
  BasicContextManager repeated_invalid_context(estimator);
  AgentLoop repeated_invalid_loop(
      repeated_invalid_model, repeated_invalid_tools, repeated_invalid_session,
      repeated_invalid_context, config);
  std::vector<AgentEvent> repeated_invalid_events;
  const auto repeated_invalid = repeated_invalid_loop.run(
      "retry invalid calls", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::model_retry ||
            event.type == AgentEventType::error) {
          repeated_invalid_events.push_back(event);
        }
      });
  assert(!repeated_invalid);
  assert(repeated_invalid.error().code == ErrorCode::model_error);
  assert(repeated_invalid_model.calls() == 3);
  assert(repeated_invalid_events.size() == 3);
  assert(repeated_invalid_events[0].text.find("retrying (1/2)") !=
         std::string::npos);
  assert(repeated_invalid_events[1].text.find("retrying (2/2)") !=
         std::string::npos);
  assert(repeated_invalid_events[2].text.find("purpose") != std::string::npos);
  assert(repeated_invalid_events[2].model_usage.has_value());
  assert(repeated_invalid_events[2].model_usage->input_tokens == 11);

  RetrySequenceModel mixed_batch_model(
      {AssistantResponse{
           {},
           {{"would-run", "echo",
             R"({"purpose":"This must not execute","text":"hello"})"},
            {"bad-batch-call", "echo", R"({"text":"hello"})"}},
           FinishReason::tool_calls,
           {4, 0, 1}},
       valid_echo_response(), completed_response()});
  ToolRegistry mixed_batch_tools;
  assert(mixed_batch_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore mixed_batch_session;
  BasicContextManager mixed_batch_context(estimator);
  AgentLoop mixed_batch_loop(mixed_batch_model, mixed_batch_tools,
                             mixed_batch_session, mixed_batch_context, config);
  std::size_t mixed_batch_tool_starts = 0;
  const auto mixed_batch = mixed_batch_loop.run(
      "do not partially execute", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::tool_start)
          ++mixed_batch_tool_starts;
      });
  assert(mixed_batch);
  assert(mixed_batch_tool_starts == 1);
  const auto mixed_batch_history = mixed_batch_session.load();
  assert(mixed_batch_history);
  assert(mixed_batch_history.value().size() == 4);
  assert(mixed_batch_history.value()[1].tool_calls[0].id == "corrected-call");

  AgentLoopConfig single_turn_config = config;
  single_turn_config.max_turns = 1;
  RetrySequenceModel single_turn_model({missing_purpose_response()});
  ToolRegistry single_turn_tools;
  assert(single_turn_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore single_turn_session;
  BasicContextManager single_turn_context(estimator);
  AgentLoop single_turn_loop(single_turn_model, single_turn_tools,
                             single_turn_session, single_turn_context,
                             single_turn_config);
  std::size_t false_retries = 0;
  const auto single_turn =
      single_turn_loop.run("one turn only", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::model_retry)
          ++false_retries;
      });
  assert(!single_turn);
  assert(single_turn_model.calls() == 1);
  assert(false_retries == 0);

  RetrySequenceModel cancelled_retry_model({missing_purpose_response()});
  ToolRegistry cancelled_retry_tools;
  assert(cancelled_retry_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore cancelled_retry_session;
  BasicContextManager cancelled_retry_context(estimator);
  AgentLoop cancelled_retry_loop(cancelled_retry_model, cancelled_retry_tools,
                                 cancelled_retry_session,
                                 cancelled_retry_context, config);
  CancellationSource retry_cancellation;
  const auto cancelled_retry =
      cancelled_retry_loop.run("cancel correction", retry_cancellation.token(),
                               [&](const AgentEvent &event) {
                                 if (event.type == AgentEventType::model_retry)
                                   retry_cancellation.cancel();
                               });
  assert(!cancelled_retry);
  assert(cancelled_retry.error().code == ErrorCode::cancelled);
  assert(cancelled_retry_model.calls() == 1);
  assert(cancelled_retry_session.load().value().size() == 1);

  CancelledResponseModel cancelled_response_model;
  ToolRegistry cancelled_response_tools;
  InMemorySessionStore cancelled_response_session;
  BasicContextManager cancelled_response_context(estimator);
  AgentLoop cancelled_response_loop(
      cancelled_response_model, cancelled_response_tools,
      cancelled_response_session, cancelled_response_context, config);
  std::size_t cancellation_retries = 0;
  const auto cancelled_response = cancelled_response_loop.run(
      "do not retry cancellation", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::model_retry)
          ++cancellation_retries;
      });
  assert(!cancelled_response);
  assert(cancelled_response.error().code == ErrorCode::cancelled);
  assert(cancelled_response_model.calls() == 1);
  assert(cancellation_retries == 0);

  DeferredActionModel deferred_model;
  ToolRegistry deferred_tools;
  assert(deferred_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore deferred_session;
  BasicContextManager deferred_context(estimator);
  AgentLoop deferred_loop(deferred_model, deferred_tools, deferred_session,
                          deferred_context, config);
  const auto deferred_result = deferred_loop.run("build the project");
  assert(deferred_result);
  assert(deferred_result.value() == "implemented and verified");
  assert(deferred_model.calls() == 3);
  assert(deferred_model.saw_correction());
  const auto deferred_history = deferred_session.load();
  assert(deferred_history);
  assert(deferred_history.value().size() == 4);
  assert(deferred_history.value()[0].role == Role::user);
  assert(deferred_history.value()[1].role == Role::assistant);
  assert(deferred_history.value()[1].tool_calls.size() == 1);
  assert(deferred_history.value()[2].role == Role::tool);
  assert(deferred_history.value()[3].content == "implemented and verified");

  RepeatedDeferredActionModel repeated_deferred_model;
  ToolRegistry repeated_deferred_tools;
  InMemorySessionStore repeated_deferred_session;
  BasicContextManager repeated_deferred_context(estimator);
  AgentLoop repeated_deferred_loop(
      repeated_deferred_model, repeated_deferred_tools,
      repeated_deferred_session, repeated_deferred_context, config);
  const auto repeated_deferred_result =
      repeated_deferred_loop.run("build the project");
  assert(!repeated_deferred_result);
  assert(repeated_deferred_result.error().code == ErrorCode::model_error);
  assert(repeated_deferred_result.error().message.find("repeatedly stopped") !=
         std::string::npos);
  assert(repeated_deferred_model.calls() == 2);
  const auto repeated_deferred_history = repeated_deferred_session.load();
  assert(repeated_deferred_history);
  assert(repeated_deferred_history.value().size() == 1);

  IncompleteModel incomplete_model;
  ToolRegistry incomplete_tools;
  InMemorySessionStore incomplete_session;
  BasicContextManager incomplete_context(estimator);
  AgentLoop incomplete_loop(incomplete_model, incomplete_tools,
                            incomplete_session, incomplete_context, config);
  std::size_t incomplete_usage_events = 0;
  const auto incomplete_result = incomplete_loop.run(
      "write a large file", {}, [&](const AgentEvent &event) {
        if (event.type == AgentEventType::assistant_message &&
            event.model_usage.has_value()) {
          ++incomplete_usage_events;
        }
      });
  assert(!incomplete_result);
  assert(incomplete_result.error().code == ErrorCode::model_error);
  assert(incomplete_result.error().message.find("output token limit") !=
         std::string::npos);
  assert(incomplete_usage_events == 1);
  const auto incomplete_history = incomplete_session.load();
  assert(incomplete_history);
  assert(incomplete_history.value().size() == 1);

  AdditionalSystemContextModel additional_context_model;
  ToolRegistry additional_context_tools;
  InMemorySessionStore additional_context_session;
  BasicContextManager additional_context_manager(estimator);
  auto additional_context_config = config;
  additional_context_config.system_prompt = "custom base prompt";
  AgentLoop additional_context_loop(
      additional_context_model, additional_context_tools,
      additional_context_session, additional_context_manager,
      additional_context_config);
  const auto additional_context_result = additional_context_loop.run(
      "exact user input", {}, {}, "active skill instructions");
  assert(additional_context_result);
  const auto additional_context_history = additional_context_session.load();
  assert(additional_context_history);
  assert(additional_context_history.value().size() == 2);
  assert(additional_context_history.value()[0].content == "exact user input");

  assert(is_valid_utf8("hello 你好"));
  assert(!is_valid_utf8("before\x80"
                        "after"));
  const auto sanitized = sanitize_utf8("before\x80"
                                       "after");
  assert(sanitized.replacement_count == 1);
  assert(sanitized.text.find("before") == 0);
  assert(sanitized.text.find("after") != std::string::npos);
  assert(sanitized.text.find('\x80') == std::string::npos);

  const std::vector<Message> paired_history{
      {"sys", Role::system, "sys", {}, std::nullopt},
      {"old-user", Role::user, std::string(400, 'x'), {}, std::nullopt},
      {"assistant-1",
       Role::assistant,
       {},
       {{"call-keep", "echo", std::string(40, 'a')}},
       std::nullopt},
      {"tool-1", Role::tool, "ok", {}, "call-keep"},
  };
  const auto paired_window =
      compaction_manager.build(paired_history, {80, 10, 10, true}, {});
  assert(paired_window);
  assert(paired_window.value().was_compacted);
  bool saw_assistant = false;
  bool saw_tool = false;
  bool saw_old_user = false;
  for (const auto &message : paired_window.value().messages) {
    saw_assistant = saw_assistant || message.id == "assistant-1";
    saw_tool = saw_tool || message.id == "tool-1";
    saw_old_user = saw_old_user || message.id == "old-user";
  }
  assert(saw_assistant);
  assert(saw_tool);
  assert(!saw_old_user);

  PartialToolContextController partial_controller;
  BasicContextManager controlled_compaction(compaction_estimator,
                                            &partial_controller);
  const std::vector<Message> partially_selected_history{
      {"partial-system", Role::system, "system", {}, std::nullopt},
      {"partial-old", Role::user, std::string(400, 'x'), {}, std::nullopt},
      {"partial-assistant",
       Role::assistant,
       {},
       {{"partial-call", "echo", R"({"purpose":"test"})"}},
       std::nullopt},
      {"partial-tool", Role::tool, "result", {}, "partial-call"},
      {"partial-current", Role::user, "current", {}, std::nullopt},
  };
  const auto controlled_window = controlled_compaction.build(
      partially_selected_history, {80, 10, 10, true}, {});
  assert(controlled_window);
  bool controlled_assistant = false;
  bool controlled_tool = false;
  for (const auto &message : controlled_window.value().messages) {
    controlled_assistant =
        controlled_assistant || message.id == "partial-assistant";
    controlled_tool = controlled_tool || message.id == "partial-tool";
  }
  assert(controlled_assistant);
  assert(controlled_tool);

  const std::vector<Message> oversized_pair{
      {"sys", Role::system, "sys", {}, std::nullopt},
      {"assistant-huge",
       Role::assistant,
       {},
       {{"call-huge", "echo", std::string(400, 'b')}},
       std::nullopt},
      {"tool-huge", Role::tool, "ok", {}, "call-huge"},
  };
  const auto oversized_window =
      compaction_manager.build(oversized_pair, {40, 8, 8, true}, {});
  assert(!oversized_window);
  assert(oversized_window.error().code == ErrorCode::context_error);

  CancellingContextController cancelling_controller;
  BasicContextManager cancelling_manager(compaction_estimator,
                                         &cancelling_controller);
  const std::vector<Message> cancellable_history{
      {"task", Role::user, std::string(400, 'z'), {}, std::nullopt}};
  const auto cancelled_window =
      cancelling_manager.build(cancellable_history, {80, 10, 10, true}, {});
  assert(!cancelled_window);
  assert(cancelled_window.error().code == ErrorCode::cancelled);

  CancellationSource pre_cancelled;
  pre_cancelled.cancel();
  FakeModel cancelled_model;
  ToolRegistry cancelled_tools;
  assert(cancelled_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore cancelled_session;
  BasicContextManager cancelled_context(estimator);
  AgentLoop cancelled_loop(cancelled_model, cancelled_tools, cancelled_session,
                           cancelled_context, config);
  const auto cancelled_run =
      cancelled_loop.run("cancel immediately", pre_cancelled.token());
  assert(!cancelled_run);
  assert(cancelled_run.error().code == ErrorCode::cancelled);
  assert(cancelled_model.calls() == 0);

  SlowCancelModel slow_model;
  ToolRegistry slow_tools;
  assert(slow_tools.register_tool(std::make_unique<SlowCancelTool>()));
  InMemorySessionStore slow_session;
  BasicContextManager slow_context(estimator);
  AgentLoop slow_loop(slow_model, slow_tools, slow_session, slow_context,
                      config);
  CancellationSource slow_cancellation;
  std::optional<Result<std::string>> slow_result;
  std::size_t slow_error_events = 0;
  std::thread slow_worker([&] {
    slow_result =
        slow_loop.run("cancel during the tool", slow_cancellation.token(),
                      [&](const AgentEvent &event) {
                        if (event.type == AgentEventType::error)
                          ++slow_error_events;
                      });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  slow_cancellation.cancel();
  slow_worker.join();
  assert(slow_result.has_value());
  assert(!slow_result.value());
  assert(slow_result->error().code == ErrorCode::cancelled);
  assert(slow_error_events >= 1);
  assert(slow_model.calls() == 1);

  return 0;
}
