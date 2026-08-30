#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace zed::core {

struct Utf8Sanitization {
  std::string text;
  std::size_t replacement_count{};
};

[[nodiscard]] bool is_valid_utf8(std::string_view text);
[[nodiscard]] Utf8Sanitization sanitize_utf8(std::string_view text);

} // namespace zed::core
