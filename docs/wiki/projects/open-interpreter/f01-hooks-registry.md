---
project: open-interpreter
feature: hooks-registry
title: 钩子注册与触发
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/hooks/src/registry.rs
    symbols: [Hooks, HooksConfig, list_hooks]
  - path: codex-rs/hooks/src/types.rs
    symbols: [Hook, HookPayload, HookEvent, HookResult]
---

# 钩子注册与触发

## 一句话职责

把「在 agent 生命周期的若干时刻插入外部命令或 MCP 调用」做成一个**有配置、有信任模型、有失败策略**的系统：`Hooks` 是会话级的运行时句柄，`HookPayload` / `HookEvent` 是触发时喂给钩子的输入，而 `HookResult` 定义了一个钩子失败时该不该中止整个操作。

## 机制是什么

`hooks` crate 里有两类完全不同的钩子，这个模块把它们并到一起：

| 类别 | 载体 | 触发方式 |
|---|---|---|
| legacy `notify` | `after_agent: Vec<Hook>` | 老配置里的一个命令 argv，被包成本地钩子函数 |
| Claude-code 风格的 managed hooks | `engine: ClaudeHooksEngine` | 从配置文件与插件来源发现，按事件类型运行 |

而 `registry.rs` 的公开面是一串**成对的方法**：每个事件都有 `preview_*`（不执行，只给出将要跑什么）与 `run_*`（实际执行）。九对：`session_start`、`pre_tool_use`、`permission_request`、`post_tool_use`、`pre_compact`、`post_compact`、`user_prompt_submit`、`stop`、`session_end`，外加 `interrupt`。

## 怎么实现的

**`HooksConfig` 有八个字段，逐个都能解释为什么需要。** `Default + Clone`，全部公开：

| 字段 | 作用 |
|---|---|
| `legacy_notify_argv` | 老配置里的 notify 命令 |
| `feature_enabled` | 总开关 |
| `bypass_hook_trust` | 跳过信任检查（危险开关） |
| `config_layer_stack` | 配置分层栈（钩子从哪一层来） |
| `plugin_hook_sources` | 插件提供的钩子来源 |
| `plugin_hook_load_warnings` | 加载插件钩子时的警告 |
| `shell_program` / `shell_args` | 跑命令钩子用哪个 shell |

**`shell_program` 是 `Option<String>` 且构造时用 `unwrap_or_default()`**（`CommandShell { program: config.shell_program.unwrap_or_default(), args: config.shell_args }`）。也就是说「没配 shell 程序」时传给运行时的是一个**空字符串**——而空字符串意味着「让运行时自己决定」。这个约定的下游在 `engine` 里，本模块只负责传。

**`Hooks` 的字段划分体现了「会话绑定」与「配置」的边界。** 五个字段：`environment`（进程环境变量的快照）、`after_agent`（legacy 钩子）、`engine`（managed hooks 引擎）、`plugin_hook_sources` 与 `plugin_hook_load_warnings`（后两者是为了 `matches_plugin_hooks` 与 `reconfigured` 的相等性判断而存的副本）。

而 `environment` 上的那段 `TODO` 注释很值得记：

> 等 legacy `notify` 被移除后，把这个快照捕获进 `CommandHookRuntime::new`，并从 `Hooks` 与 `from_config` 里去掉环境变量的管道。

也就是说 `environment` 现在是**两套机制的公共依赖**：legacy 钩子函数需要它，`CommandHookRuntime` 也需要它。等 legacy 走掉，这个字段就可以消失。`TODO` 在源码里指出了一个「因为旧机制而存在的技术债」，并写清了清理条件——这是技术债注释应有的形态。

**三个构造函数各自负责一件事。** `new(config, thread_id, mcp_executor)` 是会话启动路径；`reconfigured(config)` 是运行中换配置；`from_config` 是两者共用的装配。

`new` 做四件事：

1. 建一个 `async_channel::unbounded()` 用于钩子完成事件。
2. `let environment = Arc::new(std::env::vars_os().collect())`——**把整个进程环境快照下来**。
3. `from_config` 装配，并把一个闭包作为 `build_runtime` 传进去（闭包负责造 `CommandHookRuntime`，绑定 thread id 与结果发送端）。
4. **检查 `engine.required_load_errors()`，非空就 `bail!`**。

第 4 步是这一页最重要的一条设计：**「必需的 managed hooks 加载失败」会让会话创建直接失败**。错误文案是 `failed to load required managed hooks: {errors joined by ; }`。

这与 `reconfigured` 的处理形成对照——`reconfigured` 不做这个检查。也就是说「启动时缺了必需钩子是致命的，运行中换配置时缺了不是」。这个差异是合理的：启动是一个全新会话的起点，此时失败是干净的；而运行中换配置若失败，中止当前会话的代价太大，所以让警告留着。

**`reconfigured` 的关键是「保留在飞的钩子」。** 它的文档注释只有一句话：**应用刷新后的配置时保留在飞的后台钩子**。实现是复用 `self.engine.command_runtime.reconfigured(shell)` 而不是新建一个运行时。

这一点很实：一个已经开始的命令钩子可能已经跑了十几秒，若换配置时把它杀掉，那个钩子的副作用会半途中断。`reconfigured` 让旧的在飞实例继续跑完，新配置只影响后续触发。

**`matches_plugin_hooks` 是为「需不需要重新装配」提供判据的。** 实现是两次 `.iter().eq(...)`：来源列表与警告列表都按顺序逐项相等。于是调用方可以问「这份新配置的插件钩子部分和当前的一样吗」，一样就跳过重装配。

用 `Iterator::eq` 而不是集合比较，说明**顺序被当作语义的一部分**：同样的来源换个顺序就算不同。对钩子来说这是对的——来源顺序可能影响执行顺序。

**`dispatch` 是唯一直接执行本地 `Hook` 列表的方法，它的失败策略写在 `HookResult` 里。** 实现很简单：

```
for hook in hooks {
    let outcome = hook.execute(&hook_payload).await;
    let should_abort = outcome.result.should_abort_operation();
    outcomes.push(outcome);
    if should_abort { break; }
}
```

也就是说**逐个串行执行，遇到 `FailedAbort` 就停**。而 `should_abort_operation()` 的定义是 `matches!(self, Self::FailedAbort(_))`——只看这一个变体。

**`HookResult` 的三态是本页的核心语义。**

| 变体 | 含义 |
|---|---|
| `Success` | 成功 |
| `FailedContinue(Box<dyn Error>)` | **失败，但后续钩子照跑、操作继续** |
| `FailedAbort(Box<dyn Error>)` | **失败，后续钩子不跑、操作中止** |

文档注释把三条都写清了，尤其是 `FailedContinue`：「其他后续钩子仍应执行，且操作应继续」。

这个三态与 `d01`（goose 的 `InspectionAction`）是一个路数的判断：**「判定」与「处置」分开**。这里是「钩子失败了」与「失败要不要中止」分开。区别在于这里把两件事压进一个枚举的三个变体，而 goose 用两个布尔字段加一个转换点。**两者都能表达，但枚举的好处是「不可能出现 `failed && !abort && !continue` 这种非法组合`。

**`HookPayload` 的线格式有三处刻意的形状。**

1. `#[serde(rename_all = "snake_case")]`——字段名如 `session_id`、`hook_event`。
2. `client: Option<String>` 带 `skip_serializing_if`——没客户端信息时字段缺失。
3. `triggered_at` 用**自定义序列化器** `serialize_triggered_at`，实现是 `value.to_rfc3339_opts(SecondsFormat::Secs, true)`。

第 3 点值得展开：`SecondsFormat::Secs` 表示**只到秒**（去掉纳秒），`true` 表示用 `Z` 而不是 `+00:00` 后缀。于是线格式里的时间戳形如 `2025-01-01T00:00:00Z`——一个定长、无小数、UTC 标记的字符串。

而定长这条很关键：钩子大多是**外部脚本**，它们要解析这个时间。带纳秒的 RFC 3339 在不同实现里输出的小数位数不同，定长就没有这个问题。测试 `hook_payload_serializes_stable_wire_shape` 断言的正是这个完整 JSON 形状（含 `"triggered_at": "2025-01-01T00:00:00Z"`）——**测试名里的 stable 是准确的：它把「外部脚本依赖的线格式」当成契约钉住**。

**`HookEvent` 只有一个变体，但带了 `tag = "event_type"`。** 编码是：

```
HookEvent::AfterAgent { #[serde(flatten)] event: HookEventAfterAgent }
```

`flatten` 让 `HookEventAfterAgent` 的四个字段（`thread_id`、`turn_id`、`input_messages`、`last_assistant_message`）与判别标签 `event_type` 平铺在同一层。测试的期望 JSON 正是这个形状：

```
"hook_event": { "event_type": "after_agent", "thread_id": ..., "turn_id": ..., "input_messages": ["hello"], "last_assistant_message": "hi" }
```

也就是说**外部脚本拿到的是一个扁平对象**，判别靠 `event_type`，其余字段直接在顶层。

用 `flatten` 而非嵌套一层 `payload` 字段，好处是脚本写起来简单（`$hook_event.turn_id` 而不是 `$hook_event.payload.turn_id`）；代价是**未来若加第二个事件类型，两个变体的字段名必须在扁平层里互不冲突**。这是一个为「脚本易用」付出的架构约束，且它是可以接受的，因为事件类型之间通常语义不同、字段名自然不同。

**只有 `HookEventAfterAgent` 一个事件类型，而 `Hooks` 的九对 `preview_*` / `run_*` 方法却覆盖十个事件。** 这说明两套机制的事件词汇表不在同一层：`HookEvent`（`types.rs`）服务的是**本地 `Hook` 列表**（目前只有 legacy notify 走它），而 managed hooks 的十种事件是 `engine` 内部的 `*Request` / `*Outcome` 类型。

所以在读这个模块时不能把两者混起来：`dispatch` 只跑 `after_agent` 那一个 `Hook` 列表；managed hooks 全部走 `engine.run_*`。

**`list_hooks` 是一个纯预览入口，且总开关在最前面。** 实现第一行是 `if !config.feature_enabled { return HookListOutcome::default() }`——**关掉功能时连发现都不做**。这比「发现了但过滤掉」好：发现过程要读配置层与插件目录，关掉功能时不该有任何 IO。

之后的 `discover_handlers` 接收四个参数（配置层栈、插件来源、插件加载警告、是否跳过信任检查），返回 `HookListOutcome { hooks, warnings }`。也就是说**预览结果本身带警告** ——「你配置了这个钩子但它的路径不存在」这类信息跟着列表一起给出，而不是等到触发时才发现。

**`Hooks::shutdown` 只有一句 `self.engine.command_runtime.shutdown().await`，文档注释说明它会「在会话关闭时中止并 join 未完成的异步钩子」。** 关键词是 join：中止之后还要等它们结束。一个只发中止信号不等完成的 shutdown 会让进程退出时留下半写的文件。

**`max_permission_request_timeout` 的注释解释了一个容易误判的语义。** 原文：**PermissionRequest 钩子里配置的最大超时。匹配的 handler 并发运行，所以它们的合计超时由这个最大值界定**。

也就是说这十个事件里 PermissionRequest 的处理是**并发**的（其余大概串行），于是「一批钩子要跑多久」的上界是最长的那个超时，不是总和。这个信息对上层设置自己的整体超时是必需的。

**`from_config` 里 legacy 钩子的构造带一条空值过滤。** 那段是 `config.legacy_notify_argv.filter(|argv| !argv.is_empty() && !argv[0].is_empty())`——**空 argv 或空程序名都不算配置了钩子**。

`command_from_argv` 做同样的检查（`split_first()?` 后 `if program.is_empty() { return None }`），并额外做三件事：

1. `command.env_clear()`——**清空继承的环境**；
2. `command.envs(environment)`——只注入快照里的那些；
3. `scrub_non_inheritable_env_vars(command.as_std_mut())`——再清洗一遍不可继承的变量。

三步合起来是一条清晰的策略：**外部钩子进程拿到的是「会话创建那一刻的环境快照」，不是「当下进程的环境」**。这样同一个钩子在会话开始后即使用户改了环境变量，行为也不变；而 `scrub_non_inheritable_env_vars` 保证那些标记类变量（用于告诉子进程「你在沙箱里」之类）不会泄漏出去。

## 关键取舍

**源码事实：** 必需 managed hooks 加载失败会让 `Hooks::new` 直接 `bail`，但 `reconfigured` 不做这个检查——启动失败是干净失败，运行中失败代价太大。

**源码事实：** `reconfigured` 复用 `command_runtime.reconfigured(shell)` 以保留在飞的后台钩子，而不是新建运行时。

**源码事实：** `environment` 是 legacy notify 与 `CommandHookRuntime` 的公共依赖，`TODO` 写明了 legacy 移除后可以去掉这条管道。

**源码事实：** `HookResult` 三态（`Success` / `FailedContinue` / `FailedAbort`），`dispatch` 遇到 `FailedAbort` 就 `break`。用枚举而非两个布尔，非法组合不可表示。

**源码事实：** `HookPayload.triggered_at` 用 `to_rfc3339_opts(SecondsFormat::Secs, true)`，线格式定长到秒且用 `Z` 后缀；测试断言完整 JSON 形状把这条契约钉住。

**源码事实：** `HookEvent` 用 `tag = "event_type"` 加 `flatten`，外部脚本拿到的是扁平对象；代价是将来加事件类型时扁平层字段名不能冲突。

**源码事实：** `HookEvent` 目前只有一个变体；`dispatch` 只跑 legacy `after_agent` 列表，十个 managed 事件全部走 `engine.run_*`——两套事件词汇表不在同一层。

**源码事实：** `list_hooks` 在总开关关闭时直接返回默认值，连发现都不做（无 IO）。

**源码事实：** 外部钩子进程的环境是 `env_clear()` + 快照注入 + `scrub_non_inheritable_env_vars`，不是当下进程的环境。

**源码事实：** PermissionRequest 钩子并发运行，所以一批的合计超时由 `max_permission_request_timeout`（最大值）界定，不是总和。

**架构推论：** 这个模块的真正设计点不在「钩子怎么跑」，而在**三处「失败该多重」的分级**：加载必需钩子失败 → 会话创建失败；一个钩子 `FailedAbort` → 本次操作中止但会话继续；一个钩子 `FailedContinue` → 只记录。加上 `preview_*` 与 `run_*` 的成对设计（先能问「会跑什么」再实际跑），这套接口的形状是清晰的：**它把「可观察性」做成了第一等 API，而不是日志的副产品**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/hooks/src/registry.rs` | `Hooks` | 会话级钩子运行时句柄 |
| `codex-rs/hooks/src/registry.rs` | `HooksConfig` | 八个字段的装配配置，含信任与 shell 设置 |
| `codex-rs/hooks/src/registry.rs` | `Hooks::new` | 会话启动装配，必需 hooks 加载失败即 `bail` |
| `codex-rs/hooks/src/registry.rs` | `Hooks::reconfigured` | 换配置但保留在飞钩子 |
| `codex-rs/hooks/src/registry.rs` | `Hooks::matches_plugin_hooks` | 按顺序逐项比较，判断是否需要重装配 |
| `codex-rs/hooks/src/registry.rs` | `Hooks::dispatch` | 串行执行本地 `Hook` 列表，`FailedAbort` 即停 |
| `codex-rs/hooks/src/registry.rs` | `Hooks::shutdown` | 中止并 join 未完成的异步钩子 |
| `codex-rs/hooks/src/registry.rs` | `max_permission_request_timeout` | 并发钩子批的超时上界（最大值而非总和） |
| `codex-rs/hooks/src/registry.rs` | `list_hooks` | 纯预览入口，功能关闭时不做任何发现 |
| `codex-rs/hooks/src/registry.rs` | `command_from_argv` | legacy 命令构造：清空环境 + 快照注入 + 清洗 |
| `codex-rs/hooks/src/types.rs` | `Hook` / `HookFn` | 本地钩子：名字 + 异步函数 |
| `codex-rs/hooks/src/types.rs` | `HookResult` | 三态失败策略，`FailedAbort` 可中止操作 |
| `codex-rs/hooks/src/types.rs` | `HookPayload` | 触发输入：会话、cwd、客户端、定长时间戳、事件 |
| `codex-rs/hooks/src/types.rs` | `HookEvent` / `HookEventAfterAgent` | 单变体事件，`event_type` 判别 + `flatten` 扁平载荷 |
| `codex-rs/hooks/src/types.rs` | `serialize_triggered_at` | RFC 3339 到秒 + `Z` 的自定义序列化 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [RFC 3339：日期与时间的互联网表示](https://www.rfc-editor.org/rfc/rfc3339)
- [open-interpreter 文档：钩子](https://www.openinterpreter.com/docs/terminal/hooks)

## 与主核 / AI 的关系

这套钩子机制与主核里「事件触发器 / 审计钩子」同构：数据库也有在 DDL、登录、语句执行前后插入外部动作的机制，同样要处理「钩子失败怎么办」。主核的做法通常是**分级**：审计日志失败可以降级，权限钩子失败必须拒绝语句——与这里的 `FailedContinue` / `FailedAbort` 是同一个分级思想。

对 agent 工程的直接价值有三条。其一，**失败要分级到「中止范围」而不是「是否失败」**。同一个「钩子失败」，在启动期是致命、在操作期是中止本次、在另一些场合只是记录——用三个变体表达，比一个 `Result` 加若干标志位清楚。其二，**「预览」应当是正式 API**。`preview_*` 与 `run_*` 成对，让界面能在用户确认前显示「将要执行哪些钩子」，而不是等它们跑起来才知道。其三，**外部进程的环境要快照化**。`env_clear()` + 创建时快照 + 清洗，三件加起来才能保证「同一次会话里钩子环境不变」，而继承当下环境会让行为随用户中途改变环境变量而漂移。
