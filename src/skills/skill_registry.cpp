#include "zed/skills/skill_registry.hpp"

#include <fstream>
#include <sstream>

namespace zed::skills {

namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string frontmatter_value(
    const std::string& line,
    std::string_view key) {
    const std::string prefix = std::string(key) + ":";
    if (!line.starts_with(prefix)) return {};
    return trim(line.substr(prefix.size()));
}

}  // namespace

core::Result<void> SkillRegistry::discover(
    const std::vector<std::filesystem::path>& roots) {
    skills_.clear();
    for (const auto& root : roots) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) continue;
        if (error) {
            return core::Result<void>::failure({
                core::ErrorCode::internal,
                "cannot inspect skill root: " + error.message(),
            });
        }

        std::filesystem::directory_iterator iterator(root, error);
        if (error) {
            return core::Result<void>::failure({
                core::ErrorCode::internal,
                "cannot list skill root: " + error.message(),
            });
        }
        for (const auto& entry : iterator) {
            if (!entry.is_directory(error) || error) {
                error.clear();
                continue;
            }
            const auto skill_file = entry.path() / "SKILL.md";
            if (!std::filesystem::is_regular_file(skill_file, error) || error) {
                error.clear();
                continue;
            }
            const auto skill = load_skill(skill_file);
            if (!skill) return core::Result<void>::failure(skill.error());
            skills_.push_back(skill.value());
        }
    }
    return core::Result<void>::success();
}

const Skill* SkillRegistry::find(std::string_view name) const {
    for (const auto& skill : skills_) {
        if (skill.name == name) return &skill;
    }
    return nullptr;
}

std::string SkillRegistry::prompt_context(std::string_view active_skill) const {
    if (active_skill.empty()) return {};
    const auto* skill = find(active_skill);
    if (skill == nullptr) return {};
    return "\n\n## Active skill: " + skill->name + "\n" + skill->instructions;
}

core::Result<Skill> SkillRegistry::load_skill(
    const std::filesystem::path& file) const {
    std::ifstream input(file);
    if (!input) {
        return core::Result<Skill>::failure({
            core::ErrorCode::internal,
            "cannot open skill: " + file.string(),
        });
    }

    Skill skill;
    skill.path = file;
    skill.name = file.parent_path().filename().string();
    std::string line;
    std::ostringstream instructions;
    bool in_frontmatter = false;
    bool frontmatter_seen = false;
    while (std::getline(input, line)) {
        if (!frontmatter_seen && trim(line) == "---") {
            frontmatter_seen = true;
            in_frontmatter = true;
            continue;
        }
        if (in_frontmatter && trim(line) == "---") {
            in_frontmatter = false;
            continue;
        }
        if (in_frontmatter) {
            const auto name = frontmatter_value(line, "name");
            const auto description = frontmatter_value(line, "description");
            if (!name.empty()) skill.name = name;
            if (!description.empty()) skill.description = description;
            continue;
        }
        instructions << line << '\n';
    }
    skill.instructions = trim(instructions.str());
    if (skill.description.empty()) {
        const auto newline = skill.instructions.find('\n');
        skill.description = skill.instructions.substr(0, newline);
    }
    if (skill.name.empty() || skill.instructions.empty()) {
        return core::Result<Skill>::failure({
            core::ErrorCode::invalid_argument,
            "skill must have a name and instructions: " + file.string(),
        });
    }
    return core::Result<Skill>::success(std::move(skill));
}

}  // namespace zed::skills
