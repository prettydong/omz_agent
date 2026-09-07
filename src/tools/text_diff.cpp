#include "zed/tools/text_diff.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace zed::tools {

namespace {

constexpr std::string_view kTruncated = "\n[diff truncated]\n";

enum class OperationKind : std::uint8_t { common, removed, added };

struct Lines {
  std::vector<std::string_view> values;
  bool ends_with_newline{};
};

struct Operation {
  OperationKind kind;
  std::size_t old_index{};
  std::size_t new_index{};
};

class BoundedOutput {
public:
  explicit BoundedOutput(std::size_t maximum) : maximum_(maximum) {}

  void append(std::string_view text) {
    if (truncated_ || text.empty())
      return;
    if (text.size() <= maximum_ - output_.size()) {
      output_ += text;
      return;
    }
    truncated_ = true;
    if (maximum_ >= kTruncated.size()) {
      const auto prefix_size = maximum_ - kTruncated.size();
      if (output_.size() < prefix_size)
        output_ += text.substr(0, prefix_size - output_.size());
      output_.resize(utf8_prefix_size(output_, prefix_size));
      output_ += kTruncated;
    } else {
      if (output_.size() < maximum_)
        output_ += text.substr(0, maximum_ - output_.size());
      output_.resize(utf8_prefix_size(output_, maximum_));
    }
  }

  [[nodiscard]] std::string take() { return std::move(output_); }
  [[nodiscard]] bool truncated() const { return truncated_; }

private:
  static std::size_t utf8_prefix_size(std::string_view text,
                                      std::size_t maximum) {
    const auto limit = std::min(text.size(), maximum);
    std::size_t position = 0;
    while (position < limit) {
      const auto byte = static_cast<unsigned char>(text[position]);
      std::size_t width = 1;
      if ((byte & 0xE0U) == 0xC0U)
        width = 2;
      else if ((byte & 0xF0U) == 0xE0U)
        width = 3;
      else if ((byte & 0xF8U) == 0xF0U)
        width = 4;
      if (position + width > limit)
        break;
      position += width;
    }
    return position;
  }

  std::size_t maximum_;
  std::string output_;
  bool truncated_{};
};

Lines split_lines(std::string_view text) {
  Lines result;
  result.ends_with_newline = !text.empty() && text.back() == '\n';
  std::size_t begin = 0;
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    if (end == std::string_view::npos) {
      result.values.push_back(text.substr(begin));
      break;
    }
    result.values.push_back(text.substr(begin, end - begin));
    begin = end + 1;
  }
  return result;
}

bool line_has_newline(const Lines &lines, std::size_t index) {
  return index + 1 < lines.values.size() || lines.ends_with_newline;
}

bool lines_equal(const Lines &before, std::size_t old_index, const Lines &after,
                 std::size_t new_index) {
  return before.values[old_index] == after.values[new_index] &&
         line_has_newline(before, old_index) ==
             line_has_newline(after, new_index);
}

bool comparison_is_bounded(std::size_t old_count, std::size_t new_count,
                           std::size_t maximum_cells) {
  if (old_count == 0 || new_count == 0)
    return true;
  if (old_count > maximum_cells / new_count)
    return false;
  return old_count * new_count <= maximum_cells;
}

std::vector<Operation> make_operations(const Lines &before,
                                       std::size_t old_begin,
                                       std::size_t old_end, const Lines &after,
                                       std::size_t new_begin,
                                       std::size_t new_end) {
  const std::size_t old_size = old_end - old_begin;
  const std::size_t new_size = new_end - new_begin;
  const std::size_t width = new_size + 1;
  std::vector<std::uint8_t> directions(width * (old_size + 1));
  std::vector<std::uint32_t> next(width);
  std::vector<std::uint32_t> current(width);

  for (std::size_t old_offset = old_size; old_offset > 0; --old_offset) {
    const std::size_t old_index = old_begin + old_offset - 1;
    current[new_size] = 0;
    for (std::size_t new_offset = new_size; new_offset > 0; --new_offset) {
      const std::size_t new_index = new_begin + new_offset - 1;
      const auto direction_index = (old_offset - 1) * width + new_offset - 1;
      if (lines_equal(before, old_index, after, new_index)) {
        current[new_offset - 1] = next[new_offset] + 1;
        directions[direction_index] =
            static_cast<std::uint8_t>(OperationKind::common);
      } else if (next[new_offset - 1] >= current[new_offset]) {
        current[new_offset - 1] = next[new_offset - 1];
        directions[direction_index] =
            static_cast<std::uint8_t>(OperationKind::removed);
      } else {
        current[new_offset - 1] = current[new_offset];
        directions[direction_index] =
            static_cast<std::uint8_t>(OperationKind::added);
      }
    }
    std::swap(current, next);
  }

  std::vector<Operation> operations;
  operations.reserve(old_size + new_size);
  std::size_t old_index = old_begin;
  std::size_t new_index = new_begin;
  while (old_index < old_end && new_index < new_end) {
    const auto kind = static_cast<OperationKind>(
        directions[(old_index - old_begin) * width + new_index - new_begin]);
    operations.push_back({kind, old_index, new_index});
    if (kind != OperationKind::added)
      ++old_index;
    if (kind != OperationKind::removed)
      ++new_index;
  }
  while (old_index < old_end)
    operations.push_back({OperationKind::removed, old_index++, new_index});
  while (new_index < new_end)
    operations.push_back({OperationKind::added, old_index, new_index++});
  return operations;
}

std::size_t count_old(const std::vector<Operation> &operations,
                      std::size_t begin, std::size_t end) {
  return static_cast<std::size_t>(
      std::count_if(operations.begin() + static_cast<std::ptrdiff_t>(begin),
                    operations.begin() + static_cast<std::ptrdiff_t>(end),
                    [](const Operation &operation) {
                      return operation.kind != OperationKind::added;
                    }));
}

std::size_t count_new(const std::vector<Operation> &operations,
                      std::size_t begin, std::size_t end) {
  return static_cast<std::size_t>(
      std::count_if(operations.begin() + static_cast<std::ptrdiff_t>(begin),
                    operations.begin() + static_cast<std::ptrdiff_t>(end),
                    [](const Operation &operation) {
                      return operation.kind != OperationKind::removed;
                    }));
}

void append_hunk(BoundedOutput &output,
                 const std::vector<Operation> &operations, const Lines &before,
                 const Lines &after, std::size_t old_base, std::size_t new_base,
                 std::size_t begin, std::size_t end) {
  const auto old_before = old_base + count_old(operations, 0, begin);
  const auto new_before = new_base + count_new(operations, 0, begin);
  const auto old_count = count_old(operations, begin, end);
  const auto new_count = count_new(operations, begin, end);
  const auto old_start = old_count == 0 ? old_before : old_before + 1;
  const auto new_start = new_count == 0 ? new_before : new_before + 1;
  output.append("@@ -" + std::to_string(old_start) + "," +
                std::to_string(old_count) + " +" + std::to_string(new_start) +
                "," + std::to_string(new_count) + " @@\n");

  for (std::size_t index = begin; index < end; ++index) {
    const auto &operation = operations[index];
    const char prefix = operation.kind == OperationKind::common    ? ' '
                        : operation.kind == OperationKind::removed ? '-'
                                                                   : '+';
    const auto &line = operation.kind == OperationKind::added
                           ? after.values[operation.new_index]
                           : before.values[operation.old_index];
    output.append(std::string_view(&prefix, 1));
    output.append(line);
    output.append("\n");
    const bool old_without_newline =
        operation.kind != OperationKind::added && !before.ends_with_newline &&
        operation.old_index + 1 == before.values.size();
    const bool new_without_newline =
        operation.kind != OperationKind::removed && !after.ends_with_newline &&
        operation.new_index + 1 == after.values.size();
    if (old_without_newline || new_without_newline)
      output.append("\\ No newline at end of file\n");
  }
}

} // namespace

UnifiedDiffResult make_unified_diff(std::string_view path,
                                    std::string_view before,
                                    std::string_view after, bool before_exists,
                                    UnifiedDiffOptions options) {
  const auto old_lines = split_lines(before);
  const auto new_lines = split_lines(after);
  std::size_t prefix = 0;
  while (prefix < old_lines.values.size() && prefix < new_lines.values.size() &&
         lines_equal(old_lines, prefix, new_lines, prefix)) {
    ++prefix;
  }
  if (prefix == old_lines.values.size() && prefix == new_lines.values.size()) {
    if (before_exists)
      return {};
    BoundedOutput output(options.max_output_bytes);
    output.append("--- /dev/null\n");
    output.append("+++ b/" + std::string(path) + "\n");
    return {output.take(), output.truncated(), false};
  }
  std::size_t suffix = 0;
  while (suffix < old_lines.values.size() - prefix &&
         suffix < new_lines.values.size() - prefix &&
         lines_equal(old_lines, old_lines.values.size() - suffix - 1, new_lines,
                     new_lines.values.size() - suffix - 1)) {
    ++suffix;
  }
  const auto old_core = old_lines.values.size() - prefix - suffix;
  const auto new_core = new_lines.values.size() - prefix - suffix;
  if (!comparison_is_bounded(old_core, new_core,
                             options.max_comparison_cells)) {
    BoundedOutput output(options.max_output_bytes);
    output.append(before_exists ? "--- a/" + std::string(path) + "\n"
                                : "--- /dev/null\n");
    output.append("+++ b/" + std::string(path) + "\n");
    output.append("[diff omitted: line comparison exceeds configured limit]\n");
    return {output.take(), output.truncated(), true};
  }
  const auto old_core_end = old_lines.values.size() - suffix;
  const auto new_core_end = new_lines.values.size() - suffix;
  auto operations = make_operations(old_lines, prefix, old_core_end, new_lines,
                                    prefix, new_core_end);
  const auto prefix_context = std::min(prefix, options.context_lines);
  const auto suffix_context = std::min(suffix, options.context_lines);
  std::vector<Operation> contextual_operations;
  contextual_operations.reserve(operations.size() + prefix_context +
                                suffix_context);
  for (std::size_t index = prefix - prefix_context; index < prefix; ++index)
    contextual_operations.push_back({OperationKind::common, index, index});
  contextual_operations.insert(contextual_operations.end(), operations.begin(),
                               operations.end());
  for (std::size_t offset = 0; offset < suffix_context; ++offset) {
    contextual_operations.push_back(
        {OperationKind::common, old_core_end + offset, new_core_end + offset});
  }
  operations = std::move(contextual_operations);
  std::vector<std::size_t> changes;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (operations[index].kind != OperationKind::common)
      changes.push_back(index);
  }
  BoundedOutput diff_output(options.max_output_bytes);
  diff_output.append(before_exists ? "--- a/" + std::string(path) + "\n"
                                   : "--- /dev/null\n");
  diff_output.append("+++ b/" + std::string(path) + "\n");
  std::size_t first_change = 0;
  while (first_change < changes.size()) {
    std::size_t last_change = first_change;
    while (last_change + 1 < changes.size() &&
           changes[last_change + 1] <=
               changes[last_change] + 1 + options.context_lines * 2) {
      ++last_change;
    }
    const auto begin = changes[first_change] > options.context_lines
                           ? changes[first_change] - options.context_lines
                           : 0;
    const auto end = std::min(operations.size(),
                              changes[last_change] + options.context_lines + 1);
    append_hunk(diff_output, operations, old_lines, new_lines,
                prefix - prefix_context, prefix - prefix_context, begin, end);
    first_change = last_change + 1;
  }
  return {diff_output.take(), diff_output.truncated(), false};
}

} // namespace zed::tools
