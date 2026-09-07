#include "zed/core/agent_hooks.hpp"
#include "zed/core/agent_loop.hpp"
#include "zed/core/session_store.hpp"
#include "zed/providers/opencode_go_model.hpp"
#include "zed/session/jsonl_session_store.hpp"
#include "zed/support/unique_fd.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace zed::core;
using zed::providers::OpenCodeGoModel;
using Json = nlohmann::json;

std::string event(const Json &value) {
  return "data: " + value.dump() + "\n\n";
}
const std::string kChatDone =
    event({{"choices", Json::array({{{"index", 0},
                                     {"delta", {{"content", "done"}}},
                                     {"finish_reason", "stop"}}})},
           {"usage",
            {{"prompt_tokens", 20},
             {"completion_tokens", 2},
             {"prompt_tokens_details", {{"cached_tokens", 10}}}}}});
const std::string kResponsesDone =
    event({{"type", "response.completed"},
           {"response",
            {{"status", "completed"},
             {"output", Json::array()},
             {"usage",
              {{"input_tokens", 20},
               {"output_tokens", 2},
               {"input_tokens_details", {{"cached_tokens", 10}}}}}}}});

struct Fixture {
  httplib::Server server;
  std::thread thread;
  int port{};
  std::vector<httplib::Request> requests;
  std::string body;
  int status{200};
  std::size_t chunk_size{7};
  std::function<void(httplib::DataSink &)> after_first_chunk;
  std::function<std::string(std::size_t)> response_for;
  std::mutex mutex;

  explicit Fixture(std::string data) : body(std::move(data)) {
    server.set_read_timeout(3, 0);
    server.set_write_timeout(3, 0);
    server.Post("/v1/.*", [this](const httplib::Request &request,
                                 httplib::Response &response) {
      std::string data;
      {
        std::scoped_lock lock(mutex);
        requests.push_back(request);
        data = response_for ? response_for(requests.size()) : body;
      }
      response.status = status;
      if (status != 200) {
        response.set_content(data, "application/json");
        return;
      }
      response.set_chunked_content_provider(
          "text/event-stream",
          [this, data = std::move(data)](std::size_t offset,
                                         httplib::DataSink &sink) {
            if (offset >= data.size()) {
              sink.done();
              return true;
            }
            const auto count = std::min(chunk_size, data.size() - offset);
            if (!sink.write(data.data() + offset, count))
              return false;
            if (offset == 0 && after_first_chunk)
              after_first_chunk(sink);
            return true;
          });
    });
    port = server.bind_to_any_port("127.0.0.1");
    assert(port > 0);
  }
  ~Fixture() {
    server.stop();
    if (thread.joinable())
      thread.join();
  }
  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port) + "/v1";
  }
  OpenCodeGoModel model(std::size_t timeout = 3000) {
    if (!thread.joinable())
      thread = std::thread([this] { server.listen_after_bind(); });
    return OpenCodeGoModel({"fixture-key", endpoint(), timeout});
  }
  httplib::Request last() {
    std::scoped_lock lock(mutex);
    assert(!requests.empty());
    return requests.back();
  }
  void reply(std::string data) {
    std::scoped_lock lock(mutex);
    body = std::move(data);
  }
};

ModelRequest request(std::string model = "deepseek-v4-flash") {
  ModelRequest result;
  result.model = {"opencode-go", std::move(model)};
  result.session_id = "zeda-fixture-session-0123456789abcdef";
  result.reasoning_effort = ReasoningEffort::automatic;
  result.messages = {
      {"sys", Role::system, "Stable instructions", {}, std::nullopt},
      {"user", Role::user, "Use lookup", {}, std::nullopt}};
  result.tools = {
      {"lookup", "Read fixture data",
       R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})"}};
  return result;
}

void append_response(ModelRequest &req, const AssistantResponse &response) {
  Message message{"assistant", Role::assistant, response.content,
                  response.tool_calls, std::nullopt};
  message.model_state = response.model_state;
  req.messages.push_back(std::move(message));
  for (const auto &call : response.tool_calls)
    req.messages.push_back(
        {"result-" + call.id, Role::tool, "fixture-value", {}, call.id});
}

void headers_and_cache() {
  Fixture fixture(kResponsesDone + event({{"type", "ping"}, {"cost", 0.01}}));
  auto model = fixture.model();
  auto req = request("muse-spark-1.2-contributor");
  req.tools.insert(req.tools.begin(),
                   {"zlast", "Other tool", R"({"type":"object"})"});
  assert(model.complete(req, {}, {}));
  const auto first = fixture.last();
  assert(first.get_header_value("User-Agent").starts_with("zeda/"));
  assert(first.get_header_value("x-opencode-session") == req.session_id);
  assert(first.get_header_value("Authorization") == "Bearer fixture-key");
  auto json = Json::parse(first.body);
  assert(json.at("prompt_cache_key") == req.session_id);
  assert(json.at("tools").at(0).at("name") == "lookup");
  assert(json.at("tools").at(0).at("strict") == false);
  assert(!json.contains("prompt_cache_retention"));
  req.messages.push_back({"next", Role::user, "Continue", {}, std::nullopt});
  assert(model.complete(req, {}, {}));
  const auto second = fixture.last();
  assert(second.get_header_value("x-opencode-session") ==
         first.get_header_value("x-opencode-session"));
  const auto next = Json::parse(second.body);
  assert(next.at("instructions") == json.at("instructions"));
  assert(next.at("tools") == json.at("tools"));
  assert(next.at("input").at(0) == json.at("input").at(0));
  req.session_id = "bad\r\nx-injected: yes";
  const auto rejected = model.complete(req, {}, {});
  assert(!rejected && rejected.error().code == ErrorCode::invalid_argument);
}

void responses_replay() {
  const Json reasoning{{"id", "rs_1"},
                       {"type", "reasoning"},
                       {"encrypted_content", "opaque-fixture"},
                       {"summary", Json::array()}};
  const Json call{{"id", "fc_1"},
                  {"type", "function_call"},
                  {"call_id", "call_1"},
                  {"name", "lookup"},
                  {"arguments", R"({"key":"中文"})"},
                  {"status", "completed"}};
  std::string data = event({{"type", "response.output_item.added"},
                            {"output_index", 0},
                            {"item", reasoning}});
  auto initial = call;
  initial["arguments"] = "";
  initial["status"] = "in_progress";
  data += event({{"type", "response.output_item.added"},
                 {"output_index", 1},
                 {"item", initial}});
  data += event({{"type", "response.function_call_arguments.delta"},
                 {"output_index", 1},
                 {"item_id", "fc_1"},
                 {"delta", R"({"key":)"}});
  data += event({{"type", "response.function_call_arguments.delta"},
                 {"output_index", 1},
                 {"item_id", "fc_1"},
                 {"delta", R"("中文"})"}});
  data += event({{"type", "response.function_call_arguments.done"},
                 {"output_index", 1},
                 {"item_id", "fc_1"},
                 {"arguments", call.at("arguments")}});
  data += event({{"type", "response.output_item.done"},
                 {"output_index", 1},
                 {"item", call}});
  data += event({{"type", "response.completed"},
                 {"response",
                  {{"status", "completed"},
                   {"output", Json::array({reasoning, call})}}}});
  Fixture fixture(data);
  fixture.chunk_size = 1;
  auto model = fixture.model();
  auto req = request("muse-spark-1.2-contributor");
  const auto response = model.complete(req, {}, {});
  if (!response)
    std::cerr << response.error().message << '\n';
  assert(response && response.value().tool_calls.size() == 1);
  assert(response.value().tool_calls[0].arguments_json ==
         call.at("arguments").get<std::string>());
  append_response(req, response.value());
  fixture.reply(kResponsesDone);
  assert(model.complete(req, {}, {}));
  const auto input = Json::parse(fixture.last().body).at("input");
  assert(input.at(1) == reasoning);
  assert(input.at(2) == call);
  assert(input.at(3).at("type") == "function_call_output");
  assert(input.at(3).at("call_id") == "call_1");
}

void chat_replay() {
  const auto calls = Json::array(
      {{{"index", 0},
        {"id", "one"},
        {"type", "function"},
        {"function", {{"name", "lookup"}, {"arguments", R"({"key":"one"})"}}}},
       {{"index", 1},
        {"id", "two"},
        {"type", "function"},
        {"function",
         {{"name", "lookup"}, {"arguments", R"({"key":"two"})"}}}}});
  auto initial = calls;
  initial[0]["function"]["arguments"] = "";
  const auto data =
      event({{"choices", Json::array({{{"index", 0},
                                       {"delta",
                                        {{"reasoning_content", "Need both"},
                                         {"tool_calls", initial}}},
                                       {"finish_reason", nullptr}}})}}) +
      event(
          {{"choices",
            Json::array(
                {{{"index", 0},
                  {"delta",
                   {{"tool_calls",
                     Json::array({{{"index", 0},
                                   {"id", nullptr},
                                   {"type", nullptr},
                                   {"function",
                                    {{"name", nullptr},
                                     {"arguments", R"({"key":"one"})"}}}}})}}},
                  {"finish_reason", "tool_calls"}}})}});
  Fixture fixture(
      data +
      event({{"choices",
              Json::array({{{"index", 0},
                            {"delta", {{"role", "assistant"}, {"content", ""}}},
                            {"finish_reason", "tool_calls"}}})},
             {"usage",
              {{"prompt_tokens", 30},
               {"completion_tokens", 5},
               {"prompt_tokens_details",
                {{"cached_tokens", 20}, {"cache_write_tokens", 5}}}}}}));
  auto model = fixture.model();
  auto req = request();
  const auto response = model.complete(req, {}, {});
  assert(response && response.value().tool_calls.size() == 2);
  assert(response.value().usage.input_tokens == 30 &&
         response.value().usage.cached_input_tokens == 20 &&
         response.value().usage.cache_write_input_tokens == 5);
  append_response(req, response.value());
  fixture.reply(kChatDone);
  assert(model.complete(req, {}, {}));
  const auto json = Json::parse(fixture.last().body);
  assert(!json.contains("cache_control"));
  assert(!json.contains("prompt_cache_key"));
  assert(json.at("messages").at(2).at("reasoning_content") == "Need both");
  assert(json.at("messages").at(3).at("tool_call_id") == "one");
  assert(json.at("messages").at(4).at("tool_call_id") == "two");
}

std::string messages_tool_stream() {
  std::string data = event({{"type", "message_start"},
                            {"message",
                             {{"usage",
                               {{"input_tokens", 10},
                                {"cache_read_input_tokens", 20},
                                {"cache_creation_input_tokens", 30}}}}}});
  data += event({{"type", "content_block_start"},
                 {"index", 0},
                 {"content_block", {{"type", "thinking"}, {"thinking", ""}}}});
  data += event(
      {{"type", "content_block_delta"},
       {"index", 0},
       {"delta", {{"type", "thinking_delta"}, {"thinking", "中文思考"}}}});
  data += event(
      {{"type", "content_block_delta"},
       {"index", 0},
       {"delta",
        {{"type", "signature_delta"}, {"signature", "opaque-signature"}}}});
  data += event({{"type", "content_block_stop"}, {"index", 0}});
  for (int i = 1; i <= 2; ++i) {
    data += event({{"type", "content_block_start"},
                   {"index", i},
                   {"content_block",
                    {{"type", "tool_use"},
                     {"id", "call-" + std::to_string(i)},
                     {"name", "lookup"},
                     {"input", Json::object()}}}});
    data += event(
        {{"type", "content_block_delta"},
         {"index", i},
         {"delta",
          {{"type", "input_json_delta"}, {"partial_json", R"({"key":"x"})"}}}});
    data += event({{"type", "content_block_stop"}, {"index", i}});
  }
  data += event({{"type", "message_delta"},
                 {"delta", {{"stop_reason", "tool_use"}}},
                 {"usage", {{"output_tokens", 3}}}});
  return data + event({{"type", "message_stop"}});
}

void messages_replay() {
  Fixture fixture(messages_tool_stream());
  fixture.chunk_size = 1;
  auto model = fixture.model();
  auto req = request("minimax-m3");
  const auto response = model.complete(req, {}, {});
  assert(response && response.value().tool_calls.size() == 2);
  assert(response.value().usage.input_tokens == 60);
  assert(response.value().usage.cached_input_tokens == 20);
  assert(response.value().usage.cache_write_input_tokens == 30);
  assert(response.value().usage.output_tokens == 3);
  append_response(req, response.value());
  assert(model.complete(req, {}, {}));
  const auto json = Json::parse(fixture.last().body);
  assert(json.at("system").at(0).at("cache_control").at("type") == "ephemeral");
  assert(json.at("messages").size() == 3);
  const auto assistant = json.at("messages").at(1).at("content");
  assert(assistant.at(0).at("thinking") == "中文思考");
  assert(assistant.at(0).at("signature") == "opaque-signature");
  assert(!assistant.at(0).contains("cache_control"));
  const auto results = json.at("messages").at(2).at("content");
  assert(results.size() == 2);
  assert(results.at(0).at("tool_use_id") == "call-1");
  assert(results.at(1).at("tool_use_id") == "call-2");
}

void errors() {
  for (const int status : {401, 429, 500}) {
    Fixture fixture(R"({"error":{"message":"upstream denied fixture-key"}})");
    fixture.status = status;
    auto model = fixture.model();
    const auto result = model.complete(request(), {}, {});
    assert(!result);
    assert(result.error().retryable == (status != 401));
    assert(result.error().message.find(std::to_string(status)) !=
           std::string::npos);
    assert(result.error().message.find("fixture-key") == std::string::npos);
  }
  for (const auto &data :
       {std::string("data: {invalid secret-fixture-content}\n\n"),
        event(
            {{"choices",
              Json::array(
                  {{{"delta", {{"tool_calls", Json::array({{{"index", -1}}})}}},
                    {"finish_reason", "stop"}}})}}),
        event({{"choices", Json::array()}, {"usage", {{"prompt_tokens", -1}}}}),
        event(
            {{"choices", Json::array()}, {"usage", {{"prompt_tokens", 1.5}}}}),
        event({{"choices", Json::array({{{"delta", {{"content", "partial"}}},
                                         {"finish_reason", nullptr}}})}}),
        kChatDone + event({{"error", {{"message", "late stream error"}}}})}) {
    Fixture fixture(data);
    auto model = fixture.model();
    const auto result = model.complete(request(), {}, {});
    assert(!result);
    assert(result.error().message.find("secret-fixture-content") ==
           std::string::npos);
  }
  Fixture fixture(kChatDone);
  auto model = fixture.model();
  auto req = request();
  req.tools[0].input_schema_json = "bad";
  assert(!model.complete(req, {}, {}));
  CancellationSource source;
  source.cancel();
  const auto cancelled = model.complete(request(), {}, source.token());
  assert(!cancelled && cancelled.error().code == ErrorCode::cancelled);
}

void stream_lifecycle() {
  // The server waits for the first client delta. A buffering client cannot
  // finish.
  Fixture fixture(
      event({{"choices", Json::array({{{"delta", {{"content", "中文"}}},
                                       {"finish_reason", nullptr}}})}}) +
      kChatDone);
  std::mutex mutex;
  std::condition_variable condition;
  bool observed = false;
  fixture.chunk_size =
      event({{"choices", Json::array({{{"delta", {{"content", "中文"}}},
                                       {"finish_reason", nullptr}}})}})
          .size();
  fixture.after_first_chunk = [&](httplib::DataSink &) {
    std::unique_lock lock(mutex);
    assert(condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return observed; }));
  };
  auto model = fixture.model();
  std::string text;
  const auto result = model.complete(request(),
                                     [&](const ModelDelta &delta) {
                                       text += delta.text;
                                       {
                                         std::scoped_lock lock(mutex);
                                         observed = true;
                                       }
                                       condition.notify_one();
                                     },
                                     {});
  assert(result && text == "中文done");

  for (const bool cancel : {false, true}) {
    Fixture stalled("data: {\"choices\":[");
    stalled.chunk_size = 1024;
    CancellationSource source;
    std::mutex gate;
    std::condition_variable release;
    bool done = false;
    stalled.after_first_chunk = [&](httplib::DataSink &) {
      if (cancel)
        source.cancel();
      std::unique_lock lock(gate);
      release.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
    };
    auto timed_model = stalled.model(200);
    const auto stopped = timed_model.complete(request(), {}, source.token());
    {
      std::scoped_lock lock(gate);
      done = true;
    }
    release.notify_one();
    assert(!stopped && stopped.error().code == (cancel ? ErrorCode::cancelled
                                                       : ErrorCode::timeout));
  }
}

void framing_contract() {
  // SSE joins multiple data fields with LF; transport may split inside
  // UTF-8/CRLF.
  const std::string body =
      ": keepalive\r\nevent: completion\r\n"
      "data: {\r\ndata: "
      "\"choices\":[{\"index\":0,\"delta\":{\"content\":\"中文\"},\"finish_"
      "reason\":\"stop\"}]\r\ndata: }\r\n\r\ndata: [DONE]\r\n\r\n";
  for (const std::size_t size : {1U, 2U, 7U, 64U}) {
    Fixture fixture(body);
    fixture.chunk_size = size;
    auto model = fixture.model();
    std::string text;
    const auto response = model.complete(
        request(), [&](const ModelDelta &delta) { text += delta.text; }, {});
    assert(response && response.value().content == "中文" && text == "中文");
  }
}

void session_and_hooks() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "zeda-go-contract-XXXXXX")
          .string();
  std::vector<char> path(pattern.begin(), pattern.end());
  path.push_back('\0');
  assert(mkdtemp(path.data()));
  const std::filesystem::path root(path.data());
  std::string id;
  {
    zed::session::JsonlSessionStore store(root / "session.jsonl");
    assert(store.initialize({"stable-id", "title", root.string(), "opencode-go",
                             "deepseek-v4-flash"}));
    id = store.conversation_id().value();
    assert(store.begin_turn("t", {"u", Role::user, "hello", {}, std::nullopt}));
    Message message{"a", Role::assistant, "answer", {}, std::nullopt};
    message.model_state = "opaque-fixture-state";
    AgentHooks hooks(nullptr);
    const auto hooked =
        hooks.session_write_message("append_message", "t", message, {});
    assert(hooked && hooked.value().model_state == message.model_state);
    assert(store.append(hooked.value()));
    assert(store.finish_turn("t", SessionTurnOutcome::completed));
    assert(store.set_title("renamed"));
    assert(store.conversation_id().value() == id);
    assert(store.fork_to(root / "fork.jsonl"));
    auto req = request();
    const auto hooked_request = hooks.before_model_request("t", 0, req, {});
    assert(hooked_request &&
           hooked_request.value().session_id == req.session_id);
  }
  {
    zed::session::JsonlSessionStore restored(root / "session.jsonl");
    assert(restored.conversation_id().value() == id);
    assert(restored.load().value().back().model_state ==
           "opaque-fixture-state");
    zed::session::JsonlSessionStore forked(root / "fork.jsonl");
    assert(forked.conversation_id().value() != id);
    assert(forked.load().value().back().model_state == "opaque-fixture-state");
  }
  InMemorySessionStore a, b;
  assert(a.conversation_id().value() != b.conversation_id().value());
  std::filesystem::remove_all(root);
}

class LookupTool final : public Tool {
public:
  int calls{};
  ToolDefinition schema{
      "lookup", "Read fixture data",
      R"({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})"};
  const ToolDefinition &definition() const override { return schema; }
  Result<ToolResult> execute(const ToolCall &call, CancellationToken) override {
    ++calls;
    return Result<ToolResult>::success({call.id, "fixture-value"});
  }
};

void agent_loop_contract() {
  const auto tool_stream = event(
      {{"choices",
        Json::array(
            {{{"delta",
               {{"reasoning_content", "Need lookup"},
                {"tool_calls",
                 Json::array(
                     {{{"index", 0},
                       {"id", "lookup-1"},
                       {"function",
                        {{"name", "lookup"},
                         {"arguments",
                          R"({"key":"alpha","purpose":"Check fixture"})"}}}}})}}},
              {"finish_reason", "tool_calls"}}})}});
  Fixture fixture(tool_stream);
  fixture.response_for = [&](std::size_t index) {
    return index == 1 ? tool_stream : kChatDone;
  };
  auto model = fixture.model();
  ToolRegistry tools;
  auto tool = std::make_unique<LookupTool>();
  auto *lookup = tool.get();
  assert(tools.register_tool(std::move(tool)));
  InMemorySessionStore store;
  ApproximateTokenEstimator estimator;
  BasicContextManager context(estimator);
  AgentLoopConfig config;
  config.model_request = request();
  config.context_limits = {10000, 1000, 0};
  HookRegistry registry;
  AgentLoop loop(model, tools, store, context, config, &registry);
  int ends = 0, results = 0;
  const auto result = loop.run("lookup alpha", {}, [&](const AgentEvent &e) {
    if (e.type == AgentEventType::agent_end)
      ++ends;
    if (e.type == AgentEventType::tool_result)
      ++results;
  });
  if (!result)
    std::cerr << result.error().message << '\n';
  assert(result && result.value() == "done");
  assert(lookup->calls == 1 && results == 1 && ends == 1);
  const auto history = store.load().value();
  assert(history.size() == 4);
  assert(history[1].role == Role::assistant && !history[1].model_state.empty());
  assert(history[2].role == Role::tool &&
         history[2].tool_call_id == "lookup-1");
  const auto wire = fixture.last();
  const auto sent = Json::parse(wire.body);
  assert(sent.at("messages").at(2).at("reasoning_content") == "Need lookup");
  assert(wire.get_header_value("x-opencode-session") ==
         store.conversation_id().value());
  // Invalid streamed JSON never crosses the ToolRegistry execution boundary.
  {
    std::scoped_lock lock(fixture.mutex);
    fixture.response_for = {};
  }
  fixture.reply("data: {bad}\n\n");
  ends = 0;
  const auto failed = loop.run("retry", {}, [&](const AgentEvent &e) {
    if (e.type == AgentEventType::agent_end || e.type == AgentEventType::error)
      ++ends;
  });
  assert(!failed && lookup->calls == 1 && ends == 1);
}

int sdk_reference(std::string endpoint, std::string model_id) {
  OpenCodeGoModel model({"fixture-key", std::move(endpoint), 3000});
  const auto response = model.complete(request(std::move(model_id)), {}, {});
  if (!response) {
    std::cerr << response.error().message << '\n';
    return 1;
  }
  const auto &value = response.value();
  std::cout << Json{{"content", value.content},
                    {"input", value.usage.input_tokens},
                    {"cache_read", value.usage.cached_input_tokens},
                    {"cache_write", value.usage.cache_write_input_tokens},
                    {"output", value.usage.output_tokens}}
                   .dump()
            << '\n';
  return 0;
}

int live_probe(std::string model_id) {
  const char *key = std::getenv("OPENCODE_GO_API_KEY");
  if (key == nullptr || *key == '\0') {
    std::cerr << "OPENCODE_GO_API_KEY required\n";
    return 2;
  }
  OpenCodeGoModel model({key, "https://opencode.ai/zen/go/v1", 120'000});
  auto req = request(std::move(model_id));
  req.session_id = new_conversation_id();
  req.max_output_tokens = 1024;
  req.messages[0].content =
      "This is a synthetic API compatibility test. Use lookup once with key "
      "alpha, then answer DONE. Keep reasoning brief. Fixture data follows:\n";
  for (int i = 0; i < 600; ++i)
    req.messages[0].content +=
        "fixture " + std::to_string(i) + ": alpha=42 beta=17 gamma=9\n";
  for (int turn = 0; turn < 3; ++turn) {
    const auto start = std::chrono::steady_clock::now();
    const auto result = model.complete(req, {}, {});
    if (!result) {
      std::cout << Json{{"model", req.model.model},
                        {"turn", turn},
                        {"error", result.error().message}}
                       .dump()
                << '\n';
      return 1;
    }
    const auto &response = result.value();
    std::cout << Json{{"model", req.model.model},
                      {"turn", turn},
                      {"input", response.usage.input_tokens},
                      {"cache_read", response.usage.cached_input_tokens},
                      {"cache_write", response.usage.cache_write_input_tokens},
                      {"output", response.usage.output_tokens},
                      {"tool_calls", response.tool_calls.size()},
                      {"finish_reason",
                       static_cast<int>(response.finish_reason)},
                      {"milliseconds",
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count()}}
                     .dump()
              << std::endl;
    if (response.finish_reason == FinishReason::length ||
        response.finish_reason == FinishReason::content_filter)
      return 1;
    append_response(req, response);
    if (response.tool_calls.empty())
      req.messages.push_back({"next-" + std::to_string(turn),
                              Role::user,
                              "Reply DONE without tools.",
                              {},
                              std::nullopt});
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 4 && std::string(argv[1]) == "sdk-reference")
    return sdk_reference(argv[2], argv[3]);
  if (argc == 3 && std::string(argv[1]) == "live")
    return live_probe(argv[2]);
  assert(argc == 2);
  const std::string test = argv[1];
  if (test == "headers")
    headers_and_cache();
  else if (test == "responses")
    responses_replay();
  else if (test == "chat")
    chat_replay();
  else if (test == "messages")
    messages_replay();
  else if (test == "errors")
    errors();
  else if (test == "lifecycle")
    stream_lifecycle();
  else if (test == "session")
    session_and_hooks();
  else if (test == "loop")
    agent_loop_contract();
  else if (test == "framing")
    framing_contract();
  else {
    std::cerr << "unknown test\n";
    return 2;
  }
}
