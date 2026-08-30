#include "zed/core/utf8.hpp"

namespace zed::core {

namespace {

bool is_continuation(unsigned char byte) {
  return byte >= 0x80U && byte <= 0xBFU;
}

std::size_t valid_sequence_size(std::string_view text, std::size_t offset) {
  const auto byte = static_cast<unsigned char>(text[offset]);
  if (byte <= 0x7FU)
    return 1;

  const auto remaining = text.size() - offset;
  if (byte >= 0xC2U && byte <= 0xDFU) {
    return remaining >= 2 &&
                   is_continuation(static_cast<unsigned char>(text[offset + 1]))
               ? 2
               : 0;
  }
  if (remaining < 3)
    return 0;

  const auto second = static_cast<unsigned char>(text[offset + 1]);
  const auto third = static_cast<unsigned char>(text[offset + 2]);
  if (byte == 0xE0U) {
    return second >= 0xA0U && second <= 0xBFU && is_continuation(third) ? 3 : 0;
  }
  if ((byte >= 0xE1U && byte <= 0xECU) || (byte >= 0xEEU && byte <= 0xEFU)) {
    return is_continuation(second) && is_continuation(third) ? 3 : 0;
  }
  if (byte == 0xEDU) {
    return second >= 0x80U && second <= 0x9FU && is_continuation(third) ? 3 : 0;
  }
  if (remaining < 4)
    return 0;

  const auto fourth = static_cast<unsigned char>(text[offset + 3]);
  if (byte == 0xF0U) {
    return second >= 0x90U && second <= 0xBFU && is_continuation(third) &&
                   is_continuation(fourth)
               ? 4
               : 0;
  }
  if (byte >= 0xF1U && byte <= 0xF3U) {
    return is_continuation(second) && is_continuation(third) &&
                   is_continuation(fourth)
               ? 4
               : 0;
  }
  if (byte == 0xF4U) {
    return second >= 0x80U && second <= 0x8FU && is_continuation(third) &&
                   is_continuation(fourth)
               ? 4
               : 0;
  }
  return 0;
}

} // namespace

bool is_valid_utf8(std::string_view text) {
  for (std::size_t offset = 0; offset < text.size();) {
    const auto size = valid_sequence_size(text, offset);
    if (size == 0)
      return false;
    offset += size;
  }
  return true;
}

Utf8Sanitization sanitize_utf8(std::string_view text) {
  Utf8Sanitization result;
  result.text.reserve(text.size());
  for (std::size_t offset = 0; offset < text.size();) {
    const auto size = valid_sequence_size(text, offset);
    if (size == 0) {
      result.text += "\xEF\xBF\xBD";
      ++result.replacement_count;
      ++offset;
      continue;
    }
    result.text.append(text.substr(offset, size));
    offset += size;
  }
  return result;
}

} // namespace zed::core
