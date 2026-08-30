#include "zed/plugins/plugin_sdk.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;

constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::size_t kMaximumSourceBytes = 1024 * 1024;
constexpr std::size_t kChunkLines = 100;
constexpr std::size_t kChunkOverlapLines = 10;
constexpr std::size_t kMaximumEvidenceBytes = 72 * 1024;
constexpr std::size_t kMaximumTuiPages = 64;
constexpr std::size_t kMaximumTuiMarkdownBytes = 12 * 1024 * 1024;
constexpr std::size_t kMaximumTuiDocumentBytes = 16 * 1024 * 1024;

std::string text(ZedaStringView value) {
  if (value.data == nullptr || value.size == 0)
    return {};
  return {value.data, value.size};
}

ZedaStringView view(std::string_view value) {
  return {value.data(), value.size()};
}

void write_sink(ZedaTextSinkV1 sink, std::string_view value) {
  if (sink.write != nullptr)
    sink.write(sink.context, view(value));
}

bool cancelled(ZedaCancellationV1 cancellation) {
  return cancellation.is_cancelled != nullptr &&
         cancellation.is_cancelled(cancellation.context) != 0;
}

void event(ZedaEventCallbackV1 callback, void *context, std::uint32_t kind,
           std::string_view value) {
  if (callback != nullptr)
    callback(context, kind, view(value));
}

std::string trim(std::string value) {
  const auto visible = [](unsigned char character) {
    return std::isspace(character) == 0;
  };
  const auto begin = std::find_if(value.begin(), value.end(), visible);
  const auto end = std::find_if(value.rbegin(), value.rend(), visible).base();
  if (begin >= end)
    return {};
  return {begin, end};
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

std::string read_file(const std::filesystem::path &path,
                      std::size_t maximum_bytes = kMaximumSourceBytes) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  std::string content;
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0 || static_cast<std::uintmax_t>(length) > maximum_bytes)
    return {};
  content.resize(static_cast<std::size_t>(length));
  input.seekg(0, std::ios::beg);
  input.read(content.data(), static_cast<std::streamsize>(content.size()));
  if (!input && !input.eof())
    return {};
  return content;
}

bool atomic_write(const std::filesystem::path &path, std::string_view content,
                  std::string &error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(path.parent_path(), filesystem_error);
  if (filesystem_error) {
    error = "cannot create DeepWiki directory: " + filesystem_error.message();
    return false;
  }
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = "cannot write temporary file: " + temporary;
      return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.flush();
    if (!output) {
      error = "cannot flush temporary file: " + temporary;
      return false;
    }
  }
  std::filesystem::rename(temporary, path, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(path, filesystem_error);
    filesystem_error.clear();
    std::filesystem::rename(temporary, path, filesystem_error);
  }
  if (filesystem_error) {
    error = "cannot replace DeepWiki file: " + filesystem_error.message();
    return false;
  }
  return true;
}

std::string hash_content(std::string_view content) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char raw_byte : content) {
    const auto byte = static_cast<unsigned char>(raw_byte);
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  std::ostringstream formatted;
  formatted << std::hex << std::setw(16) << std::setfill('0') << hash;
  return formatted.str();
}

bool path_is_inside(const std::filesystem::path &root,
                    const std::filesystem::path &candidate) {
  const auto relative = candidate.lexically_relative(root);
  if (relative.empty())
    return candidate == root;
  return *relative.begin() != "..";
}

bool is_cpp_path(const std::filesystem::path &path) {
  static const std::set<std::string> extensions{
      ".c", ".cc", ".cpp", ".cxx", ".c++", ".h", ".hh", ".hpp", ".hxx", ".h++",
  };
  return extensions.contains(lowercase(path.extension().string()));
}

bool supported_path(const std::filesystem::path &path) {
  if (is_cpp_path(path))
    return true;
  const auto filename = lowercase(path.filename().string());
  const auto extension = lowercase(path.extension().string());
  return filename == "cmakelists.txt" || filename.starts_with("readme") ||
         extension == ".cmake" || extension == ".json" ||
         extension == ".yaml" || extension == ".yml" || extension == ".toml" ||
         extension == ".md";
}

bool ignored_directory(std::string_view name) {
  return name == ".git" || name == ".zed" || name == ".cache" ||
         name == "node_modules" || name == "cmake-build-debug" ||
         name == "cmake-build-release" || name == "build" ||
         name == "build-debug" || name == "build-release";
}

double path_weight(std::string_view path) {
  const auto normalized = lowercase(std::string(path));
  if (normalized.find("/test") != std::string::npos ||
      normalized.starts_with("test") ||
      normalized.find("/example") != std::string::npos ||
      normalized.starts_with("example"))
    return 0.45;
  if (normalized.find("third_party") != std::string::npos ||
      normalized.find("vendor") != std::string::npos ||
      normalized.find("generated") != std::string::npos)
    return 0.30;
  if (normalized.find("/src/") != std::string::npos ||
      normalized.find("/lib/") != std::string::npos ||
      normalized.starts_with("src/") || normalized.starts_with("lib/"))
    return 1.25;
  return 1.0;
}

std::string strip_code_fence(std::string value) {
  value = trim(std::move(value));
  if (!value.starts_with("```"))
    return value;
  const auto first_newline = value.find('\n');
  const auto last_fence = value.rfind("```");
  if (first_newline == std::string::npos || last_fence <= first_newline)
    return value;
  return trim(value.substr(first_newline + 1, last_fence - first_newline - 1));
}

std::string safe_id(std::string value) {
  std::string result;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isalnum(character) != 0) {
      result.push_back(static_cast<char>(std::tolower(character)));
    } else if ((character == '-' || character == '_') && !result.empty()) {
      result.push_back(static_cast<char>(character));
    }
  }
  if (result.empty())
    result = "page";
  return result.substr(0, 64);
}

std::string git_commit(const std::filesystem::path &workspace) {
  auto git_directory = workspace / ".git";
  if (std::filesystem::is_regular_file(git_directory)) {
    const auto pointer = trim(read_file(git_directory, 4096));
    constexpr std::string_view prefix = "gitdir:";
    if (!pointer.starts_with(prefix))
      return {};
    auto target = std::filesystem::path(trim(pointer.substr(prefix.size())));
    if (target.is_relative())
      target = workspace / target;
    git_directory = std::filesystem::weakly_canonical(target);
  }
  const auto head = trim(read_file(git_directory / "HEAD", 4096));
  constexpr std::string_view reference_prefix = "ref:";
  if (!head.starts_with(reference_prefix))
    return head;
  const auto reference = trim(head.substr(reference_prefix.size()));
  const auto loose = trim(read_file(git_directory / reference, 4096));
  if (!loose.empty())
    return loose;
  std::istringstream packed(
      read_file(git_directory / "packed-refs", 1024 * 1024));
  std::string line;
  while (std::getline(packed, line)) {
    if (line.empty() || line.front() == '#' || line.front() == '^')
      continue;
    const auto separator = line.find(' ');
    if (separator != std::string::npos &&
        line.substr(separator + 1) == reference)
      return line.substr(0, separator);
  }
  return {};
}

std::u32string utf8_codepoints(std::string_view value) {
  std::u32string result;
  for (std::size_t index = 0; index < value.size();) {
    const auto first = static_cast<unsigned char>(value[index]);
    std::uint32_t codepoint = first;
    std::size_t width = 1;
    if ((first & 0xE0U) == 0xC0U && index + 1 < value.size()) {
      codepoint = first & 0x1FU;
      width = 2;
    } else if ((first & 0xF0U) == 0xE0U && index + 2 < value.size()) {
      codepoint = first & 0x0FU;
      width = 3;
    } else if ((first & 0xF8U) == 0xF0U && index + 3 < value.size()) {
      codepoint = first & 0x07U;
      width = 4;
    }
    for (std::size_t offset = 1; offset < width; ++offset) {
      const auto next = static_cast<unsigned char>(value[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        width = 1;
        codepoint = first;
        break;
      }
      codepoint = (codepoint << 6U) | (next & 0x3FU);
    }
    result.push_back(static_cast<char32_t>(codepoint));
    index += width;
  }
  return result;
}

std::size_t related_text_score(std::string_view question,
                               std::string_view candidate) {
  const auto lowered_question = lowercase(std::string(question));
  const auto lowered_candidate = lowercase(std::string(candidate));
  const auto question32 = utf8_codepoints(lowered_question);
  const auto candidate32 = utf8_codepoints(lowered_candidate);
  std::size_t score = 0;
  for (std::size_t index = 0; index + 1 < question32.size(); ++index) {
    if (question32[index] < 0x80 || question32[index + 1] < 0x80)
      continue;
    const std::u32string pair{question32[index], question32[index + 1]};
    if (candidate32.find(pair) != std::u32string::npos)
      ++score;
  }
  std::string token;
  const auto flush = [&] {
    if (token.size() >= 2 && lowered_candidate.find(token) != std::string::npos)
      score += 2;
    token.clear();
  };
  for (const char raw_character : lowered_question) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isalnum(character) != 0 || character == '_')
      token.push_back(static_cast<char>(character));
    else
      flush();
  }
  flush();
  return score;
}

class Statement {
public:
  Statement(sqlite3 *database, const char *sql) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK)
      statement_ = nullptr;
  }
  ~Statement() {
    if (statement_ != nullptr)
      sqlite3_finalize(statement_);
  }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  [[nodiscard]] sqlite3_stmt *get() const { return statement_; }
  [[nodiscard]] explicit operator bool() const { return statement_ != nullptr; }

private:
  sqlite3_stmt *statement_{};
};

bool sql_exec(sqlite3 *database, const char *sql, std::string &error) {
  char *message = nullptr;
  const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (status == SQLITE_OK)
    return true;
  error = message == nullptr ? sqlite3_errmsg(database) : message;
  sqlite3_free(message);
  return false;
}

void bind_text(sqlite3_stmt *statement, int index, std::string_view value) {
  sqlite3_bind_text(statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string column_text(sqlite3_stmt *statement, int column) {
  const auto *value = sqlite3_column_text(statement, column);
  if (value == nullptr)
    return {};
  return reinterpret_cast<const char *>(value);
}

struct PagePlan {
  std::string id;
  std::string title;
  std::string description;
  std::vector<std::string> queries;
};

struct RefreshResult {
  std::size_t scanned{};
  std::size_t changed{};
  std::size_t removed{};
  bool topology_changed{false};
  bool degraded{true};
  std::set<std::string> changed_paths;
};

class DeepWikiPlugin {
public:
  ~DeepWikiPlugin() { stop_server(); }

  int initialize(const ZedaHostApiV1 *host, ZedaTextSinkV1 error) {
    if (host == nullptr || host->abi_version != ZEDA_PLUGIN_ABI_VERSION) {
      write_sink(error, "DeepWiki received an incompatible host API");
      return 1;
    }
    host_ = host;
    workspace_ = std::filesystem::weakly_canonical(text(host->workspace_root));
    resources_ = std::filesystem::weakly_canonical(text(host->resource_root));
    cache_ = workspace_ / ".zed" / "deepwiki";

    const std::string options =
        R"([{"value":"generate","description":"建立索引并生成 Wiki。"},{"value":"update","description":"增量更新索引和过期页面。"},{"value":"status","description":"查看索引和 clangd 状态。"},{"value":"tui","description":"在终端内浏览 Wiki。","view":"document"},{"value":"open","description":"启动并打开本地网页。"}])";
    const ZedaCommandV1 command{
        view("deepwiki"),
        view("Generate, update, inspect, or browse the local C/C++ DeepWiki."),
        view(options),
        this,
        execute_command,
    };
    if (host_->register_command(host_->context, &command, error) != 0)
      return 1;

    const ZedaToolV1 tools[]{
        {view("deepwiki_structure"),
         view("Read the generated DeepWiki table of contents for this "
              "workspace."),
         view(R"({"type":"object","properties":{}})"), this, execute_structure},
        {view("deepwiki_contents"),
         view("Read one generated DeepWiki page by page id."),
         view(
             R"({"type":"object","required":["page"],"properties":{"page":{"type":"string","minLength":1}}})"),
         this, execute_contents},
        {view("deepwiki_search"),
         view("Search the local C/C++ DeepWiki source index and return cited "
              "excerpts."),
         view(
             R"({"type":"object","required":["query"],"properties":{"query":{"type":"string","minLength":1},"limit":{"type":"integer","minimum":1,"maximum":20}}})"),
         this, execute_search},
    };
    for (const auto &tool : tools) {
      if (host_->register_tool(host_->context, &tool, error) != 0)
        return 1;
    }
    return 0;
  }

  void shutdown() { stop_server(); }

private:
  static int execute_command(void *context, ZedaStringView arguments,
                             ZedaCancellationV1 cancellation,
                             ZedaEventCallbackV1 on_event, void *event_context,
                             ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    return static_cast<DeepWikiPlugin *>(context)->command(
        trim(text(arguments)), cancellation, on_event, event_context, output,
        error);
  }

  static int execute_structure(void *context, ZedaStringView,
                               ZedaCancellationV1, ZedaTextSinkV1 output,
                               ZedaTextSinkV1 error) {
    return static_cast<DeepWikiPlugin *>(context)->structure(output, error);
  }

  static int execute_contents(void *context, ZedaStringView arguments,
                              ZedaCancellationV1, ZedaTextSinkV1 output,
                              ZedaTextSinkV1 error) {
    return static_cast<DeepWikiPlugin *>(context)->contents(text(arguments),
                                                            output, error);
  }

  static int execute_search(void *context, ZedaStringView arguments,
                            ZedaCancellationV1, ZedaTextSinkV1 output,
                            ZedaTextSinkV1 error) {
    return static_cast<DeepWikiPlugin *>(context)->search_tool(text(arguments),
                                                               output, error);
  }

  int command(const std::string &action, ZedaCancellationV1 cancellation,
              ZedaEventCallbackV1 on_event, void *event_context,
              ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    if (action.empty() || action == "status")
      return status(output, error);
    if (action == "tui")
      return tui_document(cancellation, output, error);
    if (action == "open")
      return open_web(output, error);
    if (action != "generate" && action != "update") {
      write_sink(error, "usage: /deepwiki <generate|update|status|tui|open>");
      return 1;
    }

    std::unique_lock lock(job_mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
      write_sink(error, "another DeepWiki task is already running");
      return 1;
    }
    event(on_event, event_context, ZEDA_EVENT_STATUS,
          "正在扫描 C/C++ 仓库并更新本地索引…");
    std::string detail;
    const auto refreshed =
        refresh(cancellation, on_event, event_context, detail);
    if (!refreshed.has_value()) {
      write_sink(error, detail);
      return 1;
    }
    if (cancelled(cancellation)) {
      write_sink(error, "DeepWiki generation cancelled");
      return 1;
    }
    if (action == "update" && refreshed->changed == 0 &&
        refreshed->removed == 0) {
      write_sink(output, "DeepWiki 已是最新状态；未调用模型。\n");
      return 0;
    }

    std::optional<std::vector<PagePlan>> plans;
    const bool replace_toc =
        action == "generate" || refreshed->topology_changed ||
        !std::filesystem::is_regular_file(cache_ / "toc.json");
    if (replace_toc) {
      event(on_event, event_context, ZEDA_EVENT_STATUS,
            "正在规划 4–6 个中文 Wiki 页面…");
      plans = plan_pages(cancellation, detail);
      if (!plans.has_value()) {
        write_sink(error, detail);
        return 1;
      }
    } else {
      plans = existing_plans(detail);
      if (!plans.has_value()) {
        write_sink(error, detail);
        return 1;
      }
      const auto stale = stale_page_ids(refreshed->changed_paths);
      if (!stale.empty()) {
        mark_pages_stale(stale);
        plans->erase(std::remove_if(plans->begin(), plans->end(),
                                    [&](const PagePlan &plan) {
                                      return !stale.contains(plan.id);
                                    }),
                     plans->end());
      } else {
        write_sink(output, "索引已更新，没有已生成页面依赖这些文件。\n");
        return 0;
      }
    }
    if (!generate_pages(*plans, replace_toc, cancellation, on_event,
                        event_context, detail)) {
      write_sink(error, detail);
      return 1;
    }
    std::ostringstream summary;
    summary << "DeepWiki 完成：扫描 " << refreshed->scanned << " 个文件，更新 "
            << refreshed->changed << " 个，删除 " << refreshed->removed
            << " 个，生成 " << plans->size() << " 个页面。\n"
            << "缓存：" << cache_.string() << "\n";
    if (refreshed->degraded)
      summary << "状态：degraded（未发现 compile_commands.json）。\n";
    summary
        << "使用 /deepwiki tui 在终端浏览，或用 /deepwiki open 打开网页。\n";
    write_sink(output, summary.str());
    return 0;
  }

  bool ensure_database(std::string &error) {
    if (database_ != nullptr)
      return true;
    std::error_code filesystem_error;
    std::filesystem::create_directories(cache_ / "pages", filesystem_error);
    if (filesystem_error) {
      error = "cannot create DeepWiki cache: " + filesystem_error.message();
      return false;
    }
    if (sqlite3_open_v2((cache_ / "index.sqlite").c_str(), &database_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      error = "cannot open DeepWiki index: " +
              std::string(sqlite3_errmsg(database_));
      return false;
    }
    const char *schema =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT NOT "
        "NULL);"
        "CREATE TABLE IF NOT EXISTS files(path TEXT PRIMARY KEY,hash TEXT NOT "
        "NULL,size INTEGER NOT NULL,weight REAL NOT NULL);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(path "
        "UNINDEXED,start_line UNINDEXED,end_line UNINDEXED,weight "
        "UNINDEXED,content,tokenize='unicode61');"
        "CREATE TABLE IF NOT EXISTS symbols(path TEXT NOT NULL,name TEXT NOT "
        "NULL,kind INTEGER,line INTEGER,end_line INTEGER);"
        "CREATE INDEX IF NOT EXISTS symbols_name ON symbols(name);"
        "CREATE TABLE IF NOT EXISTS relations(path TEXT NOT NULL,source TEXT "
        "NOT NULL,target TEXT NOT NULL,kind TEXT NOT NULL,line INTEGER NOT "
        "NULL);"
        "CREATE INDEX IF NOT EXISTS relations_target ON relations(target);"
        "CREATE TABLE IF NOT EXISTS pages(id TEXT PRIMARY KEY,title TEXT NOT "
        "NULL,file TEXT NOT NULL,sources_json TEXT NOT NULL,stale INTEGER NOT "
        "NULL DEFAULT 0);";
    if (!sql_exec(database_, schema, error))
      return false;
    Statement version(database_,
                      "SELECT value FROM meta WHERE key='schema_version'");
    std::uint32_t current = 0;
    if (version && sqlite3_step(version.get()) == SQLITE_ROW) {
      const auto stored = column_text(version.get(), 0);
      std::from_chars(stored.data(), stored.data() + stored.size(), current);
    }
    if (current != 0 && current != kSchemaVersion) {
      if (!sql_exec(database_,
                    "DELETE FROM files;DELETE FROM chunks_fts;DELETE FROM "
                    "symbols;DELETE FROM relations;DELETE FROM pages;DELETE "
                    "FROM meta;",
                    error))
        return false;
    }
    Statement set_version(
        database_,
        "INSERT OR REPLACE INTO meta(key,value) VALUES('schema_version',?)");
    const auto version_text = std::to_string(kSchemaVersion);
    bind_text(set_version.get(), 1, version_text);
    if (sqlite3_step(set_version.get()) != SQLITE_DONE) {
      error = sqlite3_errmsg(database_);
      return false;
    }
    return true;
  }

  std::vector<std::filesystem::path> source_files() const {
    std::vector<std::filesystem::path> result;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        workspace_, std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
      const auto path = iterator->path();
      if (iterator->is_directory(error) &&
          ignored_directory(path.filename().string())) {
        iterator.disable_recursion_pending();
      } else if (iterator->is_regular_file(error) && supported_path(path)) {
        const auto size = iterator->file_size(error);
        if (!error && size <= kMaximumSourceBytes)
          result.push_back(path);
      }
      error.clear();
      iterator.increment(error);
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  std::optional<RefreshResult> refresh(ZedaCancellationV1 cancellation,
                                       ZedaEventCallbackV1 on_event,
                                       void *event_context,
                                       std::string &error) {
    if (!ensure_database(error))
      return std::nullopt;
    RefreshResult result;
    result.degraded = !has_compile_commands();
    const auto files = source_files();
    result.scanned = files.size();
    bool rebuild_relations = true;
    {
      Statement relation_version(
          database_, "SELECT value FROM meta WHERE key='relation_version'");
      if (relation_version &&
          sqlite3_step(relation_version.get()) == SQLITE_ROW)
        rebuild_relations = column_text(relation_version.get(), 0) != "1";
    }
    std::unordered_map<std::string, std::string> previous;
    {
      Statement query(database_, "SELECT path,hash FROM files");
      while (query && sqlite3_step(query.get()) == SQLITE_ROW)
        previous.emplace(column_text(query.get(), 0),
                         column_text(query.get(), 1));
    }
    std::set<std::string> current;
    if (!sql_exec(database_, "BEGIN IMMEDIATE", error))
      return std::nullopt;
    const auto rollback = [&] {
      sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    };
    std::size_t processed = 0;
    for (const auto &path : files) {
      ++processed;
      if (processed == 1 || processed % 25 == 0 || processed == files.size()) {
        event(on_event, event_context, ZEDA_EVENT_STATUS,
              "正在索引 " + std::to_string(processed) + "/" +
                  std::to_string(files.size()) + "：" +
                  path.lexically_relative(workspace_).generic_string());
      }
      if (cancelled(cancellation)) {
        rollback();
        error = "DeepWiki indexing cancelled";
        return std::nullopt;
      }
      const auto relative =
          path.lexically_relative(workspace_).generic_string();
      current.insert(relative);
      const auto content = read_file(path);
      if (content.empty() || content.find('\0') != std::string::npos)
        continue;
      const auto hash = hash_content(content);
      const auto found = previous.find(relative);
      if (found != previous.end() && found->second == hash)
        continue;
      ++result.changed;
      result.changed_paths.insert(relative);
      if (found == previous.end())
        result.topology_changed = true;
      if (!replace_file(relative, hash, content, path_weight(relative),
                        cancellation, error)) {
        rollback();
        return std::nullopt;
      }
    }
    for (const auto &[path, unused_hash] : previous) {
      static_cast<void>(unused_hash);
      if (current.contains(path))
        continue;
      ++result.removed;
      result.topology_changed = true;
      result.changed_paths.insert(path);
      if (!delete_file(path, error)) {
        rollback();
        return std::nullopt;
      }
    }
    if (rebuild_relations) {
      if (!sql_exec(database_, "DELETE FROM relations", error)) {
        rollback();
        return std::nullopt;
      }
      for (const auto &path : files) {
        if (cancelled(cancellation)) {
          rollback();
          error = "DeepWiki relation indexing cancelled";
          return std::nullopt;
        }
        const auto content = read_file(path);
        if (!content.empty() && content.find('\0') == std::string::npos) {
          index_relations(path.lexically_relative(workspace_).generic_string(),
                          content);
        }
      }
      Statement set_relation_version(database_,
                                     "INSERT OR REPLACE INTO meta(key,value) "
                                     "VALUES('relation_version','1')");
      if (!set_relation_version ||
          sqlite3_step(set_relation_version.get()) != SQLITE_DONE) {
        rollback();
        error = sqlite3_errmsg(database_);
        return std::nullopt;
      }
    }
    if (!sql_exec(database_, "COMMIT", error)) {
      rollback();
      return std::nullopt;
    }
    Json state{
        {"schema_version", kSchemaVersion},
        {"workspace", workspace_.string()},
        {"commit", git_commit(workspace_)},
        {"file_count", result.scanned},
        {"degraded", result.degraded},
        {"updated_at",
         static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
             std::chrono::system_clock::now()))},
    };
    if (!atomic_write(cache_ / "state.json", state.dump(2) + "\n", error))
      return std::nullopt;
    return result;
  }

  bool replace_file(const std::string &path, const std::string &hash,
                    const std::string &content, double weight,
                    ZedaCancellationV1 cancellation, std::string &error) {
    if (!delete_file(path, error))
      return false;
    Statement insert_file(
        database_, "INSERT INTO files(path,hash,size,weight) VALUES(?,?,?,?)");
    bind_text(insert_file.get(), 1, path);
    bind_text(insert_file.get(), 2, hash);
    sqlite3_bind_int64(insert_file.get(), 3,
                       static_cast<sqlite3_int64>(content.size()));
    sqlite3_bind_double(insert_file.get(), 4, weight);
    if (sqlite3_step(insert_file.get()) != SQLITE_DONE) {
      error = sqlite3_errmsg(database_);
      return false;
    }

    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin <= content.size()) {
      const auto newline = content.find('\n', begin);
      const auto end = newline == std::string::npos ? content.size() : newline;
      lines.emplace_back(content.data() + begin, end - begin);
      if (newline == std::string::npos)
        break;
      begin = newline + 1;
    }
    Statement insert_chunk(
        database_,
        "INSERT INTO chunks_fts(path,start_line,end_line,weight,content) "
        "VALUES(?,?,?,?,?)");
    const auto step = kChunkLines - kChunkOverlapLines;
    for (std::size_t first = 0; first < lines.size(); first += step) {
      if (cancelled(cancellation)) {
        error = "DeepWiki indexing cancelled";
        return false;
      }
      const auto last = std::min(first + kChunkLines, lines.size());
      std::string chunk;
      for (std::size_t index = first; index < last; ++index) {
        chunk.append(lines[index]);
        chunk.push_back('\n');
      }
      sqlite3_reset(insert_chunk.get());
      sqlite3_clear_bindings(insert_chunk.get());
      bind_text(insert_chunk.get(), 1, path);
      sqlite3_bind_int64(insert_chunk.get(), 2,
                         static_cast<sqlite3_int64>(first + 1));
      sqlite3_bind_int64(insert_chunk.get(), 3,
                         static_cast<sqlite3_int64>(last));
      sqlite3_bind_double(insert_chunk.get(), 4, weight);
      bind_text(insert_chunk.get(), 5, chunk);
      if (sqlite3_step(insert_chunk.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
      }
      if (last == lines.size())
        break;
    }
    if (is_cpp_path(path))
      index_symbols(path, cancellation);
    index_relations(path, content);
    return true;
  }

  bool delete_file(const std::string &path, std::string &error) {
    const char *statements[]{
        "DELETE FROM chunks_fts WHERE path=?",
        "DELETE FROM symbols WHERE path=?",
        "DELETE FROM relations WHERE path=?",
        "DELETE FROM files WHERE path=?",
    };
    for (const char *sql : statements) {
      Statement statement(database_, sql);
      if (!statement) {
        error = sqlite3_errmsg(database_);
        return false;
      }
      bind_text(statement.get(), 1, path);
      if (sqlite3_step(statement.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
      }
    }
    return true;
  }

  void index_symbols(const std::string &path, ZedaCancellationV1 cancellation) {
    std::string output;
    std::string ignored_error;
    ZedaTextSinkV1 output_sink{&output, append_string};
    ZedaTextSinkV1 error_sink{&ignored_error, append_string};
    if (host_->clangd_query(host_->context, view("document_symbols"),
                            view(path), 0, 0, cancellation, output_sink,
                            error_sink) != 0)
      return;
    const auto symbols = Json::parse(output, nullptr, false);
    if (!symbols.is_array())
      return;
    Statement insert(
        database_,
        "INSERT INTO symbols(path,name,kind,line,end_line) VALUES(?,?,?,?,?)");
    const auto walk = [&](const auto &self, const Json &items) -> void {
      for (const auto &item : items) {
        if (!item.is_object())
          continue;
        const auto name = item.value("name", std::string{});
        const auto kind = item.value("kind", 0);
        const auto range = item.value("range", Json::object());
        const auto start = range.value("start", Json::object());
        const auto end = range.value("end", Json::object());
        if (!name.empty()) {
          sqlite3_reset(insert.get());
          sqlite3_clear_bindings(insert.get());
          bind_text(insert.get(), 1, path);
          bind_text(insert.get(), 2, name);
          sqlite3_bind_int(insert.get(), 3, kind);
          sqlite3_bind_int64(
              insert.get(), 4,
              static_cast<sqlite3_int64>(start.value("line", 0U) + 1U));
          sqlite3_bind_int64(
              insert.get(), 5,
              static_cast<sqlite3_int64>(end.value("line", 0U) + 1U));
          sqlite3_step(insert.get());
        }
        const auto children = item.find("children");
        if (children != item.end() && children->is_array())
          self(self, *children);
      }
    };
    walk(walk, symbols);
  }

  void index_relations(const std::string &path, const std::string &content) {
    Statement insert(database_,
                     "INSERT INTO relations(path,source,target,kind,line) "
                     "VALUES(?,?,?,?,?)");
    if (!insert)
      return;
    const auto add = [&](std::string_view source, std::string_view target,
                         std::string_view kind, std::size_t line) {
      if (source.empty() || target.empty())
        return;
      sqlite3_reset(insert.get());
      sqlite3_clear_bindings(insert.get());
      bind_text(insert.get(), 1, path);
      bind_text(insert.get(), 2, source);
      bind_text(insert.get(), 3, target);
      bind_text(insert.get(), 4, kind);
      sqlite3_bind_int64(insert.get(), 5, static_cast<sqlite3_int64>(line));
      sqlite3_step(insert.get());
    };

    if (is_cpp_path(path)) {
      const std::regex include_pattern(
          R"(^\s*#\s*include\s*[<\"]([^>\"]+)[>\"])");
      std::istringstream lines(content);
      std::string line;
      std::size_t line_number = 0;
      while (std::getline(lines, line)) {
        ++line_number;
        std::smatch match;
        if (std::regex_search(line, match, include_pattern))
          add(path, match[1].str(), "include", line_number);
      }
    }

    const auto filename =
        lowercase(std::filesystem::path(path).filename().string());
    const auto extension =
        lowercase(std::filesystem::path(path).extension().string());
    if (filename != "cmakelists.txt" && extension != ".cmake")
      return;
    const auto relation_line = [&](std::size_t offset) {
      return static_cast<std::size_t>(std::count(
                 content.begin(),
                 content.begin() + static_cast<std::ptrdiff_t>(offset), '\n')) +
             1;
    };
    const std::regex target_pattern(
        R"(\b(add_library|add_executable)\s*\(\s*([A-Za-z0-9_.:+-]+))",
        std::regex_constants::icase);
    for (std::sregex_iterator
             current(content.begin(), content.end(), target_pattern),
         end;
         current != end; ++current) {
      add(path, (*current)[2].str(), "cmake_target",
          relation_line(static_cast<std::size_t>(current->position())));
    }
    const std::regex link_pattern(
        R"(\btarget_link_libraries\s*\(\s*([A-Za-z0-9_.:+-]+)\s+([^\)]*))",
        std::regex_constants::icase);
    const std::regex dependency_pattern(R"([A-Za-z0-9_.:+/-]+)");
    for (std::sregex_iterator
             current(content.begin(), content.end(), link_pattern),
         end;
         current != end; ++current) {
      const auto owner = (*current)[1].str();
      const auto body = (*current)[2].str();
      for (std::sregex_iterator
               dependency(body.begin(), body.end(), dependency_pattern),
           dependency_end;
           dependency != dependency_end; ++dependency) {
        const auto name = dependency->str();
        const auto normalized = lowercase(name);
        if (normalized == "public" || normalized == "private" ||
            normalized == "interface" || name == owner)
          continue;
        add(owner, name, "cmake_link",
            relation_line(static_cast<std::size_t>(current->position())));
      }
    }
  }

  static int append_string(void *context, ZedaStringView content) {
    if (context == nullptr)
      return 1;
    static_cast<std::string *>(context)->append(content.data, content.size);
    return 0;
  }

  bool has_compile_commands() const {
    const std::filesystem::path candidates[]{workspace_, workspace_ / "build",
                                             workspace_ / "build-debug"};
    std::error_code error;
    return std::any_of(std::begin(candidates), std::end(candidates),
                       [&](const auto &candidate) {
                         return std::filesystem::is_regular_file(
                                    candidate / "compile_commands.json",
                                    error) &&
                                !error;
                       });
  }

  std::string repository_map() {
    std::ostringstream result;
    result << "仓库：" << workspace_.filename().string() << "\n\n文件：\n";
    Statement files(database_, "SELECT path,size,weight FROM files ORDER BY "
                               "weight DESC,size DESC LIMIT 120");
    while (files && sqlite3_step(files.get()) == SQLITE_ROW) {
      result << "- " << column_text(files.get(), 0) << " ("
             << sqlite3_column_int64(files.get(), 1) << " bytes)\n";
    }
    result << "\n主要符号：\n";
    Statement symbols(database_,
                      "SELECT name,path,line FROM symbols LIMIT 180");
    while (symbols && sqlite3_step(symbols.get()) == SQLITE_ROW) {
      result << "- " << column_text(symbols.get(), 0) << " — "
             << column_text(symbols.get(), 1) << ':'
             << sqlite3_column_int64(symbols.get(), 2) << "\n";
    }
    result << "\n结构关系：\n";
    Statement relations(
        database_,
        "SELECT source,target,kind,path,line FROM relations LIMIT 240");
    while (relations && sqlite3_step(relations.get()) == SQLITE_ROW) {
      result << "- " << column_text(relations.get(), 0) << " -> "
             << column_text(relations.get(), 1) << " ["
             << column_text(relations.get(), 2) << "] — "
             << column_text(relations.get(), 3) << ':'
             << sqlite3_column_int64(relations.get(), 4) << "\n";
    }
    return result.str();
  }

  std::optional<std::vector<PagePlan>>
  plan_pages(ZedaCancellationV1 cancellation, std::string &error) {
    const std::string system =
        "你是 C++ 代码库文档规划器。根据仓库地图设计 4 到 6 个互不重复的中文 "
        "Wiki 页面。"
        "只输出 JSON 数组。每项必须包含 "
        "id、title、description、queries；queries 是 2 到 5 个英文源码检索词。"
        "必须覆盖总体架构和至少一条关键运行路径。id 只能使用小写 "
        "ASCII、数字和连字符。";
    std::string response;
    if (!complete(system, repository_map(), 4096, cancellation, nullptr,
                  nullptr, response, error))
      return std::nullopt;
    auto parsed = Json::parse(strip_code_fence(response), nullptr, false);
    std::vector<PagePlan> plans;
    if (parsed.is_array()) {
      std::set<std::string> ids;
      for (const auto &item : parsed) {
        if (!item.is_object())
          continue;
        PagePlan plan;
        plan.id = safe_id(item.value("id", std::string{}));
        plan.title = trim(item.value("title", std::string{}));
        plan.description = trim(item.value("description", std::string{}));
        const auto queries = item.find("queries");
        if (queries != item.end() && queries->is_array()) {
          for (const auto &query : *queries) {
            if (query.is_string() && !query.empty())
              plan.queries.push_back(query.get<std::string>());
          }
        }
        if (!plan.title.empty() && !plan.queries.empty() &&
            ids.insert(plan.id).second)
          plans.push_back(std::move(plan));
        if (plans.size() == 6)
          break;
      }
    }
    if (plans.size() < 4) {
      plans = {
          {"overview",
           "项目总览",
           "项目目标、边界和模块地图",
           {"architecture main modules application"}},
          {"core-flow",
           "核心运行流程",
           "关键入口到主要处理路径",
           {"main run request handler route"}},
          {"async-runtime",
           "异步与网络运行时",
           "事件循环、并发与异步边界",
           {"event loop async coroutine callback"}},
          {"data-layer",
           "数据与持久化",
           "数据库、ORM 与状态管理",
           {"database orm transaction client"}},
          {"extension-build",
           "扩展与构建系统",
           "插件、配置和构建组织",
           {"plugin config CMake target"}},
      };
    }
    return plans;
  }

  std::optional<std::vector<PagePlan>> existing_plans(std::string &error) {
    const auto content = read_file(cache_ / "toc.json", 1024 * 1024);
    const auto toc = Json::parse(content, nullptr, false);
    if (!toc.is_array()) {
      error = "DeepWiki table of contents is invalid; run /deepwiki generate";
      return std::nullopt;
    }
    std::vector<PagePlan> plans;
    for (const auto &item : toc) {
      if (!item.is_object())
        continue;
      PagePlan plan;
      plan.id = safe_id(item.value("id", std::string{}));
      plan.title = item.value("title", std::string{});
      plan.description = item.value("description", std::string{});
      const auto queries = item.find("queries");
      if (queries != item.end() && queries->is_array()) {
        for (const auto &query : *queries) {
          if (query.is_string())
            plan.queries.push_back(query.get<std::string>());
        }
      }
      if (plan.queries.empty())
        plan.queries.push_back(plan.title);
      if (!plan.id.empty() && !plan.title.empty())
        plans.push_back(std::move(plan));
    }
    if (plans.empty()) {
      error = "DeepWiki table of contents contains no pages";
      return std::nullopt;
    }
    return plans;
  }

  bool generate_pages(const std::vector<PagePlan> &plans, bool replace_toc,
                      ZedaCancellationV1 cancellation,
                      ZedaEventCallbackV1 on_event, void *event_context,
                      std::string &error) {
    Json toc = Json::array();
    if (!replace_toc) {
      toc = Json::parse(read_file(cache_ / "toc.json", 1024 * 1024), nullptr,
                        false);
      if (!toc.is_array())
        toc = Json::array();
    }
    for (std::size_t index = 0; index < plans.size(); ++index) {
      if (cancelled(cancellation)) {
        error = "DeepWiki generation cancelled";
        return false;
      }
      const auto &plan = plans[index];
      event(on_event, event_context, ZEDA_EVENT_STATUS,
            "正在生成页面 " + std::to_string(index + 1) + "/" +
                std::to_string(plans.size()) + "：" + plan.title);
      std::string evidence;
      for (const auto &query : plan.queries) {
        evidence += search_index(query, 6);
        if (evidence.size() >= kMaximumEvidenceBytes)
          break;
      }
      if (evidence.empty())
        evidence = search_index(plan.title, 8);
      if (evidence.size() > kMaximumEvidenceBytes)
        evidence.resize(kMaximumEvidenceBytes);
      const std::string system =
          "你是严谨的 C++ 架构文档作者。只使用提供的证据写中文 Markdown。"
          "每个技术事实都要紧邻 [相对路径:行号] "
          "引用；禁止编造不存在的类型、调用关系或行为。"
          "页面必须以一级标题开始，包含用途、关键组件、执行流程和限制。"
          "如果证据足够，加入一个 ```mermaid "
          "图，并在图后列出支持图中关系的源码引用。"
          "不要输出 JSON，不要使用仓库外部知识。";
      const std::string prompt = "页面标题：" + plan.title + "\n页面目标：" +
                                 plan.description + "\n\n源码证据：\n" +
                                 evidence;
      std::string page;
      if (!complete(system, prompt, 8192, cancellation, nullptr, nullptr, page,
                    error))
        return false;
      page = trim(std::move(page)) + "\n";
      std::set<std::string> sources;
      if (!validate_citations(page, sources, error))
        return false;
      if (!atomic_write(cache_ / "pages" / (plan.id + ".md"), page, error))
        return false;
      const Json source_json(sources);
      Statement save(
          database_,
          "INSERT OR REPLACE INTO pages(id,title,file,sources_json,stale) "
          "VALUES(?,?,?,?,0)");
      bind_text(save.get(), 1, plan.id);
      bind_text(save.get(), 2, plan.title);
      bind_text(save.get(), 3, "pages/" + plan.id + ".md");
      bind_text(save.get(), 4, source_json.dump());
      if (sqlite3_step(save.get()) != SQLITE_DONE) {
        error = sqlite3_errmsg(database_);
        return false;
      }
      const Json toc_entry{{"id", plan.id},
                           {"title", plan.title},
                           {"description", plan.description},
                           {"queries", plan.queries}};
      auto existing =
          std::find_if(toc.begin(), toc.end(), [&](const Json &item) {
            return item.is_object() &&
                   item.value("id", std::string{}) == plan.id;
          });
      if (existing == toc.end())
        toc.push_back(toc_entry);
      else
        *existing = toc_entry;
    }
    return atomic_write(cache_ / "toc.json", toc.dump(2) + "\n", error);
  }

  bool validate_citations(const std::string &page,
                          std::set<std::string> &sources,
                          std::string &error) const {
    static const std::regex citation(R"(\[([^\]\n]+):(\d+)(?:-\d+)?\])");
    for (std::sregex_iterator iterator(page.begin(), page.end(), citation), end;
         iterator != end; ++iterator) {
      const auto relative = (*iterator)[1].str();
      const auto line_text = (*iterator)[2].str();
      std::size_t line = 0;
      std::from_chars(line_text.data(), line_text.data() + line_text.size(),
                      line);
      const auto resolved =
          std::filesystem::weakly_canonical(workspace_ / relative);
      if (!path_is_inside(workspace_, resolved) || line == 0 ||
          !std::filesystem::is_regular_file(resolved)) {
        error = "generated page contains an invalid citation: " + relative +
                ":" + line_text;
        return false;
      }
      const auto source = read_file(resolved);
      const auto line_count = static_cast<std::size_t>(std::count(
                                  source.begin(), source.end(), '\n')) +
                              1;
      if (source.empty() || line > line_count) {
        error = "generated page citation line is outside the source file: " +
                relative + ":" + line_text;
        return false;
      }
      sources.insert(relative);
    }
    if (sources.empty()) {
      error = "generated page contains no verifiable source citation";
      return false;
    }
    return true;
  }

  std::set<std::string>
  stale_page_ids(const std::set<std::string> &changed_paths) {
    std::set<std::string> result;
    Statement pages(database_, "SELECT id,sources_json FROM pages");
    while (pages && sqlite3_step(pages.get()) == SQLITE_ROW) {
      const auto id = column_text(pages.get(), 0);
      const auto sources =
          Json::parse(column_text(pages.get(), 1), nullptr, false);
      if (!sources.is_array())
        continue;
      const bool stale =
          std::any_of(sources.begin(), sources.end(), [&](const Json &source) {
            return source.is_string() &&
                   changed_paths.contains(source.get<std::string>());
          });
      if (stale)
        result.insert(id);
    }
    return result;
  }

  void mark_pages_stale(const std::set<std::string> &page_ids) {
    Statement mark(database_, "UPDATE pages SET stale=1 WHERE id=?");
    if (!mark)
      return;
    for (const auto &id : page_ids) {
      sqlite3_reset(mark.get());
      sqlite3_clear_bindings(mark.get());
      bind_text(mark.get(), 1, id);
      sqlite3_step(mark.get());
    }
  }

  std::string fts_query(std::string_view query) const {
    std::vector<std::string> tokens;
    std::string token;
    for (const char raw_character : query) {
      const auto character = static_cast<unsigned char>(raw_character);
      if (std::isalnum(character) != 0 || character == '_' ||
          character >= 0x80) {
        token.push_back(static_cast<char>(character));
      } else if (!token.empty()) {
        tokens.push_back(std::exchange(token, {}));
      }
    }
    if (!token.empty())
      tokens.push_back(std::move(token));
    if (tokens.size() > 12)
      tokens.resize(12);
    std::string result;
    for (const auto &item : tokens) {
      if (!result.empty())
        result += " OR ";
      result += '"' + item + '"';
    }
    return result;
  }

  std::string search_index(std::string_view query, std::size_t limit) {
    if (database_ == nullptr)
      return {};
    const auto match = fts_query(query);
    if (match.empty())
      return {};
    Statement search(database_,
                     "SELECT path,start_line,end_line,content FROM chunks_fts "
                     "WHERE chunks_fts MATCH ? ORDER BY "
                     "bm25(chunks_fts)-CAST(weight AS REAL)*0.05 LIMIT ?");
    if (!search)
      return {};
    bind_text(search.get(), 1, match);
    sqlite3_bind_int64(search.get(), 2, static_cast<sqlite3_int64>(limit));
    std::ostringstream result;
    const auto relation_query = "%" + lowercase(trim(std::string(query))) + "%";
    Statement relations(
        database_, "SELECT source,target,kind,path,line FROM relations WHERE "
                   "lower(source) LIKE ? OR lower(target) LIKE ? LIMIT ?");
    if (relations) {
      bind_text(relations.get(), 1, relation_query);
      bind_text(relations.get(), 2, relation_query);
      sqlite3_bind_int64(relations.get(), 3, static_cast<sqlite3_int64>(limit));
      while (sqlite3_step(relations.get()) == SQLITE_ROW) {
        result << "\n## " << column_text(relations.get(), 3) << ':'
               << sqlite3_column_int64(relations.get(), 4) << "\n["
               << column_text(relations.get(), 2) << "] "
               << column_text(relations.get(), 0) << " -> "
               << column_text(relations.get(), 1) << "\n";
      }
    }
    while (sqlite3_step(search.get()) == SQLITE_ROW) {
      const auto path = column_text(search.get(), 0);
      const auto start = sqlite3_column_int64(search.get(), 1);
      const auto end = sqlite3_column_int64(search.get(), 2);
      result << "\n## " << path << ':' << start << '-' << end << "\n```cpp\n"
             << column_text(search.get(), 3) << "```\n";
      if (result.tellp() >= static_cast<std::streampos>(kMaximumEvidenceBytes))
        break;
    }
    return result.str();
  }

  std::string related_wiki_evidence(std::string_view question,
                                    std::vector<std::string> &source_queries) {
    const auto toc = Json::parse(read_file(cache_ / "toc.json", 1024 * 1024),
                                 nullptr, false);
    if (!toc.is_array())
      return {};
    struct Candidate {
      std::size_t score{};
      Json item;
    };
    std::vector<Candidate> candidates;
    for (const auto &item : toc) {
      if (!item.is_object())
        continue;
      const auto searchable = item.value("title", std::string{}) + " " +
                              item.value("description", std::string{});
      candidates.push_back({related_text_score(question, searchable), item});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right) {
                return left.score > right.score;
              });
    std::string evidence;
    std::size_t selected = 0;
    for (const auto &candidate : candidates) {
      if (candidate.score == 0 || selected == 2)
        break;
      const auto id = safe_id(candidate.item.value("id", std::string{}));
      const auto page = read_file(cache_ / "pages" / (id + ".md"), 48 * 1024);
      if (page.empty())
        continue;
      evidence += "\n## 已验证 Wiki 页面：" +
                  candidate.item.value("title", std::string{}) + "\n" + page;
      const auto queries = candidate.item.find("queries");
      if (queries != candidate.item.end() && queries->is_array()) {
        for (const auto &query : *queries) {
          if (query.is_string())
            source_queries.push_back(query.get<std::string>());
        }
      }
      ++selected;
      if (evidence.size() >= kMaximumEvidenceBytes / 2)
        break;
    }
    return evidence;
  }

  bool complete(std::string_view system, std::string_view prompt,
                std::size_t max_tokens, ZedaCancellationV1 cancellation,
                ZedaEventCallbackV1 on_event, void *event_context,
                std::string &output, std::string &error) {
    ZedaTextSinkV1 output_sink{&output, append_string};
    ZedaTextSinkV1 error_sink{&error, append_string};
    return host_->complete(host_->context, view(system), view(prompt),
                           max_tokens, 1, cancellation, on_event, event_context,
                           output_sink, error_sink) == 0;
  }

  int tui_document(ZedaCancellationV1 cancellation, ZedaTextSinkV1 output,
                   ZedaTextSinkV1 error) {
    if (cancelled(cancellation)) {
      write_sink(error, "DeepWiki TUI loading cancelled");
      return 1;
    }
    const auto toc_text = read_file(cache_ / "toc.json", 1024 * 1024);
    const auto toc = Json::parse(toc_text, nullptr, false);
    if (!toc.is_array() || toc.empty()) {
      write_sink(error,
                 "DeepWiki has not been generated; run /deepwiki generate");
      return 1;
    }
    if (toc.size() > kMaximumTuiPages) {
      write_sink(error, "DeepWiki TUI supports at most 64 pages");
      return 1;
    }

    std::string database_error;
    if (!ensure_database(database_error)) {
      write_sink(error, database_error);
      return 1;
    }
    std::unordered_map<std::string, bool> stale_pages;
    Statement stale_query(database_, "SELECT id,stale FROM pages");
    while (stale_query && sqlite3_step(stale_query.get()) == SQLITE_ROW) {
      stale_pages.emplace(column_text(stale_query.get(), 0),
                          sqlite3_column_int(stale_query.get(), 1) != 0);
    }

    Json pages = Json::array();
    std::size_t markdown_bytes = 0;
    std::size_t stale_count = 0;
    std::set<std::string> ids;
    for (const auto &item : toc) {
      if (cancelled(cancellation)) {
        write_sink(error, "DeepWiki TUI loading cancelled");
        return 1;
      }
      if (!item.is_object() || !item.contains("id") ||
          !item.at("id").is_string() || !item.contains("title") ||
          !item.at("title").is_string()) {
        continue;
      }
      const auto raw_id = item.at("id").get<std::string>();
      const auto title = item.at("title").get<std::string>();
      if (raw_id.empty() || title.empty())
        continue;
      const auto id = safe_id(raw_id);
      if (!ids.insert(id).second) {
        write_sink(error, "DeepWiki table of contents has duplicate page ids");
        return 1;
      }
      auto markdown =
          read_file(cache_ / "pages" / (id + ".md"), 2 * 1024 * 1024);
      if (markdown.empty()) {
        write_sink(error, "DeepWiki page not found: " + id);
        return 1;
      }
      if (markdown.size() > kMaximumTuiMarkdownBytes - markdown_bytes) {
        write_sink(error, "DeepWiki pages exceed the 12 MiB TUI limit");
        return 1;
      }
      markdown_bytes += markdown.size();
      const auto stale = stale_pages.find(id);
      const bool page_is_stale = stale != stale_pages.end() && stale->second;
      if (page_is_stale)
        ++stale_count;
      const auto description =
          item.contains("description") && item.at("description").is_string()
              ? item.at("description").get<std::string>()
              : std::string{};
      pages.push_back({
          {"id", id},
          {"title", title},
          {"description", description},
          {"markdown", std::move(markdown)},
          {"badge", page_is_stale ? "stale" : ""},
      });
    }
    if (pages.empty()) {
      write_sink(error, "DeepWiki table of contents contains no valid pages");
      return 1;
    }

    std::string subtitle = workspace_.filename().string() + " · " +
                           std::to_string(pages.size()) + " pages";
    if (stale_count != 0)
      subtitle += " · " + std::to_string(stale_count) + " stale";
    if (!has_compile_commands())
      subtitle += " · degraded";
    Json document{
        {"schema_version", 1},
        {"title", "DeepWiki"},
        {"subtitle", std::move(subtitle)},
        {"pages", std::move(pages)},
    };
    if (cancelled(cancellation)) {
      write_sink(error, "DeepWiki TUI loading cancelled");
      return 1;
    }
    const auto serialized =
        document.dump(-1, ' ', false, Json::error_handler_t::replace);
    if (serialized.size() > kMaximumTuiDocumentBytes) {
      write_sink(error, "DeepWiki TUI document exceeds the 16 MiB limit");
      return 1;
    }
    write_sink(output, serialized);
    return 0;
  }

  int status(ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    std::string database_error;
    if (!ensure_database(database_error)) {
      write_sink(error, database_error);
      return 1;
    }
    std::int64_t files = 0;
    std::int64_t pages = 0;
    std::int64_t stale_pages = 0;
    Statement file_count(database_, "SELECT count(*) FROM files");
    if (file_count && sqlite3_step(file_count.get()) == SQLITE_ROW)
      files = sqlite3_column_int64(file_count.get(), 0);
    Statement page_count(database_, "SELECT count(*) FROM pages");
    if (page_count && sqlite3_step(page_count.get()) == SQLITE_ROW)
      pages = sqlite3_column_int64(page_count.get(), 0);
    Statement stale_count(database_,
                          "SELECT count(*) FROM pages WHERE stale!=0");
    if (stale_count && sqlite3_step(stale_count.get()) == SQLITE_ROW)
      stale_pages = sqlite3_column_int64(stale_count.get(), 0);
    const auto commit = git_commit(workspace_);
    std::ostringstream report;
    report << "DeepWiki\n"
           << "workspace: " << workspace_.string() << "\n"
           << "cache: " << cache_.string() << "\n"
           << "files: " << files << "\n"
           << "pages: " << pages << "\n"
           << "stale pages: " << stale_pages << "\n"
           << "commit: "
           << (commit.empty() ? "unavailable" : commit.substr(0, 12)) << "\n"
           << "clangd: "
           << (has_compile_commands() ? "ready"
                                      : "degraded — no compile_commands.json")
           << "\n";
    if (server_ != nullptr)
      report << "web: http://127.0.0.1:" << port_ << "/?token=" << token_
             << "\n";
    write_sink(output, report.str());
    return 0;
  }

  int structure(ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    const auto toc = read_file(cache_ / "toc.json", 1024 * 1024);
    if (toc.empty()) {
      write_sink(error,
                 "DeepWiki has not been generated; run /deepwiki generate");
      return 1;
    }
    write_sink(output, toc);
    return 0;
  }

  int contents(const std::string &arguments, ZedaTextSinkV1 output,
               ZedaTextSinkV1 error) {
    const auto json = Json::parse(arguments, nullptr, false);
    if (!json.is_object() || !json.contains("page") ||
        !json.at("page").is_string()) {
      write_sink(error, "deepwiki_contents requires a page string");
      return 1;
    }
    const auto id = safe_id(json.at("page").get<std::string>());
    const auto page =
        read_file(cache_ / "pages" / (id + ".md"), 2 * 1024 * 1024);
    if (page.empty()) {
      write_sink(error, "DeepWiki page not found: " + id);
      return 1;
    }
    write_sink(output, page);
    return 0;
  }

  int search_tool(const std::string &arguments, ZedaTextSinkV1 output,
                  ZedaTextSinkV1 error) {
    std::string database_error;
    if (!ensure_database(database_error)) {
      write_sink(error, database_error);
      return 1;
    }
    const auto json = Json::parse(arguments, nullptr, false);
    if (!json.is_object() || !json.contains("query") ||
        !json.at("query").is_string()) {
      write_sink(error, "deepwiki_search requires a query string");
      return 1;
    }
    const auto limit = std::clamp(json.value("limit", 8), 1, 20);
    const auto matches = search_index(json.at("query").get<std::string>(),
                                      static_cast<std::size_t>(limit));
    write_sink(output, matches.empty() ? "No DeepWiki matches.\n" : matches);
    return 0;
  }

  bool authorized(const httplib::Request &request) const {
    if (request.has_param("token") &&
        request.get_param_value("token") == token_)
      return true;
    return request.get_header_value("X-DeepWiki-Token") == token_;
  }

  bool origin_allowed(const httplib::Request &request) const {
    const auto origin = request.get_header_value("Origin");
    if (origin.empty())
      return true;
    return origin == "http://127.0.0.1:" + std::to_string(port_) ||
           origin == "http://localhost:" + std::to_string(port_);
  }

  int open_web(ZedaTextSinkV1 output, ZedaTextSinkV1 error) {
    std::scoped_lock lock(server_mutex_);
    if (server_ == nullptr && !start_server()) {
      write_sink(error, "cannot start DeepWiki web server");
      return 1;
    }
    const auto url =
        "http://127.0.0.1:" + std::to_string(port_) + "/?token=" + token_;
    launch_browser(url);
    write_sink(output, "DeepWiki: " + url + "\n");
    return 0;
  }

  bool start_server() {
    token_ = random_token();
    server_ = std::make_unique<httplib::Server>();
    server_->set_payload_max_length(128 * 1024);
    server_->Get("/", [this](const httplib::Request &request,
                             httplib::Response &response) {
      if (!authorized(request)) {
        response.status = 403;
        return;
      }
      respond_asset("index.html", "text/html; charset=utf-8", response);
    });
    server_->Get("/app.js", [this](const httplib::Request &request,
                                   httplib::Response &response) {
      static_cast<void>(request);
      respond_asset("app.js", "text/javascript; charset=utf-8", response);
    });
    server_->Get("/style.css", [this](const httplib::Request &request,
                                      httplib::Response &response) {
      static_cast<void>(request);
      respond_asset("style.css", "text/css; charset=utf-8", response);
    });
    server_->Get(
        R"(/vendor/(marked|purify|mermaid)\.js)",
        [this](const httplib::Request &request, httplib::Response &response) {
          respond_asset("vendor/" + request.matches[1].str() + ".js",
                        "text/javascript; charset=utf-8", response);
        });
    server_->Get("/api/toc", [this](const httplib::Request &request,
                                    httplib::Response &response) {
      if (!authorized(request)) {
        response.status = 403;
        return;
      }
      const auto content = read_file(cache_ / "toc.json", 1024 * 1024);
      response.set_content(content.empty() ? "[]" : content,
                           "application/json; charset=utf-8");
    });
    server_->Get("/api/page", [this](const httplib::Request &request,
                                     httplib::Response &response) {
      if (!authorized(request) || !request.has_param("id")) {
        response.status = 403;
        return;
      }
      const auto id = safe_id(request.get_param_value("id"));
      const auto content =
          read_file(cache_ / "pages" / (id + ".md"), 2 * 1024 * 1024);
      if (content.empty()) {
        response.status = 404;
        return;
      }
      response.set_content(content, "text/markdown; charset=utf-8");
    });
    server_->Get("/api/source", [this](const httplib::Request &request,
                                       httplib::Response &response) {
      source_preview(request, response);
    });
    server_->Post("/api/ask", [this](const httplib::Request &request,
                                     httplib::Response &response) {
      ask(request, response);
    });
    port_ = server_->bind_to_any_port("127.0.0.1");
    if (port_ <= 0) {
      server_.reset();
      return false;
    }
    server_thread_ = std::thread([this] { server_->listen_after_bind(); });
    return true;
  }

  void respond_asset(const std::filesystem::path &relative,
                     const char *content_type,
                     httplib::Response &response) const {
    const auto resolved =
        std::filesystem::weakly_canonical(resources_ / relative);
    if (!path_is_inside(resources_, resolved)) {
      response.status = 403;
      return;
    }
    const auto content = read_file(resolved, 8 * 1024 * 1024);
    if (content.empty()) {
      response.status = 404;
      return;
    }
    response.set_content(content, content_type);
    response.set_header("Cache-Control", "no-store");
  }

  void source_preview(const httplib::Request &request,
                      httplib::Response &response) const {
    if (!authorized(request) || !request.has_param("path")) {
      response.status = 403;
      return;
    }
    const auto resolved = std::filesystem::weakly_canonical(
        workspace_ / request.get_param_value("path"));
    if (!path_is_inside(workspace_, resolved) ||
        !std::filesystem::is_regular_file(resolved)) {
      response.status = 404;
      return;
    }
    std::size_t requested_line = 1;
    if (request.has_param("line")) {
      const auto value = request.get_param_value("line");
      std::from_chars(value.data(), value.data() + value.size(),
                      requested_line);
    }
    requested_line = std::max<std::size_t>(1, requested_line);
    const auto first = requested_line > 20 ? requested_line - 20 : 1;
    const auto last = requested_line + 80;
    std::ifstream input(resolved);
    std::string line;
    std::size_t number = 0;
    std::ostringstream excerpt;
    while (std::getline(input, line)) {
      ++number;
      if (number >= first && number <= last)
        excerpt << number << "  " << line << '\n';
      if (number > last)
        break;
    }
    response.set_content(excerpt.str(), "text/plain; charset=utf-8");
  }

  void ask(const httplib::Request &request, httplib::Response &response) {
    if (!authorized(request) || !origin_allowed(request)) {
      response.status = 403;
      return;
    }
    const auto body = Json::parse(request.body, nullptr, false);
    if (!body.is_object() || !body.contains("question") ||
        !body.at("question").is_string() || body.at("question").empty()) {
      response.status = 400;
      return;
    }
    const auto question = body.at("question").get<std::string>();
    if (!job_mutex_.try_lock()) {
      response.status = 409;
      response.set_content("DeepWiki is busy", "text/plain");
      return;
    }
    job_mutex_.unlock();
    response.set_header("Cache-Control", "no-store");
    response.set_chunked_content_provider(
        "text/event-stream; charset=utf-8",
        [this, question, started = false](std::size_t,
                                          httplib::DataSink &sink) mutable {
          if (started)
            return false;
          started = true;
          std::unique_lock lock(job_mutex_, std::try_to_lock);
          if (!lock.owns_lock()) {
            constexpr std::string_view busy =
                "event: error\ndata: DeepWiki is busy\n\n";
            sink.write(busy.data(), busy.size());
            sink.done();
            return false;
          }
          std::string error;
          if (!ensure_database(error)) {
            const auto message = "event: error\ndata: " + trim(error) + "\n\n";
            sink.write(message.data(), message.size());
            sink.done();
            return false;
          }
          std::string search_terms;
          const std::string rewrite_system =
              "把中文 C++ 代码问题改写成 3 到 8 "
              "个英文源码检索关键词或标识符。只输出空格分隔的词，不要解释。";
          complete(rewrite_system, question, 512, {}, nullptr, nullptr,
                   search_terms, error);
          std::vector<std::string> source_queries;
          auto evidence = related_wiki_evidence(question, source_queries);
          if (!search_terms.empty())
            source_queries.insert(source_queries.begin(), search_terms);
          for (const auto &query : source_queries) {
            evidence += search_index(query, 3);
            if (evidence.size() >= kMaximumEvidenceBytes)
              break;
          }
          if (evidence.empty())
            evidence = search_index(question, 10);
          if (evidence.size() > kMaximumEvidenceBytes)
            evidence.resize(kMaximumEvidenceBytes);
          const std::string system =
              "你是本地 C++ 代码库问答助手。只能依据给定源码证据回答中文。"
              "所有技术结论必须带 [相对路径:行号] 引用；证据不足时明确说明。";
          const std::string prompt =
              "问题：" + question + "\n\n源码证据：\n" + evidence;
          struct StreamContext {
            httplib::DataSink *sink{};
          } stream{&sink};
          const auto stream_event = [](void *context, std::uint32_t kind,
                                       ZedaStringView content) {
            if (kind != ZEDA_EVENT_DELTA)
              return;
            auto *target = static_cast<StreamContext *>(context);
            Json payload{{"delta", text(content)}};
            const auto message = "data: " + payload.dump() + "\n\n";
            target->sink->write(message.data(), message.size());
          };
          std::string answer;
          if (!complete(system, prompt, 4096, {}, stream_event, &stream, answer,
                        error)) {
            const auto message = "event: error\ndata: " + trim(error) + "\n\n";
            sink.write(message.data(), message.size());
          } else {
            constexpr std::string_view done = "event: done\ndata: {}\n\n";
            sink.write(done.data(), done.size());
          }
          sink.done();
          return false;
        });
  }

  void stop_server() {
    std::scoped_lock lock(server_mutex_);
    if (server_ != nullptr)
      server_->stop();
    if (server_thread_.joinable())
      server_thread_.join();
    server_.reset();
    if (database_ != nullptr) {
      sqlite3_close(database_);
      database_ = nullptr;
    }
  }

  static std::string random_token() {
    std::random_device random;
    std::ostringstream token;
    for (int index = 0; index < 4; ++index)
      token << std::hex << std::setw(8) << std::setfill('0') << random();
    return token.str();
  }

  static void launch_browser(const std::string &url) {
    const char *disabled = std::getenv("ZED_DEEPWIKI_NO_BROWSER");
    if (disabled != nullptr && std::string_view(disabled) == "1")
      return;
#if defined(__APPLE__) || defined(__linux__)
    const pid_t child = fork();
    if (child != 0)
      return;
#if defined(__APPLE__)
    execlp("open", "open", url.c_str(), static_cast<char *>(nullptr));
#else
    execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
#endif
    _exit(127);
#else
    static_cast<void>(url);
#endif
  }

  const ZedaHostApiV1 *host_{};
  std::filesystem::path workspace_;
  std::filesystem::path resources_;
  std::filesystem::path cache_;
  sqlite3 *database_{};
  std::mutex job_mutex_;
  std::mutex server_mutex_;
  std::unique_ptr<httplib::Server> server_;
  std::thread server_thread_;
  int port_{};
  std::string token_;
};

void *create_plugin() {
  try {
    return new DeepWikiPlugin();
  } catch (...) {
    return nullptr;
  }
}

int initialize_plugin(void *instance, const ZedaHostApiV1 *host,
                      ZedaTextSinkV1 error) {
  if (instance == nullptr)
    return 1;
  try {
    return static_cast<DeepWikiPlugin *>(instance)->initialize(host, error);
  } catch (const std::exception &exception) {
    write_sink(error, exception.what());
    return 1;
  } catch (...) {
    write_sink(error, "DeepWiki initialization failed");
    return 1;
  }
}

void shutdown_plugin(void *instance) {
  if (instance != nullptr)
    static_cast<DeepWikiPlugin *>(instance)->shutdown();
}

void destroy_plugin(void *instance) {
  delete static_cast<DeepWikiPlugin *>(instance);
}

const ZedaPluginDescriptorV1 kDescriptor{
    ZEDA_PLUGIN_ABI_VERSION, view("deepwiki"),
    view("DeepWiki"),        view("0.2"),
    create_plugin,           initialize_plugin,
    shutdown_plugin,         destroy_plugin,
};

} // namespace

extern "C" const ZedaPluginDescriptorV1 *zeda_plugin_entry_v1() {
  return &kDescriptor;
}
