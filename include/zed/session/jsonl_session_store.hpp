#pragma once

#include <filesystem>

#include "zed/core/session_store.hpp"

namespace zed::session {

class JsonlSessionStore final : public core::SessionStore {
public:
    explicit JsonlSessionStore(std::filesystem::path path);

    core::Result<void> append(const core::Message& message) override;
    core::Result<std::vector<core::Message>> load() const override;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace zed::session
