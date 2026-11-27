---
project: mcp-python-sdk
feature: method-registry
title: 方法名注册与能力协商
page_type: feature
schema_version: 1
refs:
  - path: src/mcp-types/mcp_types/version.py
    symbols: [SUPPORTED_PROTOCOL_VERSIONS, LATEST_PROTOCOL_VERSION, is_version_at_least]
  - path: src/mcp-types/mcp_types/methods.py
    symbols: [is_input_required, parse_client_request]
---

# 方法名注册与能力协商

## 一句话职责

`version.py` 把协议版本当成**枚举**而不是可排序的标量来管；`methods.py` 把「方法名 + 协议版本」映射到具体的线上类型，于是「这个方法在这个版本存不存在」由一次字典查找表达，版本能力协商的最硬的那部分就落在两张映射表上。

## 机制是什么

MCP 有五个已发布的协议版本，而方法集在版本之间**有增有减**：2026-07-28 删掉了 `initialize` 握手、`logging/setLevel` 与 `resources/subscribe` 这一对，新增了 `server/discover` 与 `subscriptions/listen`。如果一个服务端用「方法名 → 处理器」的单层表实现，它就无法表达「这个方法存在但在这个版本不存在」。

这里的解法是**两层映射**：

- **surface 映射**：键是 `(method, version)` 二元组，值是**该版本该方法的线上类型**。键不存在即「这个方法在这个版本不可用」——版本闸门就是键的缺席。
- **monolith 映射**：键只有 `method`，值是**与版本无关的模型**。这是用户代码实际收到的对象类型。

请求进来先按 surface 校验形状，再按 monolith 产出模型。用户代码永远看到同一套模型，不必关心对端说的是哪个版本。

## 怎么实现的

**版本按枚举管理，不按字典序比较。**

`version.py` 的模块 docstring 把理由写在了最前面：日期字符串恰好可按字典序排，但「协议版本是一个枚举集合，不是有序标量」，未来的标识符不保证是日期形状，而且**未识别的对端字符串必须保守处理**——否则 `"zzz" > "2025-11-25"` 会让一个垃圾版本号被判定成「比最新版还新」。所以一切排序都走 `KNOWN_PROTOCOL_VERSIONS.index(...)`。

版本集被切成三个，切法本身就编码了协议史：

- `KNOWN_PROTOCOL_VERSIONS`：全部五个，从旧到新；
- `HANDSHAKE_PROTOCOL_VERSIONS`：能通过 `initialize` 握手到达的四个（`2024-11-05` … `2025-11-25`）；
- `MODERN_PROTOCOL_VERSIONS`：只有 `2026-07-28`，即「用无状态逐请求信封」的那一档。

`SUPPORTED_PROTOCOL_VERSIONS` 是三者的并（实为前两者拼接），docstring 直接标了 `Deprecated`：**保留它只为 v1.x 兼容**，新代码应当分别用 `HANDSHAKE_PROTOCOL_VERSIONS` 或 `MODERN_PROTOCOL_VERSIONS`。这是「一个曾经正确的抽象在新事实出现后被迫降级为兼容层」的清晰记录。

派生常量都指向各自集合的末尾或开头：`LATEST_PROTOCOL_VERSION = KNOWN_PROTOCOL_VERSIONS[-1]`、`LATEST_HANDSHAKE_VERSION = HANDSHAKE_PROTOCOL_VERSIONS[-1]`、`LATEST_MODERN_VERSION = MODERN_PROTOCOL_VERSIONS[-1]`、`OLDEST_SUPPORTED_VERSION = HANDSHAKE_PROTOCOL_VERSIONS[0]`。三个 LATEST 并存，正是因为「最新」这个词在不同语境下指不同的东西：客户端握手邀约用握手版最新，`server/discover` 探测用现代版最新。

**`is_version_at_least` 的两个非对称行为。**

```
if minimum not in KNOWN_PROTOCOL_VERSIONS:
    raise ValueError(f"minimum must be a known protocol version, got {minimum!r}")
if version not in KNOWN_PROTOCOL_VERSIONS:
    return False
return KNOWN_PROTOCOL_VERSIONS.index(version) >= KNOWN_PROTOCOL_VERSIONS.index(minimum)
```

`minimum` 是**代码里的常量**，写错就是程序员的 bug，所以 `ValueError`；`version` 是**对端送来的字符串**，不可信，所以未知就返回 `False`（保守地认为「不够新」）。同一个函数里两种失败处理方式，区分依据是「这个值是谁写的」。

**surface 映射的规模来自「每个版本都单独列一行」。**

`CLIENT_REQUESTS` 这类映射是逐版本展开的字面量：`("completion/complete", "2024-11-05"): v2025.CompleteRequest`，然后是 `2025-03-26` 的同一行，再是 `2025-06-18`、`2025-11-25`，最后 `2026-07-28` 换成 `v2026.CompleteRequest`。看起来啰嗦，但这正是「键缺席即不可用」能工作的前提——如果用一个「从某版本起生效」的区间表示，那么「某版本删除」就必须额外编码，而逐版本列举让增删都退化为「这一行写不写」。

注释里补了两条重要事实：

- 「`2025-11-25` 的 `tasks/*` 是**刻意缺席**的」——这一档协议有任务概念但方法不从客户端发。
- 「`2026-07-28` 移除了 lifecycle、logging、subscribe 这一对，新增了 discover / listen」。

另一条注释解释了为什么 `2024-11-05` 与 `2025-03-26` 都指向 `v2025.*` 类型：「形状校验是按 schema 年代做的，即所有 pre-2026 版本用 `2025-11-25` 的形状、2026 用 `2026-07-28` 的形状」。所以映射的键是精确到版本的（闸门要精确），值是粗到年代的（形状只有两套）。

**结果映射的值可以是联合类型。**

`SERVER_RESULTS` / `CLIENT_RESULTS` / `MONOLITH_RESULTS` 的值类型是 `type[BaseModel] | UnionType`，因为有些方法有两种合法结果。`MONOLITH_RESULTS` 里有一处注释明确把顺序标成「load-bearing」：

```
# Arm order load-bearing: a single-block body satisfies both arms and
# smart-union ties resolve leftmost. Pinned by tests/types/test_methods.py.
"sampling/createMessage": types.CreateMessageResult | types.CreateMessageResultWithTools,
```

即：单块正文既能满足「无工具」也能满足「有工具」的模型，pydantic 的 smart union 在平局时取最左，所以左边的顺序是语义的一部分，且由测试钉住。这类「看着像格式、其实承重」的细节是映射表里最容易在重构中被顺手改坏的地方。

**两组派生集合把映射反过来读。**

```
SPEC_CLIENT_METHODS = frozenset(m for m, _ in CLIENT_REQUESTS)
INPUT_REQUIRED_METHODS = frozenset(
    method for method, row in MONOLITH_RESULTS.items()
    if any(issubclass(arm, types.InputRequiredResult) for arm in (get_args(row) if isinstance(row, UnionType) else (row,)))
)
```

前者是「任何版本下客户端可发的方法名」——服务端用它区分「规范方法」与「自定义方法」（自定义方法不做 surface 校验，直接落到注册的 `params_type`）。后者是「结果可能是 `input_required` 的方法」，供多轮往返（MRTR）路径判断。

`CACHEABLE_METHODS` 同样是派生出来的，而 `CacheableMethod`（一个手写 `Literal`）是它的类型镜像，注释写明「由测试焊死」——手写枚举无法从运行时集合自动生成，所以用测试保证两者不漂移。

**`is_input_required` 同时认类型与线形态。**

```
def is_input_required(result: object) -> TypeGuard[types.InputRequiredResult | dict[str, Any]]:
    if isinstance(result, types.InputRequiredResult):
        return True
    return isinstance(result, Mapping) and cast("Mapping[str, Any]", result).get("resultType") == "input_required"
```

它的参数是 `object`，返回 `TypeGuard`：既能收已解析的模型，也能收还没解析成模型的线格式字典。这不是为了方便，而是因为它在两个时机被调用——处理完的结果（模型）与序列化前的结果（可能是 handler 手写的 dict）。

**`parse_client_request` 是两道校验的组合。**

```
def parse_client_request(method, version, params, *, surface=CLIENT_REQUESTS, monolith=MONOLITH_REQUESTS):
    validate_client_request(method, version, params, surface=surface)
    return _monolith_row(monolith, method).model_validate(_body(method, params), by_name=False)
```

第一道 `validate_client_request` 做三件事：`_check_known_version(version)` 把版本笔误变成 `ValueError`（注释：「让一个笔误不能悄悄地对所有方法关闸」）、`surface[(method, version)]` 做版本闸门（`KeyError` 即不可用）、最后把 `{**{"jsonrpc": "2.0", "id": 0}, **{"method": ..., "params": ...}}` 喂给该版本的线上类型做形状校验——因为 surface 类是**完整帧**，缺 `jsonrpc`/`id` 会校验失败，所以代码里准备了 `_REQUEST_STUB` / `_NOTIFICATION_STUB` 两个桩。

第二道查 monolith 表。`_monolith_row` 的失败刻意**不是 `KeyError`**：

```
# Not KeyError: the surface row already matched, so a miss is inconsistent
# extension maps and must not be caught by the session's `except KeyError` gate.
raise RuntimeError(f"inconsistent extension maps: surface defines {method!r} but monolith does not") from None
```

因为上层用 `except KeyError` 表示「这个方法在这个版本不存在」，若这里也抛 `KeyError`，一个**扩展映射写错了**的内部错误就会被误判成「方法不可用」，静默吞掉。用 `RuntimeError` 把它顶出去。

`_body` 也在细节上对齐规范：`params` 为 `None` 时**不带这个键**，而不是带 `"params": null`（JSON-RPC 2.0 不允许后者）。

**序列化侧的一致做法。** `serialize_server_result` 用 `@cache` 的 `TypeAdapter` 做 `dump_python(by_alias=True, mode="json", exclude_none=True)`，并在 docstring 里点明 surface 模型都是 `extra="ignore"`，所以**不属于该版本的字段会被丢掉**——这既是对老客户端的保护，也意味着「新版加了字段但服务端在跟老客户端说话时会自动降级」。

## 关键取舍

**源码事实：** 版本比较走 `KNOWN_PROTOCOL_VERSIONS.index(...)`，注释明确拒绝字典序，并给出 `"zzz" > "2025-11-25"` 这个反例。

**源码事实：** 同一函数里 `minimum` 未知抛 `ValueError`、`version` 未知返回 `False`，区别在于前者是代码常量、后者是对端输入。

**源码事实：** 版本集被切成 handshake / modern 两组，`SUPPORTED_PROTOCOL_VERSIONS` 已被标 `Deprecated`，只保留做 v1.x 兼容。

**源码事实：** 版本闸门是「`(method, version)` 键缺席」，因此方法集的增删表现为逐版本字面量的增删，而不是区间表达式。

**源码事实：** 形状校验按年代粗分（pre-2026 全用 `2025-11-25` 类型，2026 用 `2026-07-28` 类型），而闸门精确到版本。

**源码事实：** `MONOLITH_RESULTS` 里 `sampling/createMessage` 的联合分支顺序「承重」，因为 smart union 平局取最左，且由测试钉住。

**源码事实：** surface 命中而 monolith 落空时抛 `RuntimeError` 而非 `KeyError`，避免被上层的 `except KeyError` 吞成「方法不存在」。

**架构推论：** 「surface + monolith」的两层映射，把「线上兼容性」和「用户代码稳定性」这两个正交的关注点分开了。若只有一层，要么用户代码随版本变形（monolith 变 surface），要么无法表达逐版差异（surface 变 monolith）。

**架构推论：** 派生集合（`CACHEABLE_METHODS`、`INPUT_REQUIRED_METHODS`）、手写 `Literal`（`CacheableMethod`）、测试三者构成一个三角：运行时能从映射表推导，类型层不能，于是用测试钉住两边一致。这是「类型系统表达不了但从数据可推导」时的标准补偿手段。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/mcp-types/mcp_types/version.py` | `KNOWN_PROTOCOL_VERSIONS` | 五个已发布版本，按新旧排列，作为唯一排序依据 |
| `src/mcp-types/mcp_types/version.py` | `SUPPORTED_PROTOCOL_VERSIONS` | 握手版 ∪ 现代版的并集，已标 Deprecated |
| `src/mcp-types/mcp_types/version.py` | `LATEST_PROTOCOL_VERSION` | 三个「最新」之一：跨年代最新版本号 |
| `src/mcp-types/mcp_types/version.py` | `is_version_at_least` | 枚举序比较，对未知对端版本保守返回 False |
| `src/mcp-types/mcp_types/methods.py` | `parse_client_request` | surface 校验 + monolith 解析的两段式入口 |
| `src/mcp-types/mcp_types/methods.py` | `_monolith_row` | 映射不一致时抛 RuntimeError，避开上层的 KeyError 闸门 |
| `src/mcp-types/mcp_types/methods.py` | `is_input_required` | 同时识别已解析模型与线格式字典的 TypeGuard |
| `src/mcp-types/mcp_types/methods.py` | `MONOLITH_RESULTS` | 方法 → 版本无关结果模型，联合分支顺序承重 |
| `src/mcp-types/mcp_types/methods.py` | `CACHEABLE_METHODS` | 从结果映射派生出的可缓存方法集 |

## 参考

- [mcp-python-sdk 源码：mcp_types/version.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp-types/mcp_types/version.py)
- [mcp-python-sdk 源码：mcp_types/methods.py](https://github.com/modelcontextprotocol/python-sdk/blob/main/src/mcp-types/mcp_types/methods.py)
- [MCP 规范：版本协商](https://modelcontextprotocol.io/specification/2025-06-18/basic/lifecycle)
- [MCP 规范：方法索引](https://modelcontextprotocol.io/specification/2025-06-18)

## 与主核 / AI 的关系

「按协议版本分流」在主核里对应的是「按客户端协议版本选择报文格式」——典型做法是一个 switch 或一张处理器表，但主核通常只有一两个历史版本要兼容，表也不大。MCP 的复杂度来自**方法集本身在版本间增删**，于是「版本」不再是一个标量开关，而是「这张表里哪些键存在」。

对 AI 工程的实际意义在于那条「surface 命中而 monolith 落空抛 `RuntimeError`」的注释。它防的是一类非常隐蔽的故障：扩展映射写得不一致时，如果用 `KeyError` 表达，就会与「这个版本不支持该方法」的正常语义混在同一个异常类型里，被上层的 `except KeyError` 静默吞掉，表现为「某些自定义方法在某些版本下静默不工作」。**用异常类型区分「预期内的不可用」与「配置写错了」**，是任何做版本分流的系统都该有的一条纪律。

还有一条值得抄走的是 `is_version_at_least` 的非对称失败处理：允许来自代码的输入「大声失败」（`ValueError`），要求来自对端的输入「安静保守」（返回 `False`）。大量协议实现把两者混为一谈，结果是一个恶意或错误的版本串要么把服务器打崩，要么被当成最新版。
