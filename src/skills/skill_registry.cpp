#include "zed/skills/skill_registry.hpp"

#include "zed/core/utf8.hpp"
#include "zed/support/atomic_file.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace zed::skills {

namespace {

constexpr std::size_t kMaximumSkills = 64;
constexpr std::size_t kMaximumInstructionsBytes = 1024 * 1024;
constexpr std::size_t kMaximumSkillFileBytes =
    kMaximumInstructionsBytes + 4 * 1024;

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string frontmatter_value(const std::string &line, std::string_view key) {
  const std::string prefix = std::string(key) + ":";
  if (!line.starts_with(prefix))
    return {};
  return trim(line.substr(prefix.size()));
}

bool valid_id(std::string_view value) {
  if (value.empty() || value.size() > 64 ||
      std::isalnum(static_cast<unsigned char>(value.front())) == 0) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '-' ||
           character == '_';
  });
}

bool valid_metadata(std::string_view value, std::size_t maximum,
                    bool allow_empty = false) {
  if ((!allow_empty && value.empty()) || value.size() > maximum ||
      !core::is_valid_utf8(value)) {
    return false;
  }
  return std::none_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

core::Result<std::string>
read_regular_skill_file(const std::filesystem::path &file) {
  support::UniqueFd descriptor(
      open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!descriptor.valid()) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "cannot open regular skill file " + file.string() + ": " +
            std::string(std::strerror(errno)),
    });
  }
  struct stat status{};
  if (fstat(descriptor.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "skill file must be a regular file: " + file.string(),
    });
  }
  if (status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > kMaximumSkillFileBytes) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "skill file exceeds the 1 MiB instruction limit: " + file.string(),
    });
  }

  std::string content;
  content.reserve(static_cast<std::size_t>(status.st_size));
  std::array<char, 8192> buffer{};
  while (true) {
    const auto count = read(descriptor.get(), buffer.data(), buffer.size());
    if (count > 0) {
      const auto chunk_size = static_cast<std::size_t>(count);
      if (chunk_size > kMaximumSkillFileBytes - content.size()) {
        return core::Result<std::string>::failure({
            core::ErrorCode::invalid_argument,
            "skill file exceeds the 1 MiB instruction limit: " + file.string(),
        });
      }
      content.append(buffer.data(), chunk_size);
      continue;
    }
    if (count == 0)
      break;
    if (errno == EINTR)
      continue;
    return core::Result<std::string>::failure({
        core::ErrorCode::internal,
        "cannot read skill file " + file.string() + ": " +
            std::string(std::strerror(errno)),
    });
  }
  if (!core::is_valid_utf8(content)) {
    return core::Result<std::string>::failure({
        core::ErrorCode::invalid_argument,
        "skill file must contain valid UTF-8: " + file.string(),
    });
  }
  return core::Result<std::string>::success(std::move(content));
}

std::string serialize_skill(const ManagedSkill &skill) {
  return "---\nname: " + skill.name + "\ndescription: " + skill.description +
         "\n---\n\n" + skill.instructions + "\n";
}

core::Result<void> ensure_regular_root(const std::filesystem::path &root) {
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "cannot create skill root " + root.string() + ": " + error.message(),
    });
  }
  const auto status = std::filesystem::symlink_status(root, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_directory(status)) {
    return core::Result<void>::failure({
        core::ErrorCode::invalid_argument,
        "skill root must be a regular directory: " + root.string(),
    });
  }
  return core::Result<void>::success();
}

core::Result<void> archive_skill(const std::filesystem::path &workspace,
                                 const ManagedSkill &skill) {
  const auto source = workspace / ".zed" /
                      (skill.enabled ? "skills" : "skills-disabled") / skill.id;
  std::error_code error;
  if (!std::filesystem::exists(source, error)) {
    if (error) {
      return core::Result<void>::failure({
          core::ErrorCode::internal,
          "cannot inspect removed skill " + skill.id + ": " + error.message(),
      });
    }
    return core::Result<void>::success();
  }
  const auto archive_root = workspace / ".zed" / "skill-archive";
  const auto ready = ensure_regular_root(archive_root);
  if (!ready)
    return ready;
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  static std::atomic<unsigned long> sequence{0};
  const auto destination =
      archive_root / (skill.id + "-" + std::to_string(milliseconds) + "-" +
                      std::to_string(sequence.fetch_add(1)));
  std::filesystem::rename(source, destination, error);
  if (error) {
    return core::Result<void>::failure({
        core::ErrorCode::internal,
        "cannot archive removed skill " + skill.id + ": " + error.message(),
    });
  }
  return core::Result<void>::success();
}

} // namespace

core::Result<void>
SkillRegistry::discover(const std::vector<std::filesystem::path> &roots) {
  std::vector<Skill> discovered_skills;
  for (const auto &root : roots) {
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(root_status))) {
      continue;
    }
    if (error) {
      return core::Result<void>::failure({
          core::ErrorCode::internal,
          "cannot inspect skill root: " + error.message(),
      });
    }
    const auto parent_status =
        std::filesystem::symlink_status(root.parent_path(), error);
    if (error || std::filesystem::is_symlink(parent_status) ||
        std::filesystem::is_symlink(root_status) ||
        !std::filesystem::is_directory(root_status)) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "skill root and its parent must be regular directories: " +
              root.string(),
      });
    }

    std::filesystem::directory_iterator iterator(root, error);
    if (error) {
      return core::Result<void>::failure({
          core::ErrorCode::internal,
          "cannot list skill root: " + error.message(),
      });
    }
    for (const auto &entry : iterator) {
      const auto entry_status = entry.symlink_status(error);
      if (error) {
        error.clear();
        continue;
      }
      if (std::filesystem::is_symlink(entry_status)) {
        return core::Result<void>::failure({
            core::ErrorCode::invalid_argument,
            "skill directory cannot be a symlink: " + entry.path().string(),
        });
      }
      if (!std::filesystem::is_directory(entry_status))
        continue;
      const auto skill_file = entry.path() / "SKILL.md";
      const auto skill_status =
          std::filesystem::symlink_status(skill_file, error);
      if (error == std::errc::no_such_file_or_directory ||
          (!error && !std::filesystem::exists(skill_status))) {
        error.clear();
        continue;
      }
      if (error) {
        return core::Result<void>::failure({
            core::ErrorCode::internal,
            "cannot inspect skill file " + skill_file.string() + ": " +
                error.message(),
        });
      }
      if (std::filesystem::is_symlink(skill_status) ||
          !std::filesystem::is_regular_file(skill_status)) {
        return core::Result<void>::failure({
            core::ErrorCode::invalid_argument,
            "SKILL.md must be a regular file and cannot be a symlink: " +
                skill_file.string(),
        });
      }
      auto skill = load_skill(skill_file);
      if (!skill)
        return core::Result<void>::failure(skill.error());
      discovered_skills.push_back(std::move(skill.value()));
      if (discovered_skills.size() > kMaximumSkills) {
        return core::Result<void>::failure({
            core::ErrorCode::invalid_argument,
            "skill discovery exceeds the limit of " +
                std::to_string(kMaximumSkills),
        });
      }
    }
  }
  skills_ = std::move(discovered_skills);
  return core::Result<void>::success();
}

const Skill *SkillRegistry::find(std::string_view name) const {
  for (const auto &skill : skills_) {
    if (skill.name == name)
      return &skill;
  }
  return nullptr;
}

std::string SkillRegistry::prompt_context(std::string_view active_skill) const {
  if (active_skill.empty())
    return {};
  const auto *skill = find(active_skill);
  if (skill == nullptr)
    return {};
  return "\n\n## Active skill: " + skill->name + "\n" + skill->instructions;
}

core::Result<Skill>
SkillRegistry::load_skill(const std::filesystem::path &file) const {
  const auto content = read_regular_skill_file(file);
  if (!content)
    return core::Result<Skill>::failure(content.error());
  std::istringstream input(content.value());

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
      if (!name.empty())
        skill.name = name;
      if (!description.empty())
        skill.description = description;
      continue;
    }
    instructions << line << '\n';
  }
  skill.instructions = trim(instructions.str());
  if (skill.description.empty()) {
    const auto newline = skill.instructions.find('\n');
    skill.description = skill.instructions.substr(0, newline);
  }
  if (!valid_metadata(skill.name, 128) ||
      !valid_metadata(skill.description, 1'024, true) ||
      skill.instructions.empty() ||
      skill.instructions.size() > kMaximumInstructionsBytes) {
    return core::Result<Skill>::failure({
        core::ErrorCode::invalid_argument,
        "skill requires valid UTF-8 metadata and non-empty instructions up to "
        "1 MiB: " +
            file.string(),
    });
  }
  return core::Result<Skill>::success(std::move(skill));
}

core::Result<std::vector<ManagedSkill>>
load_workspace_skills(const std::filesystem::path &workspace) {
  std::vector<ManagedSkill> result;
  const auto load_root = [&](std::string_view directory,
                             bool enabled) -> core::Result<void> {
    const auto root = workspace / ".zed" / std::string(directory);
    std::error_code status_error;
    const auto root_status =
        std::filesystem::symlink_status(root, status_error);
    if (!status_error && std::filesystem::is_symlink(root_status)) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "skill root cannot be a symlink: " + root.string(),
      });
    }
    if (status_error != std::errc::no_such_file_or_directory && status_error) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "cannot inspect skill root " + root.string() + ": " +
              status_error.message(),
      });
    }
    SkillRegistry registry;
    const auto discovered = registry.discover({root});
    if (!discovered)
      return discovered;
    for (const auto &skill : registry.all()) {
      const auto directory_status = std::filesystem::symlink_status(
          skill.path.parent_path(), status_error);
      if (status_error || std::filesystem::is_symlink(directory_status)) {
        return core::Result<void>::failure({
            core::ErrorCode::invalid_argument,
            "skill directory cannot be a symlink: " +
                skill.path.parent_path().string(),
        });
      }
      const auto id = skill.path.parent_path().filename().string();
      const auto duplicate = std::find_if(
          result.begin(), result.end(),
          [&](const ManagedSkill &existing) { return existing.id == id; });
      if (duplicate != result.end()) {
        return core::Result<void>::failure({
            core::ErrorCode::conflict,
            "skill exists in enabled and disabled roots: " + id,
        });
      }
      result.push_back(
          {id, skill.name, skill.description, skill.instructions, enabled});
    }
    return core::Result<void>::success();
  };
  const auto enabled = load_root("skills", true);
  if (!enabled)
    return core::Result<std::vector<ManagedSkill>>::failure(enabled.error());
  const auto disabled = load_root("skills-disabled", false);
  if (!disabled)
    return core::Result<std::vector<ManagedSkill>>::failure(disabled.error());
  const auto valid = validate_workspace_skills(result);
  if (!valid)
    return core::Result<std::vector<ManagedSkill>>::failure(valid.error());
  std::sort(result.begin(), result.end(),
            [](const ManagedSkill &left, const ManagedSkill &right) {
              return left.id < right.id;
            });
  return core::Result<std::vector<ManagedSkill>>::success(std::move(result));
}

core::Result<void>
validate_workspace_skills(const std::vector<ManagedSkill> &skills) {
  if (skills.size() > kMaximumSkills) {
    return core::Result<void>::failure(
        {core::ErrorCode::invalid_argument,
         "workspace supports at most 64 managed skills"});
  }
  std::vector<std::string> ids;
  for (const auto &skill : skills) {
    if (!valid_id(skill.id) || !valid_metadata(skill.name, 128) ||
        !valid_metadata(skill.description, 1'024, true) ||
        skill.instructions.empty() ||
        skill.instructions.size() > kMaximumInstructionsBytes ||
        !core::is_valid_utf8(skill.instructions) ||
        std::find(ids.begin(), ids.end(), skill.id) != ids.end()) {
      return core::Result<void>::failure({
          core::ErrorCode::invalid_argument,
          "skills require unique lowercase ids, valid metadata, and non-empty "
          "UTF-8 instructions up to 1 MiB",
      });
    }
    ids.push_back(skill.id);
  }
  return core::Result<void>::success();
}

core::Result<void>
save_workspace_skills(const std::filesystem::path &workspace,
                      const std::vector<ManagedSkill> &skills) {
  const auto valid = validate_workspace_skills(skills);
  if (!valid)
    return valid;
  const auto existing = load_workspace_skills(workspace);
  if (!existing)
    return core::Result<void>::failure(existing.error());
  for (const auto &current : existing.value()) {
    const auto desired = std::find_if(
        skills.begin(), skills.end(),
        [&](const ManagedSkill &skill) { return skill.id == current.id; });
    if (desired == skills.end()) {
      const auto archived = archive_skill(workspace, current);
      if (!archived)
        return archived;
    }
  }

  for (const auto &skill : skills) {
    const auto current =
        std::find_if(existing.value().begin(), existing.value().end(),
                     [&](const ManagedSkill &candidate) {
                       return candidate.id == skill.id;
                     });
    const auto desired_root =
        workspace / ".zed" / (skill.enabled ? "skills" : "skills-disabled");
    const auto ready = ensure_regular_root(desired_root);
    if (!ready)
      return ready;
    const auto desired_directory = desired_root / skill.id;
    if (current != existing.value().end() &&
        current->enabled != skill.enabled) {
      const auto source = workspace / ".zed" /
                          (current->enabled ? "skills" : "skills-disabled") /
                          skill.id;
      std::error_code error;
      std::filesystem::rename(source, desired_directory, error);
      if (error) {
        return core::Result<void>::failure({
            core::ErrorCode::internal,
            "cannot change skill enabled state for " + skill.id + ": " +
                error.message(),
        });
      }
    }
    const auto saved = support::write_private_file_atomically(
        desired_directory / "SKILL.md", serialize_skill(skill), "skill file");
    if (!saved)
      return saved;
  }
  return core::Result<void>::success();
}

} // namespace zed::skills
