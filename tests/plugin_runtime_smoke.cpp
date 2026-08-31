#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zed/core/model.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/plugins/plugin_manager.hpp"

#ifndef ZED_TEST_RUNTIME_PROVIDER_PATH
#error "ZED_TEST_RUNTIME_PROVIDER_PATH must name the provider fixture library"
#endif

#ifndef ZED_TEST_RUNTIME_CONSUMER_PATH
#error "ZED_TEST_RUNTIME_CONSUMER_PATH must name the consumer fixture library"
#endif

namespace {

using Json = nlohmann::json;
using zed::plugins::PluginState;

constexpr std::string_view kProviderId = "runtime.provider";
constexpr std::string_view kConsumerId = "runtime.consumer";
constexpr std::string_view kProviderCommand = "runtime_provider_command";
constexpr std::string_view kConsumerCommand = "runtime_consumer_command";
constexpr std::string_view kProviderTool = "runtime_provider_tool";
constexpr std::string_view kConsumerTool = "runtime_consumer_tool";
constexpr std::string_view kProviderReady = "runtime-provider.ready";
constexpr std::string_view kConsumerReady = "runtime-consumer.ready";
constexpr std::string_view kLifecycleLog = "runtime-plugin-lifecycle.log";
constexpr std::string_view kToolStarted = "runtime-tool-started.ready";
constexpr std::string_view kToolCancelled = "runtime-tool-cancelled.ready";
constexpr std::string_view kTruncationMarker = "\n[plugin output truncated]";
constexpr std::size_t kOutputLimit = 64;

class FakeModel final : public zed::core::Model {
public:
  zed::core::Result<zed::core::AssistantResponse>
  complete(const zed::core::ModelRequest &request,
           const zed::core::StreamCallback &on_delta,
           zed::core::CancellationToken cancellation) override {
    static_cast<void>(request);
    static_cast<void>(on_delta);
    static_cast<void>(cancellation);
    return zed::core::Result<zed::core::AssistantResponse>::success(
        {"unused", {}, zed::core::FinishReason::stop, {}});
  }
};

class TemporaryDirectory {
public:
  explicit TemporaryDirectory(std::string_view label) {
    static std::atomic_uint64_t sequence{};
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (std::uint64_t attempt = 0; attempt < 100; ++attempt) {
      root_ = std::filesystem::temp_directory_path() /
              ("zeda-" + std::string(label) + "-" + std::to_string(nonce) +
               "-" + std::to_string(sequence.fetch_add(1)));
      std::error_code error;
      if (std::filesystem::create_directory(root_, error))
        return;
      if (error && error != std::errc::file_exists) {
        throw std::runtime_error("cannot create test directory: " +
                                 error.message());
      }
    }
    throw std::runtime_error("cannot allocate a unique test directory");
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(root_, ignored));
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return root_; }

private:
  std::filesystem::path root_;
};

struct Harness {
  FakeModel model;
  zed::core::ToolRegistry tools;
  zed::extensions::ExtensionRegistry extensions;
  zed::lsp::ClangdClient clangd;
  zed::plugins::PluginManager manager;

  Harness(const std::filesystem::path &workspace,
          std::vector<std::filesystem::path> search_paths,
          std::size_t max_output_bytes = 256 * 1024)
      : clangd({workspace, "/usr/bin/false", {}}),
        manager({workspace,
                 std::move(search_paths),
                 {"fixture", "fixture"},
                 zed::core::ReasoningEffort::low,
                 max_output_bytes},
                extensions, tools, model, clangd) {}
};

std::filesystem::path install_plugin(
    const std::filesystem::path &search_root, std::string_view directory_name,
    const std::filesystem::path &fixture_library, std::string_view id,
    std::string_view name, const std::vector<std::string> &dependencies = {}) {
  const auto plugin_root = search_root / directory_name;
  std::filesystem::create_directories(plugin_root / "resources");
  const auto installed_library = plugin_root / fixture_library.filename();
  std::filesystem::copy_file(fixture_library, installed_library,
                             std::filesystem::copy_options::overwrite_existing);

  Json manifest = {
      {"id", std::string(id)},
      {"name", std::string(name)},
      {"version", "1.0.0"},
      {"abi_version", 1},
      {"library", installed_library.filename().string()},
      {"resources", "resources"},
  };
  if (!dependencies.empty())
    manifest["requires"] = dependencies;

  const auto manifest_path = plugin_root / "zeda-plugin.json";
  std::ofstream output(manifest_path, std::ios::binary | std::ios::trunc);
  output << manifest.dump(2) << '\n';
  if (!output)
    throw std::runtime_error("cannot write runtime plugin manifest");
  return manifest_path;
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string_view::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

bool wait_for_file(const std::filesystem::path &path,
                   std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    if (std::filesystem::is_regular_file(path))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  return std::filesystem::is_regular_file(path);
}

bool has_command(const zed::extensions::ExtensionRegistry &extensions,
                 std::string_view name) {
  const auto commands = extensions.commands_snapshot();
  return std::any_of(commands.begin(), commands.end(),
                     [&](const auto &command) { return command.name == name; });
}

bool has_tool(const zed::core::ToolRegistry &tools, std::string_view name) {
  const auto definitions = tools.registered_definitions();
  return std::any_of(
      definitions.begin(), definitions.end(),
      [&](const auto &definition) { return definition.name == name; });
}

const zed::plugins::PluginStatus &
status_for(const zed::plugins::PluginManager &manager, std::string_view id) {
  const auto &statuses = manager.statuses();
  const auto iterator =
      std::find_if(statuses.begin(), statuses.end(),
                   [&](const auto &status) { return status.id == id; });
  assert(iterator != statuses.end());
  return *iterator;
}

void assert_truncated(std::string_view output) {
  assert(output.size() == kOutputLimit);
  assert(output.ends_with(kTruncationMarker));
}

void test_runtime_lifecycle_and_cancellation(
    const std::filesystem::path &provider_library) {
  TemporaryDirectory temporary("plugin-runtime");
  const auto workspace = temporary.path() / "workspace";
  const auto search_root = temporary.path() / "plugins";
  std::filesystem::create_directories(workspace);
  const auto manifest =
      install_plugin(search_root, "provider", provider_library, kProviderId,
                     "Runtime provider fixture");

  Harness harness(workspace, {search_root}, kOutputLimit);
  assert(harness.manager.discover_and_load());
  assert(harness.manager.statuses().size() == 1);
  const auto &active = harness.manager.statuses().front();
  assert(active.id == kProviderId);
  assert(active.state == PluginState::active);
  assert(active.loaded);
  assert(active.detail == "active");
  assert(active.manifest_path == std::filesystem::canonical(manifest));
  assert(std::filesystem::is_regular_file(workspace / kProviderReady));
  assert(has_command(harness.extensions, kProviderCommand));
  assert(has_tool(harness.tools, kProviderTool));

  const auto command = harness.extensions.execute(kProviderCommand, "hello");
  assert(command);
  assert(command.value() == "runtime_provider_command:hello");

  const auto tool_arguments =
      Json{{"purpose", "exercise the runtime provider"}, {"mode", "echo"}}
          .dump();
  const auto tool = harness.tools.execute(
      {"runtime-echo", std::string(kProviderTool), tool_arguments}, {});
  assert(tool);
  const auto tool_document = Json::parse(tool.value().content);
  assert(tool_document.at("plugin").get<std::string>() == kProviderId);
  assert(tool_document.at("mode").get<std::string>() == "echo");

  const auto report_before_rediscovery = harness.manager.status_report();
  const auto lifecycle_before_rediscovery =
      read_file(workspace / kLifecycleLog);
  assert(count_occurrences(lifecycle_before_rediscovery,
                           "runtime.provider:initialize\n") == 1);
  assert(harness.manager.discover_and_load());
  assert(harness.manager.status_report() == report_before_rediscovery);
  assert(read_file(workspace / kLifecycleLog) == lifecycle_before_rediscovery);
  assert(harness.extensions.commands_snapshot().size() == 1);
  assert(harness.tools.registered_definitions().size() == 1);

  const auto large_command =
      harness.extensions.execute(kProviderCommand, "large");
  assert(large_command);
  assert_truncated(large_command.value());
  std::vector<std::string> events;
  const auto event_command = harness.extensions.execute(
      kProviderCommand, "events", {}, [&](const zed::core::AgentEvent &event) {
        events.push_back(event.text);
      });
  assert(event_command);
  std::string combined_events;
  for (const auto &event : events)
    combined_events += event;
  assert(combined_events.size() == kOutputLimit + kTruncationMarker.size());
  assert(combined_events.ends_with(kTruncationMarker));
  assert(count_occurrences(combined_events, kTruncationMarker) == 1);
  int failing_event_calls = 0;
  const auto failed_event_command = harness.extensions.execute(
      kProviderCommand, "events", {}, [&](const zed::core::AgentEvent &) {
        ++failing_event_calls;
        throw std::runtime_error("fixture event callback failure");
      });
  assert(!failed_event_command);
  assert(failed_event_command.error().code == zed::core::ErrorCode::internal);
  assert(failing_event_calls == 1);
  const auto large_tool = harness.tools.execute(
      {"runtime-large", std::string(kProviderTool),
       Json{{"purpose", "verify bounded plugin output"}, {"mode", "large"}}
           .dump()},
      {});
  assert(large_tool);
  assert_truncated(large_tool.value().content);

  auto waiting_tool = std::async(std::launch::async, [&] {
    return harness.tools.execute(
        {"runtime-wait", std::string(kProviderTool),
         Json{{"purpose", "verify shutdown cancellation"}, {"mode", "wait"}}
             .dump()},
        {});
  });
  assert(wait_for_file(workspace / kToolStarted, std::chrono::seconds(2)));

  assert(harness.manager.shutdown());
  assert(waiting_tool.wait_for(std::chrono::seconds(1)) ==
         std::future_status::ready);
  const auto cancelled = waiting_tool.get();
  assert(!cancelled);
  assert(cancelled.error().code == zed::core::ErrorCode::cancelled);
  assert(std::filesystem::is_regular_file(workspace / kToolCancelled));
  assert(!std::filesystem::exists(workspace / kProviderReady));

  assert(harness.manager.statuses().front().state == PluginState::disposed);
  assert(!harness.manager.statuses().front().loaded);
  assert(harness.manager.statuses().front().detail == "disposed");
  assert(!has_command(harness.extensions, kProviderCommand));
  assert(!has_tool(harness.tools, kProviderTool));
  const auto missing_command =
      harness.extensions.execute(kProviderCommand, "after shutdown");
  assert(!missing_command);
  assert(missing_command.error().code == zed::core::ErrorCode::not_found);
  const auto missing_tool = harness.tools.execute(
      {"runtime-missing", std::string(kProviderTool),
       Json{{"purpose", "verify tool deregistration"}}.dump()},
      {});
  assert(!missing_tool);
  assert(missing_tool.error().code == zed::core::ErrorCode::not_found);

  const auto disposed_report = harness.manager.status_report();
  assert(harness.manager.shutdown());
  assert(harness.manager.status_report() == disposed_report);
  const auto rediscovery = harness.manager.discover_and_load();
  assert(!rediscovery);
  assert(rediscovery.error().code == zed::core::ErrorCode::conflict);
  assert(rediscovery.error().message == "plugin manager has already shut down");
}

void test_reverse_lexical_topology(
    const std::filesystem::path &provider_library,
    const std::filesystem::path &consumer_library) {
  TemporaryDirectory temporary("plugin-topology");
  const auto workspace = temporary.path() / "workspace";
  const auto search_root = temporary.path() / "plugins";
  std::filesystem::create_directories(workspace);
  install_plugin(search_root, "00_consumer", consumer_library, kConsumerId,
                 "Runtime consumer fixture", {std::string(kProviderId)});
  install_plugin(search_root, "10_provider", provider_library, kProviderId,
                 "Runtime provider fixture");

  Harness harness(workspace, {search_root});
  assert(harness.manager.discover_and_load());
  const auto &statuses = harness.manager.statuses();
  assert(statuses.size() == 2);
  assert(statuses[0].id == kConsumerId);
  assert(statuses[0].dependencies ==
         std::vector<std::string>{std::string(kProviderId)});
  assert(statuses[0].state == PluginState::active);
  assert(statuses[1].id == kProviderId);
  assert(statuses[1].state == PluginState::active);
  assert(read_file(workspace / kConsumerReady) == "provider-ready-observed\n");
  assert(read_file(workspace / kLifecycleLog) ==
         "runtime.provider:initialize\n"
         "runtime.consumer:initialize\n");
  assert(has_command(harness.extensions, kProviderCommand));
  assert(has_command(harness.extensions, kConsumerCommand));
  assert(has_tool(harness.tools, kProviderTool));
  assert(has_tool(harness.tools, kConsumerTool));

  assert(harness.manager.shutdown());
  assert(!std::filesystem::exists(workspace / kProviderReady));
  assert(!std::filesystem::exists(workspace / kConsumerReady));
  assert(read_file(workspace / kLifecycleLog) == "runtime.provider:initialize\n"
                                                 "runtime.consumer:initialize\n"
                                                 "runtime.consumer:shutdown\n"
                                                 "runtime.provider:shutdown\n");
}

void test_missing_dependency(const std::filesystem::path &consumer_library) {
  TemporaryDirectory temporary("plugin-missing-dependency");
  const auto workspace = temporary.path() / "workspace";
  const auto search_root = temporary.path() / "plugins";
  std::filesystem::create_directories(workspace);
  install_plugin(search_root, "consumer", consumer_library, kConsumerId,
                 "Runtime consumer fixture", {"runtime.missing"});

  Harness harness(workspace, {search_root});
  assert(harness.manager.discover_and_load());
  assert(harness.manager.statuses().size() == 1);
  const auto &pending = harness.manager.statuses().front();
  assert(pending.id == kConsumerId);
  assert(pending.state == PluginState::pending);
  assert(!pending.loaded);
  assert(pending.dependencies == std::vector<std::string>{"runtime.missing"});
  assert(pending.detail == "required plugin is missing: runtime.missing");
  assert(!std::filesystem::exists(workspace / kConsumerReady));
  assert(!has_command(harness.extensions, kConsumerCommand));
  assert(!has_tool(harness.tools, kConsumerTool));
}

void test_dependency_cycle(const std::filesystem::path &provider_library,
                           const std::filesystem::path &consumer_library) {
  TemporaryDirectory temporary("plugin-cycle");
  const auto workspace = temporary.path() / "workspace";
  const auto search_root = temporary.path() / "plugins";
  std::filesystem::create_directories(workspace);
  install_plugin(search_root, "00_provider", provider_library, kProviderId,
                 "Runtime provider fixture", {std::string(kConsumerId)});
  install_plugin(search_root, "10_consumer", consumer_library, kConsumerId,
                 "Runtime consumer fixture", {std::string(kProviderId)});
  install_plugin(search_root, "20_observer", provider_library,
                 "runtime.observer", "Runtime observer fixture",
                 {std::string(kProviderId)});

  Harness harness(workspace, {search_root});
  assert(harness.manager.discover_and_load());
  assert(harness.manager.statuses().size() == 3);
  const auto &provider = status_for(harness.manager, kProviderId);
  const auto &consumer = status_for(harness.manager, kConsumerId);
  const auto &observer = status_for(harness.manager, "runtime.observer");
  assert(provider.state == PluginState::pending);
  assert(!provider.loaded);
  assert(provider.detail == "plugin dependency cycle: runtime.consumer");
  assert(consumer.state == PluginState::pending);
  assert(!consumer.loaded);
  assert(consumer.detail == "plugin dependency cycle: runtime.provider");
  assert(observer.state == PluginState::pending);
  assert(!observer.loaded);
  assert(observer.detail ==
         "required plugin is unavailable: runtime.provider (pending)");
  assert(!std::filesystem::exists(workspace / kProviderReady));
  assert(!std::filesystem::exists(workspace / kConsumerReady));
  assert(harness.extensions.commands_snapshot().empty());
  assert(harness.tools.registered_definitions().empty());
}

void test_search_path_priority(const std::filesystem::path &provider_library) {
  TemporaryDirectory temporary("plugin-shadowing");
  const auto workspace = temporary.path() / "workspace";
  const auto priority_root = temporary.path() / "zz_priority";
  const auto later_root = temporary.path() / "aa_later";
  std::filesystem::create_directories(workspace);
  const auto winner_manifest =
      install_plugin(priority_root, "provider", provider_library, kProviderId,
                     "Priority winner");
  const auto shadow_manifest =
      install_plugin(later_root, "provider", provider_library, kProviderId,
                     "Shadow candidate");

  Harness harness(workspace, {priority_root, later_root});
  assert(harness.manager.discover_and_load());
  const auto &statuses = harness.manager.statuses();
  assert(statuses.size() == 2);
  assert(statuses[0].id == kProviderId);
  assert(statuses[0].name == "Priority winner");
  assert(statuses[0].manifest_path ==
         std::filesystem::canonical(winner_manifest));
  assert(statuses[0].state == PluginState::active);
  assert(statuses[0].loaded);
  assert(statuses[1].id == kProviderId);
  assert(statuses[1].name == "Shadow candidate");
  assert(statuses[1].manifest_path ==
         std::filesystem::canonical(shadow_manifest));
  assert(statuses[1].state == PluginState::shadowed);
  assert(!statuses[1].loaded);
  assert(statuses[1].detail == "shadowed by earlier search-path entry: " +
                                   statuses[0].manifest_path.string());
  assert(harness.extensions.commands_snapshot().size() == 1);
  assert(harness.tools.registered_definitions().size() == 1);
  assert(count_occurrences(read_file(workspace / kLifecycleLog),
                           "runtime.provider:initialize\n") == 1);
}

} // namespace

int main() {
  const std::filesystem::path provider_library = ZED_TEST_RUNTIME_PROVIDER_PATH;
  const std::filesystem::path consumer_library = ZED_TEST_RUNTIME_CONSUMER_PATH;
  assert(std::filesystem::is_regular_file(provider_library));
  assert(std::filesystem::is_regular_file(consumer_library));

  test_runtime_lifecycle_and_cancellation(provider_library);
  test_reverse_lexical_topology(provider_library, consumer_library);
  test_missing_dependency(consumer_library);
  test_dependency_cycle(provider_library, consumer_library);
  test_search_path_priority(provider_library);
  return 0;
}
