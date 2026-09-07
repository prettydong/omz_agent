# 插件运行时设计

zeda 的本机插件使用版本化 C ABI。ABI v1 提供 command/tool，ABI v2 在保持 v1
插件可加载的前提下增加类型化 Agent Hook。当前运行时借鉴了 DeepSeek Harness/Cordis
的两个核心原则：插件贡献必须随插件生命周期可逆，插件依赖必须通过声明解析，而不是
依赖目录遍历顺序。这里借的是设计不变量，不兼容也不复刻 Cordis API。

参考资料：

- [DeepSeek Harness 架构](https://github.com/deepseek-ai/deepseek-harness/blob/0a53fb55bea101816fa226bb964ae2bed71c343b/docs/architecture.md)
- [Cordis 生命周期与 effect](https://github.com/deepseek-ai/deepseek-harness/blob/0a53fb55bea101816fa226bb964ae2bed71c343b/docs/cordis-tutorial/02-lifecycle-and-effects.md)
- [Cordis 服务依赖](https://github.com/deepseek-ai/deepseek-harness/blob/0a53fb55bea101816fa226bb964ae2bed71c343b/docs/cordis-tutorial/03-services.md)

## Manifest 与发现

内置插件可以把同一个 C ABI descriptor 静态链接进 `zeda`，无需 manifest 或
`dlopen`。内置项先进入依赖图，并优先于搜索路径中同 ID 的动态插件；动态副本会显示为
`shadowed`。DeepWiki 默认使用这种模式，可通过 CMake 的
`ZEDA_DEEPWIKI_LINKAGE=SHARED` 恢复为外部动态插件。

每个插件目录包含一个不超过 1 MiB 的 `zeda-plugin.json`：

```json
{
  "id": "wiki-export",
  "name": "Wiki Export",
  "version": "1.0.0",
  "abi_version": 1,
  "library": "libwiki_export.dylib",
  "resources": "resources",
  "requires": ["deepwiki"]
}
```

`requires` 可省略；它是必需插件 ID 数组，不是权限声明。运行时先读取并校验全部 manifest，再执行任何动态库代码。只有依赖均为 `active` 的插件才会初始化，因此加载顺序不受文件名影响。缺失或失败的依赖会让消费方保持 `pending`，依赖环也会明确显示为 `pending`。

搜索根按配置顺序保留优先级。相同 ID 的第一个 manifest 获胜，后续项标记为 `shadowed`，不会作为失败项回退加载。相同 canonical manifest 只处理一次；指向搜索根外的子目录符号链接会被拒绝。动态库和资源路径也必须留在插件目录内。

状态机为：

```text
discovered -> loading -> active -> unloading -> disposed
                  |
                  +-> failed

discovered -> pending
discovered -> shadowed
```

`discover_and_load()` 在同一个 manager 上重复调用是幂等的。`shutdown()` 是终态操作；关闭后不能再次发现或加载。

## ABI v2 与 Agent Hook

ABI v1 的 `zeda_plugin_entry_v1`、`ZedaPluginDescriptorV1` 和
`ZEDA_PLUGIN_ABI_VERSION` 保持兼容；后者仍是 v1 的源码兼容别名。需要 Hook 的插件必须在
manifest 中声明 `"abi_version": 2`，导出 `zeda_plugin_entry_v2`，并使用带
`struct_size` 的 `ZedaPluginDescriptorV2`、`ZedaHostApiV2` 和 `ZedaHookV2`。宿主接受
大于当前结构大小的 v2 结构，为以后在尾部增加字段保留空间。

插件只能在 `initialize()` 返回前调用 `register_hook`。每个 Hook 有插件内唯一名称、类型
和有符号优先级；数值较小的优先执行，相同优先级按插件加载及注册顺序稳定执行。一个
回调收到完整 JSON 对象，并返回以下动作之一：

- `ZEDA_HOOK_CONTINUE`：不修改负载，继续执行后续 Hook。
- `ZEDA_HOOK_REPLACE`：向 replacement sink 写入一个完整 JSON 对象；后续 Hook 看到替换后的负载。
- `ZEDA_HOOK_REJECT`：阻止当前操作，并可向 error sink 写入安全的拒绝原因。

最小注册形态如下；`execute_hook` 必须在插件对象销毁前保持有效：

```cpp
const ZedaHookV2 hook{
    sizeof(ZedaHookV2),
    {"search-policy", 13},
    ZEDA_HOOK_BEFORE_TOOL_CALL,
    0,
    plugin_state,
    execute_hook,
};
if (host->register_hook(host->context, &hook, error) != 0)
  return 1;
```

所有负载都有不可修改的公共 envelope：

```json
{
  "schema_version": 1,
  "hook": "before_tool_call",
  "turn_id": "turn-..."
}
```

当前 Hook 和可修改字段如下：

| Hook | 发生位置 | 可修改内容 |
| --- | --- | --- |
| `agent_turn_start` | 输入校验后、提交用户消息前 | `user_input` |
| `user_message_submit` | 创建用户消息后、持久化前 | `message.content` |
| `before_model_request` | 上下文压缩及工具快照完成后、调用 provider 前 | `request` 中的模型、消息、工具及生成参数 |
| `after_model_response` | provider 完成后、校验及持久化前 | response 的 `content`、`tool_calls`、`finish_reason`；usage 只读 |
| `before_tool_call` | assistant 消息持久化和工具执行前 | tool name 与 `arguments_json`；call id 只读 |
| `after_tool_result` | 工具返回后、结果持久化前 | `content` 与 `is_error`；call id、tool name、usage 只读 |
| `session_write` | Agent Loop 的 `begin_turn`、`append_message`、`finish_turn` 提交前 | 消息内容、tool result 的 `is_error`、turn outcome/detail；记录身份和 assistant tool calls 只读 |
| `agent_turn_end` | Agent Loop 得到最终 outcome 后、写 `turn_end` 前 | outcome 与 detail，但不能把失败操作伪装成成功 |

OpenCode Go 的 `request.session_id` 由宿主维护。`model_state` 是需要原样保留的 provider
continuation；存在此字段时，Hook 必须同时保持绑定的 assistant 内容与工具调用，
不得修改对应的工具参数。修改会返回明确错误。usage 新增只读 `cache_write_input_tokens`，
`cached_input_tokens` 仅统计缓存读取。参见 [协议契约](opencode_go.md)。

`iteration` 和 `call_index` 从零开始并且不可修改。replacement 必须保留 envelope 和对应
Hook 的只读关联字段；JSON、枚举、消息角色、tool call、schema 或关联 ID 无效时，宿主以
`hook_error` 终止当前操作。`after_model_response` 发生在流式 delta 已经送达 UI 之后，故它
修改的是最终响应和 Session 内容，不会倒改已经展示的 delta。

执行顺序为：

```text
agent_turn_start
-> user_message_submit
-> session_write(begin_turn)
-> [before_model_request -> provider -> after_model_response
    -> before_tool_call* -> session_write(assistant)
    -> tool -> after_tool_result -> session_write(tool)]*
-> agent_turn_end
-> session_write(finish_turn)
```

`session_write` 拒绝普通记录时，原记录不会落盘，turn 随后按失败路径结束。若它拒绝
`finish_turn` 本身，宿主会绕过 Hook 写入一个失败终止记录，避免留下永久 active turn；
原本请求的成功终止仍被拦截。这个 Hook 只覆盖 Agent Loop 的对话事务，不拦截 Session
启动恢复、改名、fork 或 catalog 元数据维护。

Hook 回调共享插件的取消信号、单次输出字节预算和 in-flight gate。替换输出被截断后若
不再是合法 JSON，会作为明确错误返回。插件卸载先停止新回调、撤销全部 Hook，再等待已经
进入的回调退出，因此不会在回调执行中 `dlclose`。

## 可逆贡献与安静卸载

插件初始化期间注册的 command/tool/hook 先暂存并整体校验。提交成功后，每项注册都归属于一个 `PluginContributionScope`。初始化或提交任一步失败时，scope 会按注册顺序的逆序撤销已有贡献；正常关闭使用完全相同的释放路径。初始化返回后再次调用注册 API 会得到明确错误。

卸载顺序固定为：

```text
所有插件停止接收新调用并发出取消
-> 从 command/tool/hook registry 撤销贡献
-> 等待已经进入的调用退出
-> 按依赖加载顺序的逆序调用 shutdown/destroy
-> 动态插件 dlclose
```

Registry 在解锁后执行回调时持有共享生命周期，插件回调另有 in-flight gate。这两个条件一起避免“刚取到回调，插件就被卸载”的竞态。插件必须轮询收到的 `ZedaCancellationV1`；本机插件属于受信任代码，故意忽略取消仍可能阻塞进程退出。

command、tool、事件和初始化错误使用宿主提供的有界 sink。事件预算按单次 command 调用累计，而不是按每条 event 重新计算；累计内容达到 `PluginManagerConfig::max_output_bytes` 后停止转发，并且只写入一次 `[plugin output truncated]` 标记。流式事件的固定截断标记不计入内容预算。应用使用与 Shell 工具相同的最大命令输出预算。

## 信任边界

同进程的静态或动态插件拥有 zeda 进程的全部操作系统权限。Manifest 依赖、路径检查和输出预算提高的是可组合性与故障隔离，不是 sandbox，也不能阻止插件直接访问文件、网络、环境变量或进程 API。当前不支持运行时热重载；更换原生库后应重启 zeda。

后续若面向不受信任的第三方插件开放，应优先增加独立 `plugin-host` 进程和受限协议。
后续 ABI 应继续使用 `struct_size` 做版本协商，并引入稳定 Service Definition/vtable、
显式 capability broker 和结构化错误；不要把新能力追加到 `ZedaHostApiV1`。
