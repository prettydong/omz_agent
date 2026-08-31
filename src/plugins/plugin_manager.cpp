#include "zed/plugins/plugin_manager.hpp"

#include "zed/plugins/plugin_sdk.h"
#include "zed/support/unique_library.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#ifndef ZEDA_INSTALL_LIBDIR
#define ZEDA_INSTALL_LIBDIR "lib"
#endif

namespace zed::plugins {

namespace {

using Json = nlohmann::json;
using core::ErrorCode;

constexpr std::size_t kMaximumManifestBytes = 1024 * 1024;
constexpr std::string_view kTruncationMarker = "\n[plugin output truncated]";

std::string copy_string(ZedaStringView value) {
  if (value.data == nullptr) {
    if (value.size == 0)
      return {};
    throw std::invalid_argument("plugin string has a null data pointer");
  }
  return {value.data, value.size};
}

ZedaStringView string_view(std::string_view value) {
  return {value.data(), value.size()};
}

class BoundedText {
public:
  explicit BoundedText(std::size_t limit) : limit_(limit) {}

  int append(ZedaStringView value) noexcept {
    if (value.data == nullptr && value.size != 0)
      return 1;
    try {
      const auto available =
          text_.size() < limit_ ? limit_ - text_.size() : std::size_t{};
      const auto copied = std::min(available, value.size);
      if (copied != 0)
        text_.append(value.data, copied);
      if (copied != value.size)
        truncated_ = true;
      return 0;
    } catch (...) {
      return 1;
    }
  }

  std::string take() {
    if (truncated_ && limit_ != 0) {
      const auto marker_size = std::min(limit_, kTruncationMarker.size());
      if (text_.size() > limit_ - marker_size)
        text_.resize(limit_ - marker_size);
      text_.append(kTruncationMarker.data(), marker_size);
    }
    return std::move(text_);
  }

private:
  std::size_t limit_{};
  std::string text_;
  bool truncated_{false};
};

int append_bounded_text(void *context, ZedaStringView value) {
  if (context == nullptr)
    return 1;
  return static_cast<BoundedText *>(context)->append(value);
}

ZedaTextSinkV1 text_sink(BoundedText &target) {
  return {&target, append_bounded_text};
}

bool write_sink(ZedaTextSinkV1 sink, std::string_view value) noexcept {
  if (sink.write == nullptr)
    return false;
  try {
    return sink.write(sink.context, string_view(value)) == 0;
  } catch (...) {
    return false;
  }
}

void append_cleanup_error(std::string &errors, std::string_view prefix,
                          const char *detail = nullptr) noexcept {
  try {
    if (!errors.empty())
      errors += "; ";
    errors += prefix;
    if (detail != nullptr)
      errors += detail;
  } catch (...) {
    try {
      errors = "cleanup failed; diagnostic unavailable";
    } catch (...) {
      errors.clear();
    }
  }
}

class InvocationGate final
    : public std::enable_shared_from_this<InvocationGate> {
public:
  class Lease {
  public:
    explicit Lease(std::shared_ptr<InvocationGate> gate)
        : gate_(std::move(gate)) {}
    ~Lease() {
      if (gate_ != nullptr)
        gate_->leave();
    }

    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept
        : gate_(std::exchange(other.gate_, nullptr)) {}
    Lease &operator=(Lease &&) = delete;

  private:
    std::shared_ptr<InvocationGate> gate_;
  };

  [[nodiscard]] std::optional<Lease> enter() {
    std::scoped_lock lock(mutex_);
    if (stopping_.load(std::memory_order_relaxed))
      return std::nullopt;
    ++in_flight_;
    return std::optional<Lease>(std::in_place, shared_from_this());
  }

  void begin_quiesce() {
    std::scoped_lock lock(mutex_);
    stopping_.store(true, std::memory_order_relaxed);
    if (in_flight_ == 0)
      idle_.notify_all();
  }

  void wait_until_idle() {
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [&] { return in_flight_ == 0; });
  }

  [[nodiscard]] bool is_stopping() const {
    return stopping_.load(std::memory_order_relaxed);
  }

private:
  void leave() {
    std::scoped_lock lock(mutex_);
    if (in_flight_ != 0)
      --in_flight_;
    if (in_flight_ == 0)
      idle_.notify_all();
  }

  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::atomic_bool stopping_{false};
  std::size_t in_flight_{};
};

struct PluginCancellationContext {
  core::CancellationToken caller;
  std::shared_ptr<InvocationGate> gate;
};

int plugin_cancellation_requested(void *context) {
  if (context == nullptr)
    return 0;
  const auto &cancellation =
      *static_cast<const PluginCancellationContext *>(context);
  return cancellation.caller.is_cancelled() || cancellation.gate->is_stopping()
             ? 1
             : 0;
}

ZedaCancellationV1 cancellation_view(PluginCancellationContext &cancellation) {
  return {&cancellation, plugin_cancellation_requested};
}

core::CancellationToken
native_cancellation(ZedaCancellationV1 cancellation,
                    std::shared_ptr<InvocationGate> gate) {
  return core::CancellationToken::from_probe(
      [cancellation, gate = std::move(gate)] {
        if (gate->is_stopping())
          return true;
        if (cancellation.is_cancelled == nullptr)
          return false;
        try {
          return cancellation.is_cancelled(cancellation.context) != 0;
        } catch (...) {
          return true;
        }
      });
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

bool valid_plugin_id(std::string_view id) {
  return !id.empty() &&
         std::all_of(id.begin(), id.end(), [](unsigned char character) {
           return (character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '.' ||
                  character == '_' || character == '-';
         });
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
  PluginTool(StagedTool tool, std::shared_ptr<InvocationGate> gate,
             std::size_t max_output_bytes)
      : callback_(std::move(tool)), gate_(std::move(gate)),
        max_output_bytes_(max_output_bytes),
        definition_{callback_.name, callback_.description,
                    callback_.input_schema_json} {}

  [[nodiscard]] const core::ToolDefinition &definition() const override {
    return definition_;
  }

  core::Result<core::ToolResult>
  execute(const core::ToolCall &call,
          core::CancellationToken cancellation) override {
    auto lease = gate_->enter();
    if (!lease.has_value()) {
      return core::Result<core::ToolResult>::failure(
          {ErrorCode::cancelled, "plugin is unloading: " + callback_.name});
    }

    BoundedText output(max_output_bytes_);
    BoundedText error(max_output_bytes_);
    PluginCancellationContext combined{cancellation, gate_};
    try {
      const int status = callback_.execute(
          callback_.context, string_view(call.arguments_json),
          cancellation_view(combined), text_sink(output), text_sink(error));
      if (status != 0) {
        auto detail = error.take();
        return core::Result<core::ToolResult>::failure(
            {combined.caller.is_cancelled() || gate_->is_stopping()
                 ? ErrorCode::cancelled
                 : ErrorCode::tool_error,
             detail.empty() ? "plugin tool failed: " + callback_.name
                            : std::move(detail)});
      }
      return core::Result<core::ToolResult>::success(
          {call.id, output.take(), false});
    } catch (const std::exception &exception) {
      return core::Result<core::ToolResult>::failure(
          {combined.caller.is_cancelled() || gate_->is_stopping()
               ? ErrorCode::cancelled
               : ErrorCode::tool_error,
           "plugin tool threw an exception: " + callback_.name + ": " +
               exception.what()});
    } catch (...) {
      return core::Result<core::ToolResult>::failure(
          {combined.caller.is_cancelled() || gate_->is_stopping()
               ? ErrorCode::cancelled
               : ErrorCode::tool_error,
           "plugin tool threw an unknown exception: " + callback_.name});
    }
  }

private:
  StagedTool callback_;
  std::shared_ptr<InvocationGate> gate_;
  std::size_t max_output_bytes_{};
  core::ToolDefinition definition_;
};

struct Manifest {
  std::string id;
  std::string name;
  std::string version;
  std::uint32_t abi_version{};
  std::vector<std::string> dependencies;
  std::filesystem::path manifest_path;
  std::filesystem::path library_path;
  std::filesystem::path resource_path;
};

core::Result<Manifest> read_manifest(const std::filesystem::path &path) {
  std::error_code filesystem_error;
  const auto size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error) {
    return core::Result<Manifest>::failure(
        {ErrorCode::not_found, "cannot inspect plugin manifest " +
                                   path.string() + ": " +
                                   filesystem_error.message()});
  }
  if (size > kMaximumManifestBytes) {
    return core::Result<Manifest>::failure(
        {ErrorCode::invalid_argument,
         "plugin manifest exceeds 1048576 bytes: " + path.string()});
  }

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
    if (!valid_plugin_id(result.id))
      throw std::runtime_error("id contains unsupported characters");
    if (result.name.empty() || result.version.empty())
      throw std::runtime_error("name and version must be non-empty");

    std::set<std::string> dependency_ids;
    if (const auto iterator = document.find("requires");
        iterator != document.end()) {
      if (!iterator->is_array())
        throw std::runtime_error("requires must be an array of plugin ids");
      for (const auto &dependency : *iterator) {
        if (!dependency.is_string())
          throw std::runtime_error("requires entries must be plugin ids");
        auto dependency_id = dependency.get<std::string>();
        if (!valid_plugin_id(dependency_id))
          throw std::runtime_error("requires contains an invalid plugin id");
        if (dependency_id == result.id)
          throw std::runtime_error("a plugin cannot require itself");
        if (!dependency_ids.insert(dependency_id).second)
          throw std::runtime_error("requires contains a duplicate plugin id");
        result.dependencies.push_back(std::move(dependency_id));
      }
    }

    result.manifest_path = std::filesystem::weakly_canonical(path);
    const auto root = result.manifest_path.parent_path();
    result.library_path = std::filesystem::weakly_canonical(
        root / document.at("library").get<std::string>());
    result.resource_path = std::filesystem::weakly_canonical(
        root / document.value("resources", std::string("resources")));
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

core::Result<void>
add_manifest_candidate(const std::filesystem::path &root,
                       const std::filesystem::path &candidate,
                       std::set<std::filesystem::path> &seen,
                       std::vector<std::filesystem::path> &result) {
  std::error_code error;
  const auto exists = std::filesystem::exists(candidate, error);
  if (error) {
    return core::Result<void>::failure(
        {ErrorCode::internal, "cannot inspect plugin manifest candidate " +
                                  candidate.string() + ": " + error.message()});
  }
  if (!exists)
    return core::Result<void>::success();
  if (!std::filesystem::is_regular_file(candidate, error) || error) {
    return core::Result<void>::failure(
        {ErrorCode::invalid_argument,
         "plugin manifest candidate is not a regular file: " +
             candidate.string()});
  }
  const auto canonical = std::filesystem::canonical(candidate, error);
  if (error) {
    return core::Result<void>::failure(
        {ErrorCode::internal, "cannot resolve plugin manifest " +
                                  candidate.string() + ": " + error.message()});
  }
  if (!path_is_inside(root, canonical)) {
    return core::Result<void>::failure(
        {ErrorCode::invalid_argument,
         "plugin manifest escapes configured search root: " +
             candidate.string()});
  }
  if (seen.insert(canonical).second)
    result.push_back(canonical);
  return core::Result<void>::success();
}

core::Result<std::vector<std::filesystem::path>>
manifest_files(const std::vector<std::filesystem::path> &roots) {
  std::vector<std::filesystem::path> result;
  std::set<std::filesystem::path> seen;
  for (const auto &configured_root : roots) {
    std::error_code error;
    const auto exists = std::filesystem::exists(configured_root, error);
    if (error) {
      return core::Result<std::vector<std::filesystem::path>>::failure(
          {ErrorCode::internal, "cannot inspect plugin search path " +
                                    configured_root.string() + ": " +
                                    error.message()});
    }
    if (!exists)
      continue;
    const auto root = std::filesystem::canonical(configured_root, error);
    if (error || !std::filesystem::is_directory(root, error) || error) {
      return core::Result<std::vector<std::filesystem::path>>::failure(
          {ErrorCode::invalid_argument,
           "plugin search path is not a readable directory: " +
               configured_root.string()});
    }

    const auto direct =
        add_manifest_candidate(root, root / "zeda-plugin.json", seen, result);
    if (!direct) {
      return core::Result<std::vector<std::filesystem::path>>::failure(
          direct.error());
    }

    std::vector<std::filesystem::path> child_candidates;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
      std::error_code entry_error;
      if (!iterator->is_directory(entry_error)) {
        if (entry_error) {
          return core::Result<std::vector<std::filesystem::path>>::failure(
              {ErrorCode::internal, "cannot inspect plugin search entry " +
                                        iterator->path().string() + ": " +
                                        entry_error.message()});
        }
        continue;
      }
      child_candidates.push_back(iterator->path() / "zeda-plugin.json");
    }
    if (error) {
      return core::Result<std::vector<std::filesystem::path>>::failure(
          {ErrorCode::internal, "cannot enumerate plugin search path " +
                                    root.string() + ": " + error.message()});
    }
    std::sort(child_candidates.begin(), child_candidates.end());
    for (const auto &candidate : child_candidates) {
      const auto added = add_manifest_candidate(root, candidate, seen, result);
      if (!added) {
        return core::Result<std::vector<std::filesystem::path>>::failure(
            added.error());
      }
    }
  }
  return core::Result<std::vector<std::filesystem::path>>::success(
      std::move(result));
}

struct PluginEventBridgeContext {
  core::AgentEventCallback *callback{};
  std::size_t remaining_bytes{};
  bool truncation_reported{false};
  bool callback_failed{false};
};

void bridge_plugin_event(void *context, std::uint32_t kind,
                         ZedaStringView text) {
  auto *bridge = static_cast<PluginEventBridgeContext *>(context);
  if (bridge == nullptr || bridge->callback == nullptr || !*bridge->callback)
    return;
  try {
    if (bridge->truncation_reported || bridge->callback_failed)
      return;
    if (text.data == nullptr && text.size != 0) {
      bridge->callback_failed = true;
      return;
    }
    const auto copied = std::min(bridge->remaining_bytes, text.size);
    std::string message;
    if (copied != 0)
      message.assign(text.data, copied);
    bridge->remaining_bytes -= copied;
    if (copied != text.size) {
      message += kTruncationMarker;
      bridge->truncation_reported = true;
    }
    if (message.empty())
      return;
    if (kind == ZEDA_EVENT_DELTA) {
      (*bridge->callback)({core::AgentEventType::assistant_delta, message});
    } else if (kind == ZEDA_EVENT_ERROR) {
      (*bridge->callback)({core::AgentEventType::error, message});
    } else {
      (*bridge->callback)({core::AgentEventType::tool_result, message});
    }
  } catch (...) {
    bridge->callback_failed = true;
  }
}

core::Result<std::string> execute_plugin_command(
    const StagedCommand &callback, std::string_view arguments,
    core::CancellationToken cancellation, core::AgentEventCallback on_event,
    const std::shared_ptr<InvocationGate> &gate, std::size_t max_output_bytes) {
  auto lease = gate->enter();
  if (!lease.has_value()) {
    return core::Result<std::string>::failure(
        {ErrorCode::cancelled, "plugin is unloading: " + callback.name});
  }
  BoundedText output(max_output_bytes);
  BoundedText error(max_output_bytes);
  PluginCancellationContext combined{cancellation, gate};
  PluginEventBridgeContext event_bridge{&on_event, max_output_bytes};
  try {
    const int result = callback.execute(
        callback.context, string_view(arguments), cancellation_view(combined),
        on_event ? bridge_plugin_event : nullptr,
        on_event ? &event_bridge : nullptr, text_sink(output),
        text_sink(error));
    if (result != 0) {
      auto detail = error.take();
      return core::Result<std::string>::failure(
          {combined.caller.is_cancelled() || gate->is_stopping()
               ? ErrorCode::cancelled
               : ErrorCode::tool_error,
           detail.empty() ? "plugin command failed: " + callback.name
                          : std::move(detail)});
    }
    if (event_bridge.callback_failed) {
      return core::Result<std::string>::failure(
          {ErrorCode::internal,
           "plugin command event delivery failed: " + callback.name});
    }
    return core::Result<std::string>::success(output.take());
  } catch (const std::exception &exception) {
    return core::Result<std::string>::failure(
        {combined.caller.is_cancelled() || gate->is_stopping()
             ? ErrorCode::cancelled
             : ErrorCode::tool_error,
         "plugin command threw an exception: " + callback.name + ": " +
             exception.what()});
  } catch (...) {
    return core::Result<std::string>::failure(
        {combined.caller.is_cancelled() || gate->is_stopping()
             ? ErrorCode::cancelled
             : ErrorCode::tool_error,
         "plugin command threw an unknown exception: " + callback.name});
  }
}

class PluginContributionScope {
public:
  PluginContributionScope(extensions::ExtensionRegistry &extensions,
                          core::ToolRegistry &tools)
      : extensions_(extensions), tools_(tools) {}

  ~PluginContributionScope() { release(); }

  PluginContributionScope(const PluginContributionScope &) = delete;
  PluginContributionScope &operator=(const PluginContributionScope &) = delete;

  core::Result<void> track_command(const std::string &name) {
    try {
      effects_.push_back({Kind::command, name});
      return core::Result<void>::success();
    } catch (const std::exception &exception) {
      static_cast<void>(extensions_.unregister_command(name));
      return core::Result<void>::failure(
          {ErrorCode::internal, "cannot track plugin command registration: " +
                                    std::string(exception.what())});
    }
  }

  core::Result<void> track_tool(const std::string &name) {
    try {
      effects_.push_back({Kind::tool, name});
      return core::Result<void>::success();
    } catch (const std::exception &exception) {
      static_cast<void>(tools_.unregister_tool(name));
      return core::Result<void>::failure(
          {ErrorCode::internal, "cannot track plugin tool registration: " +
                                    std::string(exception.what())});
    }
  }

  void release() noexcept {
    for (auto iterator = effects_.rbegin(); iterator != effects_.rend();
         ++iterator) {
      try {
        if (iterator->kind == Kind::tool)
          static_cast<void>(tools_.unregister_tool(iterator->name));
        else
          static_cast<void>(extensions_.unregister_command(iterator->name));
      } catch (...) {
      }
    }
    effects_.clear();
  }

private:
  enum class Kind { command, tool };
  struct Effect {
    Kind kind;
    std::string name;
  };

  extensions::ExtensionRegistry &extensions_;
  core::ToolRegistry &tools_;
  std::vector<Effect> effects_;
};

} // namespace

std::string_view plugin_state_name(PluginState state) {
  switch (state) {
  case PluginState::discovered:
    return "discovered";
  case PluginState::loading:
    return "loading";
  case PluginState::active:
    return "active";
  case PluginState::pending:
    return "pending";
  case PluginState::shadowed:
    return "shadowed";
  case PluginState::failed:
    return "failed";
  case PluginState::unloading:
    return "unloading";
  case PluginState::disposed:
    return "disposed";
  }
  return "unknown";
}

std::vector<std::filesystem::path> default_plugin_search_paths() {
  std::vector<std::filesystem::path> result;
  const auto executable = executable_path();
  if (!executable.empty()) {
    result.push_back(executable.parent_path() / "plugins");
    result.push_back(executable.parent_path().parent_path() /
                     ZEDA_INSTALL_LIBDIR / "zeda" / "plugins");
  }
  auto configured = environment_paths("ZED_PLUGIN_PATH");
  result.insert(result.end(), configured.begin(), configured.end());
  return result;
}

class PluginManager::Impl {
public:
  struct HostContext {
    Impl *manager{};
    std::shared_ptr<InvocationGate> gate;
    std::filesystem::path resource_root;
    std::string workspace_text;
    std::string resource_text;
    std::mutex registration_mutex;
    bool accepting_registrations{true};
    std::vector<StagedCommand> commands;
    std::vector<StagedTool> tools;
  };

  struct LoadedPlugin {
    Manifest manifest;
    support::UniqueLibrary library;
    const ZedaPluginDescriptorV1 *descriptor{};
    void *instance{};
    std::shared_ptr<InvocationGate> gate{std::make_shared<InvocationGate>()};
    HostContext host_context;
    ZedaHostApiV1 host_api{};
    std::unique_ptr<PluginContributionScope> contributions;
    std::size_t status_index{};
    bool initialize_entered{false};
  };

  struct Candidate {
    Manifest manifest;
    std::size_t status_index{};
    bool processed{false};
  };

  Impl(PluginManagerConfig config, extensions::ExtensionRegistry &extensions,
       core::ToolRegistry &tools, core::Model &model, lsp::ClangdClient &clangd)
      : config_(std::move(config)), extensions_(extensions), tools_(tools),
        model_(model), clangd_(clangd) {}

  ~Impl() noexcept {
    try {
      static_cast<void>(shutdown());
    } catch (...) {
      emergency_shutdown();
    }
  }

  core::Result<void> discover_and_load() {
    if (shut_down_) {
      return core::Result<void>::failure(
          {ErrorCode::conflict, "plugin manager has already shut down"});
    }
    if (discovered_)
      return core::Result<void>::success();
    if (config_.max_output_bytes == 0) {
      return core::Result<void>::failure(
          {ErrorCode::invalid_argument,
           "plugin output byte limit must be greater than zero"});
    }

    const auto files = manifest_files(config_.search_paths);
    if (!files)
      return core::Result<void>::failure(files.error());

    statuses_.clear();
    std::vector<Candidate> candidates;
    std::unordered_map<std::string, std::size_t> winners;
    for (const auto &path : files.value()) {
      PluginStatus status;
      status.manifest_path = path;
      const auto manifest = read_manifest(path);
      if (!manifest) {
        status.state = PluginState::failed;
        status.detail = manifest.error().message;
        statuses_.push_back(std::move(status));
        continue;
      }

      status.id = manifest.value().id;
      status.name = manifest.value().name;
      status.version = manifest.value().version;
      status.dependencies = manifest.value().dependencies;
      const auto winner = winners.find(status.id);
      if (winner != winners.end()) {
        status.state = PluginState::shadowed;
        status.detail =
            "shadowed by earlier search-path entry: " +
            candidates[winner->second].manifest.manifest_path.string();
        statuses_.push_back(std::move(status));
        continue;
      }

      const auto status_index = statuses_.size();
      statuses_.push_back(std::move(status));
      winners.emplace(manifest.value().id, candidates.size());
      candidates.push_back({manifest.value(), status_index, false});
    }

    bool made_progress = true;
    while (made_progress) {
      made_progress = false;
      for (auto &candidate : candidates) {
        if (candidate.processed)
          continue;
        bool ready = true;
        for (const auto &dependency : candidate.manifest.dependencies) {
          const auto provider = winners.find(dependency);
          if (provider == winners.end() ||
              statuses_[candidates[provider->second].status_index].state !=
                  PluginState::active) {
            ready = false;
            break;
          }
        }
        if (!ready)
          continue;
        candidate.processed = true;
        load(candidate.manifest, candidate.status_index);
        made_progress = true;
      }
    }

    const auto classify_blocked_dependencies = [&] {
      bool classified_dependency = true;
      while (classified_dependency) {
        classified_dependency = false;
        for (auto &candidate : candidates) {
          auto &status = statuses_[candidate.status_index];
          if (candidate.processed || status.state == PluginState::pending)
            continue;
          for (const auto &dependency : candidate.manifest.dependencies) {
            const auto provider = winners.find(dependency);
            if (provider == winners.end()) {
              status.state = PluginState::pending;
              status.detail = "required plugin is missing: " + dependency;
              classified_dependency = true;
              break;
            }
            const auto dependency_state =
                statuses_[candidates[provider->second].status_index].state;
            if (dependency_state == PluginState::failed ||
                dependency_state == PluginState::pending) {
              status.state = PluginState::pending;
              status.detail =
                  "required plugin is unavailable: " + dependency + " (" +
                  std::string(plugin_state_name(dependency_state)) + ")";
              classified_dependency = true;
              break;
            }
          }
        }
      }
    };
    classify_blocked_dependencies();

    const auto unresolved = [&](std::size_t candidate_index) {
      const auto &candidate = candidates[candidate_index];
      return !candidate.processed &&
             statuses_[candidate.status_index].state == PluginState::discovered;
    };
    std::vector<bool> cycle_members(candidates.size(), false);
    std::function<bool(std::size_t, std::size_t, std::set<std::size_t> &)>
        reaches_candidate;
    reaches_candidate = [&](std::size_t target, std::size_t current,
                            std::set<std::size_t> &visited) {
      for (const auto &dependency : candidates[current].manifest.dependencies) {
        const auto provider = winners.find(dependency);
        if (provider == winners.end() || !unresolved(provider->second))
          continue;
        if (provider->second == target)
          return true;
        if (visited.insert(provider->second).second &&
            reaches_candidate(target, provider->second, visited)) {
          return true;
        }
      }
      return false;
    };
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (!unresolved(index))
        continue;
      std::set<std::size_t> visited{index};
      cycle_members[index] = reaches_candidate(index, index, visited);
    }

    std::function<bool(std::size_t, std::size_t, std::set<std::size_t> &)>
        reaches_cycle_member;
    reaches_cycle_member = [&](std::size_t target, std::size_t current,
                               std::set<std::size_t> &visited) {
      for (const auto &dependency : candidates[current].manifest.dependencies) {
        const auto provider = winners.find(dependency);
        if (provider == winners.end() || !cycle_members[provider->second])
          continue;
        if (provider->second == target)
          return true;
        if (visited.insert(provider->second).second &&
            reaches_cycle_member(target, provider->second, visited)) {
          return true;
        }
      }
      return false;
    };

    for (std::size_t index = 0; index < candidates.size(); ++index) {
      if (!cycle_members[index])
        continue;
      auto &candidate = candidates[index];
      auto &status = statuses_[candidate.status_index];
      status.state = PluginState::pending;
      std::ostringstream detail;
      detail << "plugin dependency cycle:";
      for (const auto &dependency : candidate.manifest.dependencies) {
        const auto provider = winners.find(dependency);
        if (provider == winners.end() || !cycle_members[provider->second])
          continue;
        std::set<std::size_t> visited{provider->second};
        if (reaches_cycle_member(index, provider->second, visited))
          detail << ' ' << dependency;
      }
      status.detail = detail.str();
    }
    classify_blocked_dependencies();
    for (auto &candidate : candidates) {
      auto &status = statuses_[candidate.status_index];
      if (candidate.processed || status.state == PluginState::pending)
        continue;
      status.state = PluginState::pending;
      status.detail = "plugin dependencies could not be resolved";
    }

    discovered_ = true;
    return core::Result<void>::success();
  }

  core::Result<void> shutdown() {
    if (shut_down_)
      return core::Result<void>::success();

    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      auto &plugin = **iterator;
      statuses_[plugin.status_index].state = PluginState::unloading;
      statuses_[plugin.status_index].detail = "quiescing";
      plugin.gate->begin_quiesce();
      close_registration_phase(plugin);
    }
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      (*iterator)->contributions.reset();
    }
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      (*iterator)->gate->wait_until_idle();
    }

    std::string cleanup_errors;
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      auto &plugin = **iterator;
      const auto cleanup = destroy_instance(plugin, true);
      auto &status = statuses_[plugin.status_index];
      status.state = PluginState::disposed;
      status.loaded = false;
      status.detail = cleanup.empty() ? "disposed" : "disposed: " + cleanup;
      if (!cleanup.empty()) {
        if (!cleanup_errors.empty())
          cleanup_errors += "; ";
        cleanup_errors += plugin.manifest.id + ": " + cleanup;
      }
    }
    loaded_.clear();
    shut_down_ = true;
    if (!cleanup_errors.empty()) {
      return core::Result<void>::failure(
          {ErrorCode::internal, "plugin cleanup failed: " + cleanup_errors});
    }
    return core::Result<void>::success();
  }

  void load(const Manifest &manifest, std::size_t status_index) {
    auto &status = statuses_[status_index];
    status.state = PluginState::loading;
    status.detail = "loading";
    const auto fail = [&](std::string detail) {
      status.state = PluginState::failed;
      status.detail = std::move(detail);
    };

    if (manifest.abi_version != ZEDA_PLUGIN_ABI_VERSION) {
      fail("plugin ABI mismatch");
      return;
    }

    auto plugin = std::make_unique<LoadedPlugin>();
    plugin->manifest = manifest;
    plugin->status_index = status_index;
    plugin->library.reset(
        dlopen(plugin->manifest.library_path.c_str(), RTLD_NOW | RTLD_LOCAL));
    if (!plugin->library) {
      const char *load_error = dlerror();
      fail("cannot load plugin library: " +
           std::string(load_error == nullptr ? "unknown error" : load_error));
      return;
    }
    const auto entry = reinterpret_cast<ZedaPluginEntryV1>(
        dlsym(plugin->library.get(), ZEDA_PLUGIN_ENTRY_SYMBOL));
    if (entry == nullptr) {
      fail("plugin entry symbol is missing");
      return;
    }
    try {
      plugin->descriptor = entry();
    } catch (...) {
      plugin->descriptor = nullptr;
    }
    bool descriptor_matches = false;
    try {
      descriptor_matches =
          plugin->descriptor != nullptr &&
          plugin->descriptor->abi_version == ZEDA_PLUGIN_ABI_VERSION &&
          copy_string(plugin->descriptor->id) == plugin->manifest.id &&
          copy_string(plugin->descriptor->version) == plugin->manifest.version;
    } catch (...) {
      descriptor_matches = false;
    }
    if (!descriptor_matches) {
      fail("plugin descriptor does not match manifest");
      return;
    }
    if (plugin->descriptor->create == nullptr ||
        plugin->descriptor->initialize == nullptr ||
        plugin->descriptor->destroy == nullptr) {
      fail("plugin lifecycle is incomplete");
      return;
    }

    plugin->host_context.manager = this;
    plugin->host_context.gate = plugin->gate;
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

    BoundedText initialization_error(config_.max_output_bytes);
    try {
      plugin->instance = plugin->descriptor->create();
      if (plugin->instance == nullptr)
        throw std::runtime_error("plugin create returned null");
      plugin->initialize_entered = true;
      const int initialized = plugin->descriptor->initialize(
          plugin->instance, &plugin->host_api, text_sink(initialization_error));
      close_registration_phase(*plugin);
      if (initialized != 0) {
        auto detail = initialization_error.take();
        throw std::runtime_error(detail.empty() ? "plugin initialization failed"
                                                : std::move(detail));
      }
    } catch (const std::exception &exception) {
      close_registration_phase(*plugin);
      plugin->gate->begin_quiesce();
      plugin->gate->wait_until_idle();
      const auto cleanup = destroy_instance(*plugin, true);
      fail(std::string(exception.what()) +
           (cleanup.empty() ? "" : "; cleanup: " + cleanup));
      return;
    } catch (...) {
      close_registration_phase(*plugin);
      plugin->gate->begin_quiesce();
      plugin->gate->wait_until_idle();
      const auto cleanup = destroy_instance(*plugin, true);
      fail("plugin initialization threw an unknown exception" +
           (cleanup.empty() ? std::string{} : "; cleanup: " + cleanup));
      return;
    }

    const auto validation = validate_staged(plugin->host_context);
    if (!validation) {
      plugin->gate->begin_quiesce();
      plugin->gate->wait_until_idle();
      const auto cleanup = destroy_instance(*plugin, true);
      fail(validation.error().message +
           (cleanup.empty() ? "" : "; cleanup: " + cleanup));
      return;
    }

    try {
      loaded_.reserve(loaded_.size() + 1);
      plugin->contributions =
          std::make_unique<PluginContributionScope>(extensions_, tools_);
      for (const auto &command : plugin->host_context.commands) {
        std::vector<extensions::CommandOption> options;
        if (!command.options_json.empty()) {
          const auto document = Json::parse(command.options_json);
          for (const auto &option : document) {
            options.push_back({option.at("value").get<std::string>(),
                               option.value("description", ""),
                               option.value("view", "") == "document"});
          }
        }
        const auto callback = command;
        const auto gate = plugin->gate;
        const auto max_output_bytes = config_.max_output_bytes;
        const auto registered = extensions_.register_command({
            callback.name,
            callback.description,
            [callback, gate, max_output_bytes](std::string_view arguments) {
              return execute_plugin_command(callback, arguments, {}, {}, gate,
                                            max_output_bytes);
            },
            std::move(options),
            [callback, gate,
             max_output_bytes](std::string_view arguments,
                               core::CancellationToken cancellation,
                               core::AgentEventCallback on_event) {
              return execute_plugin_command(callback, arguments, cancellation,
                                            std::move(on_event), gate,
                                            max_output_bytes);
            },
        });
        if (!registered) {
          fail_loaded_plugin(*plugin, status, registered.error().message);
          return;
        }
        const auto tracked = plugin->contributions->track_command(command.name);
        if (!tracked) {
          fail_loaded_plugin(*plugin, status, tracked.error().message);
          return;
        }
      }
      for (const auto &tool : plugin->host_context.tools) {
        const auto registered =
            tools_.register_tool(std::make_unique<PluginTool>(
                tool, plugin->gate, config_.max_output_bytes));
        if (!registered) {
          fail_loaded_plugin(*plugin, status, registered.error().message);
          return;
        }
        const auto tracked = plugin->contributions->track_tool(tool.name);
        if (!tracked) {
          fail_loaded_plugin(*plugin, status, tracked.error().message);
          return;
        }
      }

      status.state = PluginState::active;
      status.loaded = true;
      status.detail = "active";
      loaded_.push_back(std::move(plugin));
    } catch (const std::exception &exception) {
      fail_loaded_plugin(*plugin, status,
                         "plugin contribution commit failed: " +
                             std::string(exception.what()));
    } catch (...) {
      fail_loaded_plugin(*plugin, status,
                         "plugin contribution commit failed: unknown error");
    }
  }

  void close_registration_phase(LoadedPlugin &plugin) {
    std::scoped_lock lock(plugin.host_context.registration_mutex);
    plugin.host_context.accepting_registrations = false;
  }

  void fail_loaded_plugin(LoadedPlugin &plugin, PluginStatus &status,
                          std::string detail) {
    plugin.gate->begin_quiesce();
    plugin.contributions.reset();
    plugin.gate->wait_until_idle();
    const auto cleanup = destroy_instance(plugin, true);
    status.state = PluginState::failed;
    status.loaded = false;
    status.detail = std::move(detail);
    if (!cleanup.empty())
      status.detail += "; cleanup: " + cleanup;
  }

  std::string destroy_instance(LoadedPlugin &plugin,
                               bool call_shutdown) noexcept {
    if (plugin.instance == nullptr)
      return {};
    std::string errors;
    if (call_shutdown && plugin.initialize_entered &&
        plugin.descriptor->shutdown != nullptr) {
      try {
        plugin.descriptor->shutdown(plugin.instance);
      } catch (const std::exception &exception) {
        append_cleanup_error(errors, "shutdown threw: ", exception.what());
      } catch (...) {
        append_cleanup_error(errors, "shutdown threw an unknown exception");
      }
    }
    try {
      plugin.descriptor->destroy(plugin.instance);
    } catch (const std::exception &exception) {
      append_cleanup_error(errors, "destroy threw: ", exception.what());
    } catch (...) {
      append_cleanup_error(errors, "destroy threw an unknown exception");
    }
    plugin.instance = nullptr;
    return errors;
  }

  void emergency_shutdown() noexcept {
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      try {
        (*iterator)->gate->begin_quiesce();
        close_registration_phase(**iterator);
      } catch (...) {
      }
    }
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      try {
        (*iterator)->contributions.reset();
      } catch (...) {
      }
    }
    for (auto iterator = loaded_.rbegin(); iterator != loaded_.rend();
         ++iterator) {
      try {
        (*iterator)->gate->wait_until_idle();
      } catch (...) {
      }
      static_cast<void>(destroy_instance(**iterator, true));
    }
    loaded_.clear();
    shut_down_ = true;
  }

  core::Result<void> validate_staged(const HostContext &host) const {
    try {
      return validate_staged_unchecked(host);
    } catch (const std::exception &exception) {
      return core::Result<void>::failure(
          {ErrorCode::internal, "plugin contribution validation failed: " +
                                    std::string(exception.what())});
    } catch (...) {
      return core::Result<void>::failure(
          {ErrorCode::internal,
           "plugin contribution validation failed: unknown error"});
    }
  }

  core::Result<void> validate_staged_unchecked(const HostContext &host) const {
    std::set<std::string> command_names;
    for (const auto &existing : extensions_.commands_snapshot())
      command_names.insert(existing.name);
    for (const auto &command : host.commands) {
      if (command.name.empty() || command.execute == nullptr)
        return core::Result<void>::failure(
            {ErrorCode::invalid_argument, "plugin command is incomplete"});
      if (!command_names.insert(command.name).second)
        return core::Result<void>::failure(
            {ErrorCode::conflict, "plugin command conflict: " + command.name});
      std::vector<extensions::CommandOption> options;
      if (!command.options_json.empty()) {
        const auto parsed = Json::parse(command.options_json, nullptr, false);
        if (!parsed.is_array())
          return core::Result<void>::failure(
              {ErrorCode::invalid_argument,
               "plugin command options must be a JSON array: " + command.name});
        for (const auto &option : parsed) {
          if (!option.is_object() || !option.contains("value") ||
              !option.at("value").is_string() ||
              (option.contains("description") &&
               !option.at("description").is_string()) ||
              (option.contains("view") &&
               (!option.at("view").is_string() ||
                option.at("view").get<std::string>() != "document"))) {
            return core::Result<void>::failure(
                {ErrorCode::invalid_argument,
                 "plugin command option is invalid: " + command.name});
          }
          options.push_back({option.at("value").get<std::string>(),
                             option.value("description", ""),
                             option.value("view", "") == "document"});
        }
      }
      const auto exact_validation = extensions_.validate_command(
          {command.name,
           command.description,
           [](std::string_view) {
             return core::Result<std::string>::success({});
           },
           std::move(options),
           {}});
      if (!exact_validation)
        return exact_validation;
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
      const PluginTool candidate(tool, host.gate, config_.max_output_bytes);
      const auto exact_validation =
          tools_.validate_tool(candidate.definition());
      if (!exact_validation)
        return exact_validation;
    }
    return core::Result<void>::success();
  }

  static int register_command(void *context, const ZedaCommandV1 *command,
                              ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || command == nullptr || command->execute == nullptr) {
      write_sink(error, "invalid plugin command");
      return 1;
    }
    try {
      std::scoped_lock lock(host->registration_mutex);
      if (!host->accepting_registrations) {
        write_sink(error, "plugin registration phase has ended");
        return 1;
      }
      host->commands.push_back({copy_string(command->name),
                                copy_string(command->description),
                                copy_string(command->options_json),
                                command->context, command->execute});
      return 0;
    } catch (const std::exception &exception) {
      write_sink(error, std::string("cannot stage plugin command: ") +
                            exception.what());
      return 1;
    } catch (...) {
      write_sink(error, "cannot stage plugin command: unknown error");
      return 1;
    }
  }

  static int register_tool(void *context, const ZedaToolV1 *tool,
                           ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || tool == nullptr || tool->execute == nullptr) {
      write_sink(error, "invalid plugin tool");
      return 1;
    }
    try {
      std::scoped_lock lock(host->registration_mutex);
      if (!host->accepting_registrations) {
        write_sink(error, "plugin registration phase has ended");
        return 1;
      }
      host->tools.push_back(
          {copy_string(tool->name), copy_string(tool->description),
           copy_string(tool->input_schema_json), tool->context, tool->execute});
      return 0;
    } catch (const std::exception &exception) {
      write_sink(error,
                 std::string("cannot stage plugin tool: ") + exception.what());
      return 1;
    } catch (...) {
      write_sink(error, "cannot stage plugin tool: unknown error");
      return 1;
    }
  }

  static int complete(void *context, ZedaStringView system_prompt,
                      ZedaStringView user_prompt, size_t max_output_tokens,
                      uint32_t reasoning_effort,
                      ZedaCancellationV1 cancellation,
                      ZedaEventCallbackV1 on_event, void *event_context,
                      ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || host->manager == nullptr) {
      write_sink(error, "invalid plugin host context");
      return 1;
    }
    auto lease = host->gate->enter();
    if (!lease.has_value()) {
      write_sink(error, "plugin is unloading");
      return 1;
    }
    try {
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
      std::size_t event_bytes_remaining = manager.config_.max_output_bytes;
      bool event_truncation_reported = false;
      bool event_callback_failed = false;
      const auto response = manager.model_.complete(
          request,
          [&](const core::ModelDelta &delta) {
            if (on_event == nullptr || event_truncation_reported ||
                event_callback_failed)
              return;
            try {
              const auto copied =
                  std::min(event_bytes_remaining, delta.text.size());
              auto bounded = delta.text.substr(0, copied);
              event_bytes_remaining -= copied;
              if (copied != delta.text.size()) {
                bounded += kTruncationMarker;
                event_truncation_reported = true;
              }
              if (!bounded.empty()) {
                on_event(event_context, ZEDA_EVENT_DELTA, string_view(bounded));
              }
            } catch (...) {
              event_callback_failed = true;
            }
          },
          native_cancellation(cancellation, host->gate));
      if (event_callback_failed) {
        write_sink(error, "plugin model event callback failed");
        return 1;
      }
      if (!response) {
        write_sink(error, response.error().message);
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
        write_sink(error, detail);
        return 1;
      }
      if (!write_sink(output, response.value().content)) {
        write_sink(error, "plugin model output sink rejected content");
        return 1;
      }
      return 0;
    } catch (const std::exception &exception) {
      write_sink(error, std::string("plugin model bridge failed: ") +
                            exception.what());
      return 1;
    } catch (...) {
      write_sink(error, "plugin model bridge failed: unknown error");
      return 1;
    }
  }

  static int clangd_query(void *context, ZedaStringView operation,
                          ZedaStringView relative_path, size_t line,
                          size_t character, ZedaCancellationV1 cancellation,
                          ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    auto *host = static_cast<HostContext *>(context);
    if (host == nullptr || host->manager == nullptr) {
      write_sink(error, "invalid plugin host context");
      return 1;
    }
    auto lease = host->gate->enter();
    if (!lease.has_value()) {
      write_sink(error, "plugin is unloading");
      return 1;
    }
    try {
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
        write_sink(error, "unsupported clangd operation");
        return 1;
      }
      const auto result = host->manager->clangd_.query(
          query, copy_string(relative_path), line, character,
          native_cancellation(cancellation, host->gate));
      if (!result) {
        write_sink(error, result.error().message);
        return 1;
      }
      if (!write_sink(output, result.value())) {
        write_sink(error, "plugin clangd output sink rejected content");
        return 1;
      }
      return 0;
    } catch (const std::exception &exception) {
      write_sink(error, std::string("plugin clangd bridge failed: ") +
                            exception.what());
      return 1;
    } catch (...) {
      write_sink(error, "plugin clangd bridge failed: unknown error");
      return 1;
    }
  }

  PluginManagerConfig config_;
  extensions::ExtensionRegistry &extensions_;
  core::ToolRegistry &tools_;
  core::Model &model_;
  lsp::ClangdClient &clangd_;
  std::vector<std::unique_ptr<LoadedPlugin>> loaded_;
  std::vector<PluginStatus> statuses_;
  bool discovered_{false};
  bool shut_down_{false};
};

PluginManager::PluginManager(PluginManagerConfig config,
                             extensions::ExtensionRegistry &extensions,
                             core::ToolRegistry &tools, core::Model &model,
                             lsp::ClangdClient &clangd)
    : impl_(std::make_unique<Impl>(std::move(config), extensions, tools, model,
                                   clangd)) {}

PluginManager::~PluginManager() = default;

core::Result<void> PluginManager::discover_and_load() {
  try {
    return impl_->discover_and_load();
  } catch (const std::exception &exception) {
    impl_->emergency_shutdown();
    return core::Result<void>::failure(
        {core::ErrorCode::internal,
         "plugin discovery failed: " + std::string(exception.what())});
  } catch (...) {
    impl_->emergency_shutdown();
    return core::Result<void>::failure(
        {core::ErrorCode::internal,
         "plugin discovery failed with an unknown error"});
  }
}

core::Result<void> PluginManager::shutdown() {
  try {
    return impl_->shutdown();
  } catch (const std::exception &exception) {
    impl_->emergency_shutdown();
    return core::Result<void>::failure(
        {core::ErrorCode::internal,
         "plugin shutdown failed: " + std::string(exception.what())});
  } catch (...) {
    impl_->emergency_shutdown();
    return core::Result<void>::failure(
        {core::ErrorCode::internal,
         "plugin shutdown failed with an unknown error"});
  }
}

const std::vector<PluginStatus> &PluginManager::statuses() const {
  return impl_->statuses_;
}

std::string PluginManager::status_report() const {
  if (impl_->statuses_.empty())
    return "no plugins discovered\n";
  std::string result;
  for (const auto &status : impl_->statuses_) {
    result += std::string(plugin_state_name(status.state)) + "  ";
    result += status.name.empty() ? status.id : status.name;
    if (!status.version.empty())
      result += " " + status.version;
    result += " — " + status.detail + "\n";
    if (!status.dependencies.empty()) {
      result += "        requires:";
      for (const auto &dependency : status.dependencies)
        result += " " + dependency;
      result += "\n";
    }
    result += "        " + status.manifest_path.string() + "\n";
  }
  return result;
}

} // namespace zed::plugins
