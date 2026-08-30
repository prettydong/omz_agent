#pragma once

#include <string>
#include <string_view>

#include "zed/core/context.hpp"
#include "zed/core/model.hpp"

namespace zed::core {

class ModelBackedContextController final : public ContextController {
public:
  ModelBackedContextController(Model &model, ModelRef model_ref,
                               std::string system_prompt = {},
                               std::size_t max_output_tokens = 1024);

  Result<ContextDecision> decide(const ContextRequest &request,
                                 CancellationToken cancellation) override;

private:
  Model &model_;
  ModelRef model_ref_;
  std::string system_prompt_;
  std::size_t max_output_tokens_;
};

[[nodiscard]] std::string_view default_context_system_prompt();

} // namespace zed::core
