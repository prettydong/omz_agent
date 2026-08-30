#include "zed/tools/clangd_tool.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace zed::tools {

namespace {

using Json = nlohmann::json;
using core::ErrorCode;

const core::ToolDefinition &clangd_definition() {
  static const core::ToolDefinition definition{
      "lsp",
      "Query the local clangd language server for C and C++ diagnostics, "
      "hover information, definitions, references, or document symbols.",
      R"({"type":"object","required":["purpose","operation","path"],"properties":{"purpose":{"type":"string","minLength":1,"description":"Brief user-facing reason for this tool call. Describe the goal, not implementation details."},"operation":{"type":"string","enum":["diagnostics","hover","definition","references","document_symbols"]},"path":{"type":"string","minLength":1},"line":{"type":"integer","minimum":1,"description":"One-based line for positional operations."},"character":{"type":"integer","minimum":1,"description":"One-based UTF-16 character position for positional operations."}}})",
  };
  return definition;
}

core::Result<Json> parse_arguments(const core::ToolCall &call) {
  try {
    auto arguments = Json::parse(call.arguments_json);
    if (!arguments.is_object()) {
      return core::Result<Json>::failure(
          {ErrorCode::invalid_argument, "lsp arguments must be an object"});
    }
    return core::Result<Json>::success(std::move(arguments));
  } catch (const Json::exception &error) {
    return core::Result<Json>::failure(
        {ErrorCode::invalid_argument,
         std::string("invalid lsp arguments: ") + error.what()});
  }
}

core::Result<std::string> required_string(const Json &arguments,
                                          std::string_view name) {
  const auto value = arguments.find(std::string(name));
  if (value == arguments.end() || !value->is_string() || value->empty()) {
    return core::Result<std::string>::failure(
        {ErrorCode::invalid_argument,
         "missing string argument: " + std::string(name)});
  }
  return core::Result<std::string>::success(value->get<std::string>());
}

core::Result<std::size_t> required_position(const Json &arguments,
                                            std::string_view name) {
  const auto value = arguments.find(std::string(name));
  if (value == arguments.end() || !value->is_number_integer()) {
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument,
         "missing integer argument: " + std::string(name)});
  }
  const auto parsed = value->get<std::int64_t>();
  if (parsed <= 0) {
    return core::Result<std::size_t>::failure(
        {ErrorCode::invalid_argument,
         std::string(name) + " must be greater than zero"});
  }
  return core::Result<std::size_t>::success(static_cast<std::size_t>(parsed));
}

std::optional<lsp::QueryOperation> query_operation(std::string_view name) {
  if (name == "hover")
    return lsp::QueryOperation::hover;
  if (name == "definition")
    return lsp::QueryOperation::definition;
  if (name == "references")
    return lsp::QueryOperation::references;
  if (name == "document_symbols")
    return lsp::QueryOperation::document_symbols;
  return std::nullopt;
}

} // namespace

ClangdTool::ClangdTool(std::filesystem::path workspace_root,
                       lsp::ClangdClient &client, ToolLimits limits)
    : WorkspaceToolBase(std::move(workspace_root), limits), client_(client) {}

const core::ToolDefinition &ClangdTool::definition() const {
  return clangd_definition();
}

core::Result<core::ToolResult>
ClangdTool::execute(const core::ToolCall &call,
                    core::CancellationToken cancellation) {
  if (cancellation.is_cancelled()) {
    return core::Result<core::ToolResult>::failure(
        {ErrorCode::cancelled, "lsp query cancelled"});
  }
  const auto arguments = parse_arguments(call);
  if (!arguments)
    return core::Result<core::ToolResult>::failure(arguments.error());
  const auto operation = required_string(arguments.value(), "operation");
  const auto path = required_string(arguments.value(), "path");
  if (!operation)
    return core::Result<core::ToolResult>::failure(operation.error());
  if (!path)
    return core::Result<core::ToolResult>::failure(path.error());
  const auto resolved = resolve_path(path.value());
  if (!resolved)
    return core::Result<core::ToolResult>::failure(resolved.error());
  if (!client_.supports(resolved.value())) {
    return core::Result<core::ToolResult>::failure(
        {ErrorCode::invalid_argument,
         "lsp only supports C and C++ files through clangd"});
  }

  if (operation.value() == "diagnostics") {
    const auto diagnostics =
        client_.diagnostics(resolved.value(), cancellation);
    if (!diagnostics)
      return core::Result<core::ToolResult>::failure(diagnostics.error());
    auto output = lsp::format_diagnostics(
        "clangd diagnostics for " + path.value() + ':', diagnostics.value());
    if (output.empty())
      output = "No clangd diagnostics for " + path.value() + ".";
    return core::Result<core::ToolResult>::success(
        {call.id, std::move(output), false});
  }

  const auto query = query_operation(operation.value());
  if (!query) {
    return core::Result<core::ToolResult>::failure(
        {ErrorCode::invalid_argument, "unsupported lsp operation"});
  }
  std::size_t line = 0;
  std::size_t character = 0;
  if (*query != lsp::QueryOperation::document_symbols) {
    const auto parsed_line = required_position(arguments.value(), "line");
    const auto parsed_character =
        required_position(arguments.value(), "character");
    if (!parsed_line)
      return core::Result<core::ToolResult>::failure(parsed_line.error());
    if (!parsed_character)
      return core::Result<core::ToolResult>::failure(parsed_character.error());
    line = parsed_line.value();
    character = parsed_character.value();
  }
  const auto result =
      client_.query(*query, resolved.value(), line, character, cancellation);
  if (!result)
    return core::Result<core::ToolResult>::failure(result.error());
  return core::Result<core::ToolResult>::success(
      {call.id, result.value(), false});
}

} // namespace zed::tools
