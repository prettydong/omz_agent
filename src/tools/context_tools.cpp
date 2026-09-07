#include "zed/tools/context_tools.hpp"

#include "zed/core/utf8.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string_view>

namespace zed::tools {
namespace {

using Json = nlohmann::json;
using core::ErrorCode;
using core::ToolCall;
using core::ToolDefinition;
using core::ToolResult;
constexpr std::size_t kMaxOutputBytes = 16U * 1024U;
constexpr std::size_t kDefaultPageSize = 20U;
constexpr std::size_t kMaxPageSize = 100U;
constexpr std::size_t kMaxPageBytes = 12U * 1024U;

const ToolDefinition &notes_definition() {
  static const ToolDefinition definition{
      "context_notes",
      "Manage private notes for this session. Write/append use name and text; "
      "read uses name and optional byte_offset/max_bytes. Follow "
      "next_byte_offset while truncated. List/search use offset/limit and "
      "next_offset. Search is literal and case-sensitive.",
      R"({"type":"object","required":["purpose","action"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call."},"action":{"type":"string","enum":["list","read","write","append","search"]},"name":{"type":"string"},"text":{"type":"string"},"query":{"type":"string"},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":100},"byte_offset":{"type":"integer","minimum":0},"max_bytes":{"type":"integer","minimum":1,"maximum":12288}},"additionalProperties":false})"};
  return definition;
}

const ToolDefinition &history_definition() {
  static const ToolDefinition definition{
      "context_history",
      "Search durable messages and tool arguments/results in this session. "
      "List/search/windows use offset/limit and next_offset while truncated. "
      "Read by id returns message_json as text byte pages; concatenate using "
      "next_byte_offset while truncated. Search is literal and case-sensitive. "
      "Optional window_id filters by the message's original window.",
      R"({"type":"object","required":["purpose","action"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call."},"action":{"type":"string","enum":["windows","list","read","search"]},"id":{"type":"string"},"window_id":{"type":"string"},"query":{"type":"string"},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":100},"byte_offset":{"type":"integer","minimum":0},"max_bytes":{"type":"integer","minimum":1,"maximum":12288}},"additionalProperties":false})"};
  return definition;
}

const ToolDefinition &new_context_definition() {
  static const ToolDefinition definition{
      "new_context",
      "Request a new context window after this tool batch is persisted.",
      R"({"type":"object","required":["purpose"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call."}},"additionalProperties":false})"};
  return definition;
}

core::Result<Json> parse_arguments(const ToolCall &call) {
  try {
    auto parsed = Json::parse(call.arguments_json);
    if (!parsed.is_object()) {
      return core::Result<Json>::failure(
          {ErrorCode::invalid_argument,
           "context tool arguments must be an object"});
    }
    return core::Result<Json>::success(std::move(parsed));
  } catch (const Json::exception &) {
    return core::Result<Json>::failure(
        {ErrorCode::invalid_argument,
         "context tool arguments are invalid JSON"});
  }
}

core::Result<std::string> required_string(const Json &arguments,
                                          std::string_view name) {
  const auto it = arguments.find(std::string(name));
  if (it == arguments.end() || !it->is_string() ||
      it->get_ref<const std::string &>().empty()) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "missing string argument: " + std::string(name)});
  }
  return core::Result<std::string>::success(it->get<std::string>());
}

core::Result<std::size_t>
page_value(const Json &arguments, std::string_view name, std::size_t fallback) {
  const auto it = arguments.find(std::string(name));
  if (it == arguments.end())
    return core::Result<std::size_t>::success(fallback);
  if (!it->is_number_unsigned()) {
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument,
         std::string(name) + " must be a non-negative integer"});
  }
  const auto value = it->get<std::uint64_t>();
  if (name == "limit" && value == 0)
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "limit must be positive"});
  if (value >
      static_cast<std::uint64_t>(name == "limit" ? kMaxPageSize : 1'000'000U)) {
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, std::string(name) + " exceeds limit"});
  }
  return core::Result<std::size_t>::success(static_cast<std::size_t>(value));
}

core::Result<std::size_t>
byte_value(const Json &arguments, std::string_view name, std::size_t fallback) {
  const auto it = arguments.find(std::string(name));
  if (it == arguments.end())
    return core::Result<std::size_t>::success(fallback);
  if (!it->is_number_unsigned()) {
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument,
         std::string(name) + " must be a non-negative integer"});
  }
  const auto value = it->get<std::uint64_t>();
  if (name == "max_bytes" && value == 0)
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, "max_bytes must be positive"});
  if (value > static_cast<std::uint64_t>(
                  name == "max_bytes" ? kMaxPageBytes : 8U * 1024U * 1024U)) {
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument, std::string(name) + " exceeds limit"});
  }
  return core::Result<std::size_t>::success(static_cast<std::size_t>(value));
}

std::size_t utf8_prefix(std::string_view text, std::size_t maximum) {
  const auto limit = std::min(text.size(), maximum);
  std::size_t position = 0;
  while (position < limit) {
    const auto byte = static_cast<unsigned char>(text[position]);
    std::size_t width = 1;
    if ((byte & 0xE0U) == 0xC0U)
      width = 2;
    else if ((byte & 0xF0U) == 0xE0U)
      width = 3;
    else if ((byte & 0xF8U) == 0xF0U)
      width = 4;
    if (position + width > limit)
      break;
    position += width;
  }
  return position;
}

core::Result<ToolResult> success(const ToolCall &call, const Json &value) {
  try {
    auto output = value.dump();
    if (output.size() > kMaxOutputBytes) {
      return core::Result<ToolResult>::failure(
          {ErrorCode::tool_error, "context response exceeds page limit"});
    }
    return core::Result<ToolResult>::success({call.id, std::move(output)});
  } catch (const Json::exception &) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot encode context response"});
  }
}

std::string byte_page(std::string_view text, std::size_t offset,
                      std::size_t maximum, std::size_t &next) {
  const auto safe = core::sanitize_utf8(text).text;
  if (offset >= safe.size()) {
    next = safe.size();
    return {};
  }
  const auto length = utf8_prefix(safe.substr(offset), maximum);
  next = offset + length;
  return safe.substr(offset, length);
}

core::Result<ToolResult> paged_text(const ToolCall &call, Json envelope,
                                    std::string_view field,
                                    std::string_view text, std::size_t offset,
                                    std::size_t maximum) {
  const auto safe = core::sanitize_utf8(text).text;
  if (offset > safe.size()) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::invalid_argument, "byte_offset exceeds available bytes"});
  }
  if (offset < safe.size() &&
      (static_cast<unsigned char>(safe[offset]) & 0xc0U) == 0x80U)
    return core::Result<ToolResult>::failure(
        {ErrorCode::invalid_argument, "byte_offset must be a UTF-8 boundary"});
  std::size_t candidate = std::min(maximum, safe.size() - offset);
  while (true) {
    std::size_t next = offset;
    envelope[std::string(field)] = byte_page(safe, offset, candidate, next);
    if (next == offset && offset < safe.size())
      return core::Result<ToolResult>::failure(
          {ErrorCode::invalid_argument,
           "max_bytes is too small for the next UTF-8 character"});
    envelope["byte_offset"] = offset;
    envelope["next_byte_offset"] = next;
    envelope["truncated"] = next < safe.size();
    const auto encoded = envelope.dump();
    if (encoded.size() <= kMaxOutputBytes)
      return core::Result<ToolResult>::success({call.id, encoded});
    if (candidate == 0U) {
      return core::Result<ToolResult>::failure(
          {ErrorCode::tool_error,
           "context response envelope exceeds page limit"});
    }
    candidate /= 2U;
  }
}

core::CancellationToken
budget_token(const core::CancellationToken &outer,
             std::chrono::steady_clock::time_point deadline) {
  return core::CancellationToken::from_probe([outer, deadline] {
    return outer.is_cancelled() || std::chrono::steady_clock::now() >= deadline;
  });
}

core::Result<ToolResult>
cancelled_result(const core::CancellationToken &cancellation) {
  if (!cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::internal, "unreachable"});
  return core::Result<ToolResult>::failure(
      {ErrorCode::cancelled, "context tool execution cancelled"});
}

Json message_json(const core::Message &message) {
  Json result{{"id", message.id},
              {"role", "user"},
              {"content", message.content},
              {"is_error", message.is_error},
              {"tool_calls", Json::array()}};
  switch (message.role) {
  case core::Role::system:
    result["role"] = "system";
    break;
  case core::Role::user:
    result["role"] = "user";
    break;
  case core::Role::assistant:
    result["role"] = "assistant";
    break;
  case core::Role::tool:
    result["role"] = "tool";
    break;
  }
  if (message.tool_call_id)
    result["tool_call_id"] = *message.tool_call_id;
  for (const auto &tool_call : message.tool_calls) {
    result["tool_calls"].push_back(
        {{"id", tool_call.id},
         {"name", tool_call.name},
         {"arguments_json", tool_call.arguments_json}});
  }
  return result;
}

core::Result<ToolResult> paged_items(const ToolCall &call,
                                     std::string_view field, const Json &items,
                                     std::size_t offset,
                                     std::size_t next_offset, bool truncated) {
  return success(call, {{std::string(field), items},
                        {"offset", offset},
                        {"next_offset", next_offset},
                        {"truncated", truncated}});
}

Json message_preview(const core::Message &message) {
  auto preview = message_json(message);
  std::size_t next = 0;
  const auto content = preview["content"].get<std::string>();
  if (content.size() > 512U) {
    preview["content"] = byte_page(content, 0U, 512U, next);
    preview["content_truncated"] = true;
  }
  if (preview["tool_calls"].size() > 2U) {
    preview["tool_call_count"] = preview["tool_calls"].size();
    preview["tool_calls_truncated"] = true;
    preview["tool_calls"].erase(preview["tool_calls"].begin() + 2,
                                preview["tool_calls"].end());
  }
  for (auto &tool_call : preview["tool_calls"]) {
    for (const auto *field : {"id", "name"}) {
      const auto text = tool_call[field].get<std::string>();
      if (text.size() > 128U) {
        tool_call[field] = byte_page(text, 0U, 128U, next);
        tool_call["metadata_truncated"] = true;
      }
    }
    const auto arguments = tool_call["arguments_json"].get<std::string>();
    if (arguments.size() > 512U) {
      tool_call["arguments_json"] = byte_page(arguments, 0U, 512U, next);
      tool_call["arguments_truncated"] = true;
    }
  }
  if (message.tool_call_id && message.tool_call_id->size() > 128U) {
    preview["tool_call_id"] = byte_page(*message.tool_call_id, 0U, 128U, next);
    preview["tool_call_id_truncated"] = true;
  }
  return preview;
}

bool matches(const core::Message &message, std::string_view query) {
  if (message.id.find(query) != std::string::npos ||
      message.content.find(query) != std::string::npos ||
      (message.tool_call_id &&
       message.tool_call_id->find(query) != std::string::npos))
    return true;
  for (const auto &call : message.tool_calls) {
    if (call.id.find(query) != std::string::npos ||
        call.name.find(query) != std::string::npos ||
        call.arguments_json.find(query) != std::string::npos)
      return true;
  }
  return false;
}

core::Result<std::pair<std::size_t, std::size_t>>
window_range(const context::ArchiveState &state,
             const std::vector<core::Message> &history, const Json &arguments) {
  for (const auto &window : state.windows) {
    if (window.history_size > history.size() ||
        (window.history_size > 0 &&
         history[window.history_size - 1].id != window.last_message_id))
      return core::Result<std::pair<std::size_t, std::size_t>>::failure(
          {ErrorCode::context_error,
           "saved window does not match this transcript"});
  }
  const auto it = arguments.find("window_id");
  if (it == arguments.end())
    return core::Result<std::pair<std::size_t, std::size_t>>::success(
        {0U, history.size()});
  if (!it->is_string() || it->get_ref<const std::string &>().empty()) {
    return core::Result<std::pair<std::size_t, std::size_t>>::failure(
        {ErrorCode::invalid_argument, "window_id must be a non-empty string"});
  }
  const auto &id = it->get_ref<const std::string &>();
  for (std::size_t index = 0; index < state.windows.size(); ++index) {
    if (state.windows[index].id != id)
      continue;
    const auto first =
        std::min(state.windows[index].history_size, history.size());
    const auto next =
        index + 1U < state.windows.size()
            ? std::min(state.windows[index + 1U].history_size, history.size())
            : history.size();
    return core::Result<std::pair<std::size_t, std::size_t>>::success(
        {first, std::max(first, next)});
  }
  return core::Result<std::pair<std::size_t, std::size_t>>::failure(
      {ErrorCode::not_found, "context window not found"});
}

} // namespace

const ToolDefinition &ContextNotesTool::definition() const {
  return notes_definition();
}

core::Result<ToolResult>
ContextNotesTool::execute(const ToolCall &call,
                          core::CancellationToken cancellation) {
  if (execution_budget_ <= std::chrono::milliseconds::zero())
    return core::Result<ToolResult>::failure(
        {ErrorCode::timeout, "context tool execution timed out"});
  if (cancellation.is_cancelled())
    return cancelled_result(cancellation);
  const auto deadline = std::chrono::steady_clock::now() + execution_budget_;
  const auto budgeted = budget_token(cancellation, deadline);
  try {
    auto result = [&]() -> core::Result<ToolResult> {
      const auto parsed = parse_arguments(call);
      if (!parsed)
        return core::Result<ToolResult>::failure(parsed.error());
      const auto action = required_string(parsed.value(), "action");
      if (!action)
        return core::Result<ToolResult>::failure(action.error());
      const auto state = archive_.snapshot(budgeted);
      if (!state && state.error().code == ErrorCode::cancelled &&
          !cancellation.is_cancelled() &&
          std::chrono::steady_clock::now() >= deadline)
        return core::Result<ToolResult>::failure(
            {ErrorCode::timeout, "context tool execution timed out"});
      if (!state)
        return core::Result<ToolResult>::failure(state.error());
      if (action.value() == "list") {
        const auto offset = page_value(parsed.value(), "offset", 0U);
        const auto limit =
            page_value(parsed.value(), "limit", kDefaultPageSize);
        if (!offset || !limit)
          return core::Result<ToolResult>::failure(!offset ? offset.error()
                                                           : limit.error());
        Json names = Json::array();
        std::size_t index = 0;
        std::size_t next_offset = offset.value();
        bool truncated = false;
        for (const auto &[name, text] : state.value().notes) {
          if (budgeted.is_cancelled())
            return cancelled_result(budgeted);
          static_cast<void>(text);
          if (index++ < offset.value())
            continue;
          if (names.size() >= limit.value()) {
            truncated = true;
            break;
          }
          auto candidate = names;
          candidate.push_back(name);
          if (Json{{"notes", candidate},
                   {"offset", offset.value()},
                   {"next_offset", index - 1U},
                   {"truncated", true}}
                  .dump()
                  .size() > kMaxOutputBytes) {
            truncated = true;
            break;
          }
          names.push_back(name);
          next_offset = index;
        }
        return paged_items(call, "notes", names, offset.value(), next_offset,
                           truncated);
      }
      if (action.value() == "read") {
        const auto name = required_string(parsed.value(), "name");
        if (!name)
          return core::Result<ToolResult>::failure(name.error());
        const auto it = state.value().notes.find(name.value());
        if (it == state.value().notes.end())
          return core::Result<ToolResult>::failure(
              {ErrorCode::not_found, "context note not found"});
        const auto byte_offset = byte_value(parsed.value(), "byte_offset", 0U);
        const auto max_bytes =
            byte_value(parsed.value(), "max_bytes", kMaxPageBytes);
        if (!byte_offset || !max_bytes)
          return core::Result<ToolResult>::failure(
              !byte_offset ? byte_offset.error() : max_bytes.error());
        return paged_text(call, {{"name", name.value()}}, "text", it->second,
                          byte_offset.value(), max_bytes.value());
      }
      if (action.value() == "write" || action.value() == "append") {
        const auto name = required_string(parsed.value(), "name");
        const auto text = required_string(parsed.value(), "text");
        if (!name)
          return core::Result<ToolResult>::failure(name.error());
        if (!text)
          return core::Result<ToolResult>::failure(text.error());
        const auto written = archive_.write_note(
            name.value(), text.value(), action.value() == "append", budgeted);
        if (!written)
          return core::Result<ToolResult>::failure(written.error());
        return success(call, {{"ok", true}, {"name", name.value()}});
      }
      if (action.value() == "search") {
        const auto query = required_string(parsed.value(), "query");
        if (!query)
          return core::Result<ToolResult>::failure(query.error());
        const auto offset = page_value(parsed.value(), "offset", 0U);
        const auto limit =
            page_value(parsed.value(), "limit", kDefaultPageSize);
        if (!offset || !limit)
          return core::Result<ToolResult>::failure(!offset ? offset.error()
                                                           : limit.error());
        Json matches_json = Json::array();
        std::size_t matched = 0;
        std::size_t next_offset = offset.value();
        bool truncated = false;
        for (const auto &[name, text] : state.value().notes) {
          if (budgeted.is_cancelled())
            return cancelled_result(budgeted);
          if (name.find(query.value()) == std::string::npos &&
              text.find(query.value()) == std::string::npos)
            continue;
          if (matched++ < offset.value())
            continue;
          if (matches_json.size() >= limit.value()) {
            truncated = true;
            break;
          }
          std::size_t ignored = 0;
          const auto preview = byte_page(text, 0U, 512U, ignored);
          auto candidate = matches_json;
          candidate.push_back(
              {{"name", name},
               {"text", preview},
               {"text_truncated", text.size() > preview.size()}});
          if (Json{{"matches", candidate},
                   {"offset", offset.value()},
                   {"next_offset", matched},
                   {"truncated", true}}
                  .dump()
                  .size() > kMaxOutputBytes) {
            truncated = true;
            break;
          }
          matches_json = std::move(candidate);
          next_offset = matched;
        }
        return paged_items(call, "matches", matches_json, offset.value(),
                           next_offset, truncated);
      }
      return core::Result<ToolResult>::failure(
          {ErrorCode::invalid_argument, "unknown context notes action"});
    }();
    if (!result && result.error().code == ErrorCode::cancelled &&
        !cancellation.is_cancelled() &&
        std::chrono::steady_clock::now() >= deadline)
      return core::Result<ToolResult>::failure(
          {ErrorCode::timeout, "context tool execution timed out"});
    return result;
  } catch (const Json::exception &) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot encode context tool data"});
  }
}

const ToolDefinition &ContextHistoryTool::definition() const {
  return history_definition();
}

core::Result<ToolResult>
ContextHistoryTool::execute(const ToolCall &call,
                            core::CancellationToken cancellation) {
  if (execution_budget_ <= std::chrono::milliseconds::zero())
    return core::Result<ToolResult>::failure(
        {ErrorCode::timeout, "context tool execution timed out"});
  if (cancellation.is_cancelled())
    return cancelled_result(cancellation);
  const auto deadline = std::chrono::steady_clock::now() + execution_budget_;
  const auto budgeted = budget_token(cancellation, deadline);
  try {
    auto result = [&]() -> core::Result<ToolResult> {
      const auto parsed = parse_arguments(call);
      if (!parsed)
        return core::Result<ToolResult>::failure(parsed.error());
      const auto action = required_string(parsed.value(), "action");
      if (!action)
        return core::Result<ToolResult>::failure(action.error());
      const auto state = archive_.snapshot(budgeted);
      if (!state && state.error().code == ErrorCode::cancelled &&
          !cancellation.is_cancelled() &&
          std::chrono::steady_clock::now() >= deadline)
        return core::Result<ToolResult>::failure(
            {ErrorCode::timeout, "context tool execution timed out"});
      if (!state)
        return core::Result<ToolResult>::failure(state.error());
      if (action.value() == "windows") {
        const auto offset = page_value(parsed.value(), "offset", 0U);
        const auto limit =
            page_value(parsed.value(), "limit", kDefaultPageSize);
        if (!offset || !limit)
          return core::Result<ToolResult>::failure(!offset ? offset.error()
                                                           : limit.error());
        Json windows = Json::array();
        std::size_t next_offset = offset.value();
        bool truncated = false;
        for (std::size_t index = offset.value();
             index < state.value().windows.size(); ++index) {
          if (budgeted.is_cancelled())
            return cancelled_result(budgeted);
          const auto &window = state.value().windows[index];
          if (windows.size() >= limit.value()) {
            truncated = true;
            break;
          }
          auto candidate = windows;
          std::size_t ignored = 0;
          candidate.push_back(
              {{"id", window.id},
               {"history_size", window.history_size},
               {"last_message_id", window.last_message_id},
               {"reason", byte_page(window.reason, 0, 512, ignored)}});
          if (Json{{"windows", candidate},
                   {"offset", offset.value()},
                   {"next_offset", index + 1},
                   {"truncated", true},
                   {"reset_requested", state.value().reset_requested}}
                  .dump()
                  .size() > kMaxOutputBytes) {
            truncated = true;
            break;
          }
          windows = std::move(candidate);
          next_offset = index + 1;
        }
        return success(call,
                       {{"windows", windows},
                        {"offset", offset.value()},
                        {"next_offset", next_offset},
                        {"truncated", truncated},
                        {"reset_requested", state.value().reset_requested}});
      }
      const auto history = archive_.read_history(budgeted);
      if (!history && history.error().code == ErrorCode::cancelled &&
          !cancellation.is_cancelled() &&
          std::chrono::steady_clock::now() >= deadline)
        return core::Result<ToolResult>::failure(
            {ErrorCode::timeout, "context tool execution timed out"});
      if (!history)
        return core::Result<ToolResult>::failure(history.error());
      if (!cancellation.is_cancelled() &&
          std::chrono::steady_clock::now() >= deadline) {
        return core::Result<ToolResult>::failure(
            {ErrorCode::timeout, "context tool execution timed out"});
      }
      const auto range =
          window_range(state.value(), history.value(), parsed.value());
      if (!range)
        return core::Result<ToolResult>::failure(range.error());
      if (action.value() == "read") {
        const auto id = required_string(parsed.value(), "id");
        if (!id)
          return core::Result<ToolResult>::failure(id.error());
        for (std::size_t index = range.value().first;
             index < range.value().second; ++index) {
          if (budgeted.is_cancelled())
            return cancelled_result(budgeted);
          if (history.value()[index].id == id.value()) {
            const auto byte_offset =
                byte_value(parsed.value(), "byte_offset", 0U);
            const auto max_bytes =
                byte_value(parsed.value(), "max_bytes", kMaxPageBytes);
            if (!byte_offset || !max_bytes)
              return core::Result<ToolResult>::failure(
                  !byte_offset ? byte_offset.error() : max_bytes.error());
            return paged_text(call, {{"id", id.value()}}, "message_json",
                              message_json(history.value()[index]).dump(),
                              byte_offset.value(), max_bytes.value());
          }
        }
        return core::Result<ToolResult>::failure(
            {ErrorCode::not_found, "history message not found"});
      }
      const auto offset = page_value(parsed.value(), "offset", 0U);
      const auto limit = page_value(parsed.value(), "limit", kDefaultPageSize);
      if (!offset || !limit)
        return core::Result<ToolResult>::failure(!offset ? offset.error()
                                                         : limit.error());
      Json messages = Json::array();
      std::size_t matched = 0;
      std::size_t next_offset = offset.value();
      bool truncated = false;
      std::string query;
      if (action.value() == "search") {
        const auto required_query = required_string(parsed.value(), "query");
        if (!required_query)
          return core::Result<ToolResult>::failure(required_query.error());
        query = required_query.value();
      } else if (action.value() != "list") {
        return core::Result<ToolResult>::failure(
            {ErrorCode::invalid_argument, "unknown context history action"});
      }
      for (std::size_t index = range.value().first;
           index < range.value().second; ++index) {
        if (budgeted.is_cancelled())
          return cancelled_result(budgeted);
        if (!query.empty() && !matches(history.value()[index], query))
          continue;
        if (matched++ < offset.value())
          continue;
        if (messages.size() >= limit.value()) {
          truncated = true;
          break;
        }
        const auto preview = message_preview(history.value()[index]);
        auto candidate = messages;
        candidate.push_back(preview);
        if (Json{{"messages", candidate},
                 {"offset", offset.value()},
                 {"next_offset", matched - 1U},
                 {"truncated", true}}
                .dump()
                .size() > kMaxOutputBytes) {
          truncated = true;
          break;
        }
        messages.push_back(preview);
        next_offset = matched;
      }
      return paged_items(call, "messages", messages, offset.value(),
                         next_offset, truncated);
    }();
    if (!result && result.error().code == ErrorCode::cancelled &&
        !cancellation.is_cancelled() &&
        std::chrono::steady_clock::now() >= deadline)
      return core::Result<ToolResult>::failure(
          {ErrorCode::timeout, "context tool execution timed out"});
    return result;
  } catch (const Json::exception &) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot encode context tool data"});
  }
}

const ToolDefinition &NewContextTool::definition() const {
  return new_context_definition();
}

core::Result<ToolResult>
NewContextTool::execute(const ToolCall &call,
                        core::CancellationToken cancellation) {
  if (execution_budget_ <= std::chrono::milliseconds::zero())
    return core::Result<ToolResult>::failure(
        {ErrorCode::timeout, "context tool execution timed out"});
  if (cancellation.is_cancelled())
    return cancelled_result(cancellation);
  const auto deadline = std::chrono::steady_clock::now() + execution_budget_;
  const auto budgeted = budget_token(cancellation, deadline);
  try {
    auto result = [&]() -> core::Result<ToolResult> {
      const auto parsed = parse_arguments(call);
      if (!parsed)
        return core::Result<ToolResult>::failure(parsed.error());
      const auto requested = archive_.request_new_window(budgeted);
      if (!requested)
        return core::Result<ToolResult>::failure(requested.error());
      return success(call, {{"requested", true},
                            {"applies", "after this tool batch is persisted"}});
    }();
    if (!result && result.error().code == ErrorCode::cancelled &&
        !cancellation.is_cancelled() &&
        std::chrono::steady_clock::now() >= deadline)
      return core::Result<ToolResult>::failure(
          {ErrorCode::timeout, "context tool execution timed out"});
    return result;
  } catch (const Json::exception &) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot encode context tool data"});
  }
}

core::Result<void> register_context_tools(core::ToolRegistry &registry,
                                          context::ContextArchive &archive) {
  const auto notes =
      registry.register_tool(std::make_unique<ContextNotesTool>(archive));
  if (!notes)
    return notes;
  const auto history =
      registry.register_tool(std::make_unique<ContextHistoryTool>(archive));
  if (!history)
    return history;
  return registry.register_tool(std::make_unique<NewContextTool>(archive));
}

} // namespace zed::tools
