---
project: ragflow
feature: sandbox
title: 代码执行沙箱
page_type: feature
schema_version: 1
refs:
  - path: internal/agent/sandbox/catalog.go
    symbols: [ConfigSchema, ListProviders, ValidateConfig]
  - path: internal/agent/sandbox/local.go
    symbols: [LocalProvider, newLocalProviderFromEnv]
---

# 代码执行沙箱

## 一句话职责

八个后端（`self_managed`、`ssh`、`aliyun_codeinterpreter`、`e2b`、`tenki`、`ucloud_agent_sandbox`、`kosa8`、`local`）各自有不同的配置字段与合法性约束，而这个子系统把它们统一成「一份可由管理面板渲染的表单 schema + 一个统一的校验入口」，并让 `local` 成为唯一一个**明确声明自己不提供隔离**的选项。

## 机制是什么

这个包的定位在 `provider.go` 的包注释里被写清了：

> `ProviderManager` 选择活跃的后端。`ManagerClient` 把它适配成 `tool.SandboxClient`，**而不把 provider 细节暴露给 CodeExec**。

所以是三层：工具层（CodeExec）只认一个 `SandboxClient` 接口；`ManagerClient` 做适配；`ProviderManager` 按配置选后端。而这一页讲的是**选择与配置**那一层，不是执行层。

八个后端的能力差异很大，而这张表（从 `providerMetadata` 直接读出来）说明它们的定位：

| id | 名称 | tags |
|---|---|---|
| `self_managed` | Self-Managed | self-hosted / low-latency / secure |
| `ssh` | SSH | remote / ssh / custom-runtime |
| `aliyun_codeinterpreter` | Aliyun Code Interpreter | saas / cloud / scalable / aliyun |
| `e2b` | E2B | saas / fast / global |
| `tenki` | Tenki | saas / cloud / microvm / isolated |
| `ucloud_agent_sandbox` | UCloud Agent Sandbox | saas / cloud / isolated / ucloud |
| `kosa8` | kosa8 | saas / self-hosted / microvm / isolated |
| `local` | Local | local / host / minimal |

而 `local` 的描述是唯一一句**直接的免责声明**：

> Execute code directly on the current host process.

而 `local.go` 的文件头注释把这件事说得更狠：

> Local 执行在 **RAGFlow 主机上跑可信代码**。
>
> `LocalProvider` 通过 `os/exec` 在 Go 主机本身上跑用户的代码。**没有任何沙箱化**——代码以 Go 进程的权限运行。Python 版也一样：`local` provider 是一个**便于开发 / 可信环境**的便利选项，**不是安全边界**。需要隔离的运维应当配置 Self-Managed（executor_manager Docker+gVisor）或 Aliyun / e2b。

所以这个目录名里的「sandbox」是一个**能力类别**而不是一个安全保证。八个后端里有些真隔离（microVM / gVisor），而 `local` 一个都没有——它被放在同一张表里，但它的 `tags` 是 `local / host / minimal`，没有一个 `secure` 或 `isolated`。

而 `providerOrder` 的顺序（`self_managed` 第一、`local` 最后）是给管理面板的默认排序，也是「推荐程度」的隐式表达。

## 怎么实现的

**`ConfigSchema` 是「每个后端的表单定义」，而它的实现方式按后端分成了两种。**

```go
func ConfigSchema(provider string) (map[string]any, error) {
    if provider == "self_managed" {
        return selfManagedSchema(), nil
    }
    s := staticSchema(provider)
    if s == nil {
        return nil, &ValidationError{Message: fmt.Sprintf("unknown provider: %s", provider)}
    }
    return deepCloneMap(s), nil
}
```

`self_managed` 走一条独立路径（它有自己的 `selfManagedSchema`），其余七个走 `staticSchema` 的查表。三条细节：

1. **`self_managed` 单独分支** ——因为它的 schema 大概是拼出来的（有动态成分），而其余七个是静态字面量。
2. **`deepCloneMap`** ——返回的是拷贝。这与 f02 的 `deepCopyDSL`、f04 的 `maps.Clone` 是同一条纪律：**不给调用方可以改到内部状态的引用**。
3. **未知后端返回 `ValidationError` 而不是普通 error** ——所以「未知 provider」与「配置不合法」在类型上是同一类（都是用户输入问题），而 `ValidationError` 是这个包对外的统一错误形态。

而七个静态 schema 各由一个函数产出（`sshSchema` / `tenkiSchema` / `ucloudSchema` / `kosa8Schema`…），其中四个共用辅助函数：

```go
func limits(typ string, required bool, label string, def, min, max int64, description string) map[string]any
func limitsNoDescription(typ string, required bool, label string, def, min, max int64) map[string]any
func limitsScoped(typ string, required bool, label string, def, min, max int64, description, scope string, readonly bool) map[string]any
func fieldWith(typ string, required bool, label string, extras ...any) map[string]any
```

所以 schema 的字段形态被归纳成「基础三元组（type / required / label）加可选的 min / max / default / description / scope / readonly」。`limitsScoped` 多出的 `scope` 与 `readonly` 说明有些字段是**按作用域可见 / 只读**的——那是管理面板需要的信息，不是校验需要的。

**`ListProviders` 把 `providerOrder` 与 `providerMetadata` 合成一张列表，而 `id` 是最后插进去的。**

```go
for _, id := range providerOrder {
    item := deepCloneMap(providerMetadata[id])
    item["id"] = id
    result = append(result, item)
}
```

注意 `id` 不在 metadata 里而是从 `providerOrder` 补上——所以 metadata 只需要维护「人可读的名称 / 描述 / 标签」，而 id 的唯一来源是 `providerOrder`。两处都不重复写 id，避免漂移。

**`ValidateConfig` 是「通用 schema 校验 + 每个后端的额外规则」两段。**

第一段完全通用：

```go
for name, raw := range schema {
    fieldSchema, ok := raw.(map[string]any)
    if !ok { continue }
    value, present := config[name]
    if required, _ := fieldSchema["required"].(bool); required && !present {
        return invalid("required field %q is missing", name)
    }
    if !present { continue }
    typ, _ := fieldSchema["type"].(string)
    if !validType(value, typ) {
        return invalid("field %q must be a %s", name, typ)
    }
    if typ == "integer" {
        // min / max 检查
    }
}
```

所以它做的三件事：必填检查、类型检查、整数范围检查。而 `if !present { continue }` 的位置说明**非必填且缺失就跳过**（不做类型校验，也不填默认值）。

第二段是一个 `switch provider`，每个后端有自己的规则。而这些规则**不是 schema 能表达的**，这正是分两段的原因：

| 后端 | schema 表达不了的规则 |
|---|---|
| `self_managed` | `endpoint` 前缀必须是 `http://` 或 `https://`；`timeout` 必须在 1..600 |
| `ssh` | `host` / `username` 非空；`password` **或** `private_key` 至少给一个（互斥或）；四个整数都 `> 0` 但 `max_artifacts >= 0` |
| `aliyun_codeinterpreter` | `execute_host` 必须能被 `normalizeAliyunEndpoint` 解析；`access_key_id` 必须以 `LTAI` 开头；`account_id` 必填；`region` 必须是五个枚举之一 |
| `tenki` | `api_key` 必填；四个整数 `> 0`；`max_artifacts >= 0` |
| `ucloud_agent_sandbox` | `api_key` 必填；`template` 若出现则非空 |
| `kosa8` | `api_key` 必填；`base_url` 必须 https（**但允许 `http://localhost` 与 `http://127.0.0.1`**）；两个模板名若出现则非空 |

三类「schema 表达不了」的规则：**跨字段约束**（`ssh` 的「密码或私钥之一」）、**值域内容**（`LTAI` 前缀、`region` 枚举）、**格式**（URL 前缀）。

而 `kosa8` 的 `base_url` 规则最值得注意：

```go
if baseURL := ...; baseURL != "" && !strings.HasPrefix(baseURL, "https://") && !strings.HasPrefix(baseURL, "http://localhost") && !strings.HasPrefix(baseURL, "http://127.0.0.1") {
    return invalid("kosa8 API URL must use https: %s", baseURL)
}
```

所以默认要求 https，而**明确放行两个本地回环前缀**。这个例外是必要的：自托管 kosa8 跑在同一台机器上时用 https 需要证书，而 `localhost` 与 `127.0.0.1` 是不可被网络中间人攻击的目标。注意它用 `HasPrefix` 而不是解析 host，所以 `http://localhost.attacker.com` 会**通过**——这是一个宽进严出的边界（前缀匹配不严谨）。不过在这个场景（运维自填的配置，不是攻击者可控的输入）风险有限。

**`max_artifacts >= 0` 而其余整数 `> 0`，这个区分是有意义的。**

```go
for _, key := range []string{"timeout", "max_output_bytes", "max_artifacts", "max_artifact_bytes"} {
    n := integerOr(config[key], 0)
    if key == "max_artifacts" {
        if n < 0 {
            return invalid("max_artifacts must be greater than or equal to 0")
        }
    } else if n <= 0 {
        return invalid("%s must be greater than 0", key)
    }
}
```

所以 `max_artifacts = 0` 是合法的（「不收集任何产物」），而 `timeout = 0` / `max_output_bytes = 0` 不合法。理由清楚：后者为 0 意味着「永远立即超时」或「输出上限为零」，那是配置错误；而前者为 0 是一个合理的选择（不需要产物）。

而 `integerOr(config[key], 0)` 把缺失当作 0——所以**缺失会被当作 0 然后被拒**（对 `timeout` 这类必填正整数）。这意味着「没配 `timeout`」在 `ssh` 后端是非法配置，而 schema 里 `timeout` 大概是 `required: false` 带默认值。所以这个循环实际上把几个「可选但有默认值」的字段变成了「必须能取到正值」——如果默认值来自 `staticSchema` 的 `default` 而 `ValidateConfig` 不套用默认值，那么「不填」会被拒。

这是一个值得留意的实现细节：**`ValidateConfig` 不套默认值**，所以它的判定看的是「配置里写了什么」，而运行时会用 `configInt(cfg, "timeout", localDefaultTimeout)` 这类兜底。两处对「缺失」的处理不同——校验时当作 0（然后拒绝），运行期当作默认值。所以一个「什么都不填」的 ssh 配置会校验失败，尽管运行期本来有默认值可用。

**`LocalProvider` 的配置面有 9 个字段，而每个字段对应一个具体的资源约束。**

```go
type LocalProvider struct {
    pythonBin        string
    nodeBin          string
    workDir          string
    timeout          int
    maxMemoryMB      int
    maxOutputBytes   int
    maxArtifacts     int
    maxArtifactBytes int
    ...
}
```

而默认值全部是常量：

| 字段 | 默认值 |
|---|---|
| `workDir` | `filepath.Join(os.TempDir(), "ragflow-codeexec")` |
| `timeout` | 30 秒 |
| `maxMemoryMB` | 512 |
| `maxOutputBytes` | 1 MiB |
| `maxArtifacts` | 20 |
| `maxArtifactBytes` | 10 MiB |
| `noFile`（rlimit） | 64 |
| `pythonBin` / `nodeBin` | `python3` / `node` |

而每个常量都有注释说明来源：`maxMemoryMB` 与 `noFile` 的两条注释都提到「Matches the Python `_limit_child_process` value」/「Matches the Python version」——所以这些数字**不是拍出来的，是从 Python 版对齐过来的**。

`newLocalProviderFromEnv` 与 `newLocalProviderFromConfig` 的拆分与本页主题直接相关：

```go
func newLocalProviderFromEnv() *LocalProvider {
    return newLocalProviderFromConfig(localConfigFromEnv())
}
```

而 `localConfigFromEnv` 把每个 `LOCAL_*` 环境变量读进一个 config map：

```go
return map[string]any{
    "python_bin":       common.GetEnv(common.EnvLocalPythonBin),
    "node_bin":         common.GetEnv(common.EnvLocalNodeBin),
    "work_dir":         common.GetEnv(common.EnvLocalWorkDir),
    "timeout":          common.GetEnv(common.EnvLocalTimeout),
    "max_memory_mb":    common.GetEnv(common.EnvLocalMaxMemoryMB),
    ...
}
```

注意它**把所有值都读成字符串**（`common.GetEnv` 返回 string），然后 `newLocalProviderFromConfig` 用 `configInt(cfg, "timeout", ...)` 做转换。而 `localConfigFromEnv` 的 docstring 说这个 map 的形状「mirrors the admin-panel settings JSON shape」——所以**环境变量、管理面板 JSON、以及代码里手工构造的 map 三者共用同一个形状**。

这个统一是有代价的：环境变量天然是字符串，而 `configInt` 必须能处理 string 与 int 两种输入。而 `catalog.go` 那套 `integerValue` / `integerOr` / `stringValue` 辅助函数正是为此存在——它们处理 `any`（来自 JSON 解出来的 `float64`，或环境变量的 `string`）。

**`Initialize` 的探针与 `HealthCheck` 的探针是两次独立的写测试，两者都有理由。**

```go
// Initialize
if err := os.MkdirAll(p.workDir, 0o700); err != nil { ... }
probe := filepath.Join(p.workDir, ".ragflow-probe")
if err := os.WriteFile(probe, []byte("ok"), 0o600); err != nil { ... }
_ = os.Remove(probe)
```

注释说清了为什么 `MkdirAll` 之后还要探针：

> MkdirAll 对已存在的目录不报错；探针区分「可写」与「存在但不可写」。

而 `HealthCheck` 用了另一个探针文件名（`.ragflow-healthcheck`）做同样的事，并额外检查 `info.IsDir()`。两次探针的差别是：`Initialize` 是**一次性**（启动时），`HealthCheck` 是**可重复的**（运行期随时可调）。这也是为什么它们的探针文件名不同——避免一个失败的健康检查残留文件被误认为初始化痕迹。

**`ExecuteCode` 的时间控制有两条独立的取消路径，而两条都调 `killProcessTree`。**

```go
select {
case waitErr = <-waitDone:
    // 进程自己退出
case <-ctx.Done():
    _ = killProcessTree(cmd.Process.Pid)
    <-waitDone
    return nil, fmt.Errorf("local: execution cancelled: %w", ctx.Err())
case <-time.After(time.Duration(timeout) * time.Second):
    _ = killProcessTree(cmd.Process.Pid)
    <-waitDone
    return nil, fmt.Errorf("local: execution timed out after %d seconds", timeout)
}
```

三个要点：

1. **`cmd.Wait()` 跑在 goroutine 里** ——因为主 goroutine 要同时等三个通道。注释解释了为什么不能直接 `cmd.Wait()`：「`cmd.Wait` 在 context 取消时不会杀进程（若子进程忽略 SIGTERM）；进程组 + `os.FindProcess` 让我们能升级到 SIGKILL」。
2. **两条超时路径都 `killProcessTree`** ——进程组（`Setpgid`）是为了能杀整棵树（子进程可能又 fork 了）。
3. **`<-waitDone` 在 kill 之后** ——必须等 `cmd.Wait()` 返回才能继续，否则会泄漏 goroutine，且可能读到未关闭的 stdout/stderr。

而 `SysProcAttr` 那条注释是一个很诚实的限制声明：

> 设置 `Setpgid` 把子进程放进自己的进程组，这让我们能在超时时杀掉整个组。在 Linux 与 FreeBSD 上子进程还会在父进程死亡时收到 SIGTERM（`Pdeathsig`）；**其他平台没有等价物，所以在那里子进程可能在父进程意外退出后存活下来**。

所以「进程树清理」在 Linux/FreeBSD 上是完备的，而在 Windows/macOS 上有一处残余。这个残余被明说了而不是被掩盖——而 `local_pdeathsig.go` / `local_nopdeathsig.go` 两个 build-tag 文件就是为这个平台差异存在的。

**`maxMemoryMB` 的那个 `if` 块是一个「声明但不实现」的占位。**

```go
if p.maxMemoryMB > 0 {
    // Advisory only; the subprocess inherits the parent's
    // rlimits. Real isolation requires the SelfManaged or
    // Aliyun / e2b providers. We do NOT try to set RLIMIT_AS
    // post-fork because that would require ptrace or a
    // side-channel -- out of scope for a "local" provider.
}
```

一个**空的 `if` 块**。而它的注释说了三件事：

1. **Advisory only** ——这个字段只是「建议值」，实际不生效。
2. **为什么不能实现** ——Go 的 `os/exec` 没有可移植的 pre-start hook。
3. **想要真隔离就得换后端** ——不是「我们会补上」，而是「这不是这个 provider 的职责」。

而在上一段（`cmd.SysProcAttr` 之前）注释说得更早：「Go 的 `os/exec` 不直接暴露 rlimit，所以我们通过父进程的进程组 kill 来做……我们**不**尝试精确复刻 Python 的 `preexec_fn` RLIMIT_*，因为 Go 的 `os/exec` 不支持可移植的 pre-start 钩子」。

所以这是一个**明确的知识点**：Go 里做不到 Python `preexec_fn` 那种「fork 之后、exec 之前」的钩子。留下的空 `if` 块是一个可读的「这里曾经想做事」的痕迹，比删掉它更有信息量。

**输出捕获的 `localOutputCapture` 有一个很容易写错的正确性要求。**

```go
type localOutputCapture struct {
    buffer bytes.Buffer
    limit  int
    total  int
}

func (c *localOutputCapture) Write(p []byte) (int, error) {
    n := len(p)
    c.total += n
    if c.limit > 0 {
        p = p[:min(len(p), max(0, c.limit-c.buffer.Len()))]
    }
    _, err := c.buffer.Write(p)
    return n, err
}
```

注释：「`localOutputCapture` 保留作出有效结果所需的字节，同时**接受所有写入，好让一个吵闹的子进程能跑完而不是阻塞**」。

这是 `io.Writer` 契约的一个关键细节：`Write` 必须返回**它消费的字节数**（这里是 `n = len(p)`，即全部），而实际写进 buffer 的只有截断后的部分。如果返回 `len(p[:...])`（实际写入量），那么 `os/exec` 的复制 goroutine 会因为「短写」而继续循环、永远不会消费完——子进程写满管道缓冲后就会**永久阻塞**。

所以这个实现的两行是耦合的：`c.total += n` 记录真实总量（用于事后判断是否超限），`return n, err` 声明全部消费。而「事后判断超限并报错」是另一条路径：

```go
if maxOut > 0 {
    combined := stdout.total + stderr.total
    if combined > maxOut {
        return nil, fmt.Errorf("local: output exceeds %d bytes (got %d)", maxOut, combined)
    }
}
```

所以「截断」与「报错」是并存的：buffer 里只有前 `limit` 字节，而一旦总量超限整个执行被判定为失败。这与「静默截断」不同——用户会知道输出超限了。

**`statusFromExitCode` 把任何非零码都变成 `"error"`，丢掉具体码——但码本身进了 metadata。**

```go
func statusFromExitCode(code int) string {
    if code == 0 { return "ok" }
    return "error"
}
```

而调用点同时放了 `"status": statusFromExitCode(exitCode)` 与 `ExitCode: exitCode`：

```go
metadata := map[string]any{
    ...
    "status":            statusFromExitCode(exitCode),
    ...
}
return &ExecutionResult{ ..., ExitCode: exitCode, ... }, nil
```

所以 `status` 是一个给模型看的二值结论，而 `ExitCode` 保留原始信息给上层判断。这正是 f03 讲的「同一事实两个受众」的第三次出现。

**`_ = waitErr` 那一行是这一页最值得抄的一处「显式丢弃」。**

```go
_ = waitErr // may be non-nil for non-zero exit; exitCode carries the truth
```

`cmd.Wait()` 返回的 error 在**非零退出**时也是非 nil（`*exec.ExitError`）。所以如果把 `waitErr` 当作「执行失败」的信号，那么任何一次「脚本正常返回 1」都会被误判成基础设施故障。而真相在 `cmd.ProcessState.ExitCode()` 里。

所以这一行 `_ = waitErr` 配一条注释，是一个**防止下一个人误用**的记号。这种「把一个变量显式标记为不用」的写法，比删掉它更好：读者能看到「这里有个返回值，我们故意忽略了，理由是注释里那句」。

## 关键取舍

**源码事实：** 八个后端按 `providerOrder` 排序（`self_managed` 第一、`local` 最后），而 `providerMetadata` 里 `local` 的 tags 是 `local / host / minimal`——**唯一没有 `secure` 或 `isolated` 的后端**。

**源码事实：** `local.go` 的文件头注释直接声明「没有任何沙箱化——代码以 Go 进程的权限运行」「不是安全边界」；想要隔离必须换 Self-Managed（Docker+gVisor）或 Aliyun / e2b。

**源码事实：** `ConfigSchema` 对 `self_managed` 走独立路径，其余七个走 `staticSchema` 查表；未知后端返回 `ValidationError`（与「配置不合法」同一类型）；返回前 `deepCloneMap`。

**源码事实：** `ListProviders` 从 `providerOrder` 补 `id`，metadata 里不重复写 id——避免两处漂移。

**源码事实：** `ValidateConfig` 分两段：通用 schema 校验（必填 / 类型 / 整数 min-max）与每后端的 `switch` 规则。后者处理三类 schema 表达不了的约束：跨字段（`ssh` 的「密码或私钥之一」）、值域内容（`LTAI` 前缀、`region` 五值枚举）、格式（URL 前缀）。

**源码事实：** `ValidateConfig` **不套用 schema 的默认值**——缺失的整数字段被 `integerOr(cfg[key], 0)` 当作 0，然后被「必须 > 0」的规则拒绝；而运行期 `configInt(cfg, "timeout", default)` 会用默认值。两处对「缺失」的处理不同。

**源码事实：** `max_artifacts` 允许 `= 0`（不收集产物），而 `timeout` / `max_output_bytes` / `max_artifact_bytes` 必须 `> 0`。

**源码事实：** `kosa8` 的 `base_url` 默认要求 https，但**明确放行 `http://localhost` 与 `http://127.0.0.1`**；判定用 `HasPrefix` 而不是解析 host。

**源码事实：** `newLocalProviderFromEnv` 只是 `newLocalProviderFromConfig(localConfigFromEnv())` 的薄包装；而 `localConfigFromEnv` 把所有值读成**字符串**，docstring 说这个 map 的形状「mirrors the admin-panel settings JSON shape」——环境变量、面板 JSON、手工 map 三者共用一种形状。

**源码事实：** `local` 的默认值全部是对齐 Python 版的常量：timeout 30s / maxMemoryMB 512 / maxOutputBytes 1 MiB / maxArtifacts 20 / maxArtifactBytes 10 MiB / noFile 64；注释多次写「Matches the Python version」。

**源码事实：** `Initialize` 与 `HealthCheck` 的写探针用**不同文件名**（`.ragflow-probe` vs `.ragflow-healthcheck`）；`Initialize` 的注释说明为什么 `MkdirAll` 之后还要探针——「MkdirAll 对已存在目录不报错，探针区分『可写』与『存在但不可写』」。

**源码事实：** `ExecuteCode` 把 `cmd.Wait()` 放进 goroutine，主 goroutine 用 `select` 等三个通道（正常退出 / `ctx.Done()` / 超时计时器）；两条异常路径都先 `killProcessTree` 再 `<-waitDone`。

**源码事实：** `Setpgid` 是为了能杀整个进程组；注释承认「在 Linux 与 FreeBSD 上子进程还有 `Pdeathsig`，**其他平台没有等价物**，所以子进程可能在父进程意外退出后存活」。

**源码事实：** `if p.maxMemoryMB > 0 { }` 是一个**空块**，注释说明它是 advisory-only、`RLIMIT_AS` 无法在 Go 里可移植地设置（需要 ptrace 或侧信道）。

**源码事实：** `localOutputCapture.Write` 返回 `len(p)`（全部「消费」）而只写进截断后的部分，因为返回短写会让 `os/exec` 的复制循环永不结束、子进程写满管道后永久阻塞；`total` 记录真实总量，事后超过 `maxOutputBytes` 时整个执行报错。

**源码事实：** `statusFromExitCode` 把任何非零码映射成 `"error"`，但 `ExitCode` 原始值同时放在 `ExecutionResult` 与 `metadata` 里。

**源码事实：** `_ = waitErr` 配注释「may be non-nil for non-zero exit; exitCode carries the truth」——`cmd.Wait()` 在非零退出时也返回非 nil，把它当失败信号会把「脚本正常返回 1」误判成基础设施故障。

**架构推论：** 把八个异构后端塞进「一份 schema + 一个校验入口」的价值在于**管理面板不需要知道任何后端细节**——它拿到 `ConfigSchema` 就能渲染表单，拿到 `ValidateConfig` 就能前置校验。而代价是校验被拆成两段：schema 能表达的（必填/类型/范围）与表达不了的（跨字段/枚举/格式）。**这个两段结构是任何「通用表单 + 后端特定规则」系统的必然形态**，而正确做法是把第二段集中在**一个** `switch` 里，而不是散在各自的 provider 实现里。

**架构推论：** 一个明确声明「不提供隔离」的选项是有价值的，而不是应当被删掉的。`local` 存在的理由是开发便利（不需要任何外部服务、秒级启动），而它的危险在于**名字与目录名暗示了安全性**。所以正确处置是：保留它、在 metadata 的 tags 里不写 `secure`、在文件头注释里明说「不是安全边界」、并在错误消息与注释里反复指向真正隔离的后端。**一个诚实的不安全选项比一个偷偷不安全的安全选项好得多。**

**架构推论：** 「配置校验不套默认值」与「运行期用默认值」这对不一致值得单独记住。它的实际后果是某些「省略即合法」的配置在校验时被拒。判据是：**校验看的是「用户写了什么」，还是「最终生效的值」？** 前者是「约束用户的输入」，后者是「约束系统状态」——两者都合理，但必须明确选一个并保持一致，因为混用会产生「明明有默认值却过不了校验」这种令人困惑的失败。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/agent/sandbox/catalog.go` | `ConfigSchema` | 按后端产出表单 schema，`self_managed` 独立路径 |
| `internal/agent/sandbox/catalog.go` | `ListProviders` | 合成 id + metadata 列表，按 `providerOrder` 排序 |
| `internal/agent/sandbox/catalog.go` | `ValidateConfig` | 通用 schema 校验 + 每后端 `switch` 规则 |
| `internal/agent/sandbox/catalog.go` | `providerOrder` | 八个后端的顺序，也是 id 的唯一来源 |
| `internal/agent/sandbox/catalog.go` | `providerMetadata` | 名称 / 描述 / tags，`local` 无 `secure` 标签 |
| `internal/agent/sandbox/catalog.go` | `ValidationError` | 统一的用户输入错误形态 |
| `internal/agent/sandbox/catalog.go` | `sshSchema` / `kosa8Schema` 等 | 各后端的静态字段定义 |
| `internal/agent/sandbox/catalog.go` | `limitsScoped` | 带 scope / readonly 的字段形态 |
| `internal/agent/sandbox/local.go` | `LocalProvider` | 九个配置字段 + 实例映射 |
| `internal/agent/sandbox/local.go` | `newLocalProviderFromEnv` | 从 `LOCAL_*` 环境变量构造 |
| `internal/agent/sandbox/local.go` | `newLocalProviderFromConfig` | 共用构造，缺省填默认值 |
| `internal/agent/sandbox/local.go` | `Initialize` | `MkdirAll` + 写探针，验证可写 |
| `internal/agent/sandbox/local.go` | `HealthCheck` | 用另一个探针名做可重复检查 |
| `internal/agent/sandbox/local.go` | `ExecuteCode` | 三分支 select：退出 / 取消 / 超时 |
| `internal/agent/sandbox/local.go` | `buildLocalChildEnv` | HOME / TMPDIR 指向实例目录 |
| `internal/agent/sandbox/local.go` | `localOutputCapture` | 全量消费但截断保留，防子进程阻塞 |
| `internal/agent/sandbox/local.go` | `statusFromExitCode` | 非零码映射成 `"error"`，原码另存 |
| `internal/agent/sandbox/local.go` | `_` (`_ = waitErr`) | 显式丢弃，防误把非零退出当基础设施故障 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：agent/sandbox/catalog.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/sandbox/catalog.go)
- [ragflow 源码：agent/sandbox/local.go](https://github.com/infiniflow/ragflow/blob/main/internal/agent/sandbox/local.go)
- [Go 文档：os/exec 与 Cmd](https://pkg.go.dev/os/exec)

## 与主核 / AI 的关系

主核里最接近的是**存储后端表（storage engine / tablespace）**：`pg_tablespace` 与 `pg_am` 描述「有哪些可用的存储方式」，每种有自己的必需参数（目录、块大小），而启动时会校验这些参数是否可用。这里的八个沙箱后端与那套是同一种「引擎可选 + 参数各异」的结构。

**差别在于「隔离等级是否被建模」。** 主核的存储后端之间主要差性能与特性，安全性不是选项维度。而沙箱后端的核心差异**就是隔离强度**：`self_managed` 是 gVisor、`kosa8` 是 microVM、`ssh` 是一台你信任的机器、`local` 什么都没有。而当前的 `providerMetadata` 用一个自由文本 `tags` 数组表达这件事——所以它**没有被建模成可查询的等级**，只是人类可读的标签。想做「生产环境禁止 `local`」这类策略，就得靠 tag 匹配而不是类型检查。这是一个可改进点。

与 e04（Pregel 引擎）的关系是「谁负责有界执行」：引擎用 `recursionLimit = 25` 限制超步数，而这里用 `timeout` + `maxOutputBytes` + `maxArtifacts` 限制单次代码执行。两者都是**防止无限消耗**，但前者是「图结构的深度」，后者是「进程的资源」。

与 f04（工具注册）的关系直接：`code_exec` 是注册表里的一个键，而它的工厂是 `noConfig("code_exec", func() BaseTool { return NewCodeExecTool() })`——**不接受任何节点级参数**。所以「沙箱后端选哪个」不是 CodeExec 节点的 DSL 参数，而是**进程级配置**（`SANDBOX_PROVIDER_TYPE`）。这是一个明确的分层：工具节点不知道自己跑在什么沙箱里，而 `ManagerClient` 那层负责适配。

对 AI / agent 工程最实用的一条是那个 **`localOutputCapture.Write` 必须返回全部消费量**。任何要在 Go 里包装一个「有限缓冲 + 无限输入」的 `io.Writer` 都会遇到这个陷阱：返回短写会让上游的复制循环永不结束，而症状是「子进程偶尔卡死」——极难归因，因为它在测试里通常不出现（测试脚本输出很少，管道缓冲不会写满）。判据是：**`Write` 的返回值是「我消费了多少」，不是「我存了多少」**。

另一条是那个空 `if p.maxMemoryMB > 0 { }`。一个「声明了但没实现」的配置字段，正确处置是：**保留字段、保留注释说明为什么不实现、并在注释里指向真正能实现的替代方案**。删掉字段会让既有配置文件失去那个键（静默忽略）；假装实现了会让用户以为有内存隔离。留下一个空块加一段注释，是三种选择里最诚实的。

最后一条是 `_ = waitErr` 那种「显式丢弃返回值 + 说明理由」。`cmd.Wait()` 在非零退出时返回非 nil 是一个很容易踩的 Go 细节，而踩了之后的症状是「所有失败的业务脚本都被当成基础设施故障」。把「我们故意不用这个返回值」写成一个赋值语句加注释，比留着不用更容易被后来者看到——**未被使用的变量会被 linter 提醒，而一个带注释的 `_ =` 会被人读到。**
