#pragma once

#include <cstddef>
#include <string>

#include "zed/core/agent_event.hpp"
#include "zed/core/context.hpp"
#include "zed/core/model.hpp"
#include "zed/core/result.hpp"
#include "zed/core/session_store.hpp"
#include "zed/core/tool_registry.hpp"

namespace zed::core {

struct AgentLoopConfig {
  ModelRequest model_request;
  ContextLimits context_limits;
  std::size_t max_turns{32};
  std::string system_prompt;
};

class AgentLoop {
public:
  AgentLoop(Model &model, ToolRegistry &tools, SessionStore &session,
            ContextManager &context, AgentLoopConfig config);

  Result<std::string> run(std::string user_input,
                          CancellationToken cancellation = {},
                          AgentEventCallback on_event = {});

  void set_reasoning_effort(ReasoningEffort effort);
  [[nodiscard]] ReasoningEffort reasoning_effort() const;

private:
  void emit(const AgentEvent &event, const AgentEventCallback &callback) const;

  Model &model_;
  ToolRegistry &tools_;
  SessionStore &session_;
  ContextManager &context_;
  AgentLoopConfig config_;
};

} // namespace zed::core
