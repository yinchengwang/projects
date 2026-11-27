---
project: mcp-python-sdk
feature: path-security
title: 路径穿越防护
page_type: feature
schema_version: 1
refs:
  - path: src/mcp/shared/path_security.py
    symbols: [contains_path_traversal, safe_join, PathEscapeError]
---

# 路径穿越防护

## 一句话职责

提供三件路径安全工具：一个不依赖根目录的字符串级穿越检测、一个绝对路径识别、以及一个解析后再验容器的安全拼接。它们的目标是让资源处理器在被 URI 模板参数喂进文件系统时不会被 `..`、绝对路径或符号链接带出根目录。

## 机制是什么

MCP 的资源可以用 URI 模板声明，例如 `file://docs/{+path}`。客户端传什么 `path` 由它自己决定，服务端处理器经常直接拿这个值拼文件路径。这条路就是经典的目录穿越入口。

模块给的不是一个「安全函数」，而是三个**能力递减**的检查：

| 函数 | 知道根目录吗 | 能挡什么 | 挡不住什么 |
|---|---|---|---|
| `contains_path_traversal` | 不知道 | 纯字符串级的 `..` 越界 | 绝对路径、符号链接、平台规范化差异 |
| `is_absolute_path` | 不知道 | POSIX / Windows 盘符 / UNC 形态的绝对路径 | 相对路径穿越 |
| `safe_join` | 知道 | 上述全部 + 符号链接逃逸 | 检查与使用之间的替换（TOCTOU） |

文档把 `safe_join` 明确称为 **gold-standard**，并在它的文档里主动交代 TOCTOU 局限。

## 怎么实现的

**字符串级检测。** `contains_path_traversal` 不看文件系统，只做一件很朴素的事：把值按 `/` 与 `\` 统一切分，逐段维护一个深度计数器——`..` 让深度减一，正常段加一，`.` 与空段跳过；深度一旦降到负值就判定越界。

这个实现有三个刻意选择：

- **基于组件而非子串**。文档给了反例：`1.0..2.0` 不算穿越，因为 `..` 不是独立段。用 `".." in value` 判断会产生大量误报。
- **两种分隔符都当分隔符**。Windows 路径会混用，所以先 `replace("\\", "/")`。
- **`a/../b` 不算穿越**。因为它先下到 `a` 再回来，深度没变负。这是正确语义——进去再出来没有逃逸。

文档同时声明这条检查的边界：它**不建模平台特有的规范化**（例如 Win32 会剥掉最终组件的尾随点与空格），所以只能当快速筛查，真正的判定要交给 `safe_join`。

**绝对路径识别。** `is_absolute_path` 覆盖三种形态：以 `/` 或 `\` 开头（POSIX 绝对与 UNC/根相对）、Windows 盘符绝对（`C:\foo`）、以及常被忽略的**盘符相对**（`C:foo`）。

最后一种值得单独说：`C:foo` 在 `PureWindowsPath.is_absolute()` 下是 `False`，但它仍然危险——当左侧基路径在不同盘符时，`Path` 的 `/` 运算会丢弃左侧。所以这里把它一并标为绝对。副作用是形如 `x:y` 的单字母前缀标识符也会被误判，文档给了逃逸口（`ResourceSecurity(exempt_params=...)`）。

**安全拼接。** `safe_join` 分四步：

1. `Path(base).resolve()` 先把根目录解析成真实路径（消掉符号链接与 `..`）。
2. 逐个检查每个待拼部件：含 null 字节就拒（null 字节能穿过 `Path` 构造但在系统调用处炸出难懂的错误，这里换成明确的 `PathEscapeError`）；是绝对路径就拒（否则 `Path("/data") / "/etc/passwd"` 会静默得到 `/etc/passwd`）。
3. `base.resolve().joinpath(*parts).resolve()` 拼好再解析。
4. `target.is_relative_to(base_resolved)` —— 用「解析后的真实路径是否仍在根内」来判定。这一步是它强于前两个函数的原因：符号链接在 `resolve()` 时被展开，若链到根外，`is_relative_to` 就会失败。

**主动声明的局限。** `safe_join` 的文档写得很直白：符号链接检查是**时间点检查**，如果某个目录在这次调用与调用方随后的 `open` 之间被换成符号链接，不会再被重新检查。它给出的缓解建议是「用 `O_NOFOLLOW` 打开」或使用平台的路径约束原语。

## 关键取舍

**源码事实：** 三个函数的职责按「是否知道根目录」分层，文档明确说前两个是 base-free 检查、`safe_join` 才是 gold-standard。这种分层让调用方能在不同位置选合适强度，而不是被迫处处做完整的文件系统解析。

**源码事实：** `contains_path_traversal` 是组件级判断而非子串匹配，`a/../b` 与 `1.0..2.0` 都不算穿越。文档把这些反例写进了 docstring 的 doctest。

**源码事实：** `is_absolute_path` 把 Windows 盘符相对形式（`C:foo`）也判为绝对，理由是它会丢弃 join 基路径。代价是 `x:y` 这类合法标识符会误判，文档给了显式豁免入口。

**源码事实：** `safe_join` 文档自认 TOCTOU 窗口存在，并给出缓解方向。这不是疏漏，而是清醒地把「检查与使用之间无法原子」这条事实写进接口文档。

**架构推论：** 把这三个函数放在 `shared/` 而不是 `server/` 下，说明它们是客户端与服务端共用的安全原语，不绑定某一侧。这也意味着它们**不替调用方做策略决定**——拒还是放、豁免哪些参数，都由上层配置。

**架构推论：** 「提供工具而不提供强制」是有意的：MCP 的资源处理完全由用户实现，SDK 无法替它决定用不用这些函数。这与 smolagents 的本地解释器同类——安全边界必须由调用方主动建立，框架只能提供正确的积木。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp/shared/path_security.py` | `contains_path_traversal` | 不依赖根目录的字符串级穿越检测 |
| `src/mcp/shared/path_security.py` | `is_absolute_path` | 跨平台绝对路径识别，含盘符相对形态 |
| `src/mcp/shared/path_security.py` | `safe_join` | 解析后验容器的安全拼接 |
| `src/mcp/shared/path_security.py` | `PathEscapeError` | 所有拒绝路径的统一异常 |

## 参考

- [mcp-python-sdk 源码：path_security.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp/shared/path_security.py)
- [MCP 规范：资源与 URI 模板](https://modelcontextprotocol.io/specification/2025-06-18/server/resources)
- [OWASP：路径穿越攻击](https://owasp.org/www-community/attacks/Path_Traversal)
- [Python 文档：pathlib 的 resolve 与 is_relative_to](https://docs.python.org/3/library/pathlib.html)

## 与主核 / AI 的关系

路径穿越是「不可信输入被拼进命名空间」的经典案例，主核里同类问题出现在两个地方：表空间/文件路径由 SQL 指定时、以及外部表（foreign table）的 URI 解析时。共同点是**必须在解析之后判定，不能在字符串层面判定**——`safe_join` 的 `resolve()` + `is_relative_to` 就是这条原则的实现，主核里对应的是「把路径解析成规范形式再比较前缀」。

`contains_path_traversal` 与 `safe_join` 的分层也值得对照主核的做法：主核通常只有一道严格检查，因为它的路径来源是 SQL 而非 URI 模板参数。MCP 之所以需要两道，是因为资源处理器可能在很浅的位置就想要一个便宜的筛查（例如在决定要不要读文件之前先拒绝明显可疑的模板参数）。**要不要为了性能提供弱检查，取决于弱检查的误判代价。**这里弱检查只做拒绝、不做放行决策，所以误判只会导致多拒一次。

TOCTOU 那段文档值得所有写路径安全代码的人看：它没有声称「用了 safe_join 就安全」，而是写清「检查是时间点的」。主核里同类问题是「检查权限后文件被替换」——处理方式同样是「用带标志的打开原语（`O_NOFOLLOW`）」而不是「再检查一次」。
