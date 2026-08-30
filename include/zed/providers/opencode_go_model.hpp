#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zed/core/model.hpp"
#include "zed/providers/opencode_go_catalog.hpp"

namespace zed::providers {

struct OpenCodeGoConfig {
  std::string api_key;
  std::string endpoint{"https://opencode.ai/zen/go/v1"};
  std::size_t request_timeout_ms{120'000};
  std::vector<OpenCodeGoModelInfo> models;
};

class OpenCodeGoModel final : public core::Model {
public:
  explicit OpenCodeGoModel(OpenCodeGoConfig config);

  [[nodiscard]] core::ModelCapabilities capabilities() const override;
  [[nodiscard]] const std::vector<OpenCodeGoModelInfo> &models() const;
  void set_models(std::vector<OpenCodeGoModelInfo> models);

  core::Result<core::AssistantResponse>
  complete(const core::ModelRequest &request,
           const core::StreamCallback &on_delta,
           core::CancellationToken cancellation) override;

private:
  OpenCodeGoConfig config_;
};

} // namespace zed::providers
