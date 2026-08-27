#include "zed/session/jsonl_session_store.hpp"

#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zed::session {

namespace {

using core::ErrorCode;
using core::Message;
using core::Role;
using Json = nlohmann::json;

const Json* field(const Json& object, std::string_view name) {
    if (!object.is_object()) return nullptr;
    const auto iterator = object.find(std::string(name));
    return iterator == object.end() ? nullptr : &*iterator;
}

const char* role_name(Role role) {
    switch (role) {
        case Role::system: return "system";
        case Role::user: return "user";
        case Role::assistant: return "assistant";
        case Role::tool: return "tool";
    }
    return "user";
}

std::optional<Role> parse_role(const Json* value) {
    if (value == nullptr || !value->is_string()) return std::nullopt;
    const auto role = value->get<std::string>();
    if (role == "system") return Role::system;
    if (role == "user") return Role::user;
    if (role == "assistant") return Role::assistant;
    if (role == "tool") return Role::tool;
    return std::nullopt;
}

Json message_json(const Message& message) {
    Json object = Json::object();
    object["id"] = message.id;
    object["role"] = role_name(message.role);
    object["content"] = message.content;

    Json calls = Json::array();
    for (const auto& call : message.tool_calls) {
        Json call_object = Json::object();
        call_object["id"] = call.id;
        call_object["name"] = call.name;
        call_object["arguments_json"] = call.arguments_json;
        calls.emplace_back(std::move(call_object));
    }
    object["tool_calls"] = std::move(calls);
    if (message.tool_call_id.has_value()) {
        object["tool_call_id"] = *message.tool_call_id;
    }
    return Json(std::move(object));
}

core::Result<Message> parse_message(const Json& root) {
    if (!root.is_object()) {
        return core::Result<Message>::failure({ErrorCode::session_error, "session record is not an object"});
    }
    if (!root.contains("type") || !root.at("type").is_string() ||
        root.at("type").get<std::string>() != "message") {
        return core::Result<Message>::failure({ErrorCode::session_error, "unsupported session record type"});
    }
    const auto role = parse_role(field(root, "role"));
    const auto* id = field(root, "id");
    const auto* content = field(root, "content");
    if (!role || id == nullptr || !id->is_string() || content == nullptr || !content->is_string()) {
        return core::Result<Message>::failure({ErrorCode::session_error, "invalid message record"});
    }

    Message message;
    message.id = id->get<std::string>();
    message.role = *role;
    message.content = content->get<std::string>();
    if (const auto* tool_call_id = field(root, "tool_call_id"); tool_call_id != nullptr && tool_call_id->is_string()) {
        message.tool_call_id = tool_call_id->get<std::string>();
    }
    if (const auto* calls = field(root, "tool_calls"); calls != nullptr && calls->is_array()) {
        for (const auto& call_value : *calls) {
            if (!call_value.is_object()) {
                return core::Result<Message>::failure({ErrorCode::session_error, "invalid tool call record"});
            }
            const auto* call_id = field(call_value, "id");
            const auto* name = field(call_value, "name");
            const auto* arguments = field(call_value, "arguments_json");
            if (call_id == nullptr || name == nullptr || arguments == nullptr ||
                !call_id->is_string() || !name->is_string() || !arguments->is_string()) {
                return core::Result<Message>::failure({ErrorCode::session_error, "invalid tool call fields"});
            }
            message.tool_calls.push_back({
                call_id->get<std::string>(),
                name->get<std::string>(),
                arguments->get<std::string>(),
            });
        }
    }
    return core::Result<Message>::success(std::move(message));
}

}  // namespace

JsonlSessionStore::JsonlSessionStore(std::filesystem::path path)
    : path_(std::move(path)) {}

core::Result<void> JsonlSessionStore::append(const core::Message& message) {
    std::error_code filesystem_error;
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path(), filesystem_error);
        if (filesystem_error) {
            return core::Result<void>::failure({
                ErrorCode::session_error,
                "cannot create session directory: " + filesystem_error.message(),
            });
        }
    }

    std::ofstream output(path_, std::ios::app);
    if (!output) {
        return core::Result<void>::failure({ErrorCode::session_error, "cannot open session file for append"});
    }

    Json record = Json::object();
    record["version"] = 1;
    record["type"] = "message";
    const auto serialized = message_json(message);
    record.update(serialized);
    output << record.dump() << '\n';
    if (!output) {
        return core::Result<void>::failure({ErrorCode::session_error, "cannot write session record"});
    }
    output.flush();
    return output
        ? core::Result<void>::success()
        : core::Result<void>::failure({ErrorCode::session_error, "cannot flush session record"});
}

core::Result<std::vector<core::Message>> JsonlSessionStore::load() const {
    if (!std::filesystem::exists(path_)) {
        return core::Result<std::vector<core::Message>>::success({});
    }

    std::ifstream input(path_);
    if (!input) {
        return core::Result<std::vector<core::Message>>::failure({ErrorCode::session_error, "cannot open session file"});
    }

    std::vector<core::Message> messages;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) continue;
        std::optional<Json> record;
        try {
            record = Json::parse(line);
        } catch (const Json::parse_error& parse_error) {
            // A process can be interrupted after writing part of the final JSONL record.
            // Only tolerate that exact trailing case; malformed complete records remain fatal.
            if (input.eof()) break;
            return core::Result<std::vector<core::Message>>::failure({
                ErrorCode::session_error,
                "invalid JSONL at line " + std::to_string(line_number) + ": " + parse_error.what(),
            });
        }
        const auto message = parse_message(*record);
        if (!message) {
            return core::Result<std::vector<core::Message>>::failure({
                ErrorCode::session_error,
                "invalid session record at line " + std::to_string(line_number) + ": " + message.error().message,
            });
        }
        messages.push_back(message.value());
    }
    return core::Result<std::vector<core::Message>>::success(std::move(messages));
}

}  // namespace zed::session
