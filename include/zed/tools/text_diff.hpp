#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zed::tools {

struct UnifiedDiffOptions {
  std::size_t context_lines{3};
  std::size_t max_output_bytes{256 * 1024};
  std::size_t max_comparison_cells{1'000'000};
};

struct UnifiedDiffResult {
  std::string text;
  bool output_truncated{};
  bool comparison_omitted{};
};

// Produces a bounded unified diff for UTF-8 text treated as byte strings.
// The path must be workspace-relative and use forward slashes. When
// before_exists is false, the old side is rendered as /dev/null.
UnifiedDiffResult make_unified_diff(std::string_view path,
                                    std::string_view before,
                                    std::string_view after, bool before_exists,
                                    UnifiedDiffOptions options = {});

} // namespace zed::tools
