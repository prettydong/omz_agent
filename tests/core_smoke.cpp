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

class MissingPurposeModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &,
                                     const StreamCallback &,
                                     CancellationToken) override {
    return Result<AssistantResponse>::success({
        {},
        {{"missing-purpose", "echo", R"({"text":"hello"})"}},
        FinishReason::tool_calls,
        {},
    });
  }
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

  MissingPurposeModel missing_purpose_model;
  ToolRegistry guarded_tools;
  assert(guarded_tools.register_tool(std::make_unique<EchoTool>()));
  InMemorySessionStore guarded_session;
  BasicContextManager guarded_context(estimator);
  AgentLoop guarded_loop(missing_purpose_model, guarded_tools, guarded_session,
                         guarded_context, config);
  const auto rejected = guarded_loop.run("run invalid echo");
  assert(!rejected);
  assert(rejected.error().code == ErrorCode::model_error);
  const auto guarded_history = guarded_session.load();
  assert(guarded_history);
  assert(guarded_history.value().size() == 1);
  assert(guarded_history.value()[0].role == Role::user);

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
