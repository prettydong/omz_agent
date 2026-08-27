#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "zed/core/result.hpp"

namespace zed::skills {

struct Skill {
    std::string name;
    std::string description;
    std::filesystem::path path;
    std::string instructions;
};

class SkillRegistry {
public:
    core::Result<void> discover(const std::vector<std::filesystem::path>& roots);

    [[nodiscard]] const std::vector<Skill>& all() const { return skills_; }
    [[nodiscard]] const Skill* find(std::string_view name) const;

    [[nodiscard]] std::string prompt_context(
        std::string_view active_skill = {}) const;

private:
    core::Result<Skill> load_skill(const std::filesystem::path& file) const;

    std::vector<Skill> skills_;
};

}  // namespace zed::skills
