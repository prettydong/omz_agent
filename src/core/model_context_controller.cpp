#include "zed/core/model_context_controller.hpp"

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zed::core {

namespace {

using Json = nlohmann::json;

constexpr std::string_view kDefaultContextSystemPrompt =
    "You manage coding-agent context. Return JSON only with this exact shape: "
    "{\"selected_ids\":[string],\"summarized_ids\":[string],\"summary\":"
    "string}. Always select every candidate where required is true. Keep the "
    "latest task relevant. Do not invent ids.";

const Json *field(const Json &object, std::string_view name) {
  if (!object.is_object())
    return nullptr;
  const auto iterator = object.find(std::string(name));
  return iterator == object.end() ? nullptr : &*iterator;
}

std::string decision_prompt(const ContextRequest &request) {
  Json root = Json::object();
  root["current_task"] = request.current_task;
  root["max_context_tokens"] =
      static_cast<double>(request.limits.max_context_tokens);
  root["candidates"] = Json::array();
  for (const auto &candidate : request.candidates) {
    Json value = Json::object();
    value["id"] = candidate.id;
    value["content"] = candidate.message.content;
    value["estimated_tokens"] = static_cast<double>(candidate.estimated_tokens);
    value["required"] = candidate.required;
    root["candidates"].push_back(std::move(value));
  }
  return root.dump();
}

std::string strip_code_fence(std::string text) {
  const auto first = text.find('{');
  const auto last = text.rfind('}');
  if (first != std::string::npos && last != std::string::npos &&
      last >= first) {
    return text.substr(first, last - first + 1);
  }
  return text;
}

} // namespace

ModelBackedContextController::ModelBackedContextController(
    Model &model, ModelRef model_ref, std::string system_prompt,
    std::size_t max_output_tokens)
    : model_(model), model_ref_(std::move(model_ref)),
      system_prompt_(system_prompt.empty()
                         ? std::string(kDefaultContextSystemPrompt)
                         : std::move(system_prompt)),
      max_output_tokens_(max_output_tokens) {}

std::string_view default_context_system_prompt() {
  return kDefaultContextSystemPrompt;
}

Result<ContextDecision>
ModelBackedContextController::decide(const ContextRequest &request,
                                     CancellationToken cancellation) {
  Message user_message{
      "context-controller-request",
      Role::user,
      decision_prompt(request),
      {},
      std::nullopt,
  };
  ModelRequest model_request;
  model_request.model = model_ref_;
  model_request.messages = {Message{
                                "context-controller-system",
                                Role::system,
                                system_prompt_,
                                {},
                                std::nullopt,
                            },
                            user_message};
  model_request.max_output_tokens = max_output_tokens_;
  model_request.temperature = 0.0;
  model_request.reasoning_effort = ReasoningEffort::none;

  const auto response = model_.complete(model_request, {}, cancellation);
  if (!response)
    return Result<ContextDecision>::failure(response.error());

  Json parsed;
  try {
    parsed = Json::parse(strip_code_fence(response.value().content));
  } catch (const Json::parse_error &error) {
    return Result<ContextDecision>::failure({
        ErrorCode::context_error,
        "context controller returned invalid JSON: " +
            std::string(error.what()),
    });
  }
  if (!parsed.is_object()) {
    return Result<ContextDecision>::failure({
        ErrorCode::context_error,
        "context controller returned a non-object JSON value",
    });
  }

  ContextDecision decision;
  const auto *selected = field(parsed, "selected_ids");
  if (selected != nullptr && selected->is_array()) {
    for (const auto &value : *selected) {
      if (value.is_string())
        decision.selected_ids.push_back(value.get<std::string>());
    }
  }
  const auto *summarized = field(parsed, "summarized_ids");
  if (summarized != nullptr && summarized->is_array()) {
    for (const auto &value : *summarized) {
      if (value.is_string())
        decision.summarized_ids.push_back(value.get<std::string>());
    }
  }
  if (const auto *summary = field(parsed, "summary");
      summary != nullptr && summary->is_string()) {
    decision.summary = summary->get<std::string>();
  }
  return Result<ContextDecision>::success(std::move(decision));
}

} // namespace zed::core
