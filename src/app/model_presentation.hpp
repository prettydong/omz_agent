#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "model_scores.hpp"
#include "zed/providers/opencode_go_catalog.hpp"
#include "zed/ui/terminal.hpp"

namespace zed::app {

// Dated display references, never used for billing or model routing.
// Benchmark versions, configurations and weights are documented in
// docs/model-scores.md.
inline ui::TerminalCommandOption
model_option(const providers::OpenCodeGoModelInfo &model,
             std::string_view active_model) {
  struct Reference {
    std::string_view id;
    std::string_view prices;
    std::string_view condition;
  };
  // Prices: input / output / cached read, USD per million tokens.
  // Source: https://dev.opencode.ai/docs/go/#usage-limits, checked 2026-09-07.
  static constexpr Reference references[]{
      {"gpt-5.6-luna", "$0.20 / $1.20 / $0.02",
       "<=272K; >272K: $0.40 / $1.80 / $0.04"},
      {"grok-4.6", "$2.00 / $6.00 / $0.50",
       "<=200K; >200K: $4.00 / $12.00 / $1.00"},
      {"muse-spark-1.2-contributor", "$0.10 / $0.20 / $0.002", ""},
      {"muse-spark-1.3-contributor", "$0.10 / $0.20 / $0.002", ""},
      {"deepseek-v4-flash", "$0.22 / $0.66 / $0.007",
       "Off-peak; peak: $0.44 / $1.32 / $0.014"},
      {"deepseek-v4-flash-vision-exp", "$0.22 / $0.66 / $0.007",
       "Off-peak; peak: $0.44 / $1.32 / $0.014"},
      {"deepseek-v4-pro", "$0.66 / $1.98 / $0.022",
       "Off-peak; peak: $1.32 / $3.96 / $0.044"},
      {"glm-5.1", "$1.40 / $4.40 / $0.26", ""},
      {"glm-5.2", "$1.40 / $4.40 / $0.26", ""},
      {"glm-5.3", "$1.40 / $4.40 / $0.26", ""},
      {"glm-5.3-flash", "$0.15 / $0.50 / $0.03", ""},
      {"hy3", "$0.14 / $0.58 / $0.035", ""},
      {"hy4-preview", "$0.834 / $2.501 / $0.042", ""},
      {"kimi-k2.6", "$0.95 / $4.00 / $0.16", ""},
      {"kimi-k2.7-code", "$0.95 / $4.00 / $0.19", ""},
      {"kimi-k3", "$3.00 / $15.00 / $0.30", ""},
      {"longcat-2.0", "$0.30 / $1.20 / $0.006", ""},
      {"mimo-v2.5", "$0.14 / $0.28 / $0.0028", ""},
      {"mimo-v2.5-pro", "$0.435 / $0.87 / $0.003625", ""},
      {"minimax-m2.7", "$0.30 / $1.20 / $0.06", ""},
      {"minimax-m3", "$0.30 / $1.20 / $0.06", ""},
      {"qwen3.6-plus", "$0.50 / $3.00 / $0.05",
       "<=256K; >256K: $2.00 / $6.00 / $0.20"},
      {"qwen3.7-max", "$2.50 / $7.50 / $0.50", ""},
      {"qwen3.7-plus", "$0.40 / $1.60 / $0.04",
       "<=256K; >256K: $1.20 / $4.80 / $0.12"},
      {"qwen3.8-flash", "$0.15 / $0.47 / $0.016", ""},
      {"qwen3.8-max", "$2.00 / $6.00 / $0.25", ""},
  };
  const Reference *reference = nullptr;
  for (const auto &entry : references) {
    if (entry.id == model.id) {
      reference = &entry;
      break;
    }
  }
  ui::TerminalCommandOption option;
  option.value = model.id;
  option.display_name = model.name + (model.id == active_model ? " •" : "");
  option.description = model.name;
  const auto composite = lookup_model_composite_score(model.id);
  option.score =
      composite ? std::optional<int>(composite->score) : std::nullopt;
  if (composite) {
    option.details.push_back(
        std::string(composite->provisional ? "暂定 " : "综合 ") +
        std::to_string(composite->score) + " · " +
        std::to_string(composite->benchmarks) + "榜");
  } else {
    option.details.push_back("综合 · 资料不足");
  }
  option.details.push_back("输入 / 输出 / 缓存 · USD/百万 Token");
  option.details.push_back(reference ? std::string(reference->prices)
                                     : "暂无价格 / unavailable");
  option.details.push_back(reference ? std::string(reference->condition) : "");
  option.details.push_back(
      "Context " + std::to_string(model.max_context_tokens) + " · Max output " +
      std::to_string(model.max_output_tokens));
  return option;
}

} // namespace zed::app
