#include "zed/plugins/plugin_sdk.h"

#include <cstring>

namespace {

ZedaStringView view(const char *text) { return {text, std::strlen(text)}; }

int execute_command(void *, ZedaStringView, ZedaCancellationV1,
                    ZedaEventCallbackV1, void *, ZedaTextSinkV1 output,
                    ZedaTextSinkV1) {
  return output.write(output.context, view("unexpected execution"));
}

int execute_tool(void *, ZedaStringView, ZedaCancellationV1, ZedaTextSinkV1,
                 ZedaTextSinkV1) {
  return 0;
}

void *create_plugin() { return new int(1); }

int initialize_plugin(void *, const ZedaHostApiV1 *host, ZedaTextSinkV1 error) {
  const ZedaCommandV1 command{view("faulty_command"),
                              view("must not survive failed loading"),
                              {},
                              nullptr,
                              execute_command};
  if (host->register_command(host->context, &command, error) != 0)
    return 1;

  const ZedaToolV1 tool{
      view("faulty_tool"), view("has a deliberately invalid schema"),
      view(R"({"type":"object","properties":[]})"), nullptr, execute_tool};
  return host->register_tool(host->context, &tool, error);
}

void shutdown_plugin(void *) {}

void destroy_plugin(void *instance) { delete static_cast<int *>(instance); }

const ZedaPluginDescriptorV1 kDescriptor{
    ZEDA_PLUGIN_ABI_VERSION, view("faulty"),
    view("Faulty fixture"),  view("1"),
    create_plugin,           initialize_plugin,
    shutdown_plugin,         destroy_plugin,
};

} // namespace

extern "C" const ZedaPluginDescriptorV1 *zeda_plugin_entry_v1() {
  return &kDescriptor;
}
