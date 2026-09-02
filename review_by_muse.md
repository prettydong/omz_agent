# `src` 代码质量评估报告

> 范围：`src/` 29个 `.cpp` + `include/zed/` 34个 `.hpp/.h`，约 15k~20k 行，C++20 (实际以 C++17 为主)。**未修改任何代码，仅静态评估。**
> 评估时间：2026-05-13
> 评估模型：Muse

### 总体结论：`7.8 / 10` - 中高质量，工程化扎实，显著优于同规模 C++ 开源项目平均水平

**一句话概括：** 分层清晰、错误处理统一、安全意识强、无历史债务堆积；主要短板是 `main.cpp`/`terminal.cpp` 超长函数、局部代码重复、注释偏弱、手动 fd 管理未统一 RAII。

---

### 1. 整体结构与架构：8.5/10 优秀

**优点：**
*   **职责划分清晰：** `core(引擎)` -> `providers/lsp/session(可插拔后端)` -> `ui(界面)` -> `app(组装)`，符合依赖倒置，`core` 零 UI 依赖。
*   **接口抽象到位：** `Model`/`Tool`/`SessionStore`/`ContextManager`/`TokenEstimator` 均为纯虚接口，`AgentLoop(Model&, ToolRegistry&, SessionStore&, ContextManager&)` 完全可注入，易 Mock。
*   **协议化解耦：** 子代理通过 `worker_protocol.hpp` JSON 行协议 + `ProcessSubagentRunner` 多进程隔离，`kWorkerProtocolVersion=1`，设计成熟。

**问题：**
*   **路径错位：** `src/ui/tools/` 实现 `include/zed/tools/` 接口，历史重构遗留，建议统一为 `src/tools/`。
*   `include/zed/support/` 空目录，无说明。
*   `src/main.cpp` 承担了组装根 + 10+ 个斜杠命令注册 + TUI/非TUI双分支，约 500+ 行，圈复杂度 >30，是最大架构污点。

| 模块 | 文件数 | 职责 | 评价 |
| :--- | :--- | :--- | :--- |
| `core` | 6 | Agent循环、上下文、工具注册、会话抽象 | 核心扎实 |
| `ui` | 3+4 tools | FTXUI终端、Markdown渲染、7个内置工具 | 最重，需拆分 |
| `subagents` | 4 | explorer只读代理、进程池、worker协议 | 设计亮点 |
| `session` | 2 | JSONL+flock持久化、Session Catalog | 异常安全优秀 |
| `app` | 2 | 配置解析、本地鉴权Web服务 | 安全实现到位 |
| `lsp/plugins/providers` | 各1-2 | clangd、dlopen插件、opencode模型发现 | 可插拔 |

**目录结构：**
```
src/
├── main.cpp (约800行，组装根)
├── app/          (2 files) - 应用配置与Web配置服务
├── core/         (6 files) - 核心引擎
├── extensions/   (2 files) - 扩展命令与快捷输入
├── lsp/          (1 file)  - clangd语言服务
├── plugins/      (1 file)  - 外部插件系统
├── providers/    (2 files) - 模型提供方
├── session/      (2 files) - 会话持久化
├── skills/       (1 file)  - 技能系统
├── subagents/    (4 files) - 子代理
└── ui/           (3 files + tools/ 4 files) - 终端界面与工具实现
```

---

### 2. 编码规范与可维护性：7.0/10 良好，局部债务明显

| 维度 | 评分 | 详情 |
| :--- | :--- | :--- |
| **命名一致性** | 8/10 | `snake_case`函数/`PascalCase`类型/`kCamel`常量/`zed::core`命名空间 全仓统一。瑕疵：`kVersion` vs `ZEDA_VERSION` 宏混用 |
| **代码重复度** | 6/10 | **中等偏高**。`trim_ascii_whitespace` 在 `main.cpp` 和 `agent_loop.cpp` 重复；`config.cpp` 中 `config_string/size/double/text` 四函数结构重复；`basic_tools.cpp` 中 `resolve_path`+`max_results`截断逻辑在6个工具中重复 |
| **函数长度** | 5.5/10 | **主要风险**。`main()` 500+行、`TerminalApplication::run()` 250+行含大型状态机、`clangd_client::Impl::start()` 250行、`parse_workspace_config()` 150+行。建议拆 `register_builtin_commands()`/`create_tool_registry()`/`handle_event()` |
| **注释质量** | 5/10 | **偏弱**。全仓几乎无 `///` 文档，`context_breakdown_for` 的 token调和算法、`extract_message` 的 Content-Length 状态机、`terminal` 渲染逻辑均无注释，依赖自解释代码 |
| **魔法数字** | 6/10 | `1'048'576`/`16'777'216`/`512`/`3'600'000` 散落在 `config.cpp`，`poll 50ms`/`buffer 8192`/`0.8` 触发阈值未命名。`kMaximumHeaderBytes` 等已命名是正面案例 |

**详细分析：**

*   **命名规范：** 全仓统一 `snake_case` 函数/变量（`trim_ascii_whitespace` `src/main.cpp:22`）、`PascalCase` 类型（`AgentLoop` `ClangdClient`）、`kCamel` 常量（`kVersion` `kDeferredActionCorrection`）、`namespace zed::core/lsp/ui` 分层清晰。
*   **代码重复：** `config.cpp` 中 `config_string/config_size/config_double/config_text` 四个校验函数结构高度重复，可模板化；`basic_tools.cpp` 中 `ReadFileTool/WriteFileTool/EditFileTool` 的 `resolve_path`+`parse_arguments` 流程重复。
*   **函数长度：** `src/main.cpp:main` 约 500+ 行，承担参数解析、配置加载、模型/工具/插件/命令注册、TUI/非TUI双分支，圈复杂度 >30，难以单测。
*   **注释：** 全仓几乎无块注释/函数文档；公共头（`tool_registry.hpp` `context.hpp`）未见 `///` 文档，`Result<T>` 错误码含义需查 `ErrorCode` 枚举。

---

### 3. 健壮性与安全性：8.2/10 较好

#### 资源管理：7.5/10
*   **好：** 全仓 **0处 `new/delete`**，`unique_ptr<Tool>`/`shared_ptr<CancellationSource>`/`scoped_lock` 普及，`Result<T>` 值语义。
*   **债务：** `clangd_client.cpp` 和 `BashTool` 中 `pipe/fork/dup2` 裸 `int fd` + `pid_t` 手动 `close/kill/waitpid`，异常路径依赖 `terminate_process()`，未复用 `subagent_runner.cpp` 中已有的 `UniqueFd` RAII 封装。`plugin_manager` 的 `dlopen/dlclose` 同理。

#### 线程/异常安全：8/10
*   `ToolRegistry`/`SessionStore`/`ClangdClient` 均 `scoped_lock`，`next_id` 用 `atomic_uint64_t`，`worker_cancel_requested` 用 `volatile sig_atomic_t` + `sigaction` 符合POSIX。
*   全仓以 `Result<T>` + `ErrorCode(10种)` 为主，`try/catch` 仅在边界转 `tool_error`。`write_file_atomically(O_EXCL+fsync+rename+chmod 0600)` 和 `append_records` 具备强异常安全。
*   隐患：`PluginManager::loaded_` 无锁（假设单线程）、`BasicContextManager::build` 无超时。

#### 安全性：8.5/10 亮点
*   路径逃逸：`resolve_path(weakly_canonical+inside_root)`、`path_is_inside`、`symlink_status` 拒绝 symlink 攻击，三处一致。
*   敏感信息：`fork` 子进程 `clear_sensitive_environment(OPENAI_API_KEY...)`、`safe_diagnostics` 脱敏 `authorization/token`。
*   Web服务：`127.0.0.1`随机端口 + `random_token()` + `X-Zeda-Config-Token`双校验 + `Origin`校验 + `CSP/no-store`，`32MiB` 限长，正确。

#### 性能：8/10 无大隐患
*   `tool_registry` 每次 turn `Json::parse+dump` 重建 schema 可缓存；`jsonl_session_store` 每次全量重解析文件，大会话(>10MB)会变慢；`ApproximateTokenEstimator (bytes+3)/4` 线性估算可接受。

**技术债务扫描：**
`grep TODO/FIXME/HACK/XXX/BUG src` **0命中**，`grep new/delete` **0命中**，无过时API(`strcpy/rand/auto_ptr`)，编译警告隐患低。

---

### 4. 现代C++与工程化：7.5/10 现代但保守

*   **标准：** `CMakeLists.txt: CMAKE_CXX_STANDARD 20`，实际 `C++17` 为主(`optional/variant/string_view/filesystem/[[nodiscard]]`) + 仅 `std::span` 用到 C++20，未用 `concepts/ranges/coroutine/format/jthread`。
*   **构建：** 现代 CMake (`FetchContent` 锁哈希 `nlohmann_json v3.12.0`/`ftxui v7.0.3`/`cpp-httplib`，`target_*` 声明式，`-Wall -Wextra -Wpedantic -Wconversion`)，但无 `clang-tidy/sanitizer/ccache/IPO/compile_commands`。
*   **头文件：** 34个头全 `#pragma once` (仅 `plugin_sdk.h` 用 `#ifndef` 兼容C)，无前向声明优化，增量编译成本偏高。
*   **基础设施：** 配置/会话/插件/技能 完整，但 **无日志框架** (仅 `AgentEvent` 回调)，可观测性弱。
*   **测试：** 无 `gtest/catch2`，仅 `zed_core_smoke` + `CTest` + `fake_clangd/fake_opencode` 桩，属于轻量 smoke，非真单元测试，无覆盖率。
*   **依赖管理：** 全部 `FetchContent` 声明式管理，带哈希/Tag锁定，可复现；不足是每次 `configure` 需联网，未用 `find_package` 优先系统库。

**C++特性使用：**
*   已用：`std::optional`/`variant`/`string_view`/`filesystem`/`[[nodiscard]]`/`constexpr`/`unique_ptr`/`enum class`/`=default/=delete`/`override`
*   未用：`concept/requires`/`ranges`/`coroutine`/`modules`/`std::format`/`std::jthread`

---

### 5. 优先级建议

**P0 - 影响可维护性：**
1.  拆分 `main.cpp` -> `register_builtin_commands.cpp` / `create_tool_registry.cpp`
2.  拆分 `terminal.cpp::run()` -> `build_root_element()` / `handle_event()`
3.  统一 `UniqueFd/ScopeGuard` 封装 `clangd_client`/`BashTool` 的 fd/pid

**P1 - 提升质量：**
4.  抽 `string_util.h` 消除 `trim` 重复，模板化 `config_*` 校验，抽 `ToolLimits` 公共解析
5.  为魔法数字定义 `constexpr kMaxOutputBytes/kContextTriggerRatio`
6.  补充核心算法注释 (`context_breakdown`, `extract_message`, `percent_encode`)
7.  引入 `spdlog` + `clang-tidy` + `ASan/TSan` CI

**P2 - 长期：**
8.  修正 `src/ui/tools` -> `src/tools` 路径，清理空 `support` 目录
9.  缓存 `ToolRegistry::definitions()` 的 JSON schema，`JsonlSessionStore` 增量解析
10. 引入 `gtest` + 前向声明优化头文件依赖

---

> **总结：** 这是一份“学院派+实战派”结合的代码，安全和异常处理远超平均水平，架构经得起扩展。只要解决超长函数和重复代码两大债务，可维护性可提升至 8.5+。
