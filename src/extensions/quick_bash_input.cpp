#include "zed/extensions/quick_bash_input.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zed::extensions {

namespace {

constexpr std::array<std::string_view, 8> kAllowedCommands{
    "pwd", "ls", "ps", "pgrep", "kill", "pkill", "which", "whoami",
};

struct ParsedCommand {
  std::vector<std::string> words;
  std::optional<core::Error> error;
};

bool is_space(char character) { return character == ' ' || character == '\t'; }

bool is_forbidden_unquoted(char character) {
  return character == '|' || character == '&' || character == ';' ||
         character == '<' || character == '>' || character == '(' ||
         character == ')' || character == '`' || character == '$' ||
         character == '#';
}

core::Error syntax_error(std::string message) {
  return {core::ErrorCode::invalid_argument,
          "quick bash syntax error: " + std::move(message)};
}

ParsedCommand parse_simple_command(std::string_view input) {
  enum class Quote { none, single, double_quote };

  ParsedCommand parsed;
  std::string word;
  Quote quote = Quote::none;
  bool escaped = false;
  bool word_started = false;

  const auto finish_word = [&] {
    if (!word_started)
      return;
    parsed.words.push_back(std::move(word));
    word.clear();
    word_started = false;
  };

  for (const char character : input) {
    if (character == '\n' || character == '\r' || character == '\0') {
      finish_word();
      parsed.error = syntax_error("newlines are not supported");
      return parsed;
    }

    if (escaped) {
      word.push_back(character);
      word_started = true;
      escaped = false;
      continue;
    }

    if (quote == Quote::single) {
      if (character == '\'') {
        quote = Quote::none;
      } else {
        word.push_back(character);
      }
      word_started = true;
      continue;
    }

    if (quote == Quote::double_quote) {
      if (character == '"') {
        quote = Quote::none;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '$' || character == '`') {
        finish_word();
        parsed.error =
            syntax_error("variable and command expansion are not supported");
        return parsed;
      } else {
        word.push_back(character);
      }
      word_started = true;
      continue;
    }

    if (is_space(character)) {
      finish_word();
      continue;
    }
    if (character == '\\') {
      escaped = true;
      word_started = true;
      continue;
    }
    if (character == '\'') {
      quote = Quote::single;
      word_started = true;
      continue;
    }
    if (character == '"') {
      quote = Quote::double_quote;
      word_started = true;
      continue;
    }
    if (is_forbidden_unquoted(character)) {
      finish_word();
      parsed.error = syntax_error(
          "pipes, redirects, chaining, comments, and expansion are not "
          "supported");
      return parsed;
    }
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20U) {
      finish_word();
      parsed.error = syntax_error("control characters are not supported");
      return parsed;
    }
    word.push_back(character);
    word_started = true;
  }

  if (escaped) {
    finish_word();
    parsed.error = syntax_error("trailing escape character");
    return parsed;
  }
  if (quote != Quote::none) {
    finish_word();
    parsed.error = syntax_error("unterminated quote");
    return parsed;
  }
  finish_word();
  return parsed;
}

bool is_allowed(std::string_view command) {
  for (const auto allowed : kAllowedCommands) {
    if (command == allowed)
      return true;
  }
  return false;
}

std::string trim(std::string_view value) {
  while (!value.empty() && is_space(value.front()))
    value.remove_prefix(1);
  while (!value.empty() && is_space(value.back()))
    value.remove_suffix(1);
  return std::string(value);
}

std::string next_call_id() {
  static std::atomic_uint64_t sequence{0};
  return "quick-bash-" + std::to_string(++sequence);
}

} // namespace

QuickBashInput::QuickBashInput(core::ToolRegistry &tools, bool enabled)
    : tools_(tools), enabled_(enabled) {}

bool QuickBashInput::enabled() const { return enabled_; }

void QuickBashInput::set_enabled(bool enabled) { enabled_ = enabled; }

core::Result<std::optional<std::string>>
QuickBashInput::classify(std::string_view input) const {
  if (!enabled_)
    return core::Result<std::optional<std::string>>::success(std::nullopt);

  const auto parsed = parse_simple_command(input);
  if (parsed.words.empty())
    return core::Result<std::optional<std::string>>::success(std::nullopt);
  if (!is_allowed(parsed.words.front()))
    return core::Result<std::optional<std::string>>::success(std::nullopt);
  if (parsed.error.has_value())
    return core::Result<std::optional<std::string>>::failure(*parsed.error);

  return core::Result<std::optional<std::string>>::success(trim(input));
}

core::Result<core::ToolResult>
QuickBashInput::execute(std::string_view command,
                        core::CancellationToken cancellation) {
  const auto classified = classify(command);
  if (!classified)
    return core::Result<core::ToolResult>::failure(classified.error());
  if (!classified.value().has_value()) {
    return core::Result<core::ToolResult>::failure({
        core::ErrorCode::invalid_argument,
        "input is not an enabled quick bash command",
    });
  }

  const nlohmann::json arguments = {
      {"purpose", std::string(quick_bash_purpose())},
      {"command", *classified.value()},
  };
  return tools_.execute({next_call_id(), "bash", arguments.dump()},
                        cancellation);
}

} // namespace zed::extensions
