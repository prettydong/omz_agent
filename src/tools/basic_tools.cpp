#include "zed/tools/basic_tools.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

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

  std::ifstream input(resolved.value(), std::ios::binary);
  if (!input)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot open file: " + path.value()});
  std::string content;
  content.resize(limits().max_read_bytes + 1);
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  const auto bytes_read = static_cast<std::size_t>(input.gcount());
  const bool truncated = bytes_read > limits().max_read_bytes;
  content.resize(std::min(bytes_read, limits().max_read_bytes));
  if (truncated)
    content += "\n[output truncated]";
  return core::Result<ToolResult>::success(
      {call.id, std::move(content), false});
}

const ToolDefinition &WriteFileTool::definition() const {
  return write_definition();
}

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
  const auto temporary =
      resolved.value().string() + ".zed-write-tmp-" + std::to_string(getpid());
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error,
         "cannot open file for writing: " + path.value()});
  output << content.value();
  if (!output) {
    std::filesystem::remove(temporary);
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot write file: " + path.value()});
  }
  output.close();
  if (cancellation.is_cancelled()) {
    std::filesystem::remove(temporary);
    return core::Result<ToolResult>::failure(
        {ErrorCode::cancelled, "write cancelled"});
  }
  std::filesystem::rename(temporary, resolved.value(), error);
  if (error) {
    std::filesystem::remove(temporary);
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot replace file: " + error.message()});
  }
  return core::Result<ToolResult>::success(
      {call.id, "wrote " + std::to_string(content.value().size()) + " bytes",
       false});
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
  std::size_t timeout_ms = limits().command_timeout_ms;
  if (const auto *value = field(arguments.value(), "timeout_ms");
      value != nullptr && value->is_number()) {
    timeout_ms = static_cast<std::size_t>(std::max(1.0, value->get<double>()));
  }
  std::size_t max_output = limits().max_command_output_bytes;
  if (const auto *value = field(arguments.value(), "max_output_bytes");
      value != nullptr && value->is_number()) {
    max_output = static_cast<std::size_t>(std::max(1.0, value->get<double>()));
  }

  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot create command output pipe"});
  }
  const pid_t child = fork();
  if (child == -1) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot fork command process"});
  }
  if (child == 0) {
    close(output_pipe[0]);
    setpgid(0, 0);
    if (chdir(working_directory.c_str()) != 0)
      _exit(126);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(output_pipe[1]);
    unsetenv("OPENAI_API_KEY");
    unsetenv("OPENCODE_GO_API_KEY");
    unsetenv("ANTHROPIC_API_KEY");
    execl("/bin/sh", "sh", "-c", command.value().c_str(),
          static_cast<char *>(nullptr));
    _exit(127);
  }

  setpgid(child, child);

  close(output_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);
  fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  std::string output;
  bool output_truncated = false;
  bool timed_out = false;
  bool cancelled = false;
  bool pipe_closed = false;
  int status = 0;
  bool child_finished = false;
  const auto start = std::chrono::steady_clock::now();

  while (!pipe_closed || !child_finished) {
    if (cancellation.is_cancelled()) {
      cancelled = true;
      kill(-child, SIGTERM);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (elapsed >= static_cast<long long>(timeout_ms)) {
      kill(-child, SIGTERM);
      timed_out = true;
    }

    pollfd descriptor{output_pipe[0], POLLIN, 0};
    poll(&descriptor, 1, 50);
    char buffer[4096];
    const ssize_t read_count = read(output_pipe[0], buffer, sizeof(buffer));
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

    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child)
      child_finished = true;
    if ((timed_out || cancelled) && !child_finished) {
      kill(-child, SIGKILL);
      waitpid(child, &status, 0);
      child_finished = true;
    }
  }
  close(output_pipe[0]);

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
  std::size_t max_results = 100;
  if (const auto *value = field(arguments.value(), "max_results");
      value != nullptr && value->is_number()) {
    max_results = static_cast<std::size_t>(std::max(1.0, value->get<double>()));
  }
  std::size_t max_output = limits().max_read_bytes;
  if (const auto *value = field(arguments.value(), "max_output_bytes");
      value != nullptr && value->is_number()) {
    max_output = static_cast<std::size_t>(std::max(1.0, value->get<double>()));
  }

  std::string result;
  std::size_t result_count = 0;
  std::error_code iterator_error;
  std::filesystem::recursive_directory_iterator iterator(search_root,
                                                         iterator_error);
  if (iterator_error)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error,
         "cannot scan path: " + iterator_error.message()});
  const auto end = std::filesystem::recursive_directory_iterator{};
  for (; iterator != end && result_count < max_results;
       iterator.increment(iterator_error)) {
    if (cancellation.is_cancelled())
      return core::Result<ToolResult>::failure(
          {ErrorCode::cancelled, "grep cancelled"});
    if (iterator_error)
      break;
    if (iterator->is_symlink(iterator_error) || iterator_error) {
      iterator_error.clear();
      continue;
    }
    if (!iterator->is_regular_file(iterator_error) || iterator_error) {
      iterator_error.clear();
      continue;
    }
    std::ifstream input(iterator->path(), std::ios::binary);
    if (!input)
      continue;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line) && result_count < max_results) {
      ++line_number;
      if (line.find(pattern.value()) == std::string::npos)
        continue;
      const auto relative = std::filesystem::relative(
          iterator->path(), workspace_root(), iterator_error);
      const std::string match = relative.string() + ":" +
                                std::to_string(line_number) + ":" + line + "\n";
      if (result.size() + match.size() > max_output) {
        result += "[results truncated]\n";
        return core::Result<ToolResult>::success(
            {call.id, std::move(result), false});
      }
      result += match;
      ++result_count;
    }
  }
  if (result_count == max_results)
    result += "[results truncated]\n";
  return core::Result<ToolResult>::success({call.id, std::move(result), false});
}

const ToolDefinition &EditFileTool::definition() const {
  return edit_definition();
}

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

  std::size_t expected = 1;
  if (const auto *value = field(arguments.value(), "expected_replacements");
      value != nullptr && value->is_number()) {
    expected = static_cast<std::size_t>(std::max(0.0, value->get<double>()));
  }
  const auto resolved = resolve_path(path.value());
  if (!resolved)
    return core::Result<ToolResult>::failure(resolved.error());
  std::ifstream input(resolved.value(), std::ios::binary);
  if (!input)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "cannot open file: " + path.value()});
  std::string content((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  if (content.size() > limits().max_write_bytes)
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error, "file exceeds edit limit"});

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

  std::string updated;
  updated.reserve(content.size() + new_text.value().size() * count);
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
  const auto temporary = resolved.value().string() + ".zed-edit-tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      return core::Result<ToolResult>::failure(
          {ErrorCode::tool_error, "cannot create edit temp file"});
    output << updated;
    if (!output)
      return core::Result<ToolResult>::failure(
          {ErrorCode::tool_error, "cannot write edit temp file"});
  }
  std::error_code rename_error;
  std::filesystem::rename(temporary, resolved.value(), rename_error);
  if (rename_error) {
    std::filesystem::remove(temporary);
    return core::Result<ToolResult>::failure(
        {ErrorCode::tool_error,
         "cannot replace edited file: " + rename_error.message()});
  }
  return core::Result<ToolResult>::success(
      {call.id, "replaced " + std::to_string(count) + " occurrence(s)", false});
}

} // namespace zed::tools
