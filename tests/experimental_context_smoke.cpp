#include <algorithm>
#include <cassert>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "zed/context/experimental_context.hpp"
#include "zed/core/agent_loop.hpp"
#include "zed/tools/context_tools.hpp"

namespace {

using namespace zed::core;
using zed::context::ContextArchive;
using zed::context::ExperimentalContextManager;

struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "zeda-experimental-context-XXXXXX")
                       .string();
    const auto *created = mkdtemp(pattern.data());
    assert(created != nullptr);
    path = created;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};

bool contains_id(const std::vector<Message> &messages, std::string_view id) {
  return std::any_of(messages.begin(), messages.end(),
                     [&](const auto &message) { return message.id == id; });
}

bool contains_text(const std::vector<Message> &messages,
                   std::string_view text) {
  return std::any_of(messages.begin(), messages.end(),
                     [&](const auto &message) {
                       return message.content.find(text) != std::string::npos;
                     });
}

void seed(InMemorySessionStore &session, int count = 8) {
  for (int i = 0; i < count; ++i) {
    assert(session.append(
        {"old-user-" + std::to_string(i),
         Role::user,
         i == 0 ? "RECOVERY_NEEDLE: exact value 7349" : "old request",
         {},
         std::nullopt}));
    assert(session.append({"old-assistant-" + std::to_string(i),
                           Role::assistant,
                           std::string(5000, static_cast<char>('a' + i)),
                           {},
                           std::nullopt}));
  }
}

std::vector<Message> request_messages(SessionStore &session) {
  const auto history = session.load();
  assert(history);
  std::vector<Message> messages{
      {"system", Role::system, "Follow the user's request.", {}, std::nullopt}};
  messages.insert(messages.end(), history.value().begin(),
                  history.value().end());
  return messages;
}

void budget_rotation_and_resume() {
  TemporaryDirectory directory;
  InMemorySessionStore session;
  seed(session);
  assert(session.append({"latest-user",
                         Role::user,
                         "Keep THIS latest request intact",
                         {},
                         std::nullopt}));
  assert(session.append(
      {"pending-assistant",
       Role::assistant,
       {},
       {{"pending-1", "read", "{}"}, {"pending-2", "read", "{}"}},
       std::nullopt}));
  assert(session.append(
      {"result-1", Role::tool, "first result", {}, "pending-1"}));
  assert(session.append(
      {"result-2", Role::tool, "second result", {}, "pending-2"}));
  const auto journal = directory.path / "session.context";
  ContextArchive archive(session, [&] { return journal; });
  assert(archive.write_note("checkpoint",
                            "Verified the build; next inspect parser.", false));
  ApproximateTokenEstimator estimator;
  ExperimentalContextManager context(estimator, archive);
  ToolRegistry tools;
  assert(zed::tools::register_context_tools(tools, archive));
  const auto messages = request_messages(session);
  const ContextLimits limits{6000, 500, 4000};
  const auto window =
      context.build_request(messages, tools.definitions(), limits, {});
  assert(window);
  assert(window.value().transition);
  assert(window.value().transition->window_id == "window-2");
  assert(!window.value().transition->archived_ids.empty());
  assert(window.value().usage.input_tokens <= 5500);
  assert(contains_id(window.value().messages, "latest-user"));
  assert(contains_id(window.value().messages, "pending-assistant"));
  assert(contains_id(window.value().messages, "result-1"));
  assert(contains_id(window.value().messages, "result-2"));
  assert(contains_text(window.value().messages, "Verified the build"));
  assert(session.load().value().size() + 1 == messages.size());
  assert(contains_text(session.load().value(), "RECOVERY_NEEDLE"));

  ContextArchive reopened(session, [&] { return journal; });
  ExperimentalContextManager resumed(estimator, reopened);
  const auto again =
      resumed.build_request(messages, tools.definitions(), limits, {});
  assert(again);
  assert(!again.value().transition);
  assert(again.value().messages.size() == window.value().messages.size());
  assert(reopened.snapshot().value().windows.size() == 2);

  // Disabling the experiment ignores its sidecar and retains the previous
  // manager's exact full-history behavior within the legacy budget.
  BasicContextManager legacy(estimator);
  const auto off = legacy.build_request(messages, tools.definitions(),
                                        {100000, 500, 80000}, {});
  assert(off);
  assert(off.value().messages.size() == messages.size());
  assert(
      !contains_text(off.value().messages, "Experimental context management"));
  assert(!off.value().transition);

  assert(session.append({"consumed-results",
                         Role::assistant,
                         "Both tool results were checked.",
                         {},
                         std::nullopt}));
  for (int i = 0; i < 5; ++i) {
    assert(session.append({"next-user-" + std::to_string(i),
                           Role::user,
                           "next work",
                           {},
                           std::nullopt}));
    assert(session.append({"next-assistant-" + std::to_string(i),
                           Role::assistant,
                           std::string(5000, 'z'),
                           {},
                           std::nullopt}));
  }
  assert(session.append({"newest-user",
                         Role::user,
                         "Continue with this newer constraint.",
                         {},
                         std::nullopt}));
  const auto third = resumed.build_request(request_messages(session),
                                           tools.definitions(), limits, {});
  assert(third && third.value().transition);
  assert(third.value().transition->previous_window_id == "window-2");
  assert(third.value().transition->window_id == "window-3");
  assert(contains_id(third.value().messages, "newest-user"));
  assert(contains_text(third.value().messages, "Verified the build"));
  assert(!contains_id(third.value().messages, "old-user-0"));
  assert(reopened.snapshot().value().windows.size() == 3);
}

void required_overflow_and_cancellation() {
  TemporaryDirectory directory;
  InMemorySessionStore session;
  seed(session, 1);
  assert(session.append(
      {"latest-user", Role::user, std::string(50000, 'x'), {}, std::nullopt}));
  const auto journal = directory.path / "budget.context";
  ContextArchive archive(session, [&] { return journal; });
  ApproximateTokenEstimator estimator;
  ExperimentalContextManager manager(estimator, archive);
  const auto messages = request_messages(session);
  const auto overflow = manager.build(messages, {2000, 200, 1000}, {});
  assert(!overflow);
  assert(overflow.error().code == ErrorCode::context_error);
  assert(!std::filesystem::exists(journal));
  assert(session.load().value().size() == 3);
  CancellationSource cancellation;
  cancellation.cancel();
  const auto cancelled =
      manager.build(messages, {100000, 500, 80000}, cancellation.token());
  assert(!cancelled && cancelled.error().code == ErrorCode::cancelled);
  assert(!std::filesystem::exists(journal));
  const auto missing_tools =
      manager.build_request(messages, {}, {100000, 500, 80000}, {});
  assert(!missing_tools &&
         missing_tools.error().code == ErrorCode::context_error);
}

void notes_preview_does_not_displace_request() {
  TemporaryDirectory directory;
  InMemorySessionStore session;
  assert(session.append(
      {"latest-user", Role::user, std::string(500, 'u'), {}, std::nullopt}));
  ContextArchive archive(session,
                         [&] { return directory.path / "notes.context"; });
  ApproximateTokenEstimator estimator;
  ExperimentalContextManager manager(estimator, archive);
  const auto messages = request_messages(session);
  const auto baseline = manager.build(messages, {5000, 100, 4000}, {});
  assert(baseline);
  assert(archive.write_note("large", std::string(4096, '\x01'), false));
  const auto budget = baseline.value().usage.input_tokens + 120;
  const auto result = manager.build(messages, {budget, 100, 0}, {});
  assert(result);
  assert(contains_id(result.value().messages, "latest-user"));
  assert(result.value().usage.input_tokens <= budget - 100);
  assert(!contains_id(result.value().messages, "zeda-context-notes"));
  assert(archive.snapshot().value().notes.at("large").size() == 4096);
}

class SentinelTool final : public Tool {
public:
  const ToolDefinition &definition() const override { return definition_; }
  Result<ToolResult> execute(const ToolCall &call, CancellationToken) override {
    ++calls;
    return Result<ToolResult>::success(
        {call.id, "SENTINEL_RESULT_AFTER_RESET_REQUEST"});
  }
  int calls{};

private:
  ToolDefinition definition_{"sentinel", "Deterministic integration fixture",
                             R"({"type":"object"})"};
};

class RecoveryModel final : public Model {
public:
  Result<AssistantResponse> complete(const ModelRequest &request,
                                     const StreamCallback &,
                                     CancellationToken) override {
    assert(contains_text(request.messages, "CURRENT_GOAL"));
    assert(contains_text(request.messages, "Experimental context management"));
    ++calls;
    if (calls == 1) {
      return Result<AssistantResponse>::success(
          {{},
           {{"write-note", "context_notes",
             R"({"purpose":"Save checkpoint","action":"write","name":"checkpoint","text":"CURRENT_GOAL: recover exact value; search RECOVERY_NEEDLE."})"},
            {"reset", "new_context",
             R"({"purpose":"Start next context window"})"},
            {"sentinel", "sentinel",
             R"({"purpose":"Verify full tool batch completes"})"}},
           FinishReason::tool_calls,
           {1, 0, 1}});
    }
    if (calls == 2) {
      assert(contains_text(request.messages, "window-2"));
      assert(contains_text(request.messages,
                           "SENTINEL_RESULT_AFTER_RESET_REQUEST"));
      assert(!contains_text(request.messages, "exact value 7349"));
      return Result<AssistantResponse>::success(
          {{},
           {{"search", "context_history",
             R"({"purpose":"Recover earlier detail","action":"search","query":"RECOVERY_NEEDLE"})"}},
           FinishReason::tool_calls,
           {1, 0, 1}});
    }
    if (calls == 3) {
      assert(contains_text(request.messages, "old-user-0"));
      return Result<AssistantResponse>::success(
          {{},
           {{"read", "context_history",
             R"({"purpose":"Read original evidence","action":"read","id":"old-user-0"})"}},
           FinishReason::tool_calls,
           {1, 0, 1}});
    }
    assert(calls == 4);
    assert(contains_text(request.messages, "7349"));
    return Result<AssistantResponse>::success(
        {"Recovered exact value: 7349.", {}, FinishReason::stop, {1, 0, 1}});
  }
  int calls{};
};

void full_agent_loop_recovery() {
  TemporaryDirectory directory;
  InMemorySessionStore session;
  seed(session);
  const auto journal = directory.path / "loop.context";
  ContextArchive archive(session, [&] { return journal; });
  ApproximateTokenEstimator estimator;
  ExperimentalContextManager context(estimator, archive);
  ToolRegistry tools;
  assert(zed::tools::register_context_tools(tools, archive));
  auto sentinel = std::make_unique<SentinelTool>();
  auto *sentinel_handle = sentinel.get();
  assert(tools.register_tool(std::move(sentinel)));
  RecoveryModel model;
  AgentLoopConfig config;
  config.model_request.model = {"fixture", "recovery"};
  config.context_limits = {20000, 500, 15000};
  config.max_turns = 8;
  config.system_prompt = "Complete the current task using the available tools.";
  AgentLoop loop(model, tools, session, context, config);
  std::vector<AgentEvent> events;
  const auto result =
      loop.run("CURRENT_GOAL: recover the exact earlier value.", {},
               [&](const auto &event) { events.push_back(event); });
  assert(result && result.value().find("7349") != std::string::npos);
  assert(sentinel_handle->calls == 1);
  assert(model.calls == 4);
  assert(archive.snapshot().value().windows.size() == 2);
  assert(archive.snapshot().value().notes.contains("checkpoint"));
  const auto transition =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.type == AgentEventType::context_window;
      });
  assert(transition != events.end() && transition->context_transition);
  assert(std::find(transition->context_transition->archived_ids.begin(),
                   transition->context_transition->archived_ids.end(),
                   "old-user-0") !=
         transition->context_transition->archived_ids.end());
  const auto sentinel_result =
      std::find_if(events.begin(), events.end(), [](const auto &event) {
        return event.type == AgentEventType::tool_result && event.tool_result &&
               event.tool_result->content ==
                   "SENTINEL_RESULT_AFTER_RESET_REQUEST";
      });
  assert(sentinel_result < transition);
}

} // namespace

int main() {
  budget_rotation_and_resume();
  required_overflow_and_cancellation();
  notes_preview_does_not_displace_request();
  full_agent_loop_recovery();
}
