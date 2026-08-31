#ifndef ZEDA_PLUGIN_SDK_H
#define ZEDA_PLUGIN_SDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEDA_PLUGIN_ABI_VERSION 1u
#define ZEDA_PLUGIN_ENTRY_SYMBOL "zeda_plugin_entry_v1"

typedef struct ZedaStringView {
  const char *data;
  size_t size;
} ZedaStringView;

typedef struct ZedaTextSinkV1 {
  void *context;
  /* Host-provided sinks may truncate after their configured byte budget. */
  int (*write)(void *context, ZedaStringView text);
} ZedaTextSinkV1;

typedef struct ZedaCancellationV1 {
  void *context;
  /* Long-running callbacks must poll this function and return promptly. */
  int (*is_cancelled)(void *context);
} ZedaCancellationV1;

typedef enum ZedaEventKindV1 {
  ZEDA_EVENT_STATUS = 1,
  ZEDA_EVENT_DELTA = 2,
  ZEDA_EVENT_ERROR = 3,
} ZedaEventKindV1;

typedef void (*ZedaEventCallbackV1)(void *context, uint32_t kind,
                                    ZedaStringView text);

typedef struct ZedaCommandV1 {
  ZedaStringView name;
  ZedaStringView description;
  ZedaStringView options_json;
  void *context;
  int (*execute)(void *context, ZedaStringView arguments,
                 ZedaCancellationV1 cancellation, ZedaEventCallbackV1 on_event,
                 void *event_context, ZedaTextSinkV1 output,
                 ZedaTextSinkV1 error);
} ZedaCommandV1;

typedef struct ZedaToolV1 {
  ZedaStringView name;
  ZedaStringView description;
  ZedaStringView input_schema_json;
  void *context;
  int (*execute)(void *context, ZedaStringView arguments,
                 ZedaCancellationV1 cancellation, ZedaTextSinkV1 output,
                 ZedaTextSinkV1 error);
} ZedaToolV1;

typedef struct ZedaHostApiV1 {
  uint32_t abi_version;
  void *context;
  ZedaStringView workspace_root;
  ZedaStringView resource_root;

  int (*register_command)(void *context, const ZedaCommandV1 *command,
                          ZedaTextSinkV1 error);
  int (*register_tool)(void *context, const ZedaToolV1 *tool,
                       ZedaTextSinkV1 error);

  int (*complete)(void *context, ZedaStringView system_prompt,
                  ZedaStringView user_prompt, size_t max_output_tokens,
                  uint32_t reasoning_effort, ZedaCancellationV1 cancellation,
                  ZedaEventCallbackV1 on_event, void *event_context,
                  ZedaTextSinkV1 output, ZedaTextSinkV1 error);

  int (*clangd_query)(void *context, ZedaStringView operation,
                      ZedaStringView relative_path, size_t line,
                      size_t character, ZedaCancellationV1 cancellation,
                      ZedaTextSinkV1 output, ZedaTextSinkV1 error);
} ZedaHostApiV1;

typedef struct ZedaPluginDescriptorV1 {
  uint32_t abi_version;
  ZedaStringView id;
  ZedaStringView name;
  ZedaStringView version;
  void *(*create)(void);
  /* Commands and tools may only be registered before initialize returns. */
  int (*initialize)(void *instance, const ZedaHostApiV1 *host,
                    ZedaTextSinkV1 error);
  void (*shutdown)(void *instance);
  void (*destroy)(void *instance);
} ZedaPluginDescriptorV1;

typedef const ZedaPluginDescriptorV1 *(*ZedaPluginEntryV1)(void);

#ifdef __cplusplus
}
#endif

#endif
