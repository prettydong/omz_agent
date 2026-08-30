#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "zed/core/tool_registry.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/tools/basic_tools.hpp"
#include "zed/tools/clangd_tool.hpp"

#ifndef ZED_TEST_CLANGD_PATH
#error "ZED_TEST_CLANGD_PATH must point to the local clangd executable"
#endif

int main() {
  const auto root = std::filesystem::temp_directory_path() /
                    ("zeda-clangd-smoke-" + std::to_string(getpid()));
  std::error_code cleanup_error;
  std::filesystem::remove_all(root, cleanup_error);
  std::filesystem::create_directories(root);

  const auto source_path = root / "fixture.cpp";
  {
    std::ofstream source(source_path);
    assert(source);
    source << "int fixture_value() { return missing_name; }\n";
  }
  {
    std::ofstream database(root / "compile_commands.json");
    assert(database);
    database << nlohmann::json::array(
                    {{{"directory", root.string()},
                      {"command", std::string(ZED_TEST_CXX_PATH) +
                                      " -std=c++20 -c " + source_path.string()},
                      {"file", source_path.string()}}})
                    .dump(2);
  }

  assert(zed::lsp::discover_compile_commands_directory(root) ==
         std::filesystem::weakly_canonical(root));
  zed::lsp::ClangdClient client({
      root,
      ZED_TEST_CLANGD_PATH,
      root,
      15'000,
      10'000,
  });
  assert(client.supports(source_path));
  assert(!client.supports(root / "fixture.py"));

  const auto broken = client.diagnostics(source_path);
  assert(broken);
  assert(!broken.value().empty());
  assert(std::any_of(broken.value().begin(), broken.value().end(),
                     [](const zed::lsp::Diagnostic &diagnostic) {
                       return diagnostic.severity == "ERROR" &&
                              diagnostic.line == 1;
                     }));

  zed::core::ToolRegistry registry;
  registry.register_tool(
      std::make_unique<zed::tools::ClangdTool>(root, client));
  registry.register_tool(std::make_unique<zed::tools::EditFileTool>(
      root, zed::tools::ToolLimits{}, &client));
  const auto tool_diagnostics = registry.execute(
      {"lsp-diagnostics", "lsp",
       R"({"operation":"diagnostics","path":"fixture.cpp","purpose":"verify the local clangd diagnostics tool"})"},
      {});
  assert(tool_diagnostics);
  assert(tool_diagnostics.value().content.find("clangd diagnostics") !=
         std::string::npos);

  const auto symbols = client.query(zed::lsp::QueryOperation::document_symbols,
                                    source_path, 0, 0);
  assert(symbols);
  assert(symbols.value().find("fixture_value") != std::string::npos);

  const auto edit_result = registry.execute(
      {"edit-fixture", "edit",
       R"({"path":"fixture.cpp","old_text":"missing_name","new_text":"42","purpose":"fix the fixture and request clangd diagnostics"})"},
      {});
  assert(edit_result);
  assert(edit_result.value().content.find("clangd diagnostics unavailable") ==
         std::string::npos);
  const auto fixed = client.diagnostics(source_path);
  assert(fixed);
  assert(fixed.value().empty());

  for (const auto operation :
       {zed::lsp::QueryOperation::hover, zed::lsp::QueryOperation::definition,
        zed::lsp::QueryOperation::references}) {
    const auto query = client.query(operation, source_path, 1, 5);
    assert(query);
    assert(query.value() != "No clangd results.");
  }

  zed::core::CancellationSource cancellation;
  cancellation.cancel();
  const auto cancelled = client.diagnostics(source_path, cancellation.token());
  assert(!cancelled);
  assert(cancelled.error().code == zed::core::ErrorCode::cancelled);

  std::filesystem::remove_all(root, cleanup_error);
  return 0;
}
