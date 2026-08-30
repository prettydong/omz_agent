#pragma once

#include "zed/core/cancellation.hpp"
#include "zed/core/result.hpp"

#include <filesystem>
#include <string_view>

namespace zed::support {

// Atomically writes a workspace file without following a temporary-file
// symlink. Existing file permissions are preserved. When replace_existing is
// false, a concurrently-created destination is reported as a conflict.
core::Result<void>
write_file_atomically(const std::filesystem::path &path,
                      std::string_view content, std::string_view description,
                      bool replace_existing,
                      core::CancellationToken cancellation = {});

// Replaces a private file without following symlinks at the destination.
// The resulting file is mode 0600 and both file data and the containing
// directory entry are flushed before success is reported.
core::Result<void>
write_private_file_atomically(const std::filesystem::path &path,
                              std::string_view content,
                              std::string_view description);

} // namespace zed::support
