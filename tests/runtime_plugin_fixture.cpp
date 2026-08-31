#include "zed/plugins/plugin_sdk.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

#if defined(ZEDA_RUNTIME_FIXTURE_CONSUMER)
constexpr std::string_view kPluginId = "runtime.consumer";
constexpr std::string_view kPluginName = "Runtime consumer fixture";
constexpr std::string_view kCommandName = "runtime_consumer_command";
constexpr std::string_view kToolName = "runtime_consumer_tool";
constexpr std::string_view kReadyFile = "runtime-consumer.ready";
constexpr std::string_view kProviderReadyFile = "runtime-provider.ready";
#else
constexpr std::string_view kPluginId = "runtime.provider";
constexpr std::string_view kPluginName = "Runtime provider fixture";
constexpr std::string_view kCommandName = "runtime_provider_command";
constexpr std::string_view kToolName = "runtime_provider_tool";
constexpr std::string_view kReadyFile = "runtime-provider.ready";
#endif

constexpr std::string_view kPluginVersion = "1.0.0";
constexpr std::string_view kLifecycleFile = "runtime-plugin-lifecycle.log";
constexpr std::string_view kToolStartedFile = "runtime-tool-started.ready";
constexpr std::string_view kToolCancelledFile = "runtime-tool-cancelled.ready";

struct RuntimePlugin {
  std::filesystem::path workspace;
};

ZedaStringView view(std::string_view value) {
  return {value.data(), value.size()};
}

std::string copy(ZedaStringView value) {
  if (value.data == nullptr)
    return {};
  return {value.data, value.size};
}

bool write(ZedaTextSinkV1 sink, std::string_view value) {
  return sink.write != nullptr && sink.write(sink.context, view(value)) == 0;
}

int fail(ZedaTextSinkV1 error, std::string_view message) {
  static_cast<void>(write(error, message));
  return 1;
}

bool write_file(const std::filesystem::path &path, std::string_view contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return static_cast<bool>(output);
}

void append_lifecycle(const RuntimePlugin &plugin, std::string_view event) {
  std::ofstream output(plugin.workspace / kLifecycleFile,
                       std::ios::binary | std::ios::app);
  output << kPluginId << ':' << event << '\n';
}

bool is_cancelled(ZedaCancellationV1 cancellation) {
  return cancellation.is_cancelled != nullptr &&
         cancellation.is_cancelled(cancellation.context) != 0;
}

int execute_command(void *context, ZedaStringView arguments,
                    ZedaCancellationV1 cancellation,
                    ZedaEventCallbackV1 on_event, void *event_context,
                    ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
  try {
    if (context == nullptr)
      return fail(error, "runtime command has no plugin context");
    if (is_cancelled(cancellation))
      return fail(error, "runtime command cancelled");

    const auto argument_text = copy(arguments);
    if (on_event != nullptr)
      on_event(event_context, ZEDA_EVENT_STATUS, view("runtime command"));
    if (argument_text == "events") {
      const std::string event(32, 'E');
      for (int index = 0; index < 4; ++index) {
        if (on_event != nullptr)
          on_event(event_context, ZEDA_EVENT_STATUS, view(event));
      }
    }
    if (argument_text == "large") {
      const std::string large_output(4096, 'C');
      return write(output, large_output)
                 ? 0
                 : fail(error, "runtime command output was rejected");
    }

    const auto response = std::string(kCommandName) + ":" + argument_text;
    return write(output, response)
               ? 0
               : fail(error, "runtime command output was rejected");
  } catch (const std::exception &exception) {
    return fail(error,
                std::string("runtime command failed: ") + exception.what());
  } catch (...) {
    return fail(error, "runtime command failed with an unknown error");
  }
}

int execute_tool(void *context, ZedaStringView arguments,
                 ZedaCancellationV1 cancellation, ZedaTextSinkV1 output,
                 ZedaTextSinkV1 error) {
  try {
    auto *plugin = static_cast<RuntimePlugin *>(context);
    if (plugin == nullptr)
      return fail(error, "runtime tool has no plugin context");

    const auto document = nlohmann::json::parse(copy(arguments));
    const auto mode = document.value("mode", std::string("echo"));
    if (mode == "large") {
      const std::string large_output(4096, 'T');
      return write(output, large_output)
                 ? 0
                 : fail(error, "runtime tool output was rejected");
    }
    if (mode == "wait") {
      if (!write_file(plugin->workspace / kToolStartedFile, "started\n"))
        return fail(error, "runtime tool could not write its start sentinel");

      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (std::chrono::steady_clock::now() < deadline) {
        if (is_cancelled(cancellation)) {
          static_cast<void>(write_file(plugin->workspace / kToolCancelledFile,
                                       "cancelled\n"));
          return fail(error, "runtime tool observed cancellation");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return fail(error, "runtime tool timed out waiting for cancellation");
    }

    const nlohmann::json response = {
        {"plugin", std::string(kPluginId)},
        {"mode", mode},
    };
    const auto text = response.dump();
    return write(output, text)
               ? 0
               : fail(error, "runtime tool output was rejected");
  } catch (const std::exception &exception) {
    return fail(error, std::string("runtime tool failed: ") + exception.what());
  } catch (...) {
    return fail(error, "runtime tool failed with an unknown error");
  }
}

void *create_plugin() { return new (std::nothrow) RuntimePlugin; }

int initialize_plugin(void *instance, const ZedaHostApiV1 *host,
                      ZedaTextSinkV1 error) {
  try {
    auto *plugin = static_cast<RuntimePlugin *>(instance);
    if (plugin == nullptr || host == nullptr ||
        host->abi_version != ZEDA_PLUGIN_ABI_VERSION) {
      return fail(error, "runtime plugin received an invalid host");
    }

    plugin->workspace = copy(host->workspace_root);
#if defined(ZEDA_RUNTIME_FIXTURE_CONSUMER)
    if (!std::filesystem::is_regular_file(plugin->workspace /
                                          kProviderReadyFile)) {
      return fail(error, "provider sentinel was not ready before consumer");
    }
    constexpr std::string_view kReadyContents = "provider-ready-observed\n";
#else
    constexpr std::string_view kReadyContents = "ready\n";
#endif
    if (!write_file(plugin->workspace / kReadyFile, kReadyContents))
      return fail(error, "runtime plugin could not write its ready sentinel");
    append_lifecycle(*plugin, "initialize");

    const ZedaCommandV1 command{
        view(kCommandName), view("Runtime C ABI command fixture"),
        view("[]"),         plugin,
        execute_command,
    };
    if (host->register_command == nullptr ||
        host->register_command(host->context, &command, error) != 0) {
      return 1;
    }

    const ZedaToolV1 tool{
        view(kToolName),
        view("Runtime C ABI tool fixture"),
        view(R"({"type":"object","properties":{"mode":{"type":"string"}}})"),
        plugin,
        execute_tool,
    };
    if (host->register_tool == nullptr ||
        host->register_tool(host->context, &tool, error) != 0) {
      return 1;
    }
    return 0;
  } catch (const std::exception &exception) {
    return fail(error, std::string("runtime initialization failed: ") +
                           exception.what());
  } catch (...) {
    return fail(error, "runtime initialization failed with an unknown error");
  }
}

void shutdown_plugin(void *instance) {
  auto *plugin = static_cast<RuntimePlugin *>(instance);
  if (plugin == nullptr)
    return;
  append_lifecycle(*plugin, "shutdown");
  std::error_code ignored;
  static_cast<void>(
      std::filesystem::remove(plugin->workspace / kReadyFile, ignored));
}

void destroy_plugin(void *instance) {
  delete static_cast<RuntimePlugin *>(instance);
}

const ZedaPluginDescriptorV1 kDescriptor{
    ZEDA_PLUGIN_ABI_VERSION, view(kPluginId), view(kPluginName),
    view(kPluginVersion),    create_plugin,   initialize_plugin,
    shutdown_plugin,         destroy_plugin,
};

} // namespace

extern "C" const ZedaPluginDescriptorV1 *zeda_plugin_entry_v1() {
  return &kDescriptor;
}
