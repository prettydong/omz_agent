#pragma once

#include "zed/core/context.hpp"
#include "zed/core/model.hpp"

namespace zed::core {

class ModelBackedContextController final : public ContextController {
public:
    ModelBackedContextController(Model& model, ModelRef model_ref);

    Result<ContextDecision> decide(
        const ContextRequest& request,
        CancellationToken cancellation) override;

private:
    Model& model_;
    ModelRef model_ref_;
};

}  // namespace zed::core
