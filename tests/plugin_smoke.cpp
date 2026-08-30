#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "zed/core/model.hpp"
#include "zed/core/tool_registry.hpp"
#include "zed/extensions/extension_registry.hpp"
#include "zed/lsp/clangd_client.hpp"
#include "zed/plugins/plugin_manager.hpp"

namespace {

class FakeModel final : public zed::core::Model {
public:
  zed::core::Result<zed::core::AssistantResponse>
  complete(const zed::core::ModelRequest &request,
           const zed::core::StreamCallback &on_delta,
           zed::core::CancellationToken cancellation) override {
    assert(!cancellation.is_cancelled());
    ++calls;
    const auto &system = request.messages.front().content;
    std::string response;
    if (system.find("文档规划器") != std::string::npos) {
      response = R"([
        {"id":"overview","title":"总览","description":"总体结构","queries":["foo architecture"]},
        {"id":"flow","title":"运行流程","description":"关键流程","queries":["foo run"]},
        {"id":"async","title":"异步模型","description":"异步边界","queries":["foo async"]},
        {"id":"build","title":"构建系统","description":"构建组织","queries":["foo CMake"]}
      ])";
    } else if (system.find("检索关键词") != std::string::npos) {
      response = "foo";
    } else if (system.find("问答助手") != std::string::npos) {
      last_qa_had_evidence = request.messages.back().content.find(
                                 "src/foo.cpp") != std::string::npos;
      response = "回答来自源码。[src/foo.cpp:1]";
      if (on_delta)
        on_delta({response});
    } else {
      response = "# 测试页面\n\n这个页面基于真实源码。[src/foo.cpp:1]\n";
    }
    return zed::core::Result<zed::core::AssistantResponse>::success(
        {std::move(response), {}, zed::core::FinishReason::stop, {}});
  }

  std::size_t calls{};
  bool last_qa_had_evidence{false};
};

std::filesystem::path temporary_workspace() {
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("zeda-deepwiki-test-" + std::to_string(nonce));
}

void write_fixture(const std::filesystem::path &workspace,
                   std::string_view suffix = {}) {
  std::filesystem::create_directories(workspace / "src");
  std::ofstream header(workspace / "src" / "foo.hpp");
  header << "int foo();\n";
  std::ofstream source(workspace / "src" / "foo.cpp");
  source << "#include \"foo.hpp\"\nint foo() { return 42; }\n" << suffix;
  std::ofstream cmake(workspace / "CMakeLists.txt");
  cmake << "add_library(foo src/foo.cpp)\n";
  std::ofstream readme(workspace / "README.md");
  readme << "# Fixture\n";
}

} // namespace

int main() {
  setenv("ZED_DEEPWIKI_NO_BROWSER", "1", 1);
  const auto workspace = temporary_workspace();
  write_fixture(workspace);

  FakeModel model;
  zed::core::ToolRegistry tools;
  zed::extensions::ExtensionRegistry extensions;
  zed::lsp::ClangdClient clangd({workspace, "/usr/bin/false", {}});
  {
    zed::plugins::PluginManager manager({workspace,
                                         {ZED_TEST_PLUGIN_ROOT},
                                         {"fake", "fake"},
                                         zed::core::ReasoningEffort::low},
                                        extensions, tools, model, clangd);
    assert(manager.discover_and_load());
    assert(manager.statuses().size() == 1);
    assert(manager.statuses().front().loaded);
    const auto deepwiki_command = std::find_if(
        extensions.commands().begin(), extensions.commands().end(),
        [](const auto &command) { return command.name == "deepwiki"; });
    assert(deepwiki_command != extensions.commands().end());
    const auto tui_option = std::find_if(
        deepwiki_command->options.begin(), deepwiki_command->options.end(),
        [](const auto &option) { return option.value == "tui"; });
    assert(tui_option != deepwiki_command->options.end());
    assert(tui_option->opens_document_view);
    const auto missing_tui = extensions.execute("deepwiki", "tui");
    assert(!missing_tui);
    assert(missing_tui.error().message.find("has not been generated") !=
           std::string::npos);

    const auto generated = extensions.execute("deepwiki", "generate");
    assert(generated);
    assert(model.calls == 5);
    assert(std::filesystem::is_regular_file(workspace / ".zed" / "deepwiki" /
                                            "index.sqlite"));
    assert(std::filesystem::is_regular_file(workspace / ".zed" / "deepwiki" /
                                            "toc.json"));
    const auto tui = extensions.execute("deepwiki", "tui");
    assert(tui);
    const auto tui_document =
        nlohmann::json::parse(tui.value(), nullptr, false);
    assert(tui_document.is_object());
    assert(tui_document.value("schema_version", 0) == 1);
    assert(tui_document.value("title", std::string{}) == "DeepWiki");
    assert(tui_document.at("pages").is_array());
    assert(tui_document.at("pages").size() == 4);
    assert(tui_document.at("pages")
               .front()
               .at("markdown")
               .get<std::string>()
               .find("测试页面") != std::string::npos);
    assert(model.calls == 5);

    const auto unchanged = extensions.execute("deepwiki", "update");
    assert(unchanged);
    assert(unchanged.value().find("未调用模型") != std::string::npos);
    assert(model.calls == 5);

    write_fixture(workspace, "// changed\n");
    const auto updated = extensions.execute("deepwiki", "update");
    assert(updated);
    assert(model.calls == 9);

    const auto definitions = tools.definitions();
    assert(std::any_of(
        definitions.begin(), definitions.end(),
        [](const auto &item) { return item.name == "deepwiki_search"; }));
    const auto search = tools.execute(
        {"search-1", "deepwiki_search",
         R"({"purpose":"verify local index","query":"foo","limit":4})"},
        {});
    assert(search);
    assert(search.value().content.find("src/foo.cpp") != std::string::npos);
    const auto relation_search = tools.execute(
        {"search-2", "deepwiki_search",
         R"({"purpose":"verify include graph","query":"foo.hpp","limit":4})"},
        {});
    assert(relation_search);
    assert(relation_search.value().content.find("[include]") !=
           std::string::npos);

    const auto opened = extensions.execute("deepwiki", "open");
    assert(opened);
    const std::regex url_pattern(
        R"(http://127\.0\.0\.1:(\d+)/\?token=([0-9a-f]+))");
    std::smatch match;
    assert(std::regex_search(opened.value(), match, url_pattern));
    const int port = std::stoi(match[1].str());
    const auto token = match[2].str();
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10, 0);

    const auto denied = client.Get("/api/toc");
    assert(denied && denied->status == 403);
    const auto toc = client.Get("/api/toc?token=" + token);
    assert(toc && toc->status == 200);
    assert(toc->body.find("overview") != std::string::npos);
    const auto source =
        client.Get("/api/source?token=" + token + "&path=src/foo.cpp&line=1");
    assert(source && source->status == 200);
    assert(source->body.find("int foo") != std::string::npos);
    const auto escaped = client.Get("/api/source?token=" + token +
                                    "&path=../CMakeLists.txt&line=1");
    assert(escaped && escaped->status == 404);

    httplib::Headers invalid_origin{{"Origin", "http://example.invalid"}};
    const auto rejected =
        client.Post("/api/ask?token=" + token, invalid_origin,
                    R"({"question":"foo 是什么？"})", "application/json");
    assert(rejected && rejected->status == 403);

    httplib::Headers valid_headers{
        {"Origin", "http://127.0.0.1:" + std::to_string(port)},
        {"X-DeepWiki-Token", token},
    };
    const auto answered =
        client.Post("/api/ask?token=" + token, valid_headers,
                    R"({"question":"foo 是什么？"})", "application/json");
    assert(answered && answered->status == 200);
    assert(answered->body.find("event: done") != std::string::npos);
    assert(model.last_qa_had_evidence);
  }

  {
    model.last_qa_had_evidence = false;
    zed::core::ToolRegistry reopened_tools;
    zed::extensions::ExtensionRegistry reopened_extensions;
    zed::plugins::PluginManager reopened_manager(
        {workspace,
         {ZED_TEST_PLUGIN_ROOT},
         {"fake", "fake"},
         zed::core::ReasoningEffort::low},
        reopened_extensions, reopened_tools, model, clangd);
    assert(reopened_manager.discover_and_load());
    const auto opened = reopened_extensions.execute("deepwiki", "open");
    assert(opened);
    const std::regex url_pattern(
        R"(http://127\.0\.0\.1:(\d+)/\?token=([0-9a-f]+))");
    std::smatch match;
    assert(std::regex_search(opened.value(), match, url_pattern));
    const int port = std::stoi(match[1].str());
    const auto token = match[2].str();
    httplib::Client client("127.0.0.1", port);
    client.set_read_timeout(10, 0);
    httplib::Headers headers{
        {"Origin", "http://127.0.0.1:" + std::to_string(port)},
        {"X-DeepWiki-Token", token},
    };
    const auto answered = client.Post(
        "/api/ask?token=" + token, headers,
        R"({"question":"重新打开后 foo 是什么？"})", "application/json");
    assert(answered && answered->status == 200);
    assert(model.last_qa_had_evidence);
  }

  {
    zed::core::ToolRegistry duplicate_tools;
    zed::extensions::ExtensionRegistry duplicate_extensions;
    assert(duplicate_extensions.register_command(
        {"deepwiki", "reserved", [](std::string_view) {
           return zed::core::Result<std::string>::success("reserved");
         }}));
    zed::plugins::PluginManager duplicate_manager(
        {workspace,
         {ZED_TEST_PLUGIN_ROOT},
         {"fake", "fake"},
         zed::core::ReasoningEffort::low},
        duplicate_extensions, duplicate_tools, model, clangd);
    assert(duplicate_manager.discover_and_load());
    assert(!duplicate_manager.statuses().front().loaded);
    assert(duplicate_manager.statuses().front().detail.find("conflict") !=
           std::string::npos);
    assert(duplicate_tools.definitions().empty());
  }

  {
    zed::core::ToolRegistry faulty_tools;
    zed::extensions::ExtensionRegistry faulty_extensions;
    zed::plugins::PluginManager faulty_manager(
        {workspace,
         {ZED_TEST_FAULTY_PLUGIN_ROOT},
         {"fake", "fake"},
         zed::core::ReasoningEffort::low},
        faulty_extensions, faulty_tools, model, clangd);
    assert(faulty_manager.discover_and_load());
    assert(faulty_manager.statuses().size() == 1);
    assert(!faulty_manager.statuses().front().loaded);
    assert(faulty_manager.statuses().front().detail.find("properties") !=
           std::string::npos);
    assert(faulty_extensions.commands().empty());
    assert(faulty_tools.definitions().empty());
    const auto missing = faulty_extensions.execute("faulty_command", "");
    assert(!missing);
    assert(missing.error().code == zed::core::ErrorCode::not_found);
  }

  {
    const auto invalid_root = workspace / "invalid-plugin";
    std::filesystem::create_directories(invalid_root / "resources");
    std::ofstream manifest(invalid_root / "zeda-plugin.json");
    manifest
        << R"({"id":"invalid","name":"Invalid","version":"1","abi_version":999,"library":"missing.so","resources":"resources"})";
    manifest.close();
    zed::core::ToolRegistry invalid_tools;
    zed::extensions::ExtensionRegistry invalid_extensions;
    zed::plugins::PluginManager invalid_manager(
        {workspace,
         {invalid_root},
         {"fake", "fake"},
         zed::core::ReasoningEffort::low},
        invalid_extensions, invalid_tools, model, clangd);
    assert(invalid_manager.discover_and_load());
    assert(invalid_manager.statuses().size() == 1);
    assert(!invalid_manager.statuses().front().loaded);
    assert(invalid_manager.statuses().front().detail.find("ABI") !=
           std::string::npos);
  }

  std::filesystem::remove_all(workspace);
  return 0;
}
