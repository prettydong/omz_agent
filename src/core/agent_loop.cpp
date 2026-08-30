#include "zed/core/agent_loop.hpp"

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

constexpr std::string_view kDeferredActionCorrection =
    "The previous attempt stopped at a progress update without taking the "
    "promised action. Do not provide another progress update. Use the tools "
    "now "
    "to complete and verify the current request. If action is impossible, give "
    "the exact blocker instead of promising future work.";

constexpr std::size_t kMaxDeferredActionRetries = 1;

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

std::string trim_ascii(std::string value) {
  const auto is_space = [](unsigned char character) {
    return std::isspace(character) != 0;
  };
  const auto begin = std::find_if_not(value.begin(), value.end(), is_space);
  const auto end =
      std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (begin >= end)
    return {};
  return std::string(begin, end);
}

std::string lowercase_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

bool looks_like_deferred_action(std::string_view content) {
  constexpr std::size_t kMaxProgressUpdateBytes = 512;
  const auto trimmed = trim_ascii(std::string(content));
  if (trimmed.empty() || trimmed.size() > kMaxProgressUpdateBytes)
    return false;

  static constexpr std::string_view kMarkers[] = {
      "正在构建",      "正在生成",      "正在创建",     "正在处理",
      "正在修改",      "马上就好",      "请稍等",       "稍等一下",
      "马上开始",      "现在开始",      "接下来我会",   "我现在就",
      "继续完成",      "working on it", "i'm working",  "i am working",
      "i'll start",    "i will start",  "let me start", "i'll now",
      "i will now",    "starting now",  "i'm building", "i am building",
      "be right back",
  };
  const auto normalized = lowercase_ascii(trimmed);
  return std::any_of(std::begin(kMarkers), std::end(kMarkers),
                     [&](std::string_view marker) {
                       return normalized.find(marker) != std::string::npos;
                     });
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
                     ContextManager &context, AgentLoopConfig config)
    : model_(model), tools_(tools), session_(session), context_(context),
      config_(std::move(config)) {
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
  Message user_message{
      next_id("user"), Role::user, std::move(user_input), {}, std::nullopt,
  };
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

  auto result =
      run_active_turn(cancellation, on_event, additional_system_prompt);
  const auto outcome = result ? SessionTurnOutcome::completed
                       : result.error().code == ErrorCode::cancelled
                           ? SessionTurnOutcome::cancelled
                           : SessionTurnOutcome::failed;
  const auto detail =
      result ? std::string_view{} : std::string_view(result.error().message);
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

Result<std::string>
AgentLoop::run_active_turn(CancellationToken cancellation,
                           AgentEventCallback on_event,
                           const std::string &additional_system_prompt) {
  std::size_t deferred_action_retries = 0;
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
    if (deferred_action_retries > 0) {
      system_prompt += "\n\n";
      system_prompt += kDeferredActionCorrection;
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
    request.messages = window.value().messages;
    request.tools = tools_.definitions();

    const auto model_started_at = std::chrono::steady_clock::now();
    auto response = model_.complete(
        request,
        [&](const ModelDelta &delta) {
          emit({AgentEventType::assistant_delta, delta.text, std::nullopt,
                std::nullopt},
               on_event);
        },
        cancellation);
    const auto model_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      model_started_at)
            .count();
    if (!response) {
      emit({AgentEventType::error, response.error().message, std::nullopt,
            std::nullopt},
           on_event);
      return Result<std::string>::failure(response.error());
    }
    if (response.value().usage.output_tokens > 0 && model_elapsed > 0.0) {
      response.value().usage.output_tokens_per_second =
          static_cast<double>(response.value().usage.output_tokens) /
          model_elapsed;
    }
    response.value().usage.context_breakdown =
        context_breakdown_for(request, response.value().usage.input_tokens);

    if (cancellation.is_cancelled()) {
      const auto error = cancelled_error();
      emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(error);
    }
    if (const auto error = terminal_response_error(response.value());
        error.has_value()) {
      emit({AgentEventType::assistant_message, response.value().content,
            std::nullopt, std::nullopt, response.value().usage},
           on_event);
      emit({AgentEventType::error, error->message, std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(*error);
    }
    const auto valid_tool_calls =
        validate_tool_calls(response.value().tool_calls);
    if (!valid_tool_calls) {
      emit({AgentEventType::error, valid_tool_calls.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(valid_tool_calls.error());
    }

    if (response.value().tool_calls.empty() &&
        looks_like_deferred_action(response.value().content)) {
      emit({AgentEventType::assistant_message, response.value().content,
            std::nullopt, std::nullopt, response.value().usage},
           on_event);
      if (deferred_action_retries >= kMaxDeferredActionRetries) {
        const Error error{
            ErrorCode::model_error,
            "model repeatedly stopped after a progress update without taking "
            "the promised action",
        };
        emit({AgentEventType::error, error.message, std::nullopt, std::nullopt},
             on_event);
        return Result<std::string>::failure(error);
      }
      ++deferred_action_retries;
      continue;
    }

    Message assistant_message{
        next_id("assistant"),        Role::assistant, response.value().content,
        response.value().tool_calls, std::nullopt,
    };
    const auto append_assistant = session_.append(assistant_message);
    if (!append_assistant) {
      emit({AgentEventType::error, append_assistant.error().message,
            std::nullopt, std::nullopt},
           on_event);
      return Result<std::string>::failure(append_assistant.error());
    }
    emit({AgentEventType::assistant_message, assistant_message.content,
          std::nullopt, std::nullopt, response.value().usage},
         on_event);

    if (response.value().tool_calls.empty()) {
      return Result<std::string>::success(response.value().content);
    }

    for (const auto &call : response.value().tool_calls) {
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
        return Result<std::string>::failure(execution.error());
      } else {
        tool_result = {
            call.id,
            "tool execution failed: " + execution.error().message,
            true,
        };
      }

      Message tool_message{
          next_id("tool"),          Role::tool,
          tool_result.content,      {},
          tool_result.tool_call_id, tool_result.is_error,
      };
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
