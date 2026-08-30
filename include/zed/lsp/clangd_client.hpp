#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/cancellation.hpp"
#include "zed/core/result.hpp"

namespace zed::lsp {

struct ClangdConfig {
  std::filesystem::path workspace_root;
  std::string executable{"clangd"};
  std::filesystem::path compile_commands_directory;
  std::size_t initialize_timeout_ms{15'000};
  std::size_t request_timeout_ms{5'000};
  std::size_t max_message_bytes{16 * 1024 * 1024};
  std::size_t max_diagnostics{50};
};

struct Diagnostic {
  std::filesystem::path path;
  std::size_t line{};
  std::size_t character{};
  std::string severity;
  std::string message;
  std::string code;
};

enum class QueryOperation {
  hover,
  definition,
  references,
  document_symbols,
};

[[nodiscard]] std::filesystem::path discover_compile_commands_directory(
    const std::filesystem::path &workspace_root);

class ClangdClient {
public:
  explicit ClangdClient(ClangdConfig config);
  ~ClangdClient();

  ClangdClient(const ClangdClient &) = delete;
  ClangdClient &operator=(const ClangdClient &) = delete;

  [[nodiscard]] bool supports(const std::filesystem::path &path) const;

  core::Result<std::vector<Diagnostic>>
  diagnostics(const std::filesystem::path &path,
              core::CancellationToken cancellation = {});

  core::Result<std::string> query(QueryOperation operation,
                                  const std::filesystem::path &path,
                                  std::size_t line, std::size_t character,
                                  core::CancellationToken cancellation = {});

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string
format_diagnostics(std::string_view heading,
                   const std::vector<Diagnostic> &diagnostics);

} // namespace zed::lsp
