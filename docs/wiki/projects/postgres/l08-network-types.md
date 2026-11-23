---
project: postgres
feature: network-types
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/adt/network.c
    symbols: [network_in, network_out, inet_out]
  - path: src/backend/utils/adt/mac.c
    symbols: [macaddr_in, macaddr_out]
---

# 网络类型

## 一句话职责

网络类型把 IP 地址与网段、MAC 地址解析成紧凑的定长二进制值，并在输出时按其族、掩码长度或固定格式还原文本。

## 机制是什么

`inet` 与 `cidr` 共用 `inet` 结构：`family` 表示 IPv4 或 IPv6、`bits` 是掩码位数、`ipaddr` 是最多 16 字节的地址本体，整体作为 varlena 存储。`inet` 允许地址携带主机位，`cidr` 则要求掩码右侧的位全为 0。`macaddr` 是六个字节的结构化值，只表示链路层地址，不参与 IP 掩码语义。

这些类型负责地址文本 I/O 与地址本身的表示，不负责子网匹配的规划器优化语义（如操作符族选择）、也不负责索引实现；地址运算（加减、掩码合并）在此之上按子类型比较与位运算实现。`inet_out` 是 `inet` 类型的输出入口，它复用共享的 `network_out`，仅以 `is_cidr` 参数区分是否补写掩码后缀。

## 怎么实现的

1. `network_in` 先分配零初始化 `inet` 并判断族：源串含 `:` 视为 IPv6，否则按 IPv4 处理。零初始化是必要的，因为访问宏通过 `VARDATA_ANY` 读地址，而在 `SET_INET_VARSIZE` 完成前头部尚未写定。
2. 用 `pg_inet_net_pton` 解析地址与可选 `/bits`。对 `cidr` 传入地址字节长度以便严格校验，返回值必须是合法前缀长度（不超过该族最大位数），否则报非法输入语法。
3. 若是 `cidr`，再用 `addressOK` 检查掩码右侧位是否全零；不满足则报 `invalid cidr value` 并提示存在掩码右侧的置位。
4. 写入 `ip_bits` 后调用 `SET_INET_VARSIZE`，按族与位数确定实际 varlena 长度。`inet_in` 与 `cidr_in` 只把 `is_cidr` 传给同一实现。
5. `network_out` 用 `pg_inet_net_ntop` 格式化地址与前缀到固定缓冲；格式化失败报错误。仅当 `is_cidr` 且输出中没有 `/` 时才追加 `/<bits>`，避免重复。`inet_out` 以 `is_cidr=false` 调用它，故 `inet` 只在位数为默认掩码时省略后缀。
6. `macaddr_in` 依次尝试多种常见书写法（冒号/连字符分隔、点分、分隔成对或无分隔的十二位十六进制），要求 `sscanf` 恰好匹配六个字段且无尾随垃圾；任一八位组超出 0–255 报越界错误。`macaddr_out` 固定输出小写十六进制、冒号分隔的六段。

## 关键取舍

- **源码事实：**`inet` 与 `cidr` 共用同一存储结构并以 `is_cidr` 参数区分校验与输出，`cidr` 额外要求掩码右侧位为零。**架构推论：**共享表示让两者在存储与比较上互操作，代价是校验分支必须在输入入口显式区分；若各建一套结构，类型转换与索引操作符需成对实现。
- **源码事实：**`inet_out` 复用 `network_out` 且 `is_cidr=false`，只有 `cidr` 输出会在缺少 `/` 时补掩码。**架构推论：**这避免 `inet` 值被误写成看起来像 `cidr` 的带掩码文本；若统一补后缀，`inet` 与 `cidr` 的文本表示将无法区分。
- **源码事实：**`macaddr_in` 用 `%1s` 匹配尾随垃圾并要求 `count == 6`，逐格式回退尝试。**架构推论：**以多次 `sscanf` 尝试换取容错输入，代价是失败路径要做多轮扫描；若只接受单一格式，用户常见写法需要先手工规范化。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/adt/network.c` | `network_in` | 判定地址族、解析地址与掩码并对 cidr 校验右侧零位。 |
| `src/backend/utils/adt/network.c` | `network_out` | 由族、地址与位数格式化文本，按需追加掩码后缀。 |
| `src/backend/utils/adt/network.c` | `inet_out` | `inet` 类型的输出入口，复用共享输出且不强制补后缀。 |
| `src/backend/utils/adt/mac.c` | `macaddr_in` | 尝试多种 MAC 书写格式解析六个八位组并校验范围。 |
| `src/backend/utils/adt/mac.c` | `macaddr_out` | 以固定冒号分隔小写十六进制格式输出 MAC 地址。 |

## 参考

- [PostgreSQL 文档：网络地址类型](https://www.postgresql.org/docs/current/datatype-net-types.html) —— 说明 inet/cidr/macaddr 的语义与掩码规则。
- [PostgreSQL 官方源码：inet/cidr 实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/network.c) —— 对照地址解析、校验与输出格式。

## 与主核 / AI 的关系

暂无直接对应。主核如需网络类型，应复用标准地址解析库并明确 `inet` 与 `cidr` 的校验差异，避免文本表示歧义。
