#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

#include "model_score_data.hpp"

namespace zed::app {

inline constexpr std::array<int, 4> kModelBenchmarkWeights{40, 30, 20, 10};

struct ModelCompositeScore {
  int score{};
  std::size_t benchmarks{};
  int coverage_weight{};
  bool provisional{};
  bool reference_mapping{};
};

[[nodiscard]] inline std::optional<ModelCompositeScore>
calculate_model_composite_score(
    const std::array<std::optional<double>, 4> &percentages,
    bool reference_mapping = false) {
  double weighted_sum = 0.0;
  int coverage_weight = 0;
  std::size_t benchmarks = 0;

  for (std::size_t index = 0; index < percentages.size(); ++index) {
    const auto percentage = percentages[index];
    if (!percentage)
      continue;
    if (!std::isfinite(*percentage) || *percentage < 0.0 ||
        *percentage > 100.0) {
      return std::nullopt;
    }

    const int weight = kModelBenchmarkWeights[index];
    weighted_sum += static_cast<double>(weight) * *percentage;
    coverage_weight += weight;
    ++benchmarks;
  }

  if (coverage_weight == 0)
    return std::nullopt;

  const int score = static_cast<int>(
      std::lround(weighted_sum / static_cast<double>(coverage_weight)));
  return ModelCompositeScore{
      score,
      benchmarks,
      coverage_weight,
      coverage_weight < 70 || benchmarks < 3 || reference_mapping,
      reference_mapping,
  };
}

[[nodiscard]] inline std::optional<ModelCompositeScore>
lookup_model_composite_score(std::string_view id) {
  for (const auto &row : kModelBenchmarkRows) {
    if (row.id == id) {
      return calculate_model_composite_score(row.percentages,
                                             row.reference_mapping);
    }
  }
  return std::nullopt;
}

} // namespace zed::app
