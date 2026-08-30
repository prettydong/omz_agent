#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "zed/core/result.hpp"
#include "zed/core/tool.hpp"
#include "zed/providers/opencode_go_catalog.hpp"

namespace zed::app {

class ConfigureWebServer {
public:
  explicit ConfigureWebServer(std::filesystem::path workspace);
  ~ConfigureWebServer();

  ConfigureWebServer(const ConfigureWebServer &) = delete;
  ConfigureWebServer &operator=(const ConfigureWebServer &) = delete;

  [[nodiscard]] core::Result<std::string>
  open(const std::vector<providers::OpenCodeGoModelInfo> &models,
       bool launch_browser = true,
       const std::vector<core::ToolDefinition> &tools = {});

  void stop();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace zed::app
