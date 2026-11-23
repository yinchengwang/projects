---
project: postgres
feature: authorization-checks
page_type: feature
schema_version: 1
refs:
  - path: src/backend/catalog/aclchk.c
    symbols: [object_aclcheck_ext, aclcheck_error]
  - path: src/backend/utils/adt/acl.c
    symbols: [aclmask, has_privs_of_role]
---

# 权限检查

## 一句话职责

在访问某个对象前，用 ACL 位掩码判定当前角色是否持有所需特权，并把失败翻译成统一的权限错误上报。

## 机制是什么

权限检查是对象访问的前置判定：调用者给出对象类别、对象 OID、角色与所需特权位，机制读取该对象的 `Acl` 数组，逐条累加匹配的授予位，据此返回“允许”或“无权限”。它把“谁能授予什么”与“是否被授予”分开：前者是 GRANT/REVOKE 主流程维护 ACL 内容，后者是本机制读取 ACL 并作出判定。

本机制不负责 ACL 的写入与合法性校验（那是 GRANT/REVOKE 与 ALTER DEFAULT PRIVILEGES 的职责）、不负责默认权限的构造、不负责对象是否存在这类前置错误，也不负责 `BYPASSRLS`、superuser 之外的行级策略判定。与行级安全的分界是：这里判定“能不能访问这张表/这个列”，RLS 判定“能看到表里哪些行”。两者在同一个查询中会各自独立生效。

另一条边界是角色关系：`has_privs_of_role` 只回答“当前角色是否拥有目标角色的特权”，并不回答“能否 SET ROLE 到目标角色”——后者由 `member_can_set_role` 按 set_option 链单独判定。把两者混用会把“可继承的特权”和“可切换的身份”混为一谈。

## 怎么实现的

1. `aclmask` 是判定的核心。它先做两项快速返回：`mask == 0` 直接返回 0；当请求的特权位包含授予选项位且 `has_privs_of_role(roleid, ownerId)` 成立时，所有者隐式获得全部 grant option 位。
2. 第一遍扫描 ACL，只看直接授予给 `roleid` 或 `PUBLIC` 的条目，把这些条目的 `ai_privs` 与 `mask` 相与后累加。每累加一次就按 `how` 判断是否可提前结束：`ACLMASK_ALL` 要求结果等于掩码，`ACLMASK_ANY` 只要求非零。这个提前返回使常见“任一权限即可”的判定不必扫完整个数组。
3. 第一遍结束后计算 `remaining = mask & ~result`，第二遍只看未命中的部分。这一遍要跳过第一遍已处理的条目，并且在执行代价较高的 `has_privs_of_role(roleid, aidata->ai_grantee)` 之前，先检查该条目是否真的含 `remaining` 中的位；源码注释说明这是为了把昂贵的间接成员关系测试推迟到确有需要时。
4. `has_privs_of_role` 本身也有快速路径：`member == role` 直接为真；`superuser_arg(member)` 为真直接为真；其余情况通过 `roles_is_member_of(member, ROLERECURSE_PRIVS, ...)` 展开多级成员关系，再判断目标角色是否在结果列表中。源码注释明确它不沿非继承的授权递归，只有可继承的授予才自动带来特权。
5. `object_aclcheck_ext` 是对象级检查的统一入口：它调用 `object_aclmask_ext` 取回实际持有的位，用 `ACLMASK_ANY` 语义判定，非零即 `ACLCHECK_OK`，否则 `ACLCHECK_NO_PRIV`。带 `is_missing` 参数的版本把“对象不存在”与“无权限”区分开：对象缺失时置 `*is_missing = true` 并返回无权限，而不是抛错。
6. 对象类别到 ACL 存储位置的映射、所有者 OID 的读取以及 `is_missing` 的语义都在 `object_aclmask_ext` 内部完成，因此 `object_aclcheck_ext` 自身保持极薄——它只负责把“持有的位”折算成二值结果。这种分工使各类对象的调用方不必各自实现结果折算。
7. `aclcheck_error` 把失败结果翻译成用户可见错误：`ACLCHECK_OK` 直接返回（便于调用方在统一位置调用而不必先判断）；`ACLCHECK_NO_PRIV` 按 `objtype` 选 “permission denied for <类别> %s” 模板；`ACLCHECK_NOT_OWNER` 选 “must be owner of <类别> %s”。两类都按对象类别穷举 switch，并对尚未使用的类别显式 `elog(ERROR)` 而不是给一个含糊消息。
8. 消息模板用 `gettext_noop` 标记以便翻译抽取，最终以 `ERRCODE_INSUFFICIENT_PRIVILEGE` 抛出。函数注释说明形参 `objectname` 不加双引号，因为很多调用方传入的字符串本身已带引号，避免出现双重引号。

## 关键取舍

- 源码事实：`aclmask` 分两遍扫描，第一遍只处理直接授予与 `PUBLIC`，第二遍才做带 `has_privs_of_role` 的间接授予。放弃“一遍扫描并在每个条目上做成员关系判断”换来避免对每个 ACL 条目都付出一次可能昂贵的角色递归展开；代价是数组被遍历两次，且第二遍需要额外维护 `remaining`。
- 源码事实：`object_aclcheck_ext` 用 `ACLMASK_ANY` 折算，即持有掩码中任一特权位即视为通过。放弃“必须持有全部位”的严格折算使调用方可以按位组合查询所需能力而不必逐位调用；代价是调用方必须自己保证传入的掩码确实表达了“任一即可”的语义。
- 源码事实：`has_privs_of_role` 对 superuser 直接返回真，并以 `ROLERECURSE_PRIVS` 展开成员关系。放弃“把 superuser 当作 ACL 中的显式条目”使 ACL 内容保持精简；代价是 superuser 判定必须在每次检查时短路，且该函数不能用于“能否 SET ROLE”这种需要 set_option 链的场景。
- 源码事实：`aclcheck_error` 在 `ACLCHECK_OK` 时不做任何事而直接返回，同时对未列出的对象类别用 `elog(ERROR)` 兜底。放弃“调用方必须先判断结果再报错”的接口形态，使调用点可以写成“检查后无条件调用报错函数”；代价是一个本该成功的调用也可能流入该函数，只能靠这条早退分支消化。
- 架构推论：把“取位”与“折算结果”拆成 `object_aclmask_ext` 与 `object_aclcheck_ext` 两层，使需要知道具体持有哪几位的调用方可以复用同一套取位逻辑。源码只呈现了这种分层，未说明是否有意为此；若合并成一层，`ACLMASK_ALL` 与按位查询的调用方就需要重复实现取位。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/catalog/aclchk.c` | `object_aclcheck_ext` | 把对象 ACL 持有的位折算成 ACLCHECK_OK/NO_PRIV 并回传对象缺失标记 |
| `src/backend/catalog/aclchk.c` | `aclcheck_error` | 按对象类别与失败类型选择错误模板并抛出权限不足错误 |
| `src/backend/utils/adt/acl.c` | `aclmask` | 两遍扫描 ACL 累加直接与继承授予位，并按 ANY/ALL 语义提前返回 |
| `src/backend/utils/adt/acl.c` | `has_privs_of_role` | 判定角色是否拥有目标角色的可继承特权，含自身与 superuser 快速路径 |

## 参考

- [PostgreSQL 文档：GRANT 与特权](https://www.postgresql.org/docs/current/sql-grant.html) —— 支撑 ACL 条目、授予选项与继承语义的背景。
- [PostgreSQL 文档：角色成员关系](https://www.postgresql.org/docs/current/role-membership.html) —— 支撑可继承特权与 SET ROLE 的区分。
- [PostgreSQL 文档：权限与角色检查函数](https://www.postgresql.org/docs/current/functions-info.html) —— 支撑 `has_table_privilege` 等以 ACL 掩码判定的对外接口。
- [PostgreSQL 官方源码：ACL 检查与报错](https://github.com/postgres/postgres/blob/master/src/backend/catalog/aclchk.c) —— 支撑对象级折算与错误模板细节。
- [PostgreSQL 官方源码：ACL 位运算](https://github.com/postgres/postgres/blob/master/src/backend/utils/adt/acl.c) —— 支撑两遍扫描与角色成员判定实现。

## 与主核 / AI 的关系

暂无直接对应。主核若实现权限检查，可借鉴“所需特权以位掩码表达、ANY/ALL 语义显式传入、昂贵角色展开推迟到确有需要”三点；ACL 的存储格式、默认权限与所有者语义需按主核自身的对象模型定义。
