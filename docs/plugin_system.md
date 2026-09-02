# 插件运行时设计

zeda 的本机插件使用版本化 C ABI。当前运行时借鉴了 DeepSeek Harness/Cordis 的两个核心原则：插件贡献必须随插件生命周期可逆，插件依赖必须通过声明解析，而不是依赖目录遍历顺序。这里借的是设计不变量，不兼容也不复刻 Cordis API。

参考资料：

- [DeepSeek Harness 架构](https://github.com/deepseek-ai/deepseek-harness/blob/0a53fb55bea101816fa226bb964ae2bed71c343b/docs/architecture.md)
- [Cordis 生命周期与 effect](https://github.com/deepseek-ai/deepseek-harness/blob/0a53fb55bea101816fa226bb964ae2bed71c343b/docs/cordis-tutorial/02-lifecycle-and-effects.md)
- [Cordis 服务依赖](https://github.com/deepseek-ai/deepseek-harness/blob/0a53fb55bea101816fa226bb964ae2bed71c343b/docs/cordis-tutorial/03-services.md)

## Manifest 与发现

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

## 可逆贡献与安静卸载

插件初始化期间注册的 command/tool 先暂存并整体校验。提交成功后，每项注册都归属于一个 `PluginContributionScope`。初始化或提交任一步失败时，scope 会按注册顺序的逆序撤销已有贡献；正常关闭使用完全相同的释放路径。初始化返回后再次调用注册 API 会得到明确错误。

卸载顺序固定为：

```text
所有插件停止接收新调用并发出取消
-> 从 command/tool registry 撤销贡献
-> 等待已经进入的调用退出
-> 按依赖加载顺序的逆序调用 shutdown/destroy
-> dlclose
```

Registry 在解锁后执行回调时持有共享生命周期，插件回调另有 in-flight gate。这两个条件一起避免“刚取到回调，插件就被卸载”的竞态。插件必须轮询收到的 `ZedaCancellationV1`；本机插件属于受信任代码，故意忽略取消仍可能阻塞进程退出。

command、tool、事件和初始化错误使用宿主提供的有界 sink。事件预算按单次 command 调用累计，而不是按每条 event 重新计算；累计内容达到 `PluginManagerConfig::max_output_bytes` 后停止转发，并且只写入一次 `[plugin output truncated]` 标记。流式事件的固定截断标记不计入内容预算。应用使用与 Shell 工具相同的最大命令输出预算。

## 信任边界

同进程动态库拥有 zeda 进程的全部操作系统权限。Manifest 依赖、路径检查和输出预算提高的是可组合性与故障隔离，不是 sandbox，也不能阻止插件直接访问文件、网络、环境变量或进程 API。当前不支持运行时热重载；更换原生库后应重启 zeda。

后续若面向不受信任的第三方插件开放，应优先增加独立 `plugin-host` 进程和受限协议。ABI v2 再引入带 `struct_size` 的版本协商、稳定 Service Definition/vtable、显式 capability broker 和结构化错误；不要继续把新能力无限追加到 `ZedaHostApiV1`。
