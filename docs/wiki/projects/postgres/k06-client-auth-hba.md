---
project: postgres
feature: client-auth-hba
page_type: feature
schema_version: 1
refs:
  - path: src/backend/libpq/hba.c
    symbols: [load_hba, hba_getauthmethod, check_hba, parse_hba_line]
---

# 客户端认证与 HBA

## 一句话职责

HBA 子系统把连接类型、地址、库名与角色等规则编译成内存列表，并按首个匹配行决定该连接的认证方法与参数。

## 机制是什么

`pg_hba.conf` 的每一行是一条规则：连接类型（local/host/hostssl/hostnossl/hostgssenc/hostnogssenc）、库名列表、角色列表、地址或主机名，以及认证方法与其参数。`parse_hba_line` 把 token 化的一行编译成 `HbaLine`（记录 `conntype`、`databases`、`roles`、`ip_cmp_method`、地址/掩码或主机名、`auth_method` 等）；`load_hba` 在独立内存上下文中把整份文件解析成规则列表并原子替换旧列表；`check_hba` 按顺序逐行匹配，第一个匹配行写入 `port->hba`。

本机制只做"选哪条规则、用哪种认证方法"，不执行口令校验、不校验证书链、不建 TLS 会话——这些在认证方法实现中完成。它也不解析角色权限或执行 SQL 授权检查；`get_role_oid` 只用于按名称匹配角色规则，角色不存在不会在此报错。

## 怎么实现的

1. `load_hba` 用 `open_auth_file` 打开配置，`tokenize_auth_file` 完成逐行分词；随后在 `PostmasterContext` 下新建 `hba parser context` 子上下文，逐行调用 `parse_hba_line`，把成功解析的行追加到临时列表 `new_parsed_lines`。
2. 任一行分词或解析失败都只把整体 `ok` 置为 false 并继续解析其余行，以便一次报告更多错误；分词阶段已带 `err_msg` 的行直接跳过。整份文件解析成功但零条规则时也判为失败，因为那会导致任何连接都无法匹配。
3. 失败路径 `MemoryContextDelete(hbacxt)` 丢弃整份临时结果并返回 false，旧列表继续生效；成功路径先删除旧的 `parsed_hba_context`，再让 `parsed_hba_context` 指向新上下文并把 `parsed_hba_lines` 换成新列表。这是整份替换而非增量合并。
4. `parse_hba_line` 逐字段校验：连接类型必须是已知字面量且只能有一个值；未启用 SSL 或构建不支持 GSS 时，`hostssl`/`hostgssenc` 行会记录错误信息使其在匹配期不可用；随后解析库名（必要时 `regcomp_auth_token` 编译正则）、角色名、地址或主机名，以及认证方法与其参数字段。
5. `hba_getauthmethod(port)` 只调用 `check_hba(port)`；后者先用 `get_role_oid(..., true)` 取目标角色 OID（允许无效），然后按规则顺序遍历 `parsed_hba_lines`。
6. 每行依次比较连接类型（`local` 只匹配 `AF_UNIX`，网络类型跳过 Unix socket）、SSL 与 GSS 状态（`host` 同时匹配有无加密的连接，`hostssl`/`hostnossl`/`hostgssenc`/`hostnogssenc` 只匹配对应状态）、地址（掩码比较、主机名、`samehost`/`samenet`）、库名与角色。首个全部通过的规则写入 `port->hba` 后立即返回。
7. 全部不匹配时分配一个 `uaImplicitReject` 的 `HbaLine` 赋给 `port->hba`，即隐式拒绝；因此"没有任何规则匹配"与"显式 reject 规则"在认证阶段表现为同一类结果，但诊断来源不同。

## 关键取舍

- **源码事实：**`load_hba` 采用临时上下文构建、成功后整份替换，失败则保留旧列表并返回 false，由调用者决定是启动 FATAL 还是保持当前配置。**架构推论：**以失败时不生效为代价保证规则集始终是某个完整文件的一致快照；若逐行增量更新，半个文件的错误配置可能已经部分接管匹配顺序。
- **源码事实：**`check_hba` 按首个匹配行即返回，`host` 规则同时匹配 SSL 与非 SSL 连接，`hostssl` / `hostnossl` 各自只匹配对应状态。**架构推论：**这使路由决策由规则顺序完全决定，运维可通过行序表达优先级；若对所有匹配行做加权或最优选择，配置语义将不再可预测。
- **源码事实：**无匹配时使用 `uaImplicitReject` 而非报错缺少规则。**架构推论：**默认拒绝避免"漏写规则即放行"的安全风险，代价是配置错误表现为连接被拒而非明显配置错误；若默认放行，任何规则漏配都会成为认证绕过。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/libpq/hba.c` | `load_hba` | 分词并逐行解析整份文件，成功时原子替换规则列表。 |
| `src/backend/libpq/hba.c` | `parse_hba_line` | 校验连接类型并编译库名/角色/地址与认证方法字段为 HbaLine。 |
| `src/backend/libpq/hba.c` | `check_hba` | 按顺序逐行匹配连接属性，首个匹配行写入 `port->hba`。 |
| `src/backend/libpq/hba.c` | `hba_getauthmethod` | 连接入口，转调 `check_hba` 以确定认证方法。 |

## 参考

- [PostgreSQL 文档：pg_hba.conf 文件](https://www.postgresql.org/docs/current/auth-pg-hba-conf.html) —— 说明规则字段、连接类型与匹配顺序。
- [PostgreSQL 官方源码：HBA 解析与匹配](https://github.com/postgres/postgres/blob/master/src/backend/libpq/hba.c) —— 对照行解析、整份替换与逐行匹配实现。

## 与主核 / AI 的关系

暂无直接对应。主核若需要连接级准入控制，可借鉴"整份配置原子替换 + 首匹配即决定 + 无匹配即拒绝"三条原则；具体认证方法与会话建立不在该层。
