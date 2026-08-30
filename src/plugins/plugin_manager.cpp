#include "zed/plugins/plugin_manager.hpp"

#include "zed/plugins/plugin_sdk.h"

#include <algorithm>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

namespace zed::plugins {

namespace {

using Json = nlohmann::json;
using core::ErrorCode;

std::string copy_string(ZedaStringView value) {
  if (value.data == nullptr || value.size == 0)
    return {};
  return {value.data, value.size};
}

ZedaStringView string_view(std::string_view value) {
  return {value.data(), value.size()};
}

int append_text(void *context, ZedaStringView value) {
  if (context == nullptr || (value.data == nullptr && value.size != 0))
    return 1;
  static_cast<std::string *>(context)->append(value.data, value.size);
  return 0;
}

ZedaTextSinkV1 text_sink(std::string &target) { return {&target, append_text}; }

int cancellation_requested(void *context) {
  if (context == nullptr)
    return 0;
  return static_cast<const core::CancellationToken *>(context)->is_cancelled()
             ? 1
             : 0;
}

ZedaCancellationV1 cancellation_view(const core::CancellationToken &token) {
  return {const_cast<core::CancellationToken *>(&token),
          cancellation_requested};
}

core::CancellationToken native_cancellation(ZedaCancellationV1 cancellation) {
  if (cancellation.is_cancelled == cancellation_requested &&
      cancellation.context != nullptr) {
    return *static_cast<core::CancellationToken *>(cancellation.context);
  }
  return {};
}

std::filesystem::path executable_path() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    return {};
  buffer.resize(std::char_traits<char>::length(buffer.c_str()));
  return std::filesystem::weakly_canonical(buffer);
#elif defined(__linux__)
  std::string buffer(4096, '\0');
  const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
  if (length <= 0)
    return {};
  buffer.resize(static_cast<std::size_t>(length));
  return std::filesystem::weakly_canonical(buffer);
#else
  return {};
#endif
}

bool path_is_inside(const std::filesystem::path &root,
                    const std::filesystem::path &path) {
  const auto relative = path.lexically_relative(root);
  return !relative.empty() && *relative.begin() != "..";
}

std::vector<std::filesystem::path> environment_paths(const char *name) {
  const char *raw = std::getenv(name);
  if (raw == nullptr || *raw == '\0')
    return {};
  std::vector<std::filesystem::path> paths;
  std::string_view value(raw);
  while (!value.empty()) {
    const auto separator = value.find(':');
    const auto part = value.substr(0, separator);
    if (!part.empty())
      paths.emplace_back(part);
    if (separator == std::string_view::npos)
      break;
    value.remove_prefix(separator + 1);
  }
  return paths;
}

struct StagedCommand {
  std::string name;
  std::string description;
  std::string options_json;
  void *context{};
  decltype(ZedaCommandV1::execute) execute{};
};

struct StagedTool {
  std::string name;
  std::string description;
  std::string input_schema_json;
  void *context{};
  decltype(ZedaToolV1::execute) execute{};
};

class PluginTool final : public core::Tool {
public:
  explicit PluginTool(StagedTool tool)
      : callback_(std::move(tool)),
        definition_{callback_.name, callback_.description,
                    callback_.input_schema_json} {}

  [[nodiscard]] const core::ToolDefinition &definition() const override {
    return definition_;
  }

  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override {
    std::string output;
    std::string error;
    try {
      const int status = callback_.execute(
          callback_.context, string_view(call.arguments_json),
          cancellation_view(cancellation), text_sink(output), text_sink(error));
      if (status != 0) {
        return core::Result<core::ToolResult>::failure(
            {ErrorCode::tool_error,
             error.empty() ? "plugin tool failed: " + callback_.name
                           : std::move(error)});
      }
      return core::Result<core::ToolResult>::success(
          {call.id, std::move(output), false});
    } catch (const std::exception &exception) {
      return core::Result<core::ToolResult>::failure(
          {ErrorCode::tool_error,
           "plugin tool threw an exception: " + callback_.name + ": " +
               exception.what()});
    } catch (...) {
      return core::Result<core::ToolResult>::failure(
          {ErrorCode::tool_error,
           "plugin tool threw an unknown exception: " + callback_.name});
    }
  }

private:
  StagedTool callback_;
  core::ToolDefinition definition_;
};

struct Manifest {
  std::string id;
  std::string name;
  std::string version;
  std::uint32_t abi_version{};
  std::filesystem::path manifest_path;
  std::filesystem::path library_path;
  std::filesystem::path resource_path;
};

core::Result<Manifest> read_manifest(const std::filesystem::path &path) {
  std::ifstream input(path);
  if (!input) {
    return core::Result<Manifest>::failure(
        {ErrorCode::not_found,
         "cannot open plugin manifest: " + path.string()});
  }
  try {
    const auto document = Json::parse(input);
    if (!document.is_object())
      throw std::runtime_error("root must be an object");
    Manifest result;
    result.id = document.at("id").get<std::string>();
    result.name = document.at("name").get<std::string>();
    result.version = document.at("version").get<std::string>();
    result.abi_version = document.at("abi_version").get<std::uint32_t>();
    result.manifest_path = std::filesystem::weakly_canonical(path);
    const auto root = result.manifest_path.parent_path();
    result.library_path = std::filesystem::weakly_canonical(
        root / document.at("library").get<std::string>());
    result.resource_path = std::filesystem::weakly_canonical(
        root / document.value("resources", std::string("resources")));
    if (result.id.empty() || result.name.empty() || result.version.empty())
      throw std::runtime_error("id, name, and version must be non-empty");
    if (!path_is_inside(root, result.library_path) ||
        !path_is_inside(root, result.resource_path)) {
      throw std::runtime_error("library or resource path escapes plugin root");
    }
    return core::Result<Manifest>::success(std::move(result));
  } catch (const std::exception &exception) {
    return core::Result<Manifest>::failure(
        {ErrorCode::invalid_argument,
         "invalid plugin manifest " + path.string() + ": " + exception.what()});
  }
}

std::vector<std::filesystem::path>
manifest_files(const std::vector<std::filesystem::path> &roots) {
  std::set<std::filesystem::path> result;
  for (const auto &configured_root : roots) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(configured_root, error);
    if (error || !std::filesystem::is_directory(root, error))
      continue;
    const auto direct = root / "zeda-plugin.json";
    if (std::filesystem::is_regular_file(direct, error) && !error)
      result.insert(direct);
    error.clear();
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_directory(error)) {
        error.clear();
        continue;
      }
      const auto candidate = iterator->path() / "zeda-plugin.json";
      if (std::filesystem::is_regular_file(candidate, error) && !error)
        result.insert(candidate);
      error.clear();
    }
  }
  return {result.begin(), result.end()};
}

} // namespace

std::vector<std::filesystem::path> default_plugin_search_paths() {
  std::vector<std::filesystem::path> result;
  const auto executable = executable_path();
  if (!executable.empty()) {
    result.push_back(executable.parent_path() / "plugins");
    result.push_back(executable.parent_path().parent_path() / "lib" / "zeda" /
                     "plugins");
  }
  auto configured = environment_paths("ZED_PLUGIN_PATH");
  result.insert(result.end(), configured.begin(), configured.end());
  return result;
}

class PluginManager::Impl {
public:
  struct HostContext {
    Impl *manager{};
    std::filesystem::path resource_root;
    std::string workspace_text;
    std::string resource_text;
    std::vector<StagedCommand> commands;
    std::vector<StagedTool> tools;
  };

  struct LoadedPlugin {
    Manifest manifest;
    void *library{};
    const ZedaPluginDescriptorV1 *descriptor{};
    void *instance{};
    HostContext host_context;
    ZedaHostApiV1 host_api{};
    bool initialized{false};
  };

  Impl(PluginManagerConfig config, extensions::ExtensionRegistry &extensions,
       core::ToolRegistry &tools, core::Model &model, lsp::ClangdClient &clangd)
      : config_(std::move(config)), extensions_(extensions), tools_(tools),
        model_(model), clangd_(clangd) {}

  ~Impl() {
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      auto &plugin = **iterator;
      if (plugin.initialized && plugin.descriptor->shutdown != nullptr) {
        try {
          plugin.descriptor->shutdown(plugin.instance);
        } catch (...) {
        }
      }
      if (plugin.descriptor != nullptr &&
          plugin.descriptor->destroy != nullptr) {
        try {
          plugin.descriptor->destroy(plugin.instance);
        } catch (...) {
        }
      }
      if (plugin.library != nullptr)
        dlclose(plugin.library);
    }
  }

  core::Result<void> discover_and_load() {
    statuses_.clear();
    for (const auto &path : manifest_files(config_.search_paths))
      load(path);
    return core::Result<void>::success();
  }

  void load(const std::filesystem::path &manifest_path) {
    PluginStatus status;
    status.manifest_path = manifest_path;
    const auto manifest = read_manifest(manifest_path);
    if (!manifest) {
      status.detail = manifest.error().message;
      statuses_.push_back(std::move(status));
      return;
    }
    status.id = manifest.value().id;
    status.name = manifest.value().name;
    status.version = manifest.value().version;
    if (manifest.value().abi_version != ZEDA_PLUGIN_ABI_VERSION) {
      status.detail = "plugin ABI mismatch";
      statuses_.push_back(std::move(status));
      return;
    }

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->manifest = manifest.value();
    plugin->library =
        dlopen(plugin->manifest.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (plugin->library == nullptr) {
      const char *load_error = dlerror();
      status.detail =
          "cannot load plugin library: " +
          std::string(load_error == nullptr ? "unknown error" : load_error);
      statuses_.push_back(std::move(status));
      return;
    }
    const auto entry = reinterpret_cast<ZedaPluginEntryV1>(
        dlsym(plugin->library, ZEDA_PLUGIN_ENTRY_SYMBOL));
    if (entry == nullptr) {
      status.detail = "plugin entry symbol is missing";
      dlclose(plugin->library);
      statuses_.push_back(std::move(status));
      return;
    }
    try {
      plugin->descriptor = entry();
    } catch (...) {
      plugin->descriptor = nullptr;
    }
    if (plugin->descriptor == nullptr ||
        plugin->descriptor->abi_version != ZEDA_PLUGIN_ABI_VERSION ||
        copy_string(plugin->descriptor->id) != plugin->manifest.id ||
        copy_string(plugin->descriptor->version) != plugin->manifest.version) {
      status.detail = "plugin descriptor does not match manifest";
      dlclose(plugin->library);
      statuses_.push_back(std::move(status));
      return;
    }
    if (plugin->descriptor->create == nullptr ||
        plugin->descriptor->initialize == nullptr ||
        plugin->descriptor->destroy == nullptr) {
      status.detail = "plugin lifecycle is incomplete";
      dlclose(plugin->library);
      statuses_.push_back(std::move(status));
      return;
    }

    plugin->host_context.manager = this;
    plugin->host_context.resource_root = plugin->manifest.resource_path;
    plugin->host_context.workspace_text = config_.workspace_root.string();
    plugin->host_context.resource_text =
        plugin->manifest.resource_path.string();
    plugin->host_api = {
        ZEDA_PLUGIN_ABI_VERSION,
        &plugin->host_context,
        string_view(plugin->host_context.workspace_text),
        string_view(plugin->host_context.resource_text),
        register_command,
        register_tool,
        complete,
        clangd_query,
    };

    std::string error;
    try {
      plugin->instance = plugin->descriptor->create();
      if (plugin->instance == nullptr ||
          plugin->descriptor->initialize(plugin->instance, &plugin->host_api,
                                         text_sink(error)) != 0) {
        throw std::runtime_error(error.empty() ? "plugin initialization failed"
                                               : error);
      }
    } catch (const std::exception &exception) {
      status.detail = exception.what();
      if (plugin->instance != nullptr)
        plugin->descriptor->destroy(plugin->instance);
      dlclose(plugin->library);
      statuses_.push_back(std::move(status));
      return;
    } catch (...) {
      status.detail = "plugin initialization threw an unknown exception";
      if (plugin->instance != nullptr)
        plugin->descriptor->destroy(plugin->instance);
      dlclose(plugin->library);
      statuses_.push_back(std::move(status));
      return;
    }

    const auto validation = validate_staged(plugin->host_context);
    if (!validation) {
      status.detail = validation.error().message;
      plugin->descriptor->shutdown(plugin->instance);
      plugin->descriptor->destroy(plugin->instance);
      dlclose(plugin->library);
      statuses_.push_back(std::move(status));
      return;
    }
    for (const auto &command : plugin->host_context.commands) {
      std::vector<extensions::CommandOption> options;
      if (!command.options_json.empty()) {
        const auto document = Json::parse(command.options_json, nullptr, false);
        if (document.is_array()) {
          for (const auto &option : document) {
            options.push_back(
                {option.value("value", ""), option.value("description", "")});
          }
        }
      }
      const auto callback = command;
      const auto registered = extensions_.register_command({
          callback.name,
          callback.description,
          [callback](std::string_view arguments) {
            std::string output;
            std::string error;
            core::CancellationToken cancellation;
            const int result =
                callback.execute(callback.context, string_view(arguments),
                                 cancellation_view(cancellation), nullptr,
                                 nullptr, text_sink(output), text_sink(error));
            if (result != 0) {
              return core::Result<std::string>::failure(
                  {ErrorCode::tool_error,
                   error.empty() ? "plugin command failed: " + callback.name
                                 : std::move(error)});
            }
            return core::Result<std::string>::success(std::move(output));
          },
          std::move(options),
          [callback](std::string_view arguments,
                     core::CancellationToken cancellation,
                     core::AgentEventCallback on_event) {
            std::string output;
            std::string error;
            const auto bridge = [](void *context, std::uint32_t kind,
                                   ZedaStringView text) {
              auto *target = static_cast<core::AgentEventCallback *>(context);
              if (target == nullptr || !*target)
                return;
              const auto message = copy_string(text);
              if (kind == ZEDA_EVENT_DELTA) {
                (*target)({core::AgentEventType::assistant_delta, message});
              } else if (kind == ZEDA_EVENT_ERROR) {
                (*target)({core::AgentEventType::error, message});
              } else {
                (*target)({core::AgentEventType::tool_result, message});
              }
            };
            const int result = callback.execute(
                callback.context, string_view(arguments),
                cancellation_view(cancellation), bridge, &on_event,
                text_sink(output), text_sink(error));
            if (result != 0) {
              return core::Result<std::string>::failure(
                  {ErrorCode::tool_error,
                   error.empty() ? "plugin command failed: " + callback.name
                                 : std::move(error)});
            }
            return core::Result<std::string>::success(std::move(output));
          },
      });
      if (!registered) {
        status.detail = registered.error().message;
        statuses_.push_back(std::move(status));
        return;
      }
    }
    for (const auto &tool : plugin->host_context.tools) {
      const auto registered =
          tools_.register_tool(std::make_unique<PluginTool>(tool));
      if (!registered) {
        status.detail = registered.error().message;
        statuses_.push_back(std::move(status));
        return;
      }
    }
    plugin->initialized = true;
    status.loaded = true;
    status.detail = "loaded";
    loaded_.push_back(std::move(plugin));
    statuses_.push_back(std::move(status));
  }

  core::Result<void> validate_staged(const HostContext &host) const {
    std::set<std::string> command_names;
    for (const auto &existing : extensions_.commands())
      command_names.insert(existing.name);
    for (const auto &command : host.commands) {
      if (command.name.empty() || command.execute == nullptr)
        return core::Result<void>::failure(
            {ErrorCode::invalid_argument, "plugin command is incomplete"});
      if (!command_names.insert(command.name).second)
        return core::Result<void>::failure(
            {ErrorCode::conflict, "plugin command conflict: " + command.name});
      if (!command.options_json.empty()) {
        const auto parsed = Json::parse(command.options_json, nullptr, false);
        if (!parsed.is_array())
          return core::Result<void>::failure(
              {ErrorCode::invalid_argument,
               "plugin command options must be a JSON array: " + command.name});
      }
    }
    std::set<std::string> tool_names;
    for (const auto &definition : tools_.definitions())
      tool_names.insert(definition.name);
    for (const auto &tool : host.tools) {
      if (tool.name.empty() || tool.execute == nullptr)
        return core::Result<void>::failure(
            {ErrorCode::invalid_argument, "plugin tool is incomplete"});
      if (!tool_names.insert(tool.name).second)
        return core::Result<void>::failure(
            {ErrorCode::conflict, "plugin tool conflict: " + tool.name});
      const auto schema = Json::parse(tool.input_schema_json, nullptr, false);
      if (!schema.is_object())
        return core::Result<void>::failure(
            {ErrorCode::invalid_argument,
             "plugin tool schema is invalid: " + tool.name});
    }
    return core::Result<void>::success();
  }

  static int register_command(void *context, const ZedaCommandV1 *command,
                              ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || command == nullptr || command->execute == nullptr) {
      error.write(error.context, string_view("invalid plugin command"));
      return 1;
    }
    host->commands.push_back({copy_string(command->name),
                              copy_string(command->description),
                              copy_string(command->options_json),
                              command->context, command->execute});
    return 0;
  }

  static int register_tool(void *context, const ZedaToolV1 *tool,
                           ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || tool == nullptr || tool->execute == nullptr) {
      error.write(error.context, string_view("invalid plugin tool"));
      return 1;
    }
    host->tools.push_back(
        {copy_string(tool->name), copy_string(tool->description),
         copy_string(tool->input_schema_json), tool->context, tool->execute});
    return 0;
  }

  static int complete(void *context, ZedaStringView system_prompt,
                      ZedaStringView user_prompt, size_t max_output_tokens,
                      uint32_t reasoning_effort,
                      ZedaCancellationV1 cancellation,
                      ZedaEventCallbackV1 on_event, void *event_context,
                      ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || host->manager == nullptr)
      return 1;
    auto &manager = *host->manager;
    core::ModelRequest request;
    request.model = manager.config_.model;
    request.temperature = 0.0;
    request.reasoning_effort =
        reasoning_effort <=
                static_cast<uint32_t>(core::ReasoningEffort::thinking)
            ? static_cast<core::ReasoningEffort>(reasoning_effort)
            : manager.config_.reasoning_effort;
    if (max_output_tokens != 0)
      request.max_output_tokens = max_output_tokens;
    request.messages = {
        {"plugin-system",
         core::Role::system,
         copy_string(system_prompt),
         {},
         std::nullopt,
         false},
        {"plugin-user",
         core::Role::user,
         copy_string(user_prompt),
         {},
         std::nullopt,
         false},
    };
    const auto response = manager.model_.complete(
        request,
        [&](const core::ModelDelta &delta) {
          if (on_event != nullptr)
            on_event(event_context, ZEDA_EVENT_DELTA, string_view(delta.text));
        },
        native_cancellation(cancellation));
    if (!response) {
      error.write(error.context, string_view(response.error().message));
      return 1;
    }
    if (response.value().finish_reason != core::FinishReason::stop ||
        !response.value().tool_calls.empty()) {
      std::string detail = "plugin model response did not complete cleanly";
      switch (response.value().finish_reason) {
      case core::FinishReason::length:
        detail += ": output token limit reached";
        break;
      case core::FinishReason::content_filter:
        detail += ": content filter";
        break;
      case core::FinishReason::cancelled:
        detail += ": cancelled";
        break;
      case core::FinishReason::tool_calls:
        detail += ": unexpected tool call";
        break;
      case core::FinishReason::unknown:
        detail += ": unknown finish reason";
        break;
      case core::FinishReason::stop:
        detail += ": unexpected tool call";
        break;
      }
      error.write(error.context, string_view(detail));
      return 1;
    }
    output.write(output.context, string_view(response.value().content));
    return 0;
  }

  static int clangd_query(void *context, ZedaStringView operation,
                          ZedaStringView relative_path, size_t line,
                          size_t character, ZedaCancellationV1 cancellation,
                          ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || host->manager == nullptr)
      return 1;
    const auto operation_name = copy_string(operation);
    lsp::QueryOperation query;
    if (operation_name == "hover")
      query = lsp::QueryOperation::hover;
    else if (operation_name == "definition")
      query = lsp::QueryOperation::definition;
    else if (operation_name == "references")
      query = lsp::QueryOperation::references;
    else if (operation_name == "document_symbols")
      query = lsp::QueryOperation::document_symbols;
    else {
      error.write(error.context, string_view("unsupported clangd operation"));
      return 1;
    }
    const auto result = host->manager->clangd_.query(
        query, copy_string(relative_path), line, character,
        native_cancellation(cancellation));
    if (!result) {
      error.write(error.context, string_view(result.error().message));
      return 1;
    }
    output.write(output.context, string_view(result.value()));
    return 0;
  }

  PluginManagerConfig config_;
  extensions::ExtensionRegistry &extensions_;
  core::ToolRegistry &tools_;
  core::Model &model_;
  lsp::ClangdClient &clangd_;
  std::vector<std::unique_ptr<LoadedPlugin>> loaded_;
  std::vector<PluginStatus> statuses_;
};

PluginManager::PluginManager(PluginManagerConfig config,
                             extensions::ExtensionRegistry &extensions,
                             core::ToolRegistry &tools, core::Model &model,
                             lsp::ClangdClient &clangd)
    : impl_(std::make_unique<Impl>(std::move(config), extensions, tools, model,
                                   clangd)) {}

PluginManager::~PluginManager() = default;

core::Result<void> PluginManager::discover_and_load() {
  return impl_->discover_and_load();
}

const std::vector<PluginStatus> &PluginManager::statuses() const {
  return impl_->statuses_;
}

std::string PluginManager::status_report() const {
  if (impl_->statuses_.empty())
    return "no plugins discovered\n";
  std::string result;
  for (const auto &status : impl_->statuses_) {
    result += status.loaded ? "loaded  " : "failed  ";
    result += status.name.empty() ? status.id : status.name;
    if (!status.version.empty())
      result += " " + status.version;
    result += " — " + status.detail + "\n";
    result += "        " + status.manifest_path.string() + "\n";
  }
  return result;
}

} // namespace zed::plugins
