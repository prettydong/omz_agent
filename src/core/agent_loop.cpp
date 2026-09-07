#include "zed/core/agent_loop.hpp"

#include "zed/core/agent_hooks.hpp"
#include "zed/core/default_system_prompt.hpp"
#include "zed/core/tool_registry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <numeric>
#include <string_view>
#include <unordered_set>

namespace zed::core {

namespace {

TokenCount estimated_tokens(std::size_t bytes) {
  return static_cast<TokenCount>((bytes + 3) / 4);
}

TokenCount estimated_message_tokens(const Message &message) {
  std::size_t bytes = message.content.size() + message.id.size() + 8;
  for (const auto &call : message.tool_calls) {
    bytes += call.id.size() + call.name.size() + call.arguments_json.size();
  }
  return estimated_tokens(bytes);
}

ContextTokenBreakdown context_breakdown_for(const ModelRequest &request,
                                            TokenCount exact_total) {
  // Providers usually report only a total input count. Estimate each category,
  // then scale estimates down proportionally when they exceed that exact total.
  // Rounding leftovers remain visible as `other_tokens` instead of being lost.
  std::array<TokenCount, 5> estimated{};
  for (const auto &message : request.messages) {
    const auto tokens = estimated_message_tokens(message);
    switch (message.role) {
    case Role::system:
      estimated[0] += tokens;
      break;
    case Role::user:
      estimated[1] += tokens;
      break;
    case Role::assistant:
      estimated[2] += tokens;
      break;
    case Role::tool:
      estimated[3] += tokens;
      break;
    }
  }
  for (const auto &definition : request.tools) {
    estimated[4] += estimated_tokens(definition.name.size() +
                                     definition.description.size() +
                                     definition.input_schema_json.size() + 24);
  }

  const auto estimated_total =
      std::accumulate(estimated.begin(), estimated.end(), TokenCount{});
  std::array<TokenCount, 5> reconciled = estimated;
  if (estimated_total > exact_total && estimated_total > 0) {
    for (std::size_t index = 0; index < reconciled.size(); ++index) {
      const auto share = static_cast<long double>(estimated[index]) /
                         static_cast<long double>(estimated_total);
      reconciled[index] = static_cast<TokenCount>(
          share * static_cast<long double>(exact_total));
    }
  }
  auto categorized_total =
      std::accumulate(reconciled.begin(), reconciled.end(), TokenCount{});
  for (std::size_t index = 0; index < reconciled.size(); ++index) {
    if (estimated[index] > 0 && reconciled[index] == 0 &&
        categorized_total < exact_total) {
      reconciled[index] = 1;
      ++categorized_total;
    }
  }
  const auto other_tokens = exact_total > categorized_total
                                ? exact_total - categorized_total
                                : TokenCount{};
  return {reconciled[0], reconciled[1], reconciled[2],
          reconciled[3], reconciled[4], other_tokens};
}

std::string next_id(const char *prefix) {
  static std::atomic_uint64_t sequence{0};
  static const auto process_nonce =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return std::string(prefix) + "-" + std::to_string(process_nonce) + "-" +
         std::to_string(++sequence);
}

Error cancelled_error() {
  return {ErrorCode::cancelled, "agent operation cancelled"};
}

Error invalid_tool_call_error(const ToolCall &call) {
  return {
      ErrorCode::model_error,
      "model returned an incomplete tool call" +
          (call.name.empty() ? std::string{} : ": " + call.name),
  };
}

Result<void> validate_tool_calls(const std::vector<ToolCall> &calls) {
  std::unordered_set<ToolCallId> ids;
  for (const auto &call : calls) {
    if (call.id.empty() || call.name.empty() || call.arguments_json.empty()) {
      return Result<void>::failure(invalid_tool_call_error(call));
    }
    const auto purpose = tool_call_purpose(call);
    if (!purpose) {
      return Result<void>::failure({
          ErrorCode::model_error,
          "model returned a tool call without a valid purpose: " + call.name +
              ": " + purpose.error().message,
      });
    }
    if (!ids.insert(call.id).second) {
      return Result<void>::failure({
          ErrorCode::model_error,
          "model returned duplicate tool call id: " + call.id,
      });
    }
  }
  return Result<void>::success();
}

std::optional<Error>
terminal_response_error(const AssistantResponse &response) {
  switch (response.finish_reason) {
  case FinishReason::length:
    return Error{ErrorCode::model_error,
                 "model response was incomplete because it reached the output "
                 "token limit"};
  case FinishReason::content_filter:
    return Error{ErrorCode::model_error,
                 "model response was blocked by the provider content filter"};
  case FinishReason::cancelled:
    return cancelled_error();
  case FinishReason::unknown:
    if (response.tool_calls.empty()) {
      return Error{ErrorCode::model_error,
                   "model response ended without a recognized terminal status"};
    }
    return std::nullopt;
  case FinishReason::tool_calls:
    if (response.tool_calls.empty()) {
      return Error{ErrorCode::model_error,
                   "model reported tool calls but returned no tool call"};
    }
    return std::nullopt;
  case FinishReason::stop:
    return std::nullopt;
  }
  return Error{ErrorCode::model_error,
               "model response used an unsupported terminal status"};
}

} // namespace

AgentLoop::AgentLoop(Model &model, ToolRegistry &tools, SessionStore &session,
                     ContextManager &context, AgentLoopConfig config,
                     HookRegistry *hooks)
    : model_(model), tools_(tools), session_(session), context_(context),
      config_(std::move(config)), hooks_(hooks) {
  if (config_.system_prompt.empty())
    config_.system_prompt = kDefaultSystemPrompt;
}

void AgentLoop::set_reasoning_effort(ReasoningEffort effort) {
  config_.model_request.reasoning_effort = effort;
}

ReasoningEffort AgentLoop::reasoning_effort() const {
  return config_.model_request.reasoning_effort;
}

void AgentLoop::set_model(ModelRef model) {
  config_.model_request.model = std::move(model);
}

const ModelRef &AgentLoop::model() const { return config_.model_request.model; }

void AgentLoop::set_context_limits(ContextLimits limits) {
  config_.context_limits = limits;
}

const ContextLimits &AgentLoop::context_limits() const {
  return config_.context_limits;
}

Result<std::string> AgentLoop::run(std::string user_input,
                                   CancellationToken cancellation,
                                   AgentEventCallback on_event,
                                   std::string additional_system_prompt) {
  if (user_input.empty()) {
    return Result<std::string>::failure({
        ErrorCode::invalid_argument,
        "user input cannot be empty",
    });
  }
  if (config_.max_turns == 0) {
    return Result<std::string>::failure({
        ErrorCode::invalid_argument,
        "max_turns must be greater than zero",
    });
  }
  if (cancellation.is_cancelled()) {
    const auto error = cancelled_error();
    emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
         on_event);
    return Result<std::string>::failure(error);
  }

  emit({AgentEventType::agent_start, {}, std::nullopt, std::nullopt}, on_event);

  const auto turn_id = next_id("turn");
  AgentHooks hooks(hooks_);
  auto turn_start =
      hooks.agent_turn_start(turn_id, std::move(user_input), cancellation);
  if (!turn_start) {
    emit({AgentEventType::error, turn_start.error().message, std::nullopt,
          std::nullopt},
         on_event);
    return Result<std::string>::failure(turn_start.error());
  }
  Message user_message{
      next_id("user"), Role::user, std::move(turn_start.value()), {},
      std::nullopt,
  };
  auto submitted =
      hooks.user_message_submit(turn_id, std::move(user_message), cancellation);
  if (!submitted) {
    emit({AgentEventType::error, submitted.error().message, std::nullopt,
          std::nullopt},
         on_event);
    return Result<std::string>::failure(submitted.error());
  }
  auto session_message = hooks.session_write_message(
      "begin_turn", turn_id, std::move(submitted.value()), cancellation);
  if (!session_message) {
    emit({AgentEventType::error, session_message.error().message, std::nullopt,
          std::nullopt},
         on_event);
    return Result<std::string>::failure(session_message.error());
  }
  user_message = std::move(session_message.value());
  const auto begin_turn = session_.begin_turn(turn_id, user_message);
  if (!begin_turn) {
    emit({AgentEventType::error, begin_turn.error().message, std::nullopt,
          std::nullopt},
         on_event);
    return Result<std::string>::failure(begin_turn.error());
  }
  emit({AgentEventType::user_message, user_message.content, std::nullopt,
        std::nullopt},
       on_event);

  auto result = run_active_turn(turn_id, cancellation, on_event,
                                additional_system_prompt);
  auto outcome = result ? SessionTurnOutcome::completed
                 : result.error().code == ErrorCode::cancelled
                     ? SessionTurnOutcome::cancelled
                     : SessionTurnOutcome::failed;
  std::string detail = result ? std::string{} : result.error().message;

  auto turn_end = hooks.agent_turn_end(turn_id, outcome, detail, {});
  if (!turn_end) {
    emit({AgentEventType::error, turn_end.error().message, std::nullopt,
          std::nullopt},
         on_event);
    result = Result<std::string>::failure(turn_end.error());
    outcome = turn_end.error().code == ErrorCode::cancelled
                  ? SessionTurnOutcome::cancelled
                  : SessionTurnOutcome::failed;
    detail = turn_end.error().message;
  } else {
    outcome = turn_end.value().outcome;
    detail = std::move(turn_end.value().detail);
    if (result && outcome != SessionTurnOutcome::completed) {
      const Error error{
          ErrorCode::hook_error,
          detail.empty()
              ? "agent_turn_end hook changed a successful turn to " +
                    std::string(outcome == SessionTurnOutcome::cancelled
                                    ? "cancelled"
                                    : "failed")
              : detail,
      };
      emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
           on_event);
      result = Result<std::string>::failure(error);
      detail = error.message;
    } else if (!result && outcome == SessionTurnOutcome::completed) {
      const Error error{ErrorCode::hook_error,
                        "agent_turn_end hook cannot complete a failed turn"};
      emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
           on_event);
      result = Result<std::string>::failure(error);
      outcome = SessionTurnOutcome::failed;
      detail = error.message;
    }
  }

  auto session_finish =
      hooks.session_write_finish(turn_id, outcome, detail, {});
  if (!session_finish) {
    const auto recovery_outcome =
        session_finish.error().code == ErrorCode::cancelled
            ? SessionTurnOutcome::cancelled
            : SessionTurnOutcome::failed;
    const auto recovery = session_.finish_turn(turn_id, recovery_outcome,
                                               session_finish.error().message);
    if (!recovery) {
      const Error error{
          ErrorCode::session_error,
          session_finish.error().message +
              "; cannot close rejected session write: " +
              recovery.error().message,
      };
      emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(error);
    }
    emit({AgentEventType::error, session_finish.error().message, std::nullopt,
          std::nullopt},
         on_event);
    return Result<std::string>::failure(session_finish.error());
  }
  outcome = session_finish.value().outcome;
  detail = std::move(session_finish.value().detail);
  if (result && outcome != SessionTurnOutcome::completed) {
    const Error error{
        ErrorCode::hook_error,
        detail.empty() ? "session_write hook changed a successful turn outcome"
                       : detail,
    };
    emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
         on_event);
    result = Result<std::string>::failure(error);
    detail = error.message;
  } else if (!result && outcome == SessionTurnOutcome::completed) {
    const Error error{ErrorCode::hook_error,
                      "session_write hook cannot complete a failed turn"};
    emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
         on_event);
    result = Result<std::string>::failure(error);
    outcome = SessionTurnOutcome::failed;
    detail = error.message;
  }

  const auto finish_turn = session_.finish_turn(turn_id, outcome, detail);
  if (!finish_turn) {
    emit({AgentEventType::error, finish_turn.error().message, std::nullopt,
          std::nullopt},
         on_event);
    return Result<std::string>::failure(finish_turn.error());
  }

  if (result) {
    emit(
        {AgentEventType::agent_end, result.value(), std::nullopt, std::nullopt},
        on_event);
  }
  return result;
}

Result<std::string> AgentLoop::run_active_turn(
    std::string_view turn_id, CancellationToken cancellation,
    AgentEventCallback on_event, const std::string &additional_system_prompt) {
  AgentHooks hooks(hooks_);
  for (std::size_t turn = 0; turn < config_.max_turns; ++turn) {
    if (cancellation.is_cancelled()) {
      const auto error = cancelled_error();
      emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(error);
    }

    const auto history = session_.load();
    if (!history) {
      emit({AgentEventType::error, history.error().message, std::nullopt,
            std::nullopt},
           on_event);
      return Result<std::string>::failure(history.error());
    }

    std::vector<Message> context_messages;
    context_messages.reserve(history.value().size() + 1);
    std::string system_prompt = config_.system_prompt;
    if (!additional_system_prompt.empty()) {
      system_prompt += "\n\n";
      system_prompt += additional_system_prompt;
    }
    context_messages.push_back({"zeda-agent-system",
                                Role::system,
                                std::move(system_prompt),
                                {},
                                std::nullopt});
    context_messages.insert(context_messages.end(), history.value().begin(),
                            history.value().end());

    const auto window =
        context_.build(context_messages, config_.context_limits, cancellation);
    if (!window) {
      emit({AgentEventType::error, window.error().message, std::nullopt,
            std::nullopt},
           on_event);
      return Result<std::string>::failure(window.error());
    }

    ModelRequest request = config_.model_request;
    const auto conversation = session_.conversation_id();
    if (!conversation) {
      emit({AgentEventType::error, conversation.error().message, std::nullopt,
            std::nullopt},
           on_event);
      return Result<std::string>::failure(conversation.error());
    }
    request.session_id = conversation.value();
    request.messages = window.value().messages;
    request.tools = tools_.definitions();
    auto intercepted_request = hooks.before_model_request(
        turn_id, turn, std::move(request), cancellation);
    if (!intercepted_request) {
      emit({AgentEventType::error, intercepted_request.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(intercepted_request.error());
    }
    request = std::move(intercepted_request.value());

    const auto model_started_at = std::chrono::steady_clock::now();
    auto completion = model_.complete(
        request,
        [&](const ModelDelta &delta) {
          const auto event_type = delta.kind == ModelDeltaKind::reasoning
                                      ? AgentEventType::reasoning_delta
                                      : AgentEventType::assistant_delta;
          emit({event_type, delta.text, std::nullopt, std::nullopt}, on_event);
        },
        cancellation);
    const auto model_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      model_started_at)
            .count();
    if (!completion) {
      emit({AgentEventType::error, completion.error().message, std::nullopt,
            std::nullopt},
           on_event);
      return Result<std::string>::failure(completion.error());
    }
    auto response = std::move(completion.value());
    if (response.usage.output_tokens > 0 && model_elapsed > 0.0) {
      response.usage.output_tokens_per_second =
          static_cast<double>(response.usage.output_tokens) / model_elapsed;
    }
    response.usage.context_breakdown =
        context_breakdown_for(request, response.usage.input_tokens);

    if (cancellation.is_cancelled()) {
      const auto error = cancelled_error();
      emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(error);
    }
    auto intercepted_response = hooks.after_model_response(
        turn_id, turn, std::move(response), cancellation);
    if (!intercepted_response) {
      emit({AgentEventType::error, intercepted_response.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(intercepted_response.error());
    }
    response = std::move(intercepted_response.value());
    if (const auto error = terminal_response_error(response);
        error.has_value()) {
      emit({AgentEventType::assistant_message, response.content, std::nullopt,
            std::nullopt, response.usage},
           on_event);
      emit({AgentEventType::error, error->message, std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(*error);
    }
    const auto valid_tool_calls = validate_tool_calls(response.tool_calls);
    if (!valid_tool_calls) {
      emit({AgentEventType::error, valid_tool_calls.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(valid_tool_calls.error());
    }

    for (std::size_t call_index = 0; call_index < response.tool_calls.size();
         ++call_index) {
      const auto original_call = response.tool_calls[call_index];
      auto intercepted_call = hooks.before_tool_call(
          turn_id, turn, call_index, std::move(response.tool_calls[call_index]),
          cancellation);
      if (!intercepted_call) {
        emit({AgentEventType::error, intercepted_call.error().message,
              std::nullopt, std::nullopt},
             on_event);
        return Result<std::string>::failure(intercepted_call.error());
      }
      if (!response.model_state.empty() &&
          (intercepted_call.value().name != original_call.name ||
           intercepted_call.value().arguments_json !=
               original_call.arguments_json)) {
        const Error error{
            ErrorCode::hook_error,
            "tool call linked to provider continuation cannot be modified"};
        emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
             on_event);
        return Result<std::string>::failure(error);
      }
      response.tool_calls[call_index] = std::move(intercepted_call.value());
    }
    const auto intercepted_calls_valid =
        validate_tool_calls(response.tool_calls);
    if (!intercepted_calls_valid) {
      emit({AgentEventType::error, intercepted_calls_valid.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(intercepted_calls_valid.error());
    }

    Message assistant_message{
        next_id("assistant"), Role::assistant, response.content,
        response.tool_calls,  std::nullopt,
    };
    assistant_message.model_state = response.model_state;
    auto intercepted_assistant = hooks.session_write_message(
        "append_message", turn_id, std::move(assistant_message), cancellation);
    if (!intercepted_assistant) {
      emit({AgentEventType::error, intercepted_assistant.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(intercepted_assistant.error());
    }
    assistant_message = std::move(intercepted_assistant.value());
    const auto persisted_calls_valid =
        validate_tool_calls(assistant_message.tool_calls);
    if (!persisted_calls_valid) {
      emit({AgentEventType::error, persisted_calls_valid.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(persisted_calls_valid.error());
    }
    const auto append_assistant = session_.append(assistant_message);
    if (!append_assistant) {
      emit({AgentEventType::error, append_assistant.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(append_assistant.error());
    }
    emit({AgentEventType::assistant_message, assistant_message.content,
          std::nullopt, std::nullopt, response.usage},
         on_event);

    if (assistant_message.tool_calls.empty()) {
      return Result<std::string>::success(assistant_message.content);
    }

    for (std::size_t call_index = 0;
         call_index < assistant_message.tool_calls.size(); ++call_index) {
      const auto &call = assistant_message.tool_calls[call_index];
      if (cancellation.is_cancelled()) {
        const auto error = cancelled_error();
        emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
             on_event);
        return Result<std::string>::failure(error);
      }
      const auto purpose = tool_call_purpose(call);
      emit({AgentEventType::tool_start, purpose.value(), call, std::nullopt},
           on_event);

      ToolResult tool_result;
      const auto execution =
          tools_.execute(call, cancellation, [&](const ToolProgress &progress) {
            emit({AgentEventType::tool_update, progress.text, call,
                  std::nullopt},
                 on_event);
          });
      if (execution) {
        tool_result = execution.value();
      } else if (execution.error().code == ErrorCode::cancelled) {
        emit({AgentEventType::error, execution.error().message, std::nullopt,
              std::nullopt},
             on_event);
        return Result<std::string>::failure(execution.error());
      } else {
        tool_result = {
            call.id,
            "tool execution failed: " + execution.error().message,
            true,
        };
      }

      auto intercepted_result =
          hooks.after_tool_result(turn_id, turn, call_index, call.name,
                                  std::move(tool_result), cancellation);
      if (!intercepted_result) {
        emit({AgentEventType::error, intercepted_result.error().message,
              std::nullopt, std::nullopt},
             on_event);
        return Result<std::string>::failure(intercepted_result.error());
      }
      tool_result = std::move(intercepted_result.value());

      Message tool_message{
          next_id("tool"),          Role::tool,
          tool_result.content,      {},
          tool_result.tool_call_id, tool_result.is_error,
      };
      auto intercepted_tool_message = hooks.session_write_message(
          "append_message", turn_id, std::move(tool_message), cancellation);
      if (!intercepted_tool_message) {
        emit({AgentEventType::error, intercepted_tool_message.error().message,
              std::nullopt, std::nullopt},
             on_event);
        return Result<std::string>::failure(intercepted_tool_message.error());
      }
      tool_message = std::move(intercepted_tool_message.value());
      tool_result.content = tool_message.content;
      tool_result.is_error = tool_message.is_error;
      tool_message.content = bound_tool_output(std::move(tool_message.content));
      tool_result.content = tool_message.content;
      const auto append_tool = session_.append(tool_message);
      if (!append_tool) {
        emit({AgentEventType::error, append_tool.error().message, std::nullopt,
              std::nullopt},
             on_event);
        return Result<std::string>::failure(append_tool.error());
      }
      emit({AgentEventType::tool_result, tool_result.content, std::nullopt,
            tool_result, tool_result.model_usage},
           on_event);
    }
  }

  const Error error{
      ErrorCode::internal,
      "agent exceeded the maximum number of turns",
  };
  emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
       on_event);
  return Result<std::string>::failure(error);
}

void AgentLoop::emit(const AgentEvent &event,
                     const AgentEventCallback &callback) const {
  if (callback) {
    callback(event);
  }
}

} // namespace zed::core
