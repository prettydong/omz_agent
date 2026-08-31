#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/model.hpp"
#include "zed/core/result.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/lsp/clangd_client.hpp"

namespace zed::plugins {

enum class PluginState {
  discovered,
  loading,
  active,
  pending,
  shadowed,
  failed,
  unloading,
  disposed,
};

[[nodiscard]] std::string_view plugin_state_name(PluginState state);

struct PluginStatus {
  std::string id;
  std::string name;
  std::string version;
  std::filesystem::path manifest_path;
  std::vector<std::string> dependencies;
  PluginState state{PluginState::discovered};
  bool loaded{false};
  std::string detail;
};

struct PluginManagerConfig {
  std::filesystem::path workspace_root;
  std::vector<std::filesystem::path> search_paths;
  core::ModelRef model;
  core::ReasoningEffort reasoning_effort{core::ReasoningEffort::low};
  std::size_t max_output_bytes{256 * 1024};
};

[[nodiscard]] std::vector<std::filesystem::path> default_plugin_search_paths();

class PluginManager {
public:
  PluginManager(PluginManagerConfig config,
                extensions::ExtensionRegistry &extensions,
                core::ToolRegistry &tools, core::Model &model,
                lsp::ClangdClient &clangd);
  ~PluginManager();

  PluginManager(const PluginManager &) = delete;
  PluginManager &operator=(const PluginManager &) = delete;

  core::Result<void> discover_and_load();
  core::Result<void> shutdown();
  [[nodiscard]] const std::vector<PluginStatus> &statuses() const;
  [[nodiscard]] std::string status_report() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace zed::plugins
