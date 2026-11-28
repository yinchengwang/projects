---
project: ragflow
feature: permission
title: 资源权限模型
page_type: feature
schema_version: 1
refs:
  - path: internal/permission/policy.go
    symbols: [CheckFeature, CheckResource, ResolveAccess]
  - path: internal/permission/types.go
    symbols: [ResourceKind, ResourceRef, NewChecker]
---

# 资源权限模型

## 一句话职责

把「谁能对哪个资源做什么」拆成一个纯判定函数：`Checker` 从 `context.Context` 里取当前用户，从 `Source` 里取资源与成员关系事实，按可见性与角色算出有效操作集合，再判断请求的操作是否在内。

## 机制是什么

权限判定在实现上最容易犯的错是「把判定逻辑分散到各个 handler 里」——每处都写一遍「是所有者吗、是同租户吗、是共享给这个人的吗」，必然会有一处写漏。这个包的做法是把判定收敛成三层：

| 层 | 职责 | 可变部分 |
|---|---|---|
| `Checker` | 对外统一入口，nil 安全 + 能力探测 | 无（编译期固定） |
| `policy` 接口 | 判定算法的实现 | 按版本（社区 / 企业）替换 |
| `Source` 接口 | 事实来源：资源与成员关系 | 按存储实现替换 |

**输入是 `ctx` 而不是显式参数。** 除了要检查的资源与操作，当前用户与租户都从上下文取（`UserIDFromContext` / `TenantIDFromContext`）。这让调用点极其简洁（一句 `CheckResource(ctx, ref, OperationRead)`），也让权限成为「隐式贯穿全链路」的东西——HTTP 中间件塞进 ctx，业务层任意深度都能用。

代价是**权限检查的依赖不在签名里**。读一个函数签名看不出它是否做权限校验，也看不出它依赖用户身份。

**输出是一个错误，不是布尔值。** 所有检查方法都返回 `error`，且错误是分门别类的哨兵值：`ErrUnauthenticated`（没登录）、`ErrPermissionDenied`（登录了但没权）、`ErrResourceNotFound`（资源不存在）、`ErrMembershipNotFound`（不是该租户成员）、`ErrInvalidPermission`（请求本身非法）、`ErrSourceUnavailable`（没配数据源）。上层据此决定返回 401 / 403 / 404——**这个区分很重要，它决定了对外暴露多少信息**。

## 怎么实现的

**`NewChecker` 有一个刻意的空实现。** `NewChecker(source Source)` 在 `source == nil` 时返回 `&Checker{}`（`policy` 为 nil）。于是后续所有方法第一句都是同一个守卫：

```
if c == nil || c.policy == nil { return ... ErrSourceUnavailable }
```

这让「权限系统未配置」表现为**每个检查都拒绝**，而不是 panic 或放行。这是 fail-closed 的正确方向。

**`policy` 是一个私有接口，按版本分派。** `newPolicy(source)` 先问 `newPolicyEE(source)`（带构建标签的企业版钩子）：它返回 `(policy, handled)`，`handled` 为真就用企业的实现，否则落到 `communityPolicy`。所以**同一个 `Checker` 在不同构建下行为不同**，而调用方完全无感。

`policy` 接口有八个方法，但 `Checker` 只暴露其中一部分；另有四个**可选能力接口**（`queryScopePolicy`、`accessResolverPolicy`、`dependencyBatchPolicy`、`resourceOwnerGranter`），通过类型断言探测，实现了就走批量路径，没实现就退回逐条循环。这是本包最一致的手法：**能力用类型断言探测，退化路径必须存在**。

**`CheckFeature` 在社区版几乎什么都不做，但它仍然做两件事。** 实现是：

1. 用户 ID 为空（含全空白）→ `ErrUnauthenticated`。
2. `feature` 为空或 `action` 不是四个合法值之一 → `ErrInvalidPermission`。
3. 然后返回 `nil`，注释写着「Community edition has no role-based feature restrictions」。

所以**社区版的 `CheckFeature` 实质是「形状校验 + 登录校验」**，不含任何角色判断。功能级的限制（例如「这个功能要专业版」）只存在于企业版。这个设计让调用点可以先写好 `CheckFeature`，社区版跑起来无副作用，企业版接上就生效。

**`CheckResource` 是真判定，路径最长。** 它先要求登录，再交给 `newEvaluation(source).checkResource(ctx, ref, operation)`：

1. 操作合法性校验 → `ErrInvalidPermission`。
2. `resolveAccess(ctx, ref)` 算出有效访问。
3. `accessAllows(access, operation)` 判断，不通过则 `ErrPermissionDenied`。

`resolveAccess` 的三步是：要求登录 → `loadResources` 载入事实 → `getResource` 取出 → `resolveResourceForUser` 算访问。

**`resolveResourceForUser` 是全包的核心算法，它有七道前置校验和三层分支。** 前置：

1. `TenantID`、`OwnerUserID` 非空且 `Ref` 合法——否则 `ErrInvalidPermission` 并说明「authorization facts 不完整」。
2. `Active` 为假 → `ErrResourceNotFound`（**软删的资源表现为不存在，而不是无权**）。
3. `OwnerOperations`、`TenantOperations`、`CreatorOperations` 里若有非法操作名 → `ErrInvalidPermission`。
4. `Visibility` 必须是 `private` / `tenant` / `shared` 三者之一，否则 `ErrInvalidPermission` 并带上实际值。

分支一：**用户就是 `OwnerUserID`，直接拿 `OwnerOperations`**，来源标为 `owner`。所有者判定在最前，不再看可见性。

分支二：**按可见性处理非所有者。**

- `private` → `ErrPermissionDenied`。
- `tenant` → 得出 `TenantRequirement`（为空则默认 `normal_member`），校验用户在该租户的成员关系。**成员关系不存在时把 `ErrMembershipNotFound` 转换成 `ErrPermissionDenied`**——这一层转换是有意的：对外不该区分「这个租户不存在」与「你不是这个租户的人」。校验过后，若用户是 `CreatedBy` 且 `CreatorOperations` 不为 nil，就用 `creator` 来源的那套操作；否则用 `TenantOperations`（为 nil 时回退到 `OwnerOperations`），来源标为 `tenant`。
- `shared` → 先查 `SharedWithUserIDs` 是否包含当前用户（命中即得 `shared` 来源）；再逐个 `SharedWithTenantIDs` 校验 `normal_member` 成员关系，**成员关系不存在的那个跳过、继续试下一个**，命中即返回。全都不中 → `ErrPermissionDenied`。

**三层分支里只有 `shared` 的失败是「继续下一个」。** 这是因为它是一个候选集合，而 `tenant` 是单一归属。「继续试」与「立即拒」的差别，就是「资源共享列表」与「唯一租户归属」的差别。

**`evaluation` 是单次检查的缓存作用域。** `newEvaluation` 建一个只有四个 map 的结构：`memberships`、`resources`、`resourceErr`，外加 `source`。它的作用很明确：**一次 `CheckResource` 内可能需要多次查同一个资源或同一份成员关系，缓存避免重复查询**。而每个公开方法都新建 `evaluation`，所以**缓存不跨调用**——不存在「上一个请求的权限结论影响下一个请求」的风险。

`loadResources` 是批量载入且有**负缓存**：先把所有缺失的 ref 标成 `ErrResourceNotFound`，再用查询结果逐个覆盖并删除错误标记。于是「数据库里查不到」这个负面结论也被缓存，同一批里重复请求不会反复查。

**`Access` 不只有操作列表，还有来源与权限位。** 三个字段各司其职：`Operations`（字符串操作名列表，`Allows` 线性扫描）、`Source`（四条规则中的哪一条生效）、`PermissionMask`（位掩码 `Read=1`、`Write=2`、`Manage=4`、`Owner=8`）。**两套表示并存**：操作列表用于粗粒度判定，掩码用于精细的资源授权表（注释说 UI 用同一套位布局）。`CheckResourcePermission` 走掩码路径，`CheckResource` 走操作列表路径，两者不互相推导。

**`Scope` / `QueryScope` 是「列出一批」的授权。** 列表与检索不能逐个资源检查再过滤（太慢），而是先由 `Source.ListResourceRefs` 拿到全部候选（**包括会被拒的**），再由 `Checker` 逐个授权，通过的进 `Scope.ResourceIDs`。被拒的与不存在的**静默跳过**，其他错误直接返回——这与 `ResolveAccesses` 的容错策略一致。

`QueryScope` 更进一步：若策略能把授权规则编译成 SQL 谓词（`queryScopePolicy`），就下推给数据库；否则退回 `Scope` 拿 ID 列表，再做成 `IN` 谓词。**「下推」与「枚举 ID」在语义上等价，只在性能与规模上限上不同**——后者在候选集很大时会爆。

**`WithTenantID` 拒绝切换已认证的租户。** 它只在 ctx 里没有租户时才写入，注释写明「preserves an authenticated Tenant so resource scope cannot switch tenants」。而「当前租户」与「操作租户」在实用上是两个概念（`ScopeQuery.TenantID` 的注释也强调它「不改变 ctx 携带的认证身份」）。

**`ResourceKind` 有 18 个值**，从 `dataset`、`document` 到 `model_provider`、`mcp_server`、`skill_space`。它是字符串类型而不是枚举，注释写着「identifies a resource family without coupling this package to business entities」——**权限包不认识业务实体，只知道「某个种类的某个 ID」**。这是这个包能被各层复用的前提。

## 关键取舍

**源码事实：** 判定入口是四个哨兵错误，`ErrMembershipNotFound` 在 `tenant` 可见性分支里被转换成 `ErrPermissionDenied`，而 `shared` 分支里同一个错误是「试下一个」的信号。

**源码事实：** `NewChecker(nil)` 得到的是「一切拒绝」的 checker，所有方法返回 `ErrSourceUnavailable`。

**源码事实：** 社区版 `CheckFeature` 只校验「登录 + 参数形状」，不含任何角色判断；版本差异由 `newPolicy` 的构建标签钩子决定。

**源码事实：** `Active` 为假的资源返回 `ErrResourceNotFound` 而非 `ErrPermissionDenied`。

**源码事实：** `evaluation` 的缓存只在一次检查内有效（每个公开方法新建），且 `loadResources` 对「查不到」也做负缓存。

**源码事实：** 可选能力用类型断言探测，四类批量/下推能力各有逐条退化路径；`ResolveAccesses` 的退化实现会跳过拒绝与不存在的资源、其余错误直接返回。

**源码事实：** 操作列表与权限掩码是两套并存的表示，分别服务 `CheckResource` 与 `CheckResourcePermission`。

**架构推论：** 「用户与租户从 ctx 取」让权限检查可以出现在任何深度，无需层层传参。代价是**它变成了隐式依赖**——一个函数有没有做权限检查，只能靠读实现或读调用链确认。这也是为什么这个包把方法名起得极直白（`CheckResource`、`CheckFeature`）：名字是唯一的显式痕迹。

**架构推论：** 把「资源事实」与「判定算法」分成 `Source` 与 `policy` 两个接口，是让权限逻辑可测试的关键。单元测试只需实现一个内存 `Source`，就能覆盖全部判定分支，不需要数据库。**判定逻辑与事实获取的分离度，直接决定了权限系统能被测到什么程度。**

**架构推论：** 「先取全部候选、再逐个授权」与「把规则下推成 SQL」是同一个问题的两个解法。前者的正确性容易验证（授权逻辑只有一份），后者的规模可扩展。本包**同时提供两条路并让策略选择**，这是一种诚实的折中：它不假装存在一个既简单又能扩展的答案。

**架构推论：** 错误分类的粒度是对外信息策略的一部分。`ErrResourceNotFound` 与 `ErrPermissionDenied` 分开，意味着调用方可以选择「告诉用户资源不存在」；而 `ErrMembershipNotFound` 在租户分支里被归并为拒绝，意味着「你不是成员」不会告诉用户「这个租户存在」。**同一个包内两种不同的信息披露策略，取决于场景对信息泄露的敏感程度。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/permission/types.go` | `ResourceKind` | 18 种资源族的字符串标识，与业务实体解耦 |
| `internal/permission/types.go` | `ResourceRef` | 种类 + ID 的资源引用 |
| `internal/permission/types.go` | `NewChecker` | 构造 checker；source 为空时得到全拒绝实例 |
| `internal/permission/types.go` | `Checker` | 对外门面：nil 安全 + 能力探测 + 退化路径 |
| `internal/permission/types.go` | `Source` | 事实来源三方法：成员关系、资源、候选列表 |
| `internal/permission/types.go` | `Resource` | 归一化的授权事实（租户、创建者、所有者、可见性、操作集） |
| `internal/permission/types.go` | `Access` | 有效访问：操作列表 + 来源 + 权限掩码 |
| `internal/permission/types.go` | `Scope` / `ScopeQuery` | 批量授权的候选集与查询边界 |
| `internal/permission/types.go` | `WithTenantID` | 只在无租户时写入，禁止切换已认证租户 |
| `internal/permission/policy.go` | `newPolicy` | 构建标签钩子优先，否则社区策略 |
| `internal/permission/policy.go` | `CheckFeature` | 社区版只做登录与形状校验 |
| `internal/permission/policy.go` | `CheckResource` | 真判定入口：登录 + 有效访问 + 操作判断 |
| `internal/permission/policy.go` | `ResolveAccess` | 载入事实并算出有效访问 |
| `internal/permission/policy.go` | `resolveResourceForUser` | 核心算法：所有者优先，再按三种可见性分支 |
| `internal/permission/policy.go` | `evaluation` | 单次检查内的资源与成员关系缓存 |
| `internal/permission/policy.go` | `loadResources` | 批量载入 + 对查不到做负缓存 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：permission/policy.go](https://github.com/infiniflow/ragflow/blob/main/internal/permission/policy.go)
- [ragflow 源码：permission/types.go](https://github.com/infiniflow/ragflow/blob/main/internal/permission/types.go)
- [NIST RBAC 参考模型](https://csrc.nist.gov/projects/role-based-access-control)

## 与主核 / AI 的关系

「可见性 + 角色 + 显式共享」这三个维度与主核里「表的所有者 + 模式权限 + 显式 GRANT」是同一套模型：所有者天然全权，同租户（同 schema）成员按角色获得默认权限，跨边界则需要显式授权。PostgreSQL 的 `GRANT` 表与这里的 `SharedWithUserIDs` / `SharedWithTenantIDs` 是同一角色的两个实现。

差别在于**求值时机**。主核在查询规划阶段就把权限折进计划（或者用行级安全策略下推成谓词），判定一次、影响整批行；这里的 `CheckResource` 是逐个资源求值，`Scope` 只是把「逐个」从业务层挪进了权限层。`QueryScope` 那条下推路径才真正对齐数据库的做法——**这也是它存在的原因：逐个求值在候选集大时会成为瓶颈，而数据库早就证明了规则下推是唯一可扩展的答案**。

`evaluation` 的「单次检查内缓存」与主核的语句级缓存同理：数据库在一个查询里缓存目录查找结果，跨查询则依赖计划缓存并有失效机制。这里的差别是**它不做跨请求缓存**——权限变更必须立刻生效，一致性优先于性能。这是权限系统与查询缓存的一个本质区别：**缓存的收益是可选的，缓存的过期是致命的。**

对 agent / RAG 工程的实际含义有四条。其一，权限检查依赖 `ctx` 里的身份，任何绕过中间件直接调内部服务的路径都会得到 `ErrUnauthenticated`——这是保护也是陷阱，排查 401 时先看 ctx 链路。其二，社区版 `CheckFeature` 恒通过，功能级限制只在企业版存在，不要把社区版的「能跑」当成契约。其三，软删资源表现为 404 而不是 403，前端据此决定是否从列表里移除。其四，`Scope` 会枚举全部候选再过滤，知识库与文档数量很大时应改用 `QueryScope` 走数据库下推。
