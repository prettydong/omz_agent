#include <array>
#include <cassert>
#include <limits>
#include <optional>

#include "app/model_scores.hpp"

namespace {

using Percentages = std::array<std::optional<double>, 4>;
using zed::app::calculate_model_composite_score;
using zed::app::lookup_model_composite_score;

void test_complete_weighted_score() {
  const auto score =
      calculate_model_composite_score(Percentages{50.0, 60.0, 70.0, 80.0});
  assert(score);
  assert(score->score == 60);
  assert(score->benchmarks == 4);
  assert(score->coverage_weight == 100);
  assert(!score->provisional);
  assert(!score->reference_mapping);
}

void test_missing_values_are_reweighted() {
  const auto score = calculate_model_composite_score(
      Percentages{100.0, std::nullopt, 0.0, 0.0});
  assert(score);
  assert(score->score == 50);
  assert(score->benchmarks == 3);
  assert(score->coverage_weight == 80);
  assert(!score->provisional);
}

void test_coverage_and_mapping_are_provisional() {
  const auto partial = calculate_model_composite_score(
      Percentages{50.0, std::nullopt, 50.0, std::nullopt});
  assert(partial);
  assert(partial->score == 50);
  assert(partial->benchmarks == 2);
  assert(partial->coverage_weight == 60);
  assert(partial->provisional);

  const auto mapped = calculate_model_composite_score(
      Percentages{50.0, 50.0, 50.0, 50.0}, true);
  assert(mapped);
  assert(mapped->coverage_weight == 100);
  assert(mapped->benchmarks == 4);
  assert(mapped->provisional);
  assert(mapped->reference_mapping);
}

void test_invalid_or_absent_values_are_rejected() {
  assert(!calculate_model_composite_score(
      Percentages{std::nullopt, std::nullopt, std::nullopt, std::nullopt}));
  assert(!calculate_model_composite_score(
      Percentages{std::numeric_limits<double>::quiet_NaN(), 50.0, 50.0, 50.0}));
  assert(!calculate_model_composite_score(
      Percentages{std::numeric_limits<double>::infinity(), 50.0, 50.0, 50.0}));
  assert(
      !calculate_model_composite_score(Percentages{-0.01, 50.0, 50.0, 50.0}));
  assert(
      !calculate_model_composite_score(Percentages{100.01, 50.0, 50.0, 50.0}));
}

void test_lookup_requires_an_exact_id() {
  assert(lookup_model_composite_score("gpt-5.6-luna"));
  assert(!lookup_model_composite_score("gpt-5.6-lunaa"));
  assert(!lookup_model_composite_score("gpt-5.6-luna-xhigh"));
}

void test_generated_snapshot_scores() {
  const auto luna = lookup_model_composite_score("gpt-5.6-luna");
  assert(luna);
  assert(luna->score == 82);
  assert(luna->benchmarks == 3);
  assert(luna->coverage_weight == 80);
  assert(!luna->provisional);

  const auto grok = lookup_model_composite_score("grok-4.6");
  assert(grok);
  assert(grok->score == 87);
  assert(grok->benchmarks == 4);
  assert(grok->coverage_weight == 100);

  const auto kimi = lookup_model_composite_score("kimi-k3");
  assert(kimi);
  assert(kimi->score == 85);

  const auto glm = lookup_model_composite_score("glm-5.3");
  assert(glm);
  assert(glm->score == 84);

  const auto mapped =
      lookup_model_composite_score("deepseek-v4-flash-vision-exp");
  assert(mapped);
  assert(mapped->provisional);
  assert(mapped->reference_mapping);

  assert(!lookup_model_composite_score("hy4-preview"));
}

} // namespace

int main() {
  test_complete_weighted_score();
  test_missing_values_are_reweighted();
  test_coverage_and_mapping_are_provisional();
  test_invalid_or_absent_values_are_rejected();
  test_lookup_requires_an_exact_id();
  test_generated_snapshot_scores();
}
