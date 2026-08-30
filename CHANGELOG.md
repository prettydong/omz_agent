# Changelog

## [0.2] - 2026-08-30

### Added

- 新增 Session v2：使用 JSONL 持久化回合边界、工具结果、错误和中断恢复状态，支持 Session 列表、创建、打开、重命名和派生。
- 新增本机 `clangd` 集成，支持 diagnostics、hover、definition、references 和 document symbols 查询。
- 新增外部插件 C ABI、插件发现与加载机制，并内置 DeepWiki 插件；DeepWiki 使用 SQLite FTS5、clangd 和本地模型分析 C/C++ 仓库并生成中文 Wiki。
- 新增 `multi_bash`，可在一次工具调用中并行执行互不依赖的 Shell 命令，并限制并发数和聚合输出。
- 新增 OpenCode Go 模型目录，支持查看模型、显式刷新目录，以及按模型元数据选择请求协议和 reasoning 档位。
- 新增只读 Sub Agent：Worker 在独立进程和上下文中运行，支持 Parallel/Chain 编排、动态角色配置、取消、超时和输出限制。
- 新增 `/configure-web` 本地 Web 配置页，可管理主 Agent 配置档案、Sub Agent、Workspace Skill、上下文控制参数和 Tool 白名单。
- 新增版本化配置迁移、严格 schema 校验、原子写入和 `0600` 权限保护；配置页使用随机访问令牌、仅监听 `127.0.0.1`，不保存 API Key。

### Changed

- OpenCode Go 默认从本机 `opencode` 凭证库读取 API Key，同时保留环境变量覆盖和自定义凭证路径配置。
- 终端补全支持命令与二级选项，Enter 和 Tab 均可补全；新增启动耗时、上下文占用和模型输出速率摘要。
- 增强 UTF-8、工具输出、Session 序列化和配置错误处理，异常状态会明确展示，不再静默丢弃。
- Skill 支持启用、停用、编辑、移除和归档；主 Agent 与 Worker 的工具权限均采用运行时白名单。
- 统一配置与 Skill 的原子写入、模型上下文上限策略和子进程终止流程；取消与超时会先发送 `SIGTERM`，宽限期后再升级为 `SIGKILL`。
- 外部进程统一改用 `posix_spawn` 和最小环境白名单；Sub Agent 只显式继承模型运行所需配置，且始终禁止 Shell 工具。
- DeepWiki 新增 `/deepwiki tui` 双栏终端浏览器；插件命令选项可以声明受限的结构化文档视图，由主 TUI 统一渲染和导航。

### Fixed

- 修复 TUI 中 `/session list`、`rename` 以及切换 Session 后的命令结果不显示。
- 修复 `write`/`edit` 临时文件跟随符号链接，并在原子替换时保留普通权限位和取消语义。
- 修复并发 `multi_bash`、clangd、模型进程和 Sub Agent 之间继承管道描述符导致的挂起。
- 修复后台后代持有管道导致 Shell 超时失效、Provider 流和 tool index 无界增长、Edit 替换结果绕过大小限制、TUI 命令状态数据竞争，以及 Skill 通过符号链接或超大文件越过读取边界。
- 修复插件部分注册失败后残留已卸载动态库回调；加载前执行完整校验，失败时回滚注册项。
- 修复上下文压缩拆分 assistant tool call 与对应 tool result，controller 和确定性回退均按完整工具交换选择。
