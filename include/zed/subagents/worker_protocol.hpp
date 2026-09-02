#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "zed/core/model.hpp"
#include "zed/core/result.hpp"

namespace zed::subagents {

inline constexpr std::size_t kWorkerProtocolVersion = 2;
inline constexpr std::size_t kMaximumTaskBytes = 32 * 1024;
inline constexpr std::size_t kMaximumFinalOutputBytes = 32 * 1024;

enum class WorkerCommandType {
  run,
  cancel,
};

struct WorkerRequest {
  std::string request_id;
  std::string agent;
  std::string task;
};

struct WorkerCommand {
  WorkerCommandType type{WorkerCommandType::run};
  WorkerRequest request;
};

enum class WorkerEventType {
  started,
  tool_start,
  completed,
  failed,
  cancelled,
};

struct WorkerEvent {
  WorkerEvent() = default;
  WorkerEvent(WorkerEventType event_type, std::string event_request_id,
              std::string event_agent = {}, std::string event_tool = {},
              std::string event_purpose = {}, std::string event_content = {},
              std::string event_error = {}, core::ModelUsage event_usage = {})
      : type(event_type), request_id(std::move(event_request_id)),
        agent(std::move(event_agent)), tool(std::move(event_tool)),
        purpose(std::move(event_purpose)), content(std::move(event_content)),
        error(std::move(event_error)), usage(std::move(event_usage)) {}

  WorkerEventType type{WorkerEventType::started};
  std::string request_id;
  std::string agent;
  std::string tool;
  std::string purpose;
  std::string content;
  std::string error;
  core::ModelUsage usage;
};

[[nodiscard]] core::Result<WorkerCommand>
parse_worker_command(std::string_view json_line);

[[nodiscard]] std::string
serialize_worker_request(const WorkerRequest &request);

[[nodiscard]] std::string
serialize_worker_cancellation(std::string_view request_id);

[[nodiscard]] core::Result<WorkerEvent>
parse_worker_event(std::string_view json_line);

[[nodiscard]] std::string serialize_worker_event(const WorkerEvent &event);

[[nodiscard]] bool is_terminal_event(WorkerEventType type);

} // namespace zed::subagents
