---
project: postgres
feature: acl-and-ownership
page_type: feature
schema_version: 1
refs:
  - path: src/backend/catalog/aclchk.c
    symbols: [ExecuteGrantStmt, ExecGrant_common]
  - path: src/backend/catalog/objectaddress.c
    symbols: [check_object_ownership]
---

# 权限与属主

## 一句话职责

ACL 检查把对象 ACL、默认 ACL、角色成员关系和超级用户规则汇成“此角色可做哪些操作”的掩码；对象属主检查则在 DROP/ALTER 等对象命令分派前确认调用者是否具备该类对象的 owner 身份或替代资格。

## 机制是什么

对象的 ACL 保存在所属系统目录列（如 `pg_class.relacl`），缺省 ACL 为 NULL 时由 `acldefault` 按对象类型与 owner 合成默认值。`aclmask` 将直接授权、PUBLIC 授权、角色成员继承和 owner 隐含权限折算为 bitmask；`object_aclcheck`/`pg_class_aclcheck` 再把“是否包含请求权限”转成 `AclResult`。对象所有权不是 ACL 中的某一个普通权限位：generic `object_ownercheck` 取目录里的 owner OID，再以 `has_privs_of_role(requester, owner)` 判定用户是否是 owner 的成员/继承角色，超级用户直接绕过。

`ExecuteGrantStmt` 将解析树转换为内部 GRANT/REVOKE 请求，解析目标对象与 grantee、把权限名转成 `AclMode` 并拒绝对象类型不支持的权限；之后 `ExecGrant_common` 在相应目录上锁定目标 tuple，求出最合适 grantor 与可授出的 grant option，合并 ACL，更新目录 tuple 并同步更新 shared dependency 信息。`check_object_ownership` 则由对象命令的 objectaddress 主流程调用，依 `ObjectType` 分派到 `object_ownercheck` 或特殊规则（如角色需要 CREATEROLE + ADMIN，large object 兼容模式例外）。因此“能被授予某权限”和“是对象属主”是两条不同的授权判断。

本页只讲 ACL 与属主判定主流程，不展开 namespace 解析、不讲 dependency 图如何递归删除，也不逐个重述每种 SQL 命令自己的权限语义。

## 怎么实现的

1. `ExecuteGrantStmt` 把解析器的 `GrantStmt` 转为 `InternalGrant`：先将名字解析为对象 OID（或按 schema 列举对象），将 grantee RoleSpec 变成 role OID / `ACL_ID_PUBLIC`，保留 grant/revoke、grant option、grantor 与 RESTRICT/CASCADE 行为。再按对象类型求 `all_privileges` 支持掩码，将权限字符串转成 `AclMode`，列级权限只对 relation 保留到 `col_privs`，其它类型的列授权直接报错。
2. `ExecGrantStmt_oids` 按对象类型分派：relation/sequence 用 `ExecGrant_Relation`，一般对象（database/type/function/schema/FDW 等）用 `ExecGrant_common`，large object 有单独分支。对象类型在这里决定 ACL 所在目录、ACL 字段与有效权限 bitset。
3. `ExecGrant_common` 以 RowExclusiveLock 打开对象目录，逐个 OID 用 `SearchSysCacheLocked1` 取 tuple 并建立 inplace tuple lock；此处把“锁住要修改的 catalog tuple”与“读取 ACL/owner 并形成更新”结合，降低并发 GRANT/REVOKE 互相覆盖的风险。若对象类型有专属检查回调（例如语言是否 trusted）就在改 ACL 前执行。
4. 若 ACL 列为 NULL，就用 `acldefault(objectType, ownerId)` 得到该对象类型的默认 ACL；否则复制现有 ACL。`select_best_grantor` 根据显式 grantor、已有 ACL 与 owner 选定真正 grantor 和可用 grant options；`restrict_and_check_grant` 限制请求到授予者可授出的范围，并按 SQL 标准警告或拒绝不足授权。
5. `merge_acl_with_grant` 生成新 ACL，`heap_modify_tuple` 仅替换 ACL 列，`CatalogTupleUpdate` 写回目录；随后解 tuple lock、更新 extension initial privileges，并通过 `updateAclDependencies` 校正 ACL 中涉及角色的 shared-dependency 记录。释放 syscache 引用后执行 `CommandCounterIncrement`，使同一语句后续处理重复对象时读到更新后的 ACL。
6. `object_ownercheck` 对超级用户立即成功；large object class ID 先映射到 `pg_largeobject_metadata`。若该对象类型有 syscache，就按 OID 取 owner 字段并释放缓存引用；否则按对象目录的 OID 列及 OID 索引打开关系扫描。最终通过 `has_privs_of_role(roleid, ownerId)` 判断调用角色是否具有 owner 角色权限，而不只做 `roleid == ownerId` 的直接比较。
7. `check_object_ownership` 按对象类型选择属主依据：relation 派生对象（index、sequence、column、rule、trigger、policy、table constraint）以父 relation owner 为准；domain constraint 取所属 type owner；routine/type/schema 等按 ObjectAddress 查询 owner。large object 可由 `lo_compat_privileges` 允许旧式 ACL 兼容；role 没有传统 owner，改用 CREATEROLE 并要求 ADMIN option，目标若为 superuser 则只能由 superuser 操作。某些内部对象类型被明确标为 unsupported，避免误套通用属主模型。

## 关键取舍

- 源码事实：`object_ownercheck` 以 `has_privs_of_role(roleid, ownerId)` 作为 owner 测试，超级用户直接 bypass；role 对象则不是传统 owner，而由 CREATEROLE 与 ADMIN option 规则处理。架构推论：把角色继承纳入 owner 资格，符合 PostgreSQL 的角色授权体系；若只比较 owner OID，会迫使组角色成员无法执行本应允许的 owner 操作。
- 源码事实：`ExecGrant_common` 在目录 tuple 上用 `SearchSysCacheLocked1`，复制/合并 ACL 后 `CatalogTupleUpdate`，再维护 shared dependency 并做命令计数器推进。架构推论：锁住被改 ACL 行可序列化并发 grantor/ACL 修改；若以未锁快照读后直接写回，两个 GRANT 可彼此覆盖授权。
- 源码事实：ACL 为 NULL 时读取路径使用 `acldefault`，而不是在对象创建时必须将默认权限实体化存入目录；GRANT 后再写入实际 ACL。架构推论：多数对象无需存储冗长默认 ACL，节省目录空间；代价是所有读取者必须知道默认 ACL 规则并用对象类型计算。
- 源码事实：`check_object_ownership` 按 object type 分派，部分对象用父对象 owner，角色、large object、transform 等有特殊路径。架构推论：对象类型的管理关系并不总是“其目录行的 owner 字段”；统一按 ObjectAddress 的 owner 字段做通用判断会对列、索引、角色等对象误授权或误拒绝。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/catalog/aclchk.c` | `ExecuteGrantStmt` | 从 SQL GrantStmt 构造内部授权请求，解析对象/grantee 并校验权限类别及对象类型限制。 |
| `src/backend/catalog/aclchk.c` | `ExecGrant_common` | 对通用对象目录锁定 ACL tuple、选 grantor、合并新 ACL、更新目录及 shared dependency。 |
| `src/backend/catalog/objectaddress.c` | `check_object_ownership` | 在对象命令层按 ObjectType 选择属主检查规则，并处理父对象、角色等特殊情形。 |

## 与主核 / AI 的关系

主核权限检查体系并未采用 PostgreSQL ACL 数组、PUBLIC/角色继承 bitmask 与对象 owner 规则的直接等价模型。可借鉴把“动作权限”与“对象属主资格”分开检查，并让对象类型显式声明特殊归属关系；不要将 PostgreSQL ACL 位或 superuser 规则映射为主核的现有权限语义。

## 参考

- [PostgreSQL 文档：权限](https://www.postgresql.org/docs/current/ddl-priv.html) —— 说明对象权限、所有者权限与角色授权概念。
- [PostgreSQL 文档：GRANT](https://www.postgresql.org/docs/current/sql-grant.html) —— 说明 SQL GRANT/REVOKE 的授权与 grant option 行为。
- [PostgreSQL 官方源码：ACL 检查与 GRANT](https://github.com/postgres/postgres/blob/master/src/backend/catalog/aclchk.c) —— 支撑 ACL 读取、更新与 ownercheck 的目录路径。
