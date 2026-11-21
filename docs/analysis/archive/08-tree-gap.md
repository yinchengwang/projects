> ⚠️ **归档说明**：本文分析对象（冻结内核 `engine/src/db`）已于 2026-11-18 删除，仅作历史参考。
> 当前主核是 `engine/db/`（自带 Makefile，不在 CMake 图内），见 [设计文档](../../superpowers/specs/2026-11-18-multimodal-db-project-organization-design.md)。

# Tree（Yang/层次树）模态差距深度分析

> 审查日期：2026-08-27 ｜ 审查方式：静态代码审查
> 代码位置：`engine/src/db/storage/yang/`（~1.49K 行）+ `src/db/yang/`（yang_model.c、yang_data.c）+ `src/db/netconf/`（netconf_server.c，631 行）
> 相关 commit：P3-3 Yang/NETCONF 注册（beac0aee1）

## 1. 实现现状盘点

### 1.1 模块清单

| 模块 | 文件 | 行数 |
|------|------|------|
| Yang 引擎 | `storage/yang/yang_engine.c` | 844 |
| Yang 树 | `storage/yang/yang_tree.c` | 628 |
| Yang 模型解析（递归下降） | `yang/yang_model.c` | - |
| Yang 数据节点 | `yang/yang_data.c` | - |
| NETCONF 1.0 服务器 | `netconf/netconf_server.c` | 631 |

### 1.2 关键事实修正（相对 8 月 25 日旧对比文档）

旧文档称"无 RFC 标准合规"——**部分过时**。P3-3（commit beac0aee1）注册了 Yang/NETCONF 模块，但净实现规模仅 ~1.5K 行（核心） + 631 行 NETCONF，**功能深度待核**。

## 2. 代码级质量审查

### 2.1 并发正确性

**缺陷 1：NETCONF 服务器并发模型未读全文；假定单会话「疑似·功能缺失」**

`netconf_server.c:5` 注释明示"内存态 NETCONF 会话"——多客户端并发接入的会话隔离、Session-ID 唯一性、Hello 能力协商锁定（RFC 6241 §8）未核全文。`provide-instance-selector`/锁屏等问题需结合 Yang/XPath 评估。

**缺陷 2：Yang 模型/数据节点无锁「疑似·实现质量缺陷」**

`yang_model.c` 的 parser 阶段单线程，运行时数据节点（yang_data.c）的并发访问未读到 mutex。

### 2.2 崩溃恢复

**缺陷 1：无 datastore 持久化落地「确认·功能缺失」**

sysrepo/libyang 标准实现区分 `<candidate>`/`<running>`/`<startup>` 三个 datastore；Yang 引擎 844 行未核 `persist/save/load` API 路径（来自旧文档的缺失项），运行时配置不能落地即失效。

### 2.3 内存安全

**正面证据 1：Yang 模型递归下降 parser 完整「确认」**

`yang_model.c:193-442` 提供 schema 节点添加（`yang_schema_add_child`）、`parse_leaf_inner/leaf/list/container/leaf_list/stmt_block`——YANG 1.1 (RFC 7950) 主要 statement 覆盖，结构合理。

**正面证据 2**：set_str 宏（netconf_server.c:18-26）显式处理 NULL 防截断。

**缺陷 1：手写字符串扫描式 XML parser「确认·实现质量缺陷（功能正确性）」**

`netconf_server.c:3-6` 注释承认："使用字符串扫描做 XML 解析，不支持 XML 属性、命名空间前缀"。NETCONF 1.0 (RFC 4741) / 1.1 (RFC 6241) 的所有 RPC 都包裹在 `<rpc message-id="..." xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">` 中——属性 `message-id` 与命名空间是协议必须，不是可选项。简化实现意味**任何标准 NETCONF 客户端连不上**。

`read_ident`（:36-44）允许 `:` 通过——暗合 XML namespace 前缀，但注释否认，且未实现 `<ns:name>` 分辨语义。

### 2.4 错误处理

**缺陷 1：parser 失败回滚 `goto fail` 模式「确认·实现质量缺陷」**

`yang_model.c:352` `parse_list` 中 `if (parse_stmt_block(l, node) != 0) goto fail;`——典型 C goto fail 模式，需配统一的清理标签（resource leak 风险点）。`parse_container:388` 同样模式。共 4-5 处 goto fail，是否所有 fail 都正确释放待通审。

### 2.5 算法实现质量

**正面证据**：Yang schema 解析器为递归下降标准实现；`parse_stmt_block:442` 应该是核心分发。

**缺陷 1：未实现 Yang XPath 表达式求值「确认·功能缺失」**

`yang/...` grep `xpath/XPath` 0 命中（除头文件）。sysrepo/libyang 提供完整 Yang XPath 子集（path/key/predicate）；自研无等价物意味着无法实现 RFC 7950 §6.4 的 filter/subtree 过滤。

**缺陷 2：NETCONF chunked framing (RFC 6242) 未核「疑似·功能缺失」**

netconf_server.c:5 仅提"NETCONF 1.0"——RFC 6242 chunked framing 是后续 NETCONF 1.1 主流传输层；SSH transport 也未读全文（通常依赖外部库 libssh 或类似）。

### 2.6 API 设计

**正面证据**：存储/数据模型/传输分层（storage/yang + yang/ + netconf/）——结构清晰。

**缺陷 1：与 PostgreSQL ltree 不兼容「确认·功能缺失」**

`ltree` 是 PostgreSQL 事实标准路径列类型，配套 GiST 索引 + `~`/`?`/`@>` 操作符。自研 Yang 路径走 NETCONF/XPath 风格，无 ltree 互操作层。

## 3. 业界标杆对比

| 维度 | 自实现 | libyang | sysrepo | PostgreSQL ltree | BaseX |
|------|--------|---------|---------|------------------|-------|
| Yang 1.1 模型解析 | ✓（递归下降） | ✓（最成熟） | 复用 libyang | - | - |
| Yang XPath 求值 | ✗ | ✓ 完整 | ✓ | - | - |
| NETCONF 1.1 协议 | △（无 XML 属性/命名空间） | libnetconf2 | sysrepo-netopeer | - | - |
| Datastore | 未确认 | ✓ running/candidate/startup | ✓ | - | - |
| 标准 XQuery/XPath | ✗ | - | - | - | ✓ |
| GiST 路径索引 | ✗ | - | - | ✓ ltree_ops | - |
| RFC 6020/7950/6241 | △ 部分（parser 在） | 完整 | 完整 | - | - |
| 网络自动化集成 | △ | sysrepo+netopeer 主流方案 | 同左 | ✗ | ✗ |

## 4. 差距矩阵

| 维度 | 评分 | 关键证据 |
|------|------|---------|
| 并发正确性 | 4 | Yang parser 单线程；数据节点无锁（疑似） |
| 崩溃恢复 | 3 | datastore 持久化未确认 |
| 内存安全 | 5 | parser 完整 `yang_model.c:193-442`；goto fail 多处 `yang_model.c:352,388` |
| 错误处理 | 4 | goto fail 资源清理依赖人工审计；手写 XML 解析否认必要属性/命名空间 |
| 算法实现质量 | 4 | Yang parser 完整；但无 XPath 求值；NETCONF 协议不完整 |
| API 设计 | 5 | 分层清晰；缺 ltree 互操作 |

**实现质量缺陷清单（3 项确认 + 5 项疑似）**：
1. 手写 XML parser 不支持属性/命名空间 `netconf_server.c:3-6`（功能正确性）
2. datastore 持久化未确认（功能缺失/崩溃）
3. goto fail 多处需人工审计（实现质量缺陷）
4. 无 Yang XPath 求值（功能缺失）
5. NETCONF chunked framing / SSH transport（疑似）
6. 并发 session 隔离（疑似）
7. 数据节点无锁（疑似）

## 5. 改进优先级

| 优先级 | 项目 | 分类 | 工作量 |
|--------|------|------|--------|
| P0 | XML parser 升级：支持属性/命名空间；或集成 libxml2/expat | 实现质量缺陷 | M |
| P0 | datastore 持久化（candidate/running/startup 三态）+ fsync | 崩溃/功能 | M |
| P1 | Yang XPath 子集求值 | 功能缺失 | M |
| P1 | goto fail 资源清理审计 + 自动化（AST 引用计数或 arena） | 实现质量缺陷 | M |
| P2 | NETCONF 1.1 chunked framing (RFC 6242) | 功能缺失 | M |
| P2 | SSH transport（libssh 集成） | 功能缺失 | M |
| P3 | PostgreSQL ltree 兼容层 | 功能缺失 | L |
