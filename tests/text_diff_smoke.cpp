#include <cassert>
#include <string>

#include "zed/tools/text_diff.hpp"

namespace {

std::size_t occurrences(const std::string &text, const std::string &needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

} // namespace

int main() {
  using zed::tools::make_unified_diff;
  using zed::tools::UnifiedDiffOptions;

  const auto multi_hunk =
      make_unified_diff("src/example.txt",
                        "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n"
                        "ten\neleven\ntwelve\nthirteen\nfourteen\nfifteen\n",
                        "one\nTWO\nthree\nfour\nfive\nsix\nseven\neight\nnine\n"
                        "ten\neleven\nTWELVE\nthirteen\nfourteen\nfifteen\n",
                        true);
  assert(multi_hunk.text.starts_with("--- a/src/example.txt\n"
                                     "+++ b/src/example.txt\n"));
  assert(occurrences(multi_hunk.text, "@@ -") == 2);
  assert(multi_hunk.text.find("-two\n+TWO\n") != std::string::npos);
  assert(multi_hunk.text.find("-twelve\n+TWELVE\n") != std::string::npos);

  const auto insertion =
      make_unified_diff("insert.txt", "a\nc\n", "a\nb\nc\n", true);
  assert(insertion.text.find("@@ -1,2 +1,3 @@\n") != std::string::npos);
  assert(insertion.text.find("+b\n") != std::string::npos);

  const auto deletion =
      make_unified_diff("delete.txt", "a\nb\nc\n", "a\nc\n", true);
  assert(deletion.text.find("@@ -1,3 +1,2 @@\n") != std::string::npos);
  assert(deletion.text.find("-b\n") != std::string::npos);

  const auto created = make_unified_diff("new.txt", "", "hello", false);
  assert(created.text.starts_with("--- /dev/null\n+++ b/new.txt\n"));
  assert(created.text.find("@@ -0,0 +1,1 @@\n+hello\n"
                           "\\ No newline at end of file\n") !=
         std::string::npos);

  const auto empty_created = make_unified_diff("empty.txt", "", "", false);
  assert(empty_created.text == "--- /dev/null\n+++ b/empty.txt\n");

  const auto removed = make_unified_diff("gone.txt", "hello", "", true);
  assert(removed.text.find("@@ -1,1 +0,0 @@\n-hello\n"
                           "\\ No newline at end of file\n") !=
         std::string::npos);

  const auto newline_change =
      make_unified_diff("newline.txt", "same", "same\n", true);
  assert(newline_change.text.find("-same\n\\ No newline at end of file\n"
                                  "+same\n") != std::string::npos);

  const auto limited =
      make_unified_diff("limited.txt", "a\nb\nc\nd\ne\nf\ng\nh\ni\nj\n",
                        "A\nB\nC\nD\nE\nF\nG\nH\nI\nJ\n", true,
                        {.context_lines = 3,
                         .max_output_bytes = 80,
                         .max_comparison_cells = 100});
  assert(limited.output_truncated);
  assert(limited.text.size() <= 80);
  assert(limited.text.find("[diff truncated]") != std::string::npos);

  const auto omitted = make_unified_diff(
      "large.txt", "old first\nold second\n", "new first\nnew second\n", true,
      {.context_lines = 3, .max_output_bytes = 256, .max_comparison_cells = 1});
  assert(omitted.comparison_omitted);
  assert(omitted.text.find("[diff omitted: line comparison exceeds configured "
                           "limit]") != std::string::npos);

  std::string long_prefix;
  for (int index = 0; index < 2'000; ++index)
    long_prefix += "shared line\n";
  const auto suffix_optimized = make_unified_diff(
      "prefix.txt", long_prefix + "old\n", long_prefix + "new\n", true,
      {.context_lines = 3,
       .max_output_bytes = 2'048,
       .max_comparison_cells = 1});
  assert(!suffix_optimized.comparison_omitted);
  assert(suffix_optimized.text.find("-old\n+new\n") != std::string::npos);
}
