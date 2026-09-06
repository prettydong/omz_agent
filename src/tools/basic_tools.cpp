#include "zed/tools/basic_tools.hpp"

#include "zed/core/utf8.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/support/atomic_file.hpp"
#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace zed::tools {

namespace {

using core::ErrorCode;
using core::ToolCall;
using core::ToolDefinition;
using core::ToolResult;
using Json = nlohmann::json;

const Json *field(const Json &object, std::string_view name) {
  if (!object.is_object())
    return nullptr;
  const auto iterator = object.find(std::string(name));
  return iterator == object.end() ? nullptr : &*iterator;
}

const ToolDefinition &read_definition() {
  static const ToolDefinition definition{
      "read",
      "Read a UTF-8 text file inside the workspace.",
      R"({"type":"object","required":["purpose","path"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"path":{"type":"string"}}})",
  };
  return definition;
}

const ToolDefinition &write_definition() {
  static const ToolDefinition definition{
      "write",
      "Write UTF-8 text to a file inside the workspace.",
      R"({"type":"object","required":["purpose","path","content"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"path":{"type":"string"},"content":{"type":"string"},"overwrite":{"type":"boolean"}}})",
  };
  return definition;
}

const ToolDefinition &bash_definition() {
  static const ToolDefinition definition{
      "bash",
      "Run a shell command inside the workspace with a timeout and output "
      "limit.",
      R"({"type":"object","required":["purpose","command"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal without including the exact shell command."},"command":{"type":"string"},"working_dir":{"type":"string"},"timeout_ms":{"type":"integer"},"max_output_bytes":{"type":"integer"}}})",
  };
  return definition;
}

const ToolDefinition &grep_definition() {
  static const ToolDefinition definition{
      "grep",
      "Search text files recursively inside the workspace.",
      R"({"type":"object","required":["purpose","pattern"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"pattern":{"type":"string"},"path":{"type":"string"},"max_results":{"type":"integer"},"max_output_bytes":{"type":"integer"}}})",
  };
  return definition;
}

const ToolDefinition &find_definition() {
  static const ToolDefinition definition{
      "find",
      "Find files and directories by glob pattern inside the workspace. "
      "Does not follow symlinks or scan .git.",
      R"({"type":"object","required":["purpose","pattern"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"pattern":{"type":"string","minLength":1,"description":"Glob pattern using * and ?. A pattern containing / matches paths relative to the search root; otherwise it matches entry names."},"path":{"type":"string","description":"Workspace-relative directory to search. Defaults to the workspace root."},"max_results":{"type":"integer","minimum":1,"maximum":10000},"max_output_bytes":{"type":"integer","minimum":1}}})",
  };
  return definition;
}

const ToolDefinition &list_definition() {
  static const ToolDefinition definition{
      "ls",
      "List one directory inside the workspace without following symlinks.",
      R"({"type":"object","required":["purpose"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"path":{"type":"string","description":"Workspace-relative directory to list. Defaults to the workspace root."},"max_entries":{"type":"integer","minimum":1,"maximum":10000},"max_output_bytes":{"type":"integer","minimum":1}}})",
  };
  return definition;
}

const ToolDefinition &edit_definition() {
  static const ToolDefinition definition{
      "edit",
      "Replace exact text in a workspace file. The replacement count must "
      "match expected_replacements.",
      R"({"type":"object","required":["purpose","path","old_text","new_text"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"path":{"type":"string"},"old_text":{"type":"string"},"new_text":{"type":"string"},"expected_replacements":{"type":"integer"}}})",
  };
  return definition;
}

core::Result<Json> parse_arguments(const ToolCall &call) {
  try {
    const auto arguments = Json::parse(call.arguments_json);
    if (!arguments.is_object()) {
      return core::Result<Json>::failure({
          ErrorCode::invalid_argument,
          "invalid JSON arguments for tool " + call.name + ": expected object",
      });
    }
    return core::Result<Json>::success(std::move(arguments));
  } catch (const Json::parse_error &error) {
    return core::Result<Json>::failure({
        ErrorCode::invalid_argument,
        "invalid JSON arguments for tool " + call.name + ": " + error.what(),
    });
  }
}

core::Result<std::string> required_string(const Json &object,
                                          std::string_view name,
                                          bool allow_empty = false) {
  const auto *value = field(object, name);
  if (value == nullptr || !value->is_string() ||
      (!allow_empty && value->get<std::string>().empty())) {
    return core::Result<std::string>::failure({
        ErrorCode::invalid_argument,
        "missing string argument: " + std::string(name),
    });
  }
  return core::Result<std::string>::success(value->get<std::string>());
}

core::Result<std::size_t> optional_size(const Json &object,
                                        std::string_view name,
                                        std::size_t fallback,
                                        std::size_t maximum, bool clamp = false,
                                        bool allow_zero = false) {
  const auto *value = field(object, name);
  if (value == nullptr)
    return core::Result<std::size_t>::success(fallback);

  std::uint64_t parsed = 0;
  if (value->is_number_unsigned()) {
    parsed = value->get<std::uint64_t>();
  } else if (value->is_number_integer()) {
    const auto signed_value = value->get<std::int64_t>();
    if (signed_value < 0 || (signed_value == 0 && !allow_zero)) {
      return core::Result<std::size_t>::failure({
          ErrorCode::invalid_argument,
          std::string(name) + " must be greater than zero",
      });
    }
    parsed = static_cast<std::uint64_t>(signed_value);
  } else {
    return core::Result<std::size_t>::failure({
        ErrorCode::invalid_argument,
        std::string(name) + " must be an integer",
    });
  }

  if ((!allow_zero && parsed == 0) || (!clamp && parsed > maximum)) {
    return core::Result<std::size_t>::failure({
        ErrorCode::invalid_argument,
        std::string(name) + " must be between 1 and " + std::to_string(maximum),
    });
  }
  return core::Result<std::size_t>::success(
      static_cast<std::size_t>(std::min<std::uint64_t>(parsed, maximum)));
}

// Nonblocking open prevents a FIFO from hanging before its type is checked.
core::Result<support::UniqueFd>
open_regular_file(const std::filesystem::path &path) {
  support::UniqueFd descriptor(
      open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW));
  if (!descriptor.valid())
    return core::Result<support::UniqueFd>::failure(
        {ErrorCode::tool_error,
         "cannot open file " + path.string() + ": " + std::strerror(errno)});
  struct stat status{};
  if (fstat(descriptor.get(), &status) != 0)
    return core::Result<support::UniqueFd>::failure(
        {ErrorCode::tool_error,
         "cannot inspect file " + path.string() + ": " + std::strerror(errno)});
  if (!S_ISREG(status.st_mode))
    return core::Result<support::UniqueFd>::failure(
        {ErrorCode::tool_error,
         "cannot read non-regular file: " + path.string()});
  return core::Result<support::UniqueFd>::success(std::move(descriptor));
}

struct FileRead {
  std::string content;
  bool truncated{};
};

core::Result<FileRead> read_regular_file(const std::filesystem::path &path,
                                         std::size_t limit,
                                         core::CancellationToken cancellation) {
  auto opened = open_regular_file(path);
  if (!opened)
    return core::Result<FileRead>::failure(opened.error());
  FileRead result;
  std::array<char, 4096> buffer{};
  while (true) {
    if (cancellation.is_cancelled())
      return core::Result<FileRead>::failure(
          {ErrorCode::cancelled, "file read cancelled: " + path.string()});
    const auto remaining = limit - result.content.size();
    const auto amount =
        remaining == 0 ? std::size_t{1} : std::min(buffer.size(), remaining);
    const auto count = read(opened.value().get(), buffer.data(), amount);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return core::Result<FileRead>::failure(
          {ErrorCode::tool_error,
           "cannot read file " + path.string() + ": " + std::strerror(errno)});
    }
    if (count == 0)
      return core::Result<FileRead>::success(std::move(result));
    if (remaining == 0) {
      result.truncated = true;
      return core::Result<FileRead>::success(std::move(result));
    }
    result.content.append(buffer.data(), static_cast<std::size_t>(count));
  }
}

bool wildcard_matches(std::string_view pattern, std::string_view text) {
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_index = std::string_view::npos;
  std::size_t star_text_index = 0;

  while (text_index < text.size()) {
    if (pattern_index < pattern.size() &&
        (pattern[pattern_index] == '?' ||
         pattern[pattern_index] == text[text_index])) {
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      star_text_index = text_index;
      continue;
    }
    if (star_index != std::string_view::npos) {
      pattern_index = star_index + 1;
      text_index = ++star_text_index;
      continue;
    }
    return false;
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*')
    ++pattern_index;
  return pattern_index == pattern.size();
}

core::Result<std::vector<std::filesystem::directory_entry>>
sorted_directory_entries(const std::filesystem::path &directory,
                         std::string_view operation) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(directory, error);
  if (error) {
    return core::Result<std::vector<std::filesystem::directory_entry>>::failure(
        {ErrorCode::tool_error,
         std::string(operation) + ": " + error.message()});
  }

  std::vector<std::filesystem::directory_entry> entries;
  const auto end = std::filesystem::directory_iterator{};
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return core::Result<std::vector<std::filesystem::directory_entry>>::
          failure({ErrorCode::tool_error,
                   std::string(operation) + ": " + error.message()});
    }
    entries.push_back(*iterator);
  }
  if (error) {
    return core::Result<std::vector<std::filesystem::directory_entry>>::failure(
        {ErrorCode::tool_error,
         std::string(operation) + ": " + error.message()});
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto &left, const auto &right) {
              return left.path().filename().generic_string() <
                     right.path().filename().generic_string();
            });
  return core::Result<std::vector<std::filesystem::directory_entry>>::success(
      std::move(entries));
}

bool append_limited(std::string &output, std::string_view line,
                    std::size_t maximum) {
  if (output.size() + line.size() > maximum)
    return false;
  output += line;
  return true;
}

std::string clangd_feedback(lsp::ClangdClient *client,
                            const std::filesystem::path &path,
                            core::CancellationToken cancellation) {
  if (client == nullptr || !client->supports(path))
    return {};
  const auto diagnostics = client->diagnostics(path, cancellation);
  if (!diagnostics) {
    return "\n[clangd diagnostics unavailable: " + diagnostics.error().message +
           ']';
  }
  const auto formatted = lsp::format_diagnostics(
      "clangd diagnostics after file change:", diagnostics.value());
  return formatted.empty() ? std::string{} : "\n\n" + formatted;
}

std::filesystem::path canonical_root(std::filesystem::path root) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(std::move(root), error);
  return error ? std::filesystem::absolute(std::filesystem::current_path())
               : result;
}

bool inside_root(const std::filesystem::path &root,
                 const std::filesystem::path &path) {
  const auto root_text = root.lexically_normal().string();
  const auto path_text = path.lexically_normal().string();
  if (path_text == root_text)
    return true;
  return path_text.size() > root_text.size() &&
         path_text.starts_with(root_text) &&
         path_text[root_text.size()] ==
             std::filesystem::path::preferred_separator;
}

std::string truncate_output(std::string output, std::size_t limit,
                            bool &truncated) {
  if (output.size() > limit) {
    output.resize(limit);
    truncated = true;
  }
  if (truncated)
    output += "\n[output truncated]";
  return output;
}

} // namespace

WorkspaceToolBase::WorkspaceToolBase(std::filesystem::path workspace_root,
                                     ToolLimits limits)
    : workspace_root_(canonical_root(std::move(workspace_root))),
      limits_(limits) {}

core::Result<std::filesystem::path>
WorkspaceToolBase::resolve_path(std::string_view path) const {
  if (path.empty()) {
    return core::Result<std::filesystem::path>::failure(
        {ErrorCode::invalid_argument, "path cannot be empty"});
  }
  std::filesystem::path candidate(path);
  if (!candidate.is_absolute()) {
    candidate = workspace_root_ / candidate;
  }

  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    return core::Result<std::filesystem::path>::failure(
        {ErrorCode::tool_error, "cannot resolve path: " + error.message()});
  }
  if (!inside_root(workspace_root_, canonical)) {
    return core::Result<std::filesystem::path>::failure(
        {ErrorCode::invalid_argument, "path escapes workspace root"});
  }
  return core::Result<std::filesystem::path>::success(canonical);
}

const ToolDefinition &ReadFileTool::definition() const {
  return read_definition();
}

core::Result<ToolResult>
ReadFileTool::execute(const ToolCall &call,
                      core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "read cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());
  const auto path = required_string(arguments.value(), "path");
  if (!path)
    return core::Result<ToolResult>::failure(path.error());
  const auto resolved = resolve_path(path.value());
  if (!resolved)
    return core::Result<ToolResult>::failure(resolved.error());

  auto read_result = read_regular_file(resolved.value(),
                                       limits().max_read_bytes, cancellation);
  if (!read_result)
    return core::Result<ToolResult>::failure(read_result.error());
  auto content = std::move(read_result.value().content);
  if (read_result.value().truncated)
    content += "\n[output truncated]";
  return core::Result<ToolResult>::success(
      {call.id, std::move(content), false});
}

const ToolDefinition &WriteFileTool::definition() const {
  return write_definition();
}

WriteFileTool::WriteFileTool(std::filesystem::path workspace_root,
                             ToolLimits limits, lsp::ClangdClient *clangd)
    : WorkspaceToolBase(std::move(workspace_root), limits), clangd_(clangd) {}

core::Result<ToolResult>
WriteFileTool::execute(const ToolCall &call,
                       core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "write cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());
  const auto path = required_string(arguments.value(), "path");
  const auto content = required_string(arguments.value(), "content", true);
  if (!path)
    return core::Result<ToolResult>::failure(path.error());
  if (!content)
    return core::Result<ToolResult>::failure(content.error());
  if (content.value().size() > limits().max_write_bytes) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::invalid_argument, "content exceeds write limit"});
  }
  const auto resolved = resolve_path(path.value());
  if (!resolved)
    return core::Result<ToolResult>::failure(resolved.error());

  bool overwrite = false;
  if (const auto *value = field(arguments.value(), "overwrite");
      value != nullptr) {
    if (!value->is_boolean()) {
      return core::Result<ToolResult>::failure(
          {ErrorCode::invalid_argument, "overwrite must be a boolean"});
    }
    overwrite = value->get<bool>();
  }

  std::error_code error;
  const bool exists = std::filesystem::exists(resolved.value(), error);
  if (error) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error,
         "cannot inspect target file: " + error.message()});
  }
  if (exists && !overwrite) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::conflict,
         "file already exists; set overwrite=true to replace it"});
  }
  std::filesystem::create_directories(resolved.value().parent_path(), error);
  if (error)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error,
         "cannot create parent directory: " + error.message()});
  const auto written =
      support::write_file_atomically(resolved.value(), content.value(),
                                     "workspace file", overwrite, cancellation);
  if (!written)
    return core::Result<ToolResult>::failure(written.error());
  std::string result =
      "wrote " + std::to_string(content.value().size()) + " bytes";
  result += clangd_feedback(clangd_, resolved.value(), cancellation);
  return core::Result<ToolResult>::success({call.id, std::move(result), false});
}

const ToolDefinition &BashTool::definition() const { return bash_definition(); }

core::Result<ToolResult>
BashTool::execute(const ToolCall &call, core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "bash cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());
  const auto command = required_string(arguments.value(), "command");
  if (!command)
    return core::Result<ToolResult>::failure(command.error());

  std::filesystem::path working_directory = workspace_root();
  if (const auto *value = field(arguments.value(), "working_dir");
      value != nullptr && value->is_string()) {
    const auto resolved = resolve_path(value->get<std::string>());
    if (!resolved)
      return core::Result<ToolResult>::failure(resolved.error());
    working_directory = resolved.value();
  }
  constexpr std::size_t kMaximumTimeoutMs = 24U * 60U * 60U * 1000U;
  const auto timeout =
      optional_size(arguments.value(), "timeout_ms",
                    limits().command_timeout_ms, kMaximumTimeoutMs, true);
  if (!timeout)
    return core::Result<ToolResult>::failure(timeout.error());
  const auto output_limit = optional_size(
      arguments.value(), "max_output_bytes", limits().max_command_output_bytes,
      limits().max_command_output_bytes, true);
  if (!output_limit)
    return core::Result<ToolResult>::failure(output_limit.error());
  const auto timeout_ms = timeout.value();
  const auto max_output = output_limit.value();

  auto spawn_lock = support::lock_process_spawn();
  int output_pipe[2];
  if (!support::create_cloexec_pipe(output_pipe)) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot create command output pipe"});
  }
  support::UniqueFd output_read(output_pipe[0]);
  support::UniqueFd output_write(output_pipe[1]);
  support::SpawnOptions spawn_options;
  spawn_options.executable = "/bin/sh";
  spawn_options.arguments = {"-c", command.value()};
  spawn_options.working_directory = working_directory;
  spawn_options.duplicate_descriptors = {
      {output_write.get(), STDOUT_FILENO},
      {output_write.get(), STDERR_FILENO},
  };
  spawn_options.close_descriptors = {output_read.get(), output_write.get()};
  pid_t child = -1;
  const int spawn_error = support::spawn_process(spawn_options, child);
  if (spawn_error != 0) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot start command process: " +
                                    std::string(std::strerror(spawn_error))});
  }

  spawn_lock.unlock();

  output_write.reset();
  const int flags = fcntl(output_read.get(), F_GETFL, 0);
  if (flags < 0 || fcntl(output_read.get(), F_SETFL, flags | O_NONBLOCK) < 0) {
    bool reaped = false;
    int status = 0;
    support::terminate_process_group(child, std::chrono::milliseconds(250),
                                     reaped, status);
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot configure command output"});
  }
  std::string output;
  bool output_truncated = false;
  bool timed_out = false;
  bool cancelled = false;
  bool pipe_closed = false;
  int status = 0;
  bool child_finished = false;
  bool termination_requested = false;
  const auto terminate_child = [&] {
    support::terminate_process_group(child, std::chrono::milliseconds(250),
                                     child_finished, status);
  };
  const auto start = std::chrono::steady_clock::now();

  while (!pipe_closed || !child_finished) {
    if (cancellation.is_cancelled()) {
      cancelled = true;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed >= static_cast<long long>(timeout_ms)) {
      timed_out = true;
    }

    pollfd descriptor{output_read.get(), POLLIN, 0};
    poll(&descriptor, 1, 50);
    char buffer[4096];
    const ssize_t read_count = read(output_read.get(), buffer, sizeof(buffer));
    if (read_count > 0) {
      const std::size_t remaining =
          output.size() < max_output ? max_output - output.size() : 0;
      const std::size_t copy_count =
          std::min(remaining, static_cast<std::size_t>(read_count));
      output.append(buffer, copy_count);
      if (copy_count < static_cast<std::size_t>(read_count))
        output_truncated = true;
    } else if (read_count == 0) {
      pipe_closed = true;
    } else if (errno != EAGAIN && errno != EINTR) {
      pipe_closed = true;
    }

    if (!child_finished)
      child_finished = support::try_reap_child(child, status);
    if ((timed_out || cancelled) && !termination_requested) {
      terminate_child();
      termination_requested = true;
      output_read.reset();
      pipe_closed = true;
    }
  }
  output = truncate_output(std::move(output), max_output, output_truncated);
  if (cancelled) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "bash cancelled"});
  }
  if (timed_out)
    output += "\n[command timed out]";
  const bool failed =
      timed_out || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
  if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
    output += "\n[exit code " + std::to_string(WEXITSTATUS(status)) + "]";
  }
  return core::Result<ToolResult>::success(
      {call.id, std::move(output), failed});
}

const ToolDefinition &GrepTool::definition() const { return grep_definition(); }

core::Result<ToolResult>
GrepTool::execute(const ToolCall &call, core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "grep cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());
  const auto pattern = required_string(arguments.value(), "pattern", true);
  if (!pattern)
    return core::Result<ToolResult>::failure(pattern.error());

  std::filesystem::path search_root = workspace_root();
  if (const auto *value = field(arguments.value(), "path");
      value != nullptr && value->is_string()) {
    const auto resolved = resolve_path(value->get<std::string>());
    if (!resolved)
      return core::Result<ToolResult>::failure(resolved.error());
    search_root = resolved.value();
  }
  const auto result_limit =
      optional_size(arguments.value(), "max_results", 100, 10'000, true);
  if (!result_limit)
    return core::Result<ToolResult>::failure(result_limit.error());
  const auto output_limit =
      optional_size(arguments.value(), "max_output_bytes",
                    limits().max_read_bytes, limits().max_read_bytes, true);
  if (!output_limit)
    return core::Result<ToolResult>::failure(output_limit.error());
  const auto max_results = result_limit.value();
  const auto max_output = output_limit.value();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(limits().command_timeout_ms);
  std::size_t scanned_bytes = 0;
  std::size_t scanned_entries = 0;
  std::string result;
  std::size_t result_count = 0;
  const auto incomplete = [&](std::string detail) {
    return core::Result<ToolResult>::success(
        {call.id, result + "[search incomplete: " + std::move(detail) + "]\n",
         true});
  };
  const auto cancelled = [&] {
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "grep cancelled"});
  };
  std::error_code error;
  std::filesystem::recursive_directory_iterator iterator(search_root, error);
  if (error)
    return incomplete("cannot scan path: " + error.message());
  const auto end = std::filesystem::recursive_directory_iterator{};
  for (; iterator != end; iterator.increment(error)) {
    if (error)
      return incomplete("cannot traverse directory: " + error.message());
    if (cancellation.is_cancelled())
      return cancelled();
    if (std::chrono::steady_clock::now() >= deadline)
      return incomplete("scan timed out");
    if (scanned_entries >= limits().max_search_entries)
      return incomplete("entry limit reached");
    ++scanned_entries;
    const auto status = iterator->symlink_status(error);
    if (error)
      return incomplete("cannot inspect entry: " + error.message());
    if (std::filesystem::is_symlink(status))
      continue;
    if (std::filesystem::is_directory(status)) {
      if (iterator->path().filename() == ".git")
        iterator.disable_recursion_pending();
      continue;
    }
    if (!std::filesystem::is_regular_file(status))
      continue;
    auto opened = open_regular_file(iterator->path());
    if (!opened)
      return incomplete(opened.error().message);
    const auto relative =
        iterator->path().lexically_relative(workspace_root()).string();
    std::string line;
    std::size_t line_number = 0;
    bool output_full = false;
    const auto finish_line = [&] {
      ++line_number;
      if (line.find(pattern.value()) != std::string::npos &&
          line.find('\0') == std::string::npos && core::is_valid_utf8(line)) {
        const auto match =
            relative + ":" + std::to_string(line_number) + ":" + line + "\n";
        if (match.size() > max_output - result.size()) {
          output_full = true;
        } else {
          result += match;
          ++result_count;
        }
      }
      line.clear();
    };
    std::array<char, 4096> buffer{};
    while (true) {
      if (cancellation.is_cancelled())
        return cancelled();
      if (std::chrono::steady_clock::now() >= deadline)
        return incomplete("scan timed out");
      if (scanned_bytes >= limits().max_search_bytes)
        return incomplete("byte limit reached");
      const auto amount =
          std::min(buffer.size(), limits().max_search_bytes - scanned_bytes);
      const auto count = read(opened.value().get(), buffer.data(), amount);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        return incomplete("cannot read " + relative + ": " +
                          std::strerror(errno));
      }
      if (count == 0) {
        if (!line.empty())
          finish_line();
        break;
      }
      scanned_bytes += static_cast<std::size_t>(count);
      for (std::size_t index = 0; index < static_cast<std::size_t>(count);
           ++index) {
        if (buffer[index] == '\n') {
          finish_line();
          if (output_full || result_count >= max_results)
            break;
        } else {
          if (line.size() >= limits().max_read_bytes)
            return incomplete("line length limit reached in " + relative);
          line.push_back(buffer[index]);
        }
      }
      if (output_full || result_count >= max_results)
        break;
    }
    if (output_full || result_count >= max_results) {
      result += "[results truncated]\n";
      return core::Result<ToolResult>::success(
          {call.id, std::move(result), false});
    }
  }
  if (error)
    return incomplete("cannot traverse directory: " + error.message());
  return core::Result<ToolResult>::success({call.id, std::move(result), false});
}

const ToolDefinition &FindFilesTool::definition() const {
  return find_definition();
}

core::Result<ToolResult>
FindFilesTool::execute(const ToolCall &call,
                       core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "find cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());
  const auto pattern = required_string(arguments.value(), "pattern");
  if (!pattern)
    return core::Result<ToolResult>::failure(pattern.error());

  std::filesystem::path search_root = workspace_root();
  if (const auto *value = field(arguments.value(), "path"); value != nullptr) {
    if (!value->is_string() || value->get<std::string>().empty()) {
      return core::Result<ToolResult>::failure(
          {ErrorCode::invalid_argument, "path must be a non-empty string"});
    }
    const auto resolved = resolve_path(value->get<std::string>());
    if (!resolved)
      return core::Result<ToolResult>::failure(resolved.error());
    search_root = resolved.value();
  }

  std::error_code type_error;
  if (!std::filesystem::is_directory(search_root, type_error) || type_error) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "find path is not a readable directory"});
  }
  constexpr std::size_t kMaximumResults = 10'000;
  const auto max_results =
      optional_size(arguments.value(), "max_results", 200, kMaximumResults);
  if (!max_results)
    return core::Result<ToolResult>::failure(max_results.error());
  const auto max_output =
      optional_size(arguments.value(), "max_output_bytes",
                    limits().max_read_bytes, limits().max_read_bytes);
  if (!max_output)
    return core::Result<ToolResult>::failure(max_output.error());

  const bool match_relative_path =
      pattern.value().find('/') != std::string::npos;
  std::vector<std::filesystem::path> directories{search_root};
  std::vector<std::string> matches;
  bool truncated = false;
  while (!directories.empty() && !truncated) {
    if (cancellation.is_cancelled())
      return core::Result<ToolResult>::failure(
          {ErrorCode::cancelled, "find cancelled"});
    auto directory = std::move(directories.back());
    directories.pop_back();
    const auto entries =
        sorted_directory_entries(directory, "cannot scan path");
    if (!entries)
      return core::Result<ToolResult>::failure(entries.error());

    std::vector<std::filesystem::path> child_directories;
    for (const auto &entry : entries.value()) {
      if (cancellation.is_cancelled())
        return core::Result<ToolResult>::failure(
            {ErrorCode::cancelled, "find cancelled"});
      std::error_code status_error;
      const auto status = entry.symlink_status(status_error);
      if (status_error) {
        return core::Result<ToolResult>::failure(
            {ErrorCode::tool_error,
             "cannot inspect path: " + status_error.message()});
      }
      if (std::filesystem::is_symlink(status))
        continue;

      const bool is_directory = std::filesystem::is_directory(status);
      if (is_directory && entry.path().filename() == ".git")
        continue;
      if (!is_directory && !std::filesystem::is_regular_file(status))
        continue;

      const auto search_relative =
          entry.path().lexically_relative(search_root).generic_string();
      const auto candidate = match_relative_path
                                 ? search_relative
                                 : entry.path().filename().generic_string();
      if (wildcard_matches(pattern.value(), candidate)) {
        auto workspace_relative =
            entry.path().lexically_relative(workspace_root()).generic_string();
        if (is_directory)
          workspace_relative += '/';
        matches.push_back(std::move(workspace_relative));
        if (matches.size() > max_results.value()) {
          matches.resize(max_results.value());
          truncated = true;
          break;
        }
      }
      if (is_directory)
        child_directories.push_back(entry.path());
    }
    for (auto iterator = child_directories.rbegin();
         iterator != child_directories.rend(); ++iterator) {
      directories.push_back(*iterator);
    }
  }

  std::sort(matches.begin(), matches.end());
  std::string output;
  for (const auto &match : matches) {
    if (!append_limited(output, match + '\n', max_output.value())) {
      truncated = true;
      break;
    }
  }
  if (truncated)
    output += "[results truncated]\n";
  return core::Result<ToolResult>::success({call.id, std::move(output), false});
}

const ToolDefinition &ListDirectoryTool::definition() const {
  return list_definition();
}

core::Result<ToolResult>
ListDirectoryTool::execute(const ToolCall &call,
                           core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "ls cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());

  std::filesystem::path directory = workspace_root();
  if (const auto *value = field(arguments.value(), "path"); value != nullptr) {
    if (!value->is_string() || value->get<std::string>().empty()) {
      return core::Result<ToolResult>::failure(
          {ErrorCode::invalid_argument, "path must be a non-empty string"});
    }
    const auto resolved = resolve_path(value->get<std::string>());
    if (!resolved)
      return core::Result<ToolResult>::failure(resolved.error());
    directory = resolved.value();
  }

  std::error_code type_error;
  if (!std::filesystem::is_directory(directory, type_error) || type_error) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "ls path is not a readable directory"});
  }
  constexpr std::size_t kMaximumEntries = 10'000;
  const auto max_entries =
      optional_size(arguments.value(), "max_entries", 200, kMaximumEntries);
  if (!max_entries)
    return core::Result<ToolResult>::failure(max_entries.error());
  const auto max_output =
      optional_size(arguments.value(), "max_output_bytes",
                    limits().max_read_bytes, limits().max_read_bytes);
  if (!max_output)
    return core::Result<ToolResult>::failure(max_output.error());

  const auto entries = sorted_directory_entries(directory, "cannot list path");
  if (!entries)
    return core::Result<ToolResult>::failure(entries.error());
  std::string output;
  bool truncated = entries.value().size() > max_entries.value();
  const auto entry_count =
      std::min(entries.value().size(), max_entries.value());
  for (std::size_t index = 0; index < entry_count; ++index) {
    if (cancellation.is_cancelled())
      return core::Result<ToolResult>::failure(
          {ErrorCode::cancelled, "ls cancelled"});
    const auto &entry = entries.value()[index];
    std::error_code status_error;
    const auto status = entry.symlink_status(status_error);
    if (status_error) {
      return core::Result<ToolResult>::failure(
          {ErrorCode::tool_error,
           "cannot inspect directory entry: " + status_error.message()});
    }
    std::string line = entry.path().filename().generic_string();
    if (std::filesystem::is_directory(status))
      line += '/';
    else if (std::filesystem::is_symlink(status))
      line += '@';
    line += '\n';
    if (!append_limited(output, line, max_output.value())) {
      truncated = true;
      break;
    }
  }
  if (truncated)
    output += "[results truncated]\n";
  else if (output.empty())
    output = "[empty directory]\n";
  return core::Result<ToolResult>::success({call.id, std::move(output), false});
}

const ToolDefinition &EditFileTool::definition() const {
  return edit_definition();
}

EditFileTool::EditFileTool(std::filesystem::path workspace_root,
                           ToolLimits limits, lsp::ClangdClient *clangd)
    : WorkspaceToolBase(std::move(workspace_root), limits), clangd_(clangd) {}

core::Result<ToolResult>
EditFileTool::execute(const ToolCall &call,
                      core::CancellationToken cancellation) {
  if (cancellation.is_cancelled())
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "edit cancelled"});
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<ToolResult>::failure(arguments.error());
  const auto path = required_string(arguments.value(), "path");
  const auto old_text = required_string(arguments.value(), "old_text", true);
  const auto new_text = required_string(arguments.value(), "new_text", true);
  if (!path)
    return core::Result<ToolResult>::failure(path.error());
  if (!old_text)
    return core::Result<ToolResult>::failure(old_text.error());
  if (!new_text)
    return core::Result<ToolResult>::failure(new_text.error());

  const auto expected_result =
      optional_size(arguments.value(), "expected_replacements", 1,
                    std::numeric_limits<std::size_t>::max(), false, true);
  if (!expected_result)
    return core::Result<ToolResult>::failure(expected_result.error());
  const auto expected = expected_result.value();
  const auto resolved = resolve_path(path.value());
  if (!resolved)
    return core::Result<ToolResult>::failure(resolved.error());
  auto read_result = read_regular_file(resolved.value(),
                                       limits().max_write_bytes, cancellation);
  if (!read_result)
    return core::Result<ToolResult>::failure(read_result.error());
  if (read_result.value().truncated)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "file exceeds edit limit"});
  auto content = std::move(read_result.value().content);

  std::size_t count = 0;
  std::size_t position = 0;
  while (!old_text.value().empty()) {
    position = content.find(old_text.value(), position);
    if (position == std::string::npos)
      break;
    ++count;
    position += old_text.value().size();
  }
  if (old_text.value().empty()) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::invalid_argument, "old_text cannot be empty"});
  }
  if (count != expected) {
    return core::Result<ToolResult>::failure({
        ErrorCode::conflict,
        "expected " + std::to_string(expected) + " replacements, found " +
            std::to_string(count),
    });
  }

  const std::size_t retained_bytes =
      content.size() - old_text.value().size() * count;
  if (count > 0 && new_text.value().size() >
                       (limits().max_write_bytes - retained_bytes) / count) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "edited file exceeds write limit"});
  }
  const std::size_t final_size =
      retained_bytes + new_text.value().size() * count;
  std::string updated;
  updated.reserve(final_size);
  position = 0;
  while (true) {
    const auto found = content.find(old_text.value(), position);
    if (found == std::string::npos) {
      updated += content.substr(position);
      break;
    }
    updated += content.substr(position, found - position);
    updated += new_text.value();
    position = found + old_text.value().size();
  }
  const auto written = support::write_file_atomically(
      resolved.value(), updated, "edited workspace file", true, cancellation);
  if (!written)
    return core::Result<ToolResult>::failure(written.error());
  std::string result = "replaced " + std::to_string(count) + " occurrence(s)";
  result += clangd_feedback(clangd_, resolved.value(), cancellation);
  return core::Result<ToolResult>::success({call.id, std::move(result), false});
}

} // namespace zed::tools
