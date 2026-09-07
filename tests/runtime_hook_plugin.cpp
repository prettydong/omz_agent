#include "zed/plugins/plugin_sdk.h"

#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

constexpr std::string_view kPluginId = "runtime.hooks";
constexpr std::string_view kPluginName = "Runtime hook fixture";
constexpr std::string_view kPluginVersion = "1.0.0";
constexpr std::string_view kReadyFile = "runtime-hooks.ready";
constexpr std::string_view kEventsFile = "runtime-hook-events.jsonl";

ZedaStringView view(std::string_view value) {
  return {value.data(), value.size()};
}

std::string copy(ZedaStringView value) {
  if (value.data == nullptr)
    return {};
  return {value.data, value.size};
}

int fail(ZedaTextSinkV1 error, std::string_view detail) {
  if (error.write != nullptr)
    static_cast<void>(error.write(error.context, view(detail)));
  return -1;
}

bool write_text(ZedaTextSinkV1 output, std::string_view text) {
  return output.write != nullptr &&
         output.write(output.context, view(text)) == 0;
}

struct RuntimeHookPlugin {
  std::filesystem::path workspace;
};

void append_event(const RuntimeHookPlugin &plugin, std::uint32_t kind) {
  std::ofstream output(plugin.workspace / kEventsFile,
                       std::ios::binary | std::ios::app);
  output << Json{{"kind", kind}}.dump() << '\n';
}

int execute_hook(void *context, std::uint32_t kind, ZedaStringView payload_json,
                 ZedaCancellationV1 cancellation,
                 ZedaTextSinkV1 replacement_json, ZedaTextSinkV1 error) {
  auto *plugin = static_cast<RuntimeHookPlugin *>(context);
  if (plugin == nullptr)
    return fail(error, "runtime hook has no plugin context");
  if (cancellation.is_cancelled != nullptr &&
      cancellation.is_cancelled(cancellation.context) != 0) {
    return fail(error, "runtime hook was cancelled");
  }
  try {
    auto payload = Json::parse(copy(payload_json));
    switch (kind) {
    case ZEDA_HOOK_AGENT_TURN_START:
      payload["user_input"] =
          payload.at("user_input").get<std::string>() + ":v2-start";
      break;
    case ZEDA_HOOK_USER_MESSAGE_SUBMIT:
      payload["message"]["content"] =
          payload.at("message").at("content").get<std::string>() + ":v2-submit";
      break;
    case ZEDA_HOOK_BEFORE_MODEL_REQUEST:
      payload["request"]["temperature"] = 0.5;
      break;
    case ZEDA_HOOK_AFTER_MODEL_RESPONSE:
      payload["response"]["content"] =
          payload.at("response").at("content").get<std::string>() +
          ":v2-response";
      break;
    case ZEDA_HOOK_BEFORE_TOOL_CALL: {
      auto arguments = Json::parse(
          payload.at("call").at("arguments_json").get<std::string>());
      arguments["text"] = "v2-tool";
      payload["call"]["arguments_json"] = arguments.dump();
      break;
    }
    case ZEDA_HOOK_AFTER_TOOL_RESULT:
      payload["result"]["content"] =
          payload.at("result").at("content").get<std::string>() + ":v2-result";
      break;
    case ZEDA_HOOK_SESSION_WRITE:
      if (payload.contains("message")) {
        payload["message"]["content"] =
            payload.at("message").at("content").get<std::string>() +
            ":v2-session";
      }
      break;
    case ZEDA_HOOK_AGENT_TURN_END:
      payload["detail"] = "v2-ended";
      break;
    default:
      return fail(error, "runtime hook received an unknown kind");
    }
    append_event(*plugin, kind);
    const auto replacement = payload.dump();
    if (!write_text(replacement_json, replacement))
      return fail(error, "runtime hook replacement sink rejected output");
    return ZEDA_HOOK_REPLACE;
  } catch (const std::exception &exception) {
    return fail(error, exception.what());
  } catch (...) {
    return fail(error, "runtime hook failed with an unknown error");
  }
}

void *create_plugin() { return new (std::nothrow) RuntimeHookPlugin; }

int initialize_plugin(void *instance, const ZedaHostApiV2 *host,
                      ZedaTextSinkV1 error) {
  auto *plugin = static_cast<RuntimeHookPlugin *>(instance);
  if (plugin == nullptr || host == nullptr ||
      host->abi_version != ZEDA_PLUGIN_ABI_VERSION_V2 ||
      host->struct_size < sizeof(ZedaHostApiV2) ||
      host->register_hook == nullptr) {
    return fail(error, "runtime hook plugin received an invalid v2 host");
  }
  try {
    plugin->workspace = copy(host->workspace_root);
    {
      std::ofstream ready(plugin->workspace / kReadyFile,
                          std::ios::binary | std::ios::trunc);
      ready << "ready\n";
      if (!ready)
        return fail(error, "runtime hook plugin cannot create ready file");
    }

    const struct {
      std::string_view name;
      std::uint32_t kind;
    } hooks[] = {
        {"turn-start", ZEDA_HOOK_AGENT_TURN_START},
        {"user-submit", ZEDA_HOOK_USER_MESSAGE_SUBMIT},
        {"before-model", ZEDA_HOOK_BEFORE_MODEL_REQUEST},
        {"after-model", ZEDA_HOOK_AFTER_MODEL_RESPONSE},
        {"before-tool", ZEDA_HOOK_BEFORE_TOOL_CALL},
        {"after-result", ZEDA_HOOK_AFTER_TOOL_RESULT},
        {"session-write", ZEDA_HOOK_SESSION_WRITE},
        {"turn-end", ZEDA_HOOK_AGENT_TURN_END},
    };
    for (const auto &item : hooks) {
      const ZedaHookV2 hook{
          sizeof(ZedaHookV2), view(item.name), item.kind, 0, plugin,
          execute_hook};
      if (host->register_hook(host->context, &hook, error) != 0)
        return 1;
    }
    return 0;
  } catch (const std::exception &exception) {
    return fail(error, exception.what());
  }
}

void shutdown_plugin(void *instance) {
  auto *plugin = static_cast<RuntimeHookPlugin *>(instance);
  if (plugin == nullptr)
    return;
  std::error_code ignored;
  static_cast<void>(
      std::filesystem::remove(plugin->workspace / kReadyFile, ignored));
}

void destroy_plugin(void *instance) {
  delete static_cast<RuntimeHookPlugin *>(instance);
}

const ZedaPluginDescriptorV2 kDescriptor{
    ZEDA_PLUGIN_ABI_VERSION_V2,
    sizeof(ZedaPluginDescriptorV2),
    view(kPluginId),
    view(kPluginName),
    view(kPluginVersion),
    create_plugin,
    initialize_plugin,
    shutdown_plugin,
    destroy_plugin,
};

} // namespace

extern "C" const ZedaPluginDescriptorV2 *zeda_plugin_entry_v2() {
  return &kDescriptor;
}
