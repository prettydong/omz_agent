#pragma once

#include <cstddef>
#include <string>

#include "zed/core/model.hpp"

namespace zed::providers {

struct OpenCodeGoConfig {
    std::string api_key;
    std::string endpoint{"https://opencode.ai/zen/go/v1/responses"};
    std::size_t request_timeout_ms{120'000};
};

class OpenCodeGoModel final : public core::Model {
public:
    explicit OpenCodeGoModel(OpenCodeGoConfig config);

    [[nodiscard]] core::ModelCapabilities capabilities() const override;

    core::Result<core::AssistantResponse> complete(
        const core::ModelRequest& request,
        const core::StreamCallback& on_delta,
        core::CancellationToken cancellation) override;

private:
    OpenCodeGoConfig config_;
};

}  // namespace zed::providers
