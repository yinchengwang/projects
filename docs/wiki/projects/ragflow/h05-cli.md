---
project: ragflow
feature: cli
title: 命令行与管理命令
page_type: feature
schema_version: 1
refs:
  - path: internal/cli/cli.go
    symbols: [CLI, ParseArgs, Run, CommandLineConfig]
  - path: internal/cli/admin_command.go
    symbols: [AdminPingStoreCommand, AdminListProvidersCommand]
  - path: internal/cli/user_command.go
    symbols: [RegisterUser, APIListDatasetsCommand]
---

# 命令行与管理命令

## 一句话职责

把「一行类 SQL 的命令文本」变成一次 HTTP 调用：`ParseArgs` 解析进程级选项决定运行模式，`CLI.Run` 进入读-解析-分派的循环，`ExecuteCommand` 按模式把命令交给对应的处理器方法，每个处理器方法只负责拼路径、拼载荷、把响应交给统一的格式化层。

## 机制是什么

这个 CLI 的设计有两个决定性选择，理解了它们其余都是细节：

**一，命令语法是「类 SQL」而不是「子命令 + 选项」。** 它自己实现了一个词法分析器（`lexer.go` 里那一大串 `Token*` 常量）和语法分析器（`parser.go` / `admin_parser.go` / `user_parser.go`），把 `LIST DATASETS`、`CREATE USER xxx PASSWORD 'yyy'` 这样的语句解析成 `Command{Type, Params}`。`Type` 是一个预定义的字符串键（如 `admin_ping_store`），`Params` 是一个 `map[string]interface{}`。

**二，同一个 CLI 有三种运行模式，命令集按模式隔离。** `APIMode`（普通用户）、`AdminMode`（管理员）、`IngestorMode` / `ChatterMode`，外加 `CommandMode`（一次性执行）。分派在 `ExecuteCommand` 里按 `CLIMode` 三分支，各自调用 `ExecuteUserCommand` / `ExecuteAdminCommand` / `ExecuteSingleCommand`。**同一句命令在不同模式下可能解析成不同的 `Type`**——解析器接受 `CLIMode` 参数，正是为此。

`Params` 用 `map[string]interface{}` 承载参数，意味着**参数类型在编译期不可知**。所以每个处理器方法开头都有大量 `cmd.Params["x"].(string)` 类型断言与 `if !ok` 检查——这是本层最显眼的代码形态，也是「动态参数」付出的代价。

## 怎么实现的

**`ParseArgs` 是手写的选项循环，不是标准 flag 包。** 它从默认配置出发（`CLIMode = CommandMode`、`OutputFormat = OutputFormatPlain`），逐项扫描 `args`：

- `-o` / `--output`：下一个参数**不以 `-` 开头**才被当作值，`plain` / `json` 之外一律当 `table`。所以拼错格式名不会报错，静默变成表格。
- `-v` / `--verbose`：置位。
- `--api` / `--admin` / `--ingestor` / `--chatter`：切模式，并**同时**把 `Interactive` 置真、`OutputFormat` 改成 `table`。也就是说「进入某个模式」隐含了「交互 + 表格输出」两个副作用。
- `-h` / `--help`、`-V` / `--version` 等：只置标志位，由后续流程决定是否提前退出。

手写的好处是可以做「位置相关」的解析（需要看下一个 token 是否以 `-` 开头来判断它是值还是下一个选项），标准 `flag` 包做这件事更别扭。

**`CommandLineConfig` 是三种模式配置的并集。** 它有一个 `CLIMode`、两个可选子配置（`AdminClientConfig` 与 `APIClientConfig`）、四个全局开关（`ShowHelp`、`Verbose`、`Interactive`、`ShowVersion`）、一个输出格式和一个可选的 `Command` 字符串。**按模式只有一个子配置非空**，另一个为 `nil`。这种「并集结构体 + 调用方保证」的写法省掉了一层接口，代价是任何访问 `AdminClientConfig` 的代码都必须先确认当前是 Admin 模式。

`AdminModeConfig` 带 `AdminHost` / `AdminPort` / `AdminName` / `AdminPassword` / `KeyFile`；`APIModeConfig` 是「当前服务器名 + 服务器名到配置的映射」，支持多服务器切换。

`Print()` 用 `json.MarshalIndent` 打印整份配置——调试用，也因此**会把密码字段一起打印出来**。

**API 服务器配置可以从 YAML 读。** `ReadAPIServerConfig` 读 `rf.yml`，解析 `Host` 字段（用 `parseHostPort` 拆成 IP 与端口，格式错就报错），与内置默认配置合并。所以 CLI 的服务器列表有两个来源：内置默认 + 配置文件覆盖。

**`CLI` 结构体是「连接句柄的集合」。** 五个运行期字段：`APIServerClientMap`（服务器名 → HTTP 客户端）、`AdminServerClient`、`PasswordPrompt`（可替换的密码输入函数，便于测试）、`ContextEngine`（虚拟文件系统）、`CurrentModel`（当前模型配置），加上 `line *liner.State`（行编辑库）与 `running bool`（循环标志）。

`NewCLIWithConfig` 按 `CLIMode` 建客户端：`APIMode` 与 `CommandMode` 都建 API 客户端（从服务器映射里取当前那个，填 host/port，有 APIKey 就设 APIKey 并标记 `useAPIKey`）；`AdminMode` 建 admin 客户端。

**`Run` 是「交互循环 or 一次性执行」的二选一。** 非交互时执行 `Config.Command`（单条）后退出；交互时用 `liner` 读一行、交给 `execute`、把非空行加进历史。`execute` 的流程是：`NewParser(input).Parse(cliMode)` → 若 `cmd == nil` 直接返回（空行或纯注释）→ 若 `Type == "meta"` 走 `handleMetaCommand`（处理 `quit`/`help`/`pwd` 这类本地命令，不碰网络）→ 否则 `ExecuteCommand` 并 `result.PrintOut()`。

**处理器方法的形态高度统一，四个真实例子能说明这一层的全部套路。**

`AdminPingStoreCommand`：一句 `c.AdminServerClient.Request(commandCount, "GET", "/admin/store", "admin", nil, nil)`，错误包装成 `failed to ping object store`，成功交给 `HandleSimpleResponse(resp, "ping object store")`。**`"admin"` 这个参数是 authKind**，决定请求走哪个 base URL、带哪套认证头。

`AdminListProvidersCommand`：多一道前置守卫——`c.Config.CLIMode != AdminMode || c.AdminServerClient.LoginToken == nil` 就返回 `this command is only allowed in ADMIN mode or already login`。也就是「必须在 admin 模式**且**已登录」。然后用 `HandleCommonResponse` 而不是 `HandleSimpleResponse`。

`RegisterUser`：最复杂的一条。它先取 `apiModeClient()`，然后：

1. 检查 `params["iterations"]` 是否存在（`int` 类型断言）——**存在就报错**，因为注册用户不该出现在压测语句里。这是对语法层的一道额外约束。
2. 取 `user_name`（当邮箱用）、`password`、`nickname` 三个必填字符串，各自 `if !ok` 报错。
3. `GetPublicKeyPEM()` 取 RSA 公钥——`KeyFile` 有值就读文件，否则**返回一段硬编码在源码里的默认公钥**。
4. `EncryptPassword(password, publicKey)` 加密。
5. 拼 `{email, password: 密文, nickname}` 载荷，`POST /users`，`authKind` 用 `httpClient.AuthKind()`（动态决定，而不是像 admin 命令那样写死）。
6. 先判 `StatusCode != 200`，再 `json.Unmarshal` 成 `RegisterResponse`，**再判业务码 `Code != 0`** 并把 `Message` 当错误返回，最后把 `resp.Duration` 塞进结果。

第 6 步的三层检查值得注意：HTTP 层状态码、JSON 可解析性、业务码。三层都过才算成功。而 `Duration` 只在成功路径上被赋值。

`APIListDatasetsCommand`：取客户端、`GET /datasets`、`HandleCommonResponse(resp, "list datasets")`。三行有效逻辑，没有参数处理。

**响应处理被抽成一组 `Handle*` 函数。** `HandleSimpleResponse`（只要成败）、`HandleCommonResponse`（带通用数据）、`HandleCommonDataResponse`（带 data 字段）、以及更具体的类型化处理。它们统一返回 `ResponseIf` 接口，由 `PrintOut()` 按 `OutputFormat` 渲染成 plain / json / table。**格式化与业务彻底分离**——处理器只负责「拿到响应」，不知道它最终怎么显示。

**安全上有一处显式修正。** `APIListDatasetDocumentsCommand` 里拼查询串没有用 `fmt.Sprintf`，而是 `netUrl.Values` + `query.Set`，注释写明理由：`dataset_id` 是调用方提供的，字符串插值会让人可以注入额外查询参数或逃出路径段。**这是这一层唯一在代码注释里明确记录的安全修复。**

**`GetPublicKeyPEM` 的硬编码兜底值得单看。** 未配置 `KeyFile` 时返回一段以 `-----BEGIN PUBLIC KEY-----` 开头的完整 PEM。这让「不配置密钥也能注册用户」成为可能，但**它意味着密码加密用的是所有人共享的公钥**——加密仍然防止明文传输，但不提供任何身份绑定。

## 关键取舍

**源码事实：** 命令是自定义语法的，解析分词法与语法两阶段，`Command.Params` 是 `map[string]interface{}`，所有取值都要类型断言。

**源码事实：** `--api` / `--admin` / `--ingestor` / `--chatter` 除了切模式还会把输出格式改成 table 并置交互标志。

**源码事实：** `-o` 的格式名不合法时静默退化成 table；下一个参数以 `-` 开头时不当作值。

**源码事实：** `CommandLineConfig` 是三种模式配置的并集，按模式只有一个子配置非空。

**源码事实：** 需要登录的命令自带「模式 + 已登录」双重守卫，错误消息是固定的 `this command is only allowed in ADMIN mode or already login`。

**源码事实：** `RegisterUser` 做四层校验（禁止压测迭代、三个必填参数、HTTP 200、业务码 0），公钥缺配置时用硬编码默认值。

**源码事实：** 拼查询串用 `netUrl.Values` 而非字符串插值，注释记录理由是防注入。

**架构推论：** 自定义语法 + 统一 `Params` map 让「新增一条命令」的成本很低（写解析规则 + 写一个处理器方法 + 在分派 switch 里加一行），但**参数错误只能在运行期的类型断言处暴露**。Go 的类型系统在这一层完全帮不上忙。

**架构推论：** 三层分派（模式 → 命令类型 → 处理器）让命令集天然按权限分组：普通用户模式下解析器根本不会产生 `admin_*` 类型的命令。这是「用解析层做权限边界」的手法——比在处理器里逐个检查权限更省事，但也意味着**解析器的正确性成了权限的一部分**。

**架构推论：** `Handle*` 函数族把「响应语义」与「渲染格式」分开，使 `-o json` 对全部命令自动生效。这是这一层最好的设计：任何新命令只要返回 `ResponseIf`，就自动获得三种输出格式。

**架构推论：** `NewCLIWithConfig` 按模式建不同客户端，意味着「一个 CLI 实例只能服务一种模式」。切换模式要重建实例。这与「配置是并集」形成对比：数据结构允许三种模式共存，实例不允许。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/cli/cli.go` | `CommandLineConfig` | 三模式配置并集 + 全局开关 |
| `internal/cli/cli.go` | `ParseArgs` | 手写选项循环，产出配置 |
| `internal/cli/cli.go` | `ReadAPIServerConfig` | 从 YAML 读服务器列表并合并默认值 |
| `internal/cli/cli.go` | `CLI` | 客户端句柄集合与交互循环状态 |
| `internal/cli/cli.go` | `NewCLIWithConfig` | 按模式建客户端 |
| `internal/cli/cli.go` | `CLI.execute` | 解析 → meta 分叉 → 分派 → 打印 |
| `internal/cli/cli.go` | `GetPublicKeyPEM` | 读密钥文件，缺省用硬编码公钥 |
| `internal/cli/cli_http.go` | `ExecuteCommand` | 按 `CLIMode` 三分支分派 |
| `internal/cli/types.go` | `Command` | `Type` + `Params` 的解析产物 |
| `internal/cli/admin_command.go` | `AdminPingStoreCommand` | 最简形态：GET 一个管理端点 |
| `internal/cli/admin_command.go` | `AdminListProvidersCommand` | 带「模式 + 已登录」守卫 |
| `internal/cli/user_command.go` | `RegisterUser` | 四层校验 + RSA 加密 + 三层响应检查 |
| `internal/cli/user_command.go` | `APIListDatasetsCommand` | 最简用户命令，一行转发 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：cli/cli.go](https://github.com/infiniflow/ragflow/blob/main/internal/cli/cli.go)
- [ragflow 源码：cli/user_command.go](https://github.com/infiniflow/ragflow/blob/main/internal/cli/user_command.go)
- [Go 文档：net/url.Values](https://pkg.go.dev/net/url#Values)

## 与主核 / AI 的关系

CLI 的「解析 → 分派 → 执行」与主核里「SQL 解析 → 计划 → 执行」是同一条三段流水线，而且都靠**解析层承担语义校验的第一道关**：主核在解析期就拒绝不存在的表名，这里在解析期就按模式决定哪些命令合法。差别在于主核有完整的类型系统做语义检查（列类型、函数签名），而这里的 `Params` 是裸 map——**所有类型检查都退到了运行期断言**。

`netUrl.Values` 那条注释值得与主核的参数化查询对照。主核用绑定变量把「数据」与「语句」分开，从根本上消除注入；CLI 这里用 `Values` 而不是字符串拼接，是同一个原则在 HTTP 查询串上的实现。**「不拼字符串」这条规则在两个层面是同一个规则。**

`GetPublicKeyPEM` 的硬编码兜底是一处典型的「便利性削弱安全性」：它让首次部署无需配置即可注册用户，但也让公钥失去「服务端身份」的意义。主核里对应的做法是「没有密钥就拒绝启动」，而不是给一把公共钥匙。**这类兜底在运维工具里可接受，在安全边界上不可接受——RAGFlow 这里选择的是前者。**

对 agent / RAG 工程的实际含义有四条。其一，`--api` / `--admin` 是进入交互模式的开关，不带它们时是 `CommandMode`（一次性执行），三者输出格式与交互性不同。其二，`-o` 写错不会报错而会变成表格输出，脚本里靠 `-o json` 解析输出时要确保拼写正确。其三，`RegisterUser` 的密码是 RSA 加密后传输的，但用默认公钥时加密只防明文不防伪造。其四，所有需要登录的命令都返回同一句错误，排查时该先确认 `login` 是否真的成功，而不是怀疑参数。
