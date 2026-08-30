#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/result.hpp"

namespace zed::session {

struct SessionEntry {
  std::string name;
  std::string title;
  std::filesystem::path path;
  std::filesystem::file_time_type modified_at;
  std::size_t message_count{};
  std::size_t turn_count{};
  bool interrupted{false};
  bool valid{true};
  std::string error;
};

std::filesystem::path new_session_path(const std::filesystem::path &directory);

core::Result<std::vector<SessionEntry>>
list_sessions(const std::filesystem::path &directory);

core::Result<SessionEntry> find_session(const std::filesystem::path &directory,
                                        std::string_view identifier);

} // namespace zed::session
