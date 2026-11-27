---
project: openhands
feature: mcp-redacted-credentials
title: MCP 凭据脱敏与服务端替换
page_type: feature
schema_version: 1
refs:
  - path: src/api/mcp-service/mcp-redacted-credentials.ts
    symbols: [hasRedactedValue, substituteRedactedLeaves, substituteRedactedMcpCredentials]
  - path: src/api/mcp-service/mcp-redacted-credentials.ts
    symbols: [fetchEncryptedStoredServer, findStoredServer]
---

# MCP 凭据脱敏与服务端替换

## 一句话职责

MCP 服务器的配置里有密钥（stdio 的 env、远程的 auth 与 headers）。编辑界面不能看见明文，所以服务端回给界面的是占位符；当用户没有改动某个密钥却点了「测试连接」时，前端必须把占位符换回已存储的真实值，才能让探测真正带上凭据。

## 机制是什么

这是浏览器端凭据处理的一个常见两难：**要能编辑，就不能给明文；要能测试，就必须有明文**。

OpenHands 的解法是把「展示」与「使用」拆成两条路径：

- **展示路径**：设置接口以 `encrypted` 形态返回，前端拿到的密钥字段是固定的占位符 `**********`（`REDACTED_MCP_SECRET_VALUE`）。界面把它当普通值展示与提交，用户看不出内容。
- **使用路径**：用户点「测试连接」时，前端先取一次加密存储，把提交表单里仍是占位符的叶子替换成存储中的真实值，然后发探测请求。

关键约束写在源码注释里：**持久化永远不走这个替换**。保存走的是稀疏补丁——没改的密钥不会出现在补丁里，所以服务端根本不需要「把占位符当真的写回去」。这条边界让「替换函数」只在探测这一处生效，不会污染写入路径。

## 怎么实现的

**三处按传输形态分流。** `substituteRedactedMcpCredentials` 先判断哪一类字段需要替换：

- `stdio` 类型的 `env`；
- `sse` / `shttp` 类型的 `auth`（走 `hasRedactedMcpSecretLeaf` 判嵌套结构）；
- `sse` / `shttp` 类型的 `headers`。

三类都不需要替换时**直接返回原对象**——不发起额外的设置请求。这是性能上的实际考量：设置接口往返一次不便宜，而绝大多数提交不含占位符。

**按 id 定位存储条目。** `fetchEncryptedStoredServer` 拉一次 `fetchSettingsFromApi("encrypted")`，取 `agent_settings.mcp_config`，再按 `server.id` 用 `findStoredServer` 找到同一条。找不到就返回 `undefined`，调用方退回原对象——意味着「这条服务器还没真正存过」，此时替换无从谈起，探测会拿占位符去试（会失败，但不会泄漏任何东西）。

**递归叶子替换。** `substituteRedactedLeaves` 是通用的深层替换：遍历提交对象的每个键，值等于占位符且存储里同键是字符串、且本身不是占位符时，才替换；值是嵌套记录时递归。**未改动的结构与用户新填的值都原样保留**，所以探测请求测试的正是「将要被持久化的那个配置」。

源码注释把这一点说得很明确：`probe request exercises the server exactly as it will be persisted`（探测请求所测的，正是它最终被持久化的样子）。

**`env` 与 `headers` 走扁平版。** 这两类值是扁平的字符串映射，用 `stringRecord` 过滤掉非字符串项（防脏数据），再逐键替换；`auth` 是嵌套结构，才用递归版。

## 关键取舍

**源码事实：** 占位符是固定字符串 `REDACTED_MCP_SECRET_VALUE`，替换逻辑靠「值恰好等于它」判断。这意味着如果用户真的把某个密钥设置成这个字面值，它会被当成占位符处理。这是把「简单可判定」放在「极端输入保真」之上的取舍。

**源码事实：** 注释明确写「持久化从不调用这个辅助函数；稀疏补丁省略未变的密钥」。这条边界是整套机制的安全基础——替换只在探测链路上存在，写入链路不接受占位符语义。

**源码事实：** 三类红acted 判断是提前算的，都不成立时直接返回，不发设置请求。

**源码事实：** `substituteRedactedLeaves` 在替换时还检查了「存储里的值本身不是占位符」，避免在某些异常状态下把占位符换成正占位符、让探测永远失败而没有任何报错线索。

**架构推论：** 「服务端返回脱敏值、前端按需回填」是一种比「前端缓存明文」更安全的模式。前端从未持有明文；每次需要真值时都向服务端要一次加密存储，用完即弃。代价是每次探测多一次接口往返，收益是浏览器内存里没有可被扩展或 XSS 读取的密钥。

**架构推论：** 这条设计与 MCP 服务本身的 `redactMcpTestResponse` 构成对称：**入站脱敏（前端→服务端的提交）与出站脱敏（服务端→前端的响应）都要做**，因为探测结果里也可能带回凭据（例如连接失败的报错里带 token 片段）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `substituteRedactedMcpCredentials` | 按传输形态分流的替换入口 |
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `substituteRedactedLeaves` | 嵌套结构的递归叶子替换 |
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `hasRedactedValue` | 扁平映射里是否有占位符 |
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `fetchEncryptedStoredServer` | 取一次加密设置并定位同 id 条目 |
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `findStoredServer` | 按 id 匹配存储条目 |
| `src/api/mcp-service/mcp-redacted-credentials.ts` | `stringRecord` | 过滤非字符串项，防脏数据 |

## 参考

- [OpenHands 源码：mcp-redacted-credentials.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/mcp-service/mcp-redacted-credentials.ts)
- [MCP 规范：授权](https://modelcontextprotocol.io/specification/2025-06-18/basic/authorization)
- [OpenHands 文档：MCP](https://docs.openhands.dev/openhands/usage/mcp)
- [OWASP：敏感数据暴露](https://owasp.org/Top10/A02_2021-Cryptographic_Failures/)

## 与主核 / AI 的关系

这套「展示脱敏 + 使用回填」与主核（多模态数据库内核）里的凭据处理有一条直接可用的结论：**明文只在需要它的那一刻存在**。

主核在连接外部存储时也要用密码，做法是配置里存加密形态、连接时解密到局部变量、连上后立刻丢弃。差别在于主核是长驻进程、连接池会长期持有连接，所以「用完即弃」发生在建立连接这一刻；OpenHands 是浏览器前端、每次探测是一次独立请求，所以「用完即弃」是每个请求一次。

**稀疏补丁那条边界值得记住。** 保存时不提交未改动的字段，是让「服务端永远不需要理解占位符」的关键。反过来说，如果保存也走替换逻辑，那么「占位符」就变成了协议的一部分，任何一次部分更新都可能覆盖掉真实密钥——这是很多配置界面踩过的坑。

与 `browser-use` 的敏感值脱敏对照：那边是**运行时**脱敏（模型写的代码可能读到值，靠脱敏不让值回流到上下文）；这里是**展示时**脱敏（前端从未持有值，靠回填让真实值只出现在发出的请求里）。同一个词，两种完全不同的作用点。
