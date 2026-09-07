#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "zed/core/hook_registry.hpp"
#include "zed/core/message.hpp"
#include "zed/core/model.hpp"
#include "zed/core/session_store.hpp"
#include "zed/core/tool.hpp"

namespace zed::core {

struct HookTurnEnd {
  SessionTurnOutcome outcome{SessionTurnOutcome::failed};
  std::string detail;
};

struct HookSessionFinish {
  SessionTurnOutcome outcome{SessionTurnOutcome::failed};
  std::string detail;
};

class AgentHooks {
public:
  explicit AgentHooks(HookRegistry *registry) : registry_(registry) {}

  Result<std::string> agent_turn_start(std::string_view turn_id,
                                       std::string user_input,
                                       CancellationToken cancellation) const;
  Result<Message> user_message_submit(std::string_view turn_id, Message message,
                                      CancellationToken cancellation) const;
  Result<ModelRequest>
  before_model_request(std::string_view turn_id, std::size_t iteration,
                       ModelRequest request,
                       CancellationToken cancellation) const;
  Result<AssistantResponse>
  after_model_response(std::string_view turn_id, std::size_t iteration,
                       AssistantResponse response,
                       CancellationToken cancellation) const;
  Result<ToolCall> before_tool_call(std::string_view turn_id,
                                    std::size_t iteration,
                                    std::size_t call_index, ToolCall call,
                                    CancellationToken cancellation) const;
  Result<ToolResult>
  after_tool_result(std::string_view turn_id, std::size_t iteration,
                    std::size_t call_index, std::string_view tool_name,
                    ToolResult result, CancellationToken cancellation) const;
  Result<Message> session_write_message(std::string_view operation,
                                        std::string_view turn_id,
                                        Message message,
                                        CancellationToken cancellation) const;
  Result<HookSessionFinish>
  session_write_finish(std::string_view turn_id, SessionTurnOutcome outcome,
                       std::string detail,
                       CancellationToken cancellation) const;
  Result<HookTurnEnd> agent_turn_end(std::string_view turn_id,
                                     SessionTurnOutcome outcome,
                                     std::string detail,
                                     CancellationToken cancellation) const;

private:
  HookRegistry *registry_{};
};

} // namespace zed::core
