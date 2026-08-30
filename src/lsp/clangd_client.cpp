#include "zed/lsp/clangd_client.hpp"

#include "zed/support/child_process.hpp"
#include "zed/support/unique_fd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace zed::lsp {

namespace {

using Json = nlohmann::json;
using core::ErrorCode;

constexpr std::size_t kMaximumHeaderBytes = 64 * 1024;
constexpr std::size_t kMaximumStderrBytes = 64 * 1024;

std::filesystem::path canonical_path(const std::filesystem::path &path) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  return error ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

bool inside_root(const std::filesystem::path &root,
                 const std::filesystem::path &path) {
  const auto relative = path.lexically_relative(root);
  if (relative.empty())
    return path == root;
  return *relative.begin() != "..";
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::string percent_encode_path(std::string_view path) {
  // File URIs preserve path separators but encode every other byte outside the
  // RFC 3986 unreserved set. Iterating bytes also keeps UTF-8 encoding stable.
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(path.size());
  for (const char raw_character : path) {
    const auto character = static_cast<unsigned char>(raw_character);
    const bool unreserved = std::isalnum(character) != 0 || character == '-' ||
                            character == '_' || character == '.' ||
                            character == '~' || character == '/';
    if (unreserved) {
      encoded.push_back(static_cast<char>(character));
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(kHex[character >> 4U]);
    encoded.push_back(kHex[character & 0x0FU]);
  }
  return encoded;
}

std::string file_uri(const std::filesystem::path &path) {
  return "file://" + percent_encode_path(canonical_path(path).generic_string());
}

std::optional<unsigned char> hex_value(char character) {
  if (character >= '0' && character <= '9')
    return static_cast<unsigned char>(character - '0');
  if (character >= 'a' && character <= 'f')
    return static_cast<unsigned char>(character - 'a' + 10);
  if (character >= 'A' && character <= 'F')
    return static_cast<unsigned char>(character - 'A' + 10);
  return std::nullopt;
}

std::optional<std::filesystem::path> path_from_file_uri(std::string_view uri) {
  constexpr std::string_view kPrefix = "file://";
  if (!uri.starts_with(kPrefix))
    return std::nullopt;
  uri.remove_prefix(kPrefix.size());
  std::string decoded;
  decoded.reserve(uri.size());
  for (std::size_t index = 0; index < uri.size(); ++index) {
    if (uri[index] != '%' || index + 2 >= uri.size()) {
      decoded.push_back(uri[index]);
      continue;
    }
    const auto high = hex_value(uri[index + 1]);
    const auto low = hex_value(uri[index + 2]);
    if (!high || !low) {
      decoded.push_back(uri[index]);
      continue;
    }
    decoded.push_back(static_cast<char>((*high << 4U) | *low));
    index += 2;
  }
  return canonical_path(decoded);
}

std::string diagnostic_severity(const Json &value) {
  if (!value.is_number_integer())
    return "ERROR";
  switch (value.get<int>()) {
  case 1:
    return "ERROR";
  case 2:
    return "WARN";
  case 3:
    return "INFO";
  case 4:
    return "HINT";
  default:
    return "ERROR";
  }
}

std::string diagnostic_code(const Json &value) {
  if (value.is_string())
    return value.get<std::string>();
  if (value.is_number_integer() || value.is_number_unsigned())
    return value.dump();
  return {};
}

core::Error process_error(std::string operation, std::string detail = {}) {
  if (!detail.empty())
    operation += ": " + detail;
  return {ErrorCode::tool_error, std::move(operation)};
}

} // namespace

std::filesystem::path discover_compile_commands_directory(
    const std::filesystem::path &workspace_root) {
  const auto root = canonical_path(workspace_root);
  const std::array candidates{
      root,
      root / "build",
      root / "build-debug",
  };
  for (const auto &candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate / "compile_commands.json",
                                         error) &&
        !error) {
      return candidate;
    }
  }
  return {};
}

class ClangdClient::Impl {
public:
  explicit Impl(ClangdConfig config)
      : config_(std::move(config)),
        workspace_root_(canonical_path(config_.workspace_root)) {
    if (config_.compile_commands_directory.empty()) {
      config_.compile_commands_directory =
          discover_compile_commands_directory(workspace_root_);
    } else {
      config_.compile_commands_directory =
          canonical_path(config_.compile_commands_directory);
    }
  }

  ~Impl() { shutdown(); }

  [[nodiscard]] bool supports(const std::filesystem::path &path) const {
    static const std::array<std::string_view, 10> kExtensions{
        ".c", ".cpp", ".cc", ".cxx", ".c++",
        ".h", ".hpp", ".hh", ".hxx", ".h++",
    };
    const auto extension = lowercase(path.extension().string());
    return std::find(kExtensions.begin(), kExtensions.end(), extension) !=
           kExtensions.end();
  }

  core::Result<std::vector<Diagnostic>>
  diagnostics(const std::filesystem::path &path,
              core::CancellationToken cancellation) {
    std::scoped_lock lock(mutex_);
    const auto file = resolve_source_path(path);
    if (!file)
      return core::Result<std::vector<Diagnostic>>::failure(file.error());
    const auto started = start(cancellation);
    if (!started)
      return core::Result<std::vector<Diagnostic>>::failure(started.error());
    const auto opened = open_document(file.value(), cancellation);
    if (!opened)
      return core::Result<std::vector<Diagnostic>>::failure(opened.error());
    if (opened.value().changed) {
      const auto waited = wait_for_diagnostics(
          file.value(), opened.value().generation,
          std::chrono::milliseconds(config_.request_timeout_ms), cancellation);
      if (!waited)
        return core::Result<std::vector<Diagnostic>>::failure(waited.error());
    }
    return core::Result<std::vector<Diagnostic>>::success(
        diagnostics_[file.value().string()]);
  }

  core::Result<std::string> query(QueryOperation operation,
                                  const std::filesystem::path &path,
                                  std::size_t line, std::size_t character,
                                  core::CancellationToken cancellation) {
    std::scoped_lock lock(mutex_);
    const auto file = resolve_source_path(path);
    if (!file)
      return core::Result<std::string>::failure(file.error());
    if (operation != QueryOperation::document_symbols &&
        (line == 0 || character == 0)) {
      return core::Result<std::string>::failure(
          {ErrorCode::invalid_argument,
           "clangd line and character must be greater than zero"});
    }
    const auto started = start(cancellation);
    if (!started)
      return core::Result<std::string>::failure(started.error());
    const auto opened = open_document(file.value(), cancellation);
    if (!opened)
      return core::Result<std::string>::failure(opened.error());

    std::string method;
    Json parameters{{"textDocument", {{"uri", file_uri(file.value())}}}};
    const Json position{
        {"line", line == 0 ? 0 : line - 1},
        {"character", character == 0 ? 0 : character - 1},
    };
    switch (operation) {
    case QueryOperation::hover:
      method = "textDocument/hover";
      parameters["position"] = position;
      break;
    case QueryOperation::definition:
      method = "textDocument/definition";
      parameters["position"] = position;
      break;
    case QueryOperation::references:
      method = "textDocument/references";
      parameters["position"] = position;
      parameters["context"] = {{"includeDeclaration", true}};
      break;
    case QueryOperation::document_symbols:
      method = "textDocument/documentSymbol";
      break;
    }

    const auto response = request(
        method, std::move(parameters),
        std::chrono::milliseconds(config_.request_timeout_ms), cancellation);
    if (!response)
      return core::Result<std::string>::failure(response.error());
    if (response.value().is_null() ||
        (response.value().is_array() && response.value().empty())) {
      return core::Result<std::string>::success("No clangd results.");
    }
    return core::Result<std::string>::success(response.value().dump(2));
  }

private:
  struct OpenedDocument {
    std::size_t version{};
    std::size_t generation{};
    bool changed{};
  };

  core::Result<std::filesystem::path>
  resolve_source_path(const std::filesystem::path &path) const {
    const auto candidate = path.is_absolute() ? path : workspace_root_ / path;
    const auto resolved = canonical_path(candidate);
    if (!inside_root(workspace_root_, resolved)) {
      return core::Result<std::filesystem::path>::failure(
          {ErrorCode::invalid_argument, "clangd path escapes workspace root"});
    }
    if (!supports(resolved)) {
      return core::Result<std::filesystem::path>::failure(
          {ErrorCode::invalid_argument,
           "clangd only supports C and C++ source files"});
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
      return core::Result<std::filesystem::path>::failure(
          {ErrorCode::not_found, "clangd source file does not exist"});
    }
    return core::Result<std::filesystem::path>::success(resolved);
  }

  core::Result<void> start(core::CancellationToken cancellation) {
    if (pid_ > 0)
      return core::Result<void>::success();
    if (config_.executable.empty()) {
      return core::Result<void>::failure(
          {ErrorCode::invalid_argument, "clangd executable is empty"});
    }
    if (cancellation.is_cancelled())
      return core::Result<void>::failure(
          {ErrorCode::cancelled, "clangd start cancelled"});

    auto spawn_lock = support::lock_process_spawn();
    int input_pipe[2];
    if (!support::create_cloexec_pipe(input_pipe)) {
      return core::Result<void>::failure(
          process_error("cannot create clangd pipes", std::strerror(errno)));
    }
    support::UniqueFd input_read(input_pipe[0]);
    support::UniqueFd input_write(input_pipe[1]);
    int output_pipe[2];
    if (!support::create_cloexec_pipe(output_pipe)) {
      return core::Result<void>::failure(
          process_error("cannot create clangd pipes", std::strerror(errno)));
    }
    support::UniqueFd output_read(output_pipe[0]);
    support::UniqueFd output_write(output_pipe[1]);
    int error_pipe[2];
    if (!support::create_cloexec_pipe(error_pipe)) {
      return core::Result<void>::failure(
          process_error("cannot create clangd pipes", std::strerror(errno)));
    }
    support::UniqueFd error_read(error_pipe[0]);
    support::UniqueFd error_write(error_pipe[1]);

    std::vector<std::string> arguments{config_.executable, "--clang-tidy"};
    if (config_.background_index)
      arguments.push_back("--background-index");
    if (!config_.compile_commands_directory.empty()) {
      arguments.push_back("--compile-commands-dir=" +
                          config_.compile_commands_directory.string());
    }
    support::SpawnOptions spawn_options;
    spawn_options.executable = arguments.front();
    spawn_options.arguments.assign(arguments.begin() + 1, arguments.end());
    spawn_options.working_directory = workspace_root_;
    spawn_options.duplicate_descriptors = {
        {input_read.get(), STDIN_FILENO},
        {output_write.get(), STDOUT_FILENO},
        {error_write.get(), STDERR_FILENO},
    };
    spawn_options.close_descriptors = {
        input_read.get(),   input_write.get(), output_read.get(),
        output_write.get(), error_read.get(),  error_write.get(),
    };
    spawn_options.additional_environment_variables =
        config_.environment_allowlist;
    pid_t child = -1;
    const int spawn_error = support::spawn_process(spawn_options, child);
    if (spawn_error != 0) {
      return core::Result<void>::failure(
          process_error("cannot start clangd", std::strerror(spawn_error)));
    }

    spawn_lock.unlock();
    input_read.reset();
    output_write.reset();
    error_write.reset();
    pid_ = child;
    input_fd_ = std::move(input_write);
    output_fd_ = std::move(output_read);
    error_fd_ = std::move(error_read);
    set_nonblocking(output_fd_.get());
    set_nonblocking(error_fd_.get());
    std::signal(SIGPIPE, SIG_IGN);

    const Json initialize_parameters{
        {"processId", static_cast<long long>(getpid())},
        {"rootUri", file_uri(workspace_root_)},
        {"workspaceFolders",
         Json::array(
             {{{"name", "workspace"}, {"uri", file_uri(workspace_root_)}}})},
        {"capabilities",
         {{"workspace", {{"configuration", true}}},
          {"textDocument",
           {{"synchronization", {{"didOpen", true}, {"didChange", true}}},
            {"publishDiagnostics", {{"versionSupport", true}}}}}}},
    };
    const auto initialized = request(
        "initialize", initialize_parameters,
        std::chrono::milliseconds(config_.initialize_timeout_ms), cancellation);
    if (!initialized) {
      const auto error = initialized.error();
      terminate_process();
      return core::Result<void>::failure(error);
    }
    const auto notification = notify("initialized", Json::object());
    if (!notification) {
      const auto error = notification.error();
      terminate_process();
      return core::Result<void>::failure(error);
    }
    initialized_ = true;
    return core::Result<void>::success();
  }

  static void set_nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0)
      fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
  }

  core::Result<void> send(const Json &message) {
    const std::string body = message.dump();
    const std::string framed =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    std::size_t written = 0;
    while (written < framed.size()) {
      const ssize_t count = write(input_fd_.get(), framed.data() + written,
                                  framed.size() - written);
      if (count > 0) {
        written += static_cast<std::size_t>(count);
        continue;
      }
      if (count < 0 && errno == EINTR)
        continue;
      return core::Result<void>::failure(
          process_error("cannot write clangd request", std::strerror(errno)));
    }
    return core::Result<void>::success();
  }

  core::Result<void> notify(std::string_view method, Json parameters) {
    return send({{"jsonrpc", "2.0"},
                 {"method", method},
                 {"params", std::move(parameters)}});
  }

  core::Result<Json> request(std::string_view method, Json parameters,
                             std::chrono::milliseconds timeout,
                             core::CancellationToken cancellation) {
    const std::uint64_t id = ++request_id_;
    const auto sent = send({{"jsonrpc", "2.0"},
                            {"id", id},
                            {"method", method},
                            {"params", std::move(parameters)}});
    if (!sent)
      return core::Result<Json>::failure(sent.error());
    return await_response(id, timeout, cancellation);
  }

  core::Result<Json> await_response(std::uint64_t id,
                                    std::chrono::milliseconds timeout,
                                    core::CancellationToken cancellation) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      const auto message = read_message(deadline, cancellation);
      if (!message)
        return core::Result<Json>::failure(message.error());
      if (message.value().contains("method")) {
        const auto handled = handle_server_message(message.value());
        if (!handled)
          return core::Result<Json>::failure(handled.error());
        continue;
      }
      const auto iterator = message.value().find("id");
      if (iterator == message.value().end() ||
          !iterator->is_number_unsigned() ||
          iterator->get<std::uint64_t>() != id) {
        continue;
      }
      if (const auto error = message.value().find("error");
          error != message.value().end()) {
        return core::Result<Json>::failure(
            process_error("clangd request failed", error->dump()));
      }
      const auto result = message.value().find("result");
      return core::Result<Json>::success(
          result == message.value().end() ? Json{} : *result);
    }
  }

  core::Result<std::optional<Json>> extract_message() {
    // LSP frames JSON with an ASCII header and a byte-counted body. Keep an
    // incomplete frame buffered; consume it only after the full body arrives.
    const auto header_end = output_buffer_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
      if (output_buffer_.size() > kMaximumHeaderBytes) {
        return core::Result<std::optional<Json>>::failure(
            process_error("clangd JSON-RPC header is too large"));
      }
      return core::Result<std::optional<Json>>::success(std::nullopt);
    }
    const auto length_start = output_buffer_.find("Content-Length:");
    if (length_start == std::string::npos || length_start > header_end) {
      return core::Result<std::optional<Json>>::failure(
          process_error("clangd JSON-RPC message has no Content-Length"));
    }
    const auto value_start = output_buffer_.find_first_not_of(
        " \t", length_start + std::string_view("Content-Length:").size());
    const auto value_end = output_buffer_.find("\r\n", value_start);
    if (value_start == std::string::npos || value_end == std::string::npos ||
        value_end > header_end) {
      return core::Result<std::optional<Json>>::failure(
          process_error("clangd JSON-RPC Content-Length is malformed"));
    }
    std::size_t content_length = 0;
    const auto parsed =
        std::from_chars(output_buffer_.data() + value_start,
                        output_buffer_.data() + value_end, content_length);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != output_buffer_.data() + value_end ||
        content_length > config_.max_message_bytes) {
      return core::Result<std::optional<Json>>::failure(
          process_error("clangd JSON-RPC Content-Length is invalid"));
    }
    const std::size_t body_start = header_end + 4;
    if (output_buffer_.size() - body_start < content_length)
      return core::Result<std::optional<Json>>::success(std::nullopt);
    const std::string body = output_buffer_.substr(body_start, content_length);
    output_buffer_.erase(0, body_start + content_length);
    auto message = Json::parse(body, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
      return core::Result<std::optional<Json>>::failure(
          process_error("cannot parse clangd JSON-RPC response"));
    }
    return core::Result<std::optional<Json>>::success(std::move(message));
  }

  core::Result<Json>
  read_message(std::chrono::steady_clock::time_point deadline,
               core::CancellationToken cancellation) {
    while (true) {
      const auto extracted = extract_message();
      if (!extracted)
        return core::Result<Json>::failure(extracted.error());
      if (extracted.value().has_value())
        return core::Result<Json>::success(std::move(*extracted.value()));
      if (cancellation.is_cancelled()) {
        return core::Result<Json>::failure(
            {ErrorCode::cancelled, "clangd request cancelled"});
      }
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return core::Result<Json>::failure(
            {ErrorCode::timeout, "clangd request timed out"});
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
              .count();
      const int poll_timeout =
          static_cast<int>(std::min<long long>(remaining, 50));
      std::array<pollfd, 2> descriptors{{
          {output_fd_.get(), POLLIN | POLLHUP, 0},
          {error_fd_.get(), POLLIN | POLLHUP, 0},
      }};
      const int ready =
          poll(descriptors.data(), descriptors.size(), poll_timeout);
      if (ready < 0 && errno == EINTR)
        continue;
      if (ready < 0) {
        return core::Result<Json>::failure(
            process_error("cannot poll clangd", std::strerror(errno)));
      }
      if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0)
        drain_stderr();
      if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
        std::array<char, 8192> buffer{};
        const ssize_t count =
            read(output_fd_.get(), buffer.data(), buffer.size());
        if (count > 0) {
          output_buffer_.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
          return core::Result<Json>::failure(
              process_error("clangd exited", stderr_tail_));
        } else if (errno != EAGAIN && errno != EINTR) {
          return core::Result<Json>::failure(process_error(
              "cannot read clangd response", std::strerror(errno)));
        }
      }
      if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
        return core::Result<Json>::failure(
            process_error("clangd output pipe failed", stderr_tail_));
      }
    }
  }

  void drain_stderr() {
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t count = read(error_fd_.get(), buffer.data(), buffer.size());
      if (count <= 0)
        break;
      stderr_tail_.append(buffer.data(), static_cast<std::size_t>(count));
      if (stderr_tail_.size() > kMaximumStderrBytes) {
        stderr_tail_.erase(0, stderr_tail_.size() - kMaximumStderrBytes);
      }
    }
  }

  core::Result<void> handle_server_message(const Json &message) {
    const auto method = message.find("method");
    if (method == message.end() || !method->is_string())
      return core::Result<void>::success();
    if (*method == "textDocument/publishDiagnostics") {
      update_diagnostics(message.value("params", Json::object()));
      return core::Result<void>::success();
    }
    const auto id = message.find("id");
    if (id == message.end())
      return core::Result<void>::success();

    Json result = nullptr;
    if (*method == "workspace/configuration") {
      const auto items =
          message.value("params", Json::object()).value("items", Json::array());
      result = Json::array();
      if (items.is_array()) {
        for (std::size_t index = 0; index < items.size(); ++index)
          result.push_back(nullptr);
      }
    } else if (*method == "workspace/workspaceFolders") {
      result = Json::array(
          {{{"name", "workspace"}, {"uri", file_uri(workspace_root_)}}});
    }
    return send({{"jsonrpc", "2.0"}, {"id", *id}, {"result", result}});
  }

  void update_diagnostics(const Json &parameters) {
    if (!parameters.is_object())
      return;
    const auto uri = parameters.find("uri");
    const auto items = parameters.find("diagnostics");
    if (uri == parameters.end() || !uri->is_string() ||
        items == parameters.end() || !items->is_array()) {
      return;
    }
    const auto path = path_from_file_uri(uri->get<std::string>());
    if (!path || !inside_root(workspace_root_, *path))
      return;

    std::vector<Diagnostic> next;
    next.reserve(std::min(items->size(), config_.max_diagnostics));
    for (const auto &item : *items) {
      if (!item.is_object() || next.size() >= config_.max_diagnostics)
        break;
      const auto range = item.find("range");
      const auto message = item.find("message");
      if (range == item.end() || !range->is_object() || message == item.end() ||
          !message->is_string()) {
        continue;
      }
      const auto start = range->find("start");
      if (start == range->end() || !start->is_object())
        continue;
      const auto line = start->value("line", 0U);
      const auto character = start->value("character", 0U);
      next.push_back({
          *path,
          static_cast<std::size_t>(line) + 1,
          static_cast<std::size_t>(character) + 1,
          diagnostic_severity(item.value("severity", 1)),
          message->get<std::string>(),
          diagnostic_code(item.value("code", Json{})),
      });
    }
    const auto key = path->string();
    diagnostics_[key] = std::move(next);
    ++diagnostic_generations_[key];
  }

  core::Result<OpenedDocument>
  open_document(const std::filesystem::path &path,
                core::CancellationToken cancellation) {
    if (cancellation.is_cancelled()) {
      return core::Result<OpenedDocument>::failure(
          {ErrorCode::cancelled, "clangd document open cancelled"});
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
      return core::Result<OpenedDocument>::failure(
          process_error("cannot read clangd source file"));
    }
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (text.size() > config_.max_message_bytes / 2) {
      return core::Result<OpenedDocument>::failure(
          {ErrorCode::invalid_argument, "clangd source file is too large"});
    }

    const auto key = path.string();
    const auto previous_text = document_contents_.find(key);
    if (previous_text != document_contents_.end() &&
        previous_text->second == text) {
      return core::Result<OpenedDocument>::success(
          {document_versions_[key], diagnostic_generations_[key], false});
    }
    const std::size_t previous_generation = diagnostic_generations_[key];
    auto &version = document_versions_[key];
    ++version;
    core::Result<void> sent = core::Result<void>::success();
    if (version == 1) {
      const char *language_id =
          lowercase(path.extension().string()) == ".c" ? "c" : "cpp";
      sent = notify("textDocument/didOpen", {{"textDocument",
                                              {{"uri", file_uri(path)},
                                               {"languageId", language_id},
                                               {"version", version},
                                               {"text", text}}}});
    } else {
      sent = notify(
          "textDocument/didChange",
          {{"textDocument", {{"uri", file_uri(path)}, {"version", version}}},
           {"contentChanges", Json::array({{{"text", text}}})}});
    }
    if (!sent)
      return core::Result<OpenedDocument>::failure(sent.error());
    document_contents_[key] = std::move(text);
    return core::Result<OpenedDocument>::success(
        {version, previous_generation, true});
  }

  core::Result<void> wait_for_diagnostics(
      const std::filesystem::path &path, std::size_t previous_generation,
      std::chrono::milliseconds timeout, core::CancellationToken cancellation) {
    const auto key = path.string();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (diagnostic_generations_[key] <= previous_generation) {
      const auto message = read_message(deadline, cancellation);
      if (!message)
        return core::Result<void>::failure(message.error());
      if (message.value().contains("method")) {
        const auto handled = handle_server_message(message.value());
        if (!handled)
          return handled;
      }
    }
    return core::Result<void>::success();
  }

  void shutdown() {
    std::scoped_lock lock(mutex_);
    if (pid_ <= 0)
      return;
    if (initialized_) {
      (void)request("shutdown", nullptr, std::chrono::milliseconds(500), {});
      (void)notify("exit", nullptr);
    }
    terminate_process();
  }

  void terminate_process() {
    input_fd_.reset();
    bool exited = false;
    int status = 0;
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (support::try_reap_child(pid_, status)) {
        exited = true;
        break;
      }
      usleep(10'000);
    }
    support::terminate_process_group(pid_, std::chrono::milliseconds(50),
                                     exited, status);
    output_fd_.reset();
    error_fd_.reset();
    pid_ = -1;
    initialized_ = false;
  }

  ClangdConfig config_;
  std::filesystem::path workspace_root_;
  std::mutex mutex_;
  pid_t pid_{-1};
  support::UniqueFd input_fd_;
  support::UniqueFd output_fd_;
  support::UniqueFd error_fd_;
  bool initialized_{false};
  std::uint64_t request_id_{};
  std::string output_buffer_;
  std::string stderr_tail_;
  std::map<std::string, std::size_t> document_versions_;
  std::map<std::string, std::string> document_contents_;
  std::map<std::string, std::size_t> diagnostic_generations_;
  std::map<std::string, std::vector<Diagnostic>> diagnostics_;
};

ClangdClient::ClangdClient(ClangdConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

ClangdClient::~ClangdClient() = default;

bool ClangdClient::supports(const std::filesystem::path &path) const {
  return impl_->supports(path);
}

core::Result<std::vector<Diagnostic>>
ClangdClient::diagnostics(const std::filesystem::path &path,
                          core::CancellationToken cancellation) {
  return impl_->diagnostics(path, cancellation);
}

core::Result<std::string>
ClangdClient::query(QueryOperation operation, const std::filesystem::path &path,
                    std::size_t line, std::size_t character,
                    core::CancellationToken cancellation) {
  return impl_->query(operation, path, line, character, cancellation);
}

std::string format_diagnostics(std::string_view heading,
                               const std::vector<Diagnostic> &diagnostics) {
  if (diagnostics.empty())
    return {};
  std::ostringstream output;
  output << heading << '\n';
  for (const auto &diagnostic : diagnostics) {
    output << diagnostic.severity << " [" << diagnostic.line << ':'
           << diagnostic.character << "] " << diagnostic.message;
    if (!diagnostic.code.empty())
      output << " [" << diagnostic.code << ']';
    output << '\n';
  }
  return output.str();
}

} // namespace zed::lsp
