---
project: dify
feature: rbac-and-tools-service
title: 权限与工具管理服务
page_type: feature
schema_version: 1
refs:
  - path: api/services/rbac/builtin.py
    symbols: [builtin_member_roles, builtin_permissions]
  - path: api/services/rbac/contracts.py
    symbols: [Paginated, MembersInRole]
  - path: api/services/tools/builtin_tools_manage_service.py
    symbols: [BuiltinToolManageService]
  - path: api/services/tools/mcp_tools_manage_service.py
    symbols: [ReconnectResult, OAuthDataType]
---

# 权限与工具管理服务

## 一句话职责

四个文件解决同一个问题的两面：**「谁被允许做什么」与「第三方凭据怎么存」**——`rbac` 两个文件把「老的五种租户角色」投影成新的 RBAC 契约（角色、权限键列表、分页壳），`tools` 两个文件把内置工具与 MCP 服务的凭据加密落库、把网络校验挪出数据库会话、并把 OAuth 的四种中间态写成枚举。

## 机制是什么

这里有三条互相独立的机制，各自都值得单独看。

**第一条：权限模型在迁移，而迁移期间必须同时支持两套。**

`builtin.py` 的模块 docstring 一句话说清了它的定位：

> Permission projections for the built-in workspace roles.

而紧随其后的常量注释给出了更硬的约束：

> Fallback permission snapshots for legacy Dify tenant roles when external RBAC is disabled.
> Keep these keys aligned with langgenius/rbac's built-in workspace roles and access policies.

**「fallback snapshot」+「keep aligned with 外部仓」** ——这两句合起来定义了它的角色：**它不是权限的权威来源，而是一份必须与外部 RBAC 仓保持一致的镜像**。于是这里出现了**权限键的字面量清单**（`skill.view`、`dataset.create_and_management`…），共十六组。

**第二条：网络 I/O 必须搬出数据库会话，而这件事被做成了一个显式的两步 API。**

`mcp_tools_manage_service.py` 里有一对方法：`get_provider_for_url_validation`（读库，返回一个纯数据对象）与 `validate_server_url_standalone`（纯网络，不碰 session）。后者的 docstring 是全篇最硬的约束：

> This method performs network operations and MUST be called OUTSIDE of any database session to avoid holding locks during network I/O.

**第三条：「凭据」在存储里永远是加密后的 JSON 字符串，而读出来要经过「解密 + 掩码」两道。**

```python
encrypter, _ = BuiltinToolManageService.create_tool_encrypter(
    tenant_id, provider, provider.provider, provider_controller
)
decrypted = encrypter.mask_plugin_credentials(encrypter.decrypt(provider.credentials))
```

**注意顺序是 `decrypt` 然后 `mask`** ——**先解密再掩码，而不是用掩码代替解密**。这意味着明文在进程中短暂存在，而返回给前端的永远是掩码后的值。这是一个有意的选择（掩码需要知道真实值的长度/前缀），但它把「明文不上网络」的保证落在掩码函数上而不是类型上。

## 怎么实现的

**权限清单的十六个常量是按「作用域 × 角色」手工展开的矩阵。**

```python
_BUILTIN_WORKSPACE_OWNER_KEYS: list[str] = [
    "skill.view", "skill.edit", "skill.publish", "skill.delete",
    "workspace.member.manage", "workspace.role.manage",
    "data_source.manage", "api_extension.manage", "customization.manage",
    "plugin.install", "plugin.plugin_preferences", "plugin.model_config", "plugin.delete", "plugin.debug",
    "credential.use", "credential.create", "credential.manage",
    "app.acl.preview", "app_library.access", "app.create_and_management", "app.tag.manage",
    "dataset.acl.preview", "dataset.create_and_management", "dataset.tag.manage",
    "dataset.external.connect", "dataset.api_key.manage",
    "snippets.create_and_modify", "snippets.management",
    "tool.manage", "mcp.manage",
    "agent.create", "agent.tag.manage", "agent.acl.preview", "agent.acl.access_point_view",
]
```

**四组命名（`workspace` / `app` / `dataset` / `agent`）与五种角色（owner / admin / editor / normal / dataset_operator）交叉，共十六个列表。**

**关键观察：这些列表是逐条抄写的，而不是用「包含关系」推导的。** 对照 `OWNER` 与 `ADMIN` 两份列表，后者比前者少了三条：

| 只在 owner 里 | 说明 |
|---|---|
| `app.acl.preview` | 应用 ACL 预览 |
| `dataset.acl.preview` | 数据集 ACL 预览 |
| `dataset.create_and_management`（admin 也有）| — |

实际差集是 `app.acl.preview` 与 `dataset.acl.preview` 两条（三处 `agent.acl.*` 两者都有）。**这个差集是靠人眼对照列表才能发现的**，而代码里没有任何声明表达「admin 是 owner 减去 X」。

**这就是「权限矩阵用字面量展开」的核心代价**：矩阵的**语义**（谁能做 A、谁不能做 B）不可查询，只能逐项比。改一条权限要改多个列表，而漏改一处不会报错——只会让某个角色多一个或少一个权限。

**`_BUILTIN_MY_PERMISSIONS` 是唯一一处把这些列表组装起来的映射。**

```python
_BUILTIN_MY_PERMISSIONS: dict[TenantAccountRole, dict[str, list[str]]] = {
    TenantAccountRole.OWNER: {
        "workspace": _BUILTIN_WORKSPACE_OWNER_KEYS,
        "app": _BUILTIN_APP_OWNER_KEYS,
        "dataset": _BUILTIN_DATASET_OWNER_KEYS,
        "agent": _BUILTIN_AGENT_FULL_ACCESS_KEYS,
    },
    ...
    TenantAccountRole.NORMAL: {
        "workspace": _BUILTIN_WORKSPACE_NORMAL_KEYS,
        "app": _BUILTIN_APP_NORMAL_KEYS,
        "agent": _BUILTIN_AGENT_PREVIEW_KEYS,
    },
    ...
}
```

**注意 `NORMAL` 的字典里没有 `"dataset"` 键。**

**这不是遗漏，而是「普通成员没有默认数据集权限」的表达** ——而它靠的是**键缺失 + `.get("dataset", [])` 的兜底**：

```python
def builtin_permissions(role: TenantAccountRole | str | None) -> MyPermissionsResponse:
    try:
        permissions = _BUILTIN_MY_PERMISSIONS.get(TenantAccountRole(role), {}) if role else {}
    except ValueError:
        permissions = {}
    return MyPermissionsResponse(
        workspace=WorkspacePermissionSnapshot(permission_keys=list(permissions.get("workspace", []))),
        app=ResourcePermissionSnapshot(default_permission_keys=list(permissions.get("app", []))),
        dataset=ResourcePermissionSnapshot(default_permission_keys=list(permissions.get("dataset", []))),
        agent=ResourcePermissionSnapshot(default_permission_keys=list(permissions.get("agent", []))),
    )
```

**四处：**

1. **`except ValueError` 接住 `TenantAccountRole(role)` 的非法值**，退化成空权限——**fail-closed**（未知角色什么都不能做）。这与 h01 里「RBAC 开启但无可见集 → 返回空」是同一个方向。
2. **四处 `.get(key, [])`** ——键缺失与空列表在这里等价。**于是「NORMAL 没有 dataset 权限」与「NORMAL 的 dataset 权限是空列表」在返回值上不可区分**，这削弱了「用键缺失表达策略」这个手法的可读性。
3. **`if role else {}`** ——`role` 为 `None` 时直接空权限，不做转换。
4. **`list(...)` 复制**——防止调用方改动共享常量列表。**这个防御是必需的**：`_BUILTIN_WORKSPACE_OWNER_KEYS` 是模块级可变列表，若直接返回引用，一个调用方的 `.append()` 会污染所有后续请求。

**`builtin_role_permission_keys` 用 `dict.fromkeys` 做保序去重。**

```python
def builtin_role_permission_keys(role: TenantAccountRole) -> list[str]:
    permissions = _BUILTIN_MY_PERMISSIONS.get(role, {})
    return list(
        dict.fromkeys(
            [
                *permissions.get("workspace", []),
                *permissions.get("app", []),
                *permissions.get("dataset", []),
                *permissions.get("agent", []),
            ]
        )
    )
```

**`dict.fromkeys(...)` 是 Python 里「保序去重」的惯用法** ——因为字典保持插入顺序且键唯一。这里需要去重是因为**同一个权限键可能同时出现在多个作用域列表里**（例如 `app.acl.preview` 在 workspace 组与 app 组都可能出现）。

**注意展开顺序固定为 workspace → app → dataset → agent**，所以去重后保留的是「第一次出现的位置」。这决定了返回列表的顺序，而顺序在展示上是可见的。

**`builtin_member_roles` 是「把一个老角色包装成 RBAC 角色对象」的适配器。**

```python
def builtin_member_roles(
    tenant_id: str, member_account_id: str, role: TenantAccountRole | str | None
) -> MemberRolesResponse:
    if not role:
        return MemberRolesResponse(account_id=member_account_id, roles=[])

    tenant_role = TenantAccountRole(role)
    role_value = tenant_role.value
    return MemberRolesResponse(
        account_id=member_account_id,
        roles=[
            RBACRole(
                id=role_value,
                name=role_value,
                description="",
                is_builtin=True,
                type="",
                permission_keys=builtin_role_permission_keys(tenant_role),
                role_tag="owner" if tenant_role == TenantAccountRole.OWNER else role_value,
                tenant_id=tenant_id,
            )
        ],
    )
```

**五处：**

1. **`id` / `name` 都用 `role_value`** ——因为老角色没有独立的 id 与显示名，只有枚举值。`description=""`。
2. **`is_builtin=True`、`type=""`** ——`type` 为空是被允许的（`RBACRoleType` 只有一个值 `"workspace"`，这里不用它）。
3. **`role_tag` 单独处理 owner** ——`"owner" if tenant_role == OWNER else role_value`。**对一个枚举值等于它的字符串这件事，这个条件判断是冗余的**（`OWNER.value` 就是 `"owner"`）。它存在的可能理由是「未来 owner 的 role_tag 要变成别的值」。
4. **无角色时返回空 `roles` 列表，而不是报错** ——一个没有角色的成员是合法状态（比如刚被邀请但还没分配角色）。
5. **返回的是「单元素列表」而不是单对象** ——契约允许一个成员有多个角色，而这里老模型只有一个。

**`_BUILTIN_ROLE_PERMISSION_KEYS` 与 `_BUILTIN_MY_PERMISSIONS` 是同一份数据的两种形状，而注释说清了它们的区别。**

```python
_BUILTIN_ROLE_PERMISSION_KEYS: dict[str, list[str]] = {
    # This is a compatibility projection from the pre-RBAC workspace roles into
    # the matrix documented in "Permissions 2.0". It intentionally
    # models the product-facing role surface for the new RBAC UI instead of the
    # legacy backend's exact hard-authorization checks.
    "owner": [
        *_BUILTIN_WORKSPACE_OWNER_KEYS,
        *_BUILTIN_APP_OWNER_KEYS,
        *_BUILTIN_DATASET_OWNER_KEYS,
    ],
    "normal": [
        *_BUILTIN_WORKSPACE_NORMAL_KEYS,
        *_BUILTIN_APP_NORMAL_KEYS,
    ],
    ...
}
```

**四处值得指出：**

1. **它按角色（字符串键）索引，而 `_BUILTIN_MY_PERMISSIONS` 按枚举索引且分作用域。** 两份数据的来源相同（同一批常量），但**组装方式不同**：一个按作用域分组（给 `/rbac/my-permissions` 这种要分类展示的接口），一个平铺成单列表（给 `/rbac/roles` 这种要总清单的接口）。
2. **它用 `*` 展开而不去重** ——所以同一权限键可能出现两次（与 `builtin_role_permission_keys` 的 `dict.fromkeys` 不同）。**两个函数对同一批常量给出「去重」与「不去重」两种结果**，而调用方看不到这个差别。
3. **`"normal"` 与 `"dataset_operator"` 没有展开 `_BUILTIN_DATASET_*` / `_BUILTIN_APP_*` 的对应项** ——`normal` 只有 workspace + app（没有 dataset，与 `_BUILTIN_MY_PERMISSIONS` 的键缺失一致），而 `dataset_operator` 只有 workspace + dataset（没有 app，**与 `_BUILTIN_MY_PERMISSIONS` 里它有 `"app": _BUILTIN_APP_DATASET_OPERATOR_KEYS` 矛盾**）。

**这是一处真实的不一致**：`dataset_operator` 在两个映射里的能力集不同。哪个是「正确」的从代码看不出来，而注释只解释了「这是给新 UI 用的投影」，没有解释为什么两处不同。

4. **注释里那句「It intentionally models the product-facing role surface for the new RBAC UI instead of the legacy backend's exact hard-authorization checks」** ——**这是整页最重要的一句**：它明说这份数据**不是**后端的真实授权依据，而是**UI 展示用的产品语义**。**也就是说「这个角色在界面上显示有哪些权限」与「后端真的拦不拦」是两件事**，而这一页的这个字典服务的是前者。

**`contracts.py` 的模型基类只有两个配置项。**

```python
class _RBACModel(BaseModel):
    model_config = ConfigDict(populate_by_name=True, extra="ignore")
```

**`populate_by_name=True` 允许用字段名或别名填充**（配合 `alias` 使用，处理外部 API 的命名差异）；**`extra="ignore"` 让未知字段被静默丢弃**。后者对「接收外部 RBAC 服务的响应」是必需的（对方可能加字段），但也意味着**拼错的字段名不会报错**。

**分页壳是泛型 + 可选分页信息。**

```python
class Pagination(_RBACModel):
    total_count: int = 0
    per_page: int = 0
    current_page: int = 0
    total_pages: int = 0


class Paginated[T](_RBACModel):
    data: list[T] = Field(default_factory=list)
    pagination: Pagination | None = None
```

**三处：**

1. **四个字段全默认 0** ——所以一个空 `Pagination()` 是合法的（`0/0/0/0`），而不是「未设置」。
2. **`pagination: Pagination | None = None`** ——**分页信息是可选的整体**。于是「不分页」用 `None` 表达，而「分页但为空」用 `Pagination(total_count=0, ...)` 表达。这两种状态在 JSON 里可区分（`null` vs 对象）。
3. **`Paginated[T]` 用的是 PEP 695 的泛型语法**（`class Paginated[T]`）而不是 `Generic[T]` ——说明项目的最低 Python 版本足够新（3.12+）。

**`MembersInRole` 是两个字符串的极简壳。**

```python
class MembersInRole(_RBACModel):
    account_id: str = ""
    account_name: str = ""
```

**两个字段都有默认空串** ——于是 `MembersInRole()` 合法。这个设计让「部分构造」（只有 id 没有名字）成为可能，但也意味着**空 id 与「id 是空串」不可区分**。对一个「成员列表项」来说这是一个宽松的选择，风险在于聚合时容易把空项混进结果。

**`RBACResourceType` 用 `StrEnum` + 一个 `route` 属性把「类型」与「URL 路径」绑在一起。**

```python
@dataclass(frozen=True)
class _ResourceAccessRoute:
    segment: str
    id_param: str


class RBACResourceType(StrEnum):
    """Resource types understood by access policies."""

    APP = "app"
    DATASET = "dataset"
    AGENT = "agent"

    @property
    def route(self) -> _ResourceAccessRoute:
        routes = {
            RBACResourceType.APP: _ResourceAccessRoute("apps", "app_id"),
            RBACResourceType.DATASET: _ResourceAccessRoute("datasets", "dataset_id"),
            RBACResourceType.AGENT: _ResourceAccessRoute("agents", "agent_id"),
        }
        return routes[self]
```

**四处：**

1. **`StrEnum` 而不是 `Enum`** ——成员可直接当字符串用，符合本仓 AGENTS.md 里「Use `StrEnum` instead of `Literal[...]`」的规定。
2. **`route` 是一个**属性里的字典查找** ——每次访问都重建这个三元素字典。对一个属性来说这是一个小浪费，但换来了「把映射写在类型旁边」的可读性。
3. **`routes[self]` 会在未知成员上 `KeyError`** ——而 `StrEnum` 的成员是封闭的，所以这只在新加枚举值而忘了加 route 时发生。**那正是应该报错的时刻，所以这个设计是对的。**
4. **`_ResourceAccessRoute` 是 `frozen=True` 的 dataclass** ——不可变值对象，可安全共享。`segment` 是 URL 段（复数），`id_param` 是路径参数名。**这两个名字合起来定义了「这个资源在 REST 里长什么样」**，也就是把「权限资源类型」与「API 路由」耦合在一起。

**`RBACRoleType` 只剩一个值，而 docstring 解释了原因。**

```python
class RBACRoleType(StrEnum):
    """The only concrete role type after the access-policy refactor."""

    WORKSPACE = "workspace"
```

**「after the access-policy refactor」** ——说明曾经有更多类型（可能是 `app` / `dataset` 级的角色），重构后统一成 workspace 级 + 资源级访问策略（`AccessPolicy`）。

**这解释了 `builtin_member_roles` 里 `type=""` 为什么是空的**：在一个只剩一种类型的枚举里，「不填」与「填 `workspace`」的差别只是契约的历史包袱。

**`mcp_tools_manage_service.py` 的常量和四个模型共同定义了一个「重连」的状态机。**

```python
UNCHANGED_SERVER_URL_PLACEHOLDER = "[__HIDDEN__]"
CLIENT_NAME = "Dify"
EMPTY_TOOLS_JSON = "[]"
EMPTY_CREDENTIALS_JSON = "{}"
```

**`"[__HIDDEN__]"` 是一个**哨兵占位符**：前端拿到加密后的 server_url，回传时若没改就回传这个占位符。服务端据此跳过校验。**这是「掩码字段的回传约定」**，而它用一个形似占位符的字符串而不是空串——因为空串是合法的 URL 值（虽然不合法）。

```python
class OAuthDataType(StrEnum):
    """Types of OAuth data that can be saved."""

    TOKENS = "tokens"
    CLIENT_INFO = "client_info"
    CODE_VERIFIER = "code_verifier"
    MIXED = "mixed"
```

**四值把 OAuth 流程的中间产物分成三类 + 一个混合态。** 而 `MIXED` 的存在是为了「一次调用里同时给 tokens 与 client_info」这种情况：

```python
authed = (
    data_type == OAuthDataType.TOKENS or (data_type == OAuthDataType.MIXED and "access_token" in data) or None
)
```

**这行是三支 `or` 的短路表达式，而末尾的 `or None` 把 `False` 变成了 `None`。**

**走一遍真值表：**

- `TOKENS` → `True`
- `MIXED` 且含 `access_token` → `True`
- `MIXED` 但无 `access_token` → `False or False or None` = `None`
- `CLIENT_INFO` / `CODE_VERIFIER` → `False or False or None` = `None`

**于是 `authed` 的类型是 `bool | None`，而 `None` 是 `False` 与 `False` 的合并结果** ——这在逻辑上是巧合（`False or None` 得 `None`），而它的语义是「不改变认证状态」。**这个写法依赖 Python 的 `or` 返回操作数而不是布尔的特性**，可读性很低。

**`ReconnectResult` 把「连上了吗」与「拿到了什么」放在一起。**

```python
class ReconnectResult(BaseModel):
    """Result of reconnecting to an MCP provider"""

    authed: bool = Field(description="Whether the provider is authenticated")
    tools: str = Field(description="JSON string of tool list")
    encrypted_credentials: str = Field(description="JSON string of encrypted credentials")
```

**三处：**

1. **`tools` 是 **JSON 字符串**而不是结构化列表** ——因为它要原样写入数据库的 text 列。**这个选择让「服务层返回的东西」与「存储的形状」耦合**，代价是任何消费者都要再解一次。
2. **`encrypted_credentials` 也是字符串**，且 `Field` 的 description 明确说是「encrypted」。
3. **`authed=False` 时三个字段仍有值**（空列表 `"[]"`、空对象 `"{}"`），而不是 `None`：

```python
except MCPAuthError:
    return ReconnectResult(authed=False, tools=EMPTY_TOOLS_JSON, encrypted_credentials=EMPTY_CREDENTIALS_JSON)
```

**「未认证」用 `authed=False` + 空串表达，而不是 `None` 整个对象** ——于是调用方不必判空。

**`ServerUrlValidationResult` 是「是否需要校验」的四态压缩，而 `should_update_server_url` 是一个派生属性。**

```python
class ServerUrlValidationResult(BaseModel):
    """Result of server URL validation check"""

    needs_validation: bool
    validation_passed: bool = False
    reconnect_result: ReconnectResult | None = None
    encrypted_server_url: str | None = None
    server_url_hash: str | None = None

    @property
    def should_update_server_url(self) -> bool:
        """Check if server URL should be updated based on validation result"""
        return self.needs_validation and self.validation_passed and self.reconnect_result is not None
```

**五个字段合起来表达四种情况：**

| `needs_validation` | `encrypted_server_url` | 含义 |
|---|---|---|
| `False` | `None` | URL 是占位符（没改） |
| `False` | 有值 | URL 没变（hash 相同），但仍返回加密值 |
| `True` | 有值 + `reconnect_result` | URL 变了且连上了 |
| `True` | 有值 + 无 `reconnect_result` | **在实现里不存在**（见下） |

**而 `should_update_server_url` 要求三者同时成立** ——它是「该不该写库」的唯一判据。

**`validate_server_url_standalone` 的实现只产生了前三种组合。**

```python
if UNCHANGED_SERVER_URL_PLACEHOLDER in new_server_url:
    return ServerUrlValidationResult(needs_validation=False)

parsed = urlparse(new_server_url)
if not all([parsed.scheme, parsed.netloc]) or parsed.scheme not in ["http", "https"]:
    raise ValueError("Server URL is not valid.")

encrypted_server_url = encrypter.encrypt_token(tenant_id, new_server_url)
new_server_url_hash = hashlib.sha256(new_server_url.encode()).hexdigest()

if new_server_url_hash == validation_data.current_server_url_hash:
    return ServerUrlValidationResult(
        needs_validation=False,
        encrypted_server_url=encrypted_server_url,
        server_url_hash=new_server_url_hash,
    )

reconnect_result = MCPToolManageService._reconnect_with_url(...)
return ServerUrlValidationResult(
    needs_validation=True,
    validation_passed=True,
    reconnect_result=reconnect_result,
    encrypted_server_url=encrypted_server_url,
    server_url_hash=new_server_url_hash,
)
```

**六处：**

1. **`UNCHANGED_SERVER_URL_PLACEHOLDER in new_server_url` 用的是子串判断而不是相等** ——一个恰好包含该字面量的合法 URL 会被误判为「没改」。实际不会发生（URL 不会含 `[__HIDDEN__]`），但这是一个宽松的比较。
2. **URL 格式校验只查 scheme 与 netloc** ——不查路径、不查是否 SSRF-危险（那由 `MCPClient` 内部处理）。
3. **加密与哈希**在**所有**返回路径上都被计算（除了占位符那条），所以「没改 URL」也会返回加密值——注释里的理由可从代码推出：调用方可能仍需要它。
4. **hash 用 `sha256(new_server_url.encode()).hexdigest()`** ——用来判断「是否真的变了」。用哈希而不是直接比明文，是因为库里存的是哈希（`server_url_hash` 列），于是比较不需要解密。
5. **`validation_passed=True` 是硬编码的** ——因为 `_reconnect_with_url` 抛异常就代表失败（第 6 点）。**所以 `validation_passed` 永远是 `True` 或整个调用抛错**，这个字段因此是冗余的。
6. **失败的两条路径是「抛异常」而不是「返回 `passed=False`」**：

```python
try:
    with MCPClient(server_url=..., headers=..., timeout=..., sse_read_timeout=...) as mcp_client:
        tools = mcp_client.list_tools()
        tools_payload = []
        for t in tools:
            d = t.model_dump()
            if d.get("description") is None:
                d["description"] = ""
            tools_payload.append(d)
        return ReconnectResult(authed=True, tools=json.dumps(tools_payload), encrypted_credentials=EMPTY_CREDENTIALS_JSON)
except MCPAuthError:
    return ReconnectResult(authed=False, tools=EMPTY_TOOLS_JSON, encrypted_credentials=EMPTY_CREDENTIALS_JSON)
except MCPError as e:
    raise ValueError(f"Failed to re-connect MCP server: {e}") from e
```

**这是三支分派：成功 → `authed=True` + 工具列表；需要认证 → `authed=False` + 空列表（**不是失败**，是一个合法的中间态：用户还需要走 OAuth）；其它 MCP 错误 → 抛 `ValueError`。**

**「认证错误」与「连接错误」被区别对待，这个区分是 MCP 协议的核心语义**：前者意味着「这个服务需要先授权」，后者意味着「连不上」。而 `authed=False` 的结果会被写入库（`mcp_provider.authed = reconnect_result.authed`），所以「需要授权」是一个**持久化的正常状态**。

**`update_provider` 接受一个可选的预校验结果，从而避免在会话内做网络 I/O。**

```python
def update_provider(self, *, tenant_id, provider_id, name, server_url, icon, icon_type,
                    icon_background, server_identifier, headers=None, configuration,
                    authentication=None, validation_result: ServerUrlValidationResult | None = None,
                    identity_mode: IdentityMode = IdentityMode.OFF) -> None:
    """
    Update an MCP provider.

    Args:
        validation_result: Pre-validation result from validate_server_url_standalone.
                          If provided and contains reconnect_result, it will be used
                          instead of performing network operations.
    """
```

**这一段就是「两步 API」的落点**：控制器先调 `get_provider_for_url_validation`（读库）→ 关闭会话 → 调 `validate_server_url_standalone`（网络）→ 再开会话调 `update_provider`（写库，带 `validation_result`）。

**代价是控制器必须记住这个顺序**，而服务层只能靠 docstring 提醒。若控制器忘了传 `validation_result`，`update_provider` **不会报错**——它只是不更新 URL（`if validation_result and validation_result.encrypted_server_url` 为假）。**于是「URL 改了但没生效」是一个静默失败**。

**`update_provider` 的冲突检测有两层。**

```python
if name != mcp_provider.name:
    stmt = select(MCPToolProvider).where(
        MCPToolProvider.tenant_id == tenant_id,
        MCPToolProvider.name == name,
        MCPToolProvider.id != provider_id,
    )
    existing_provider = self._session.scalar(stmt)
    if existing_provider:
        raise ValueError(f"MCP tool {name} already exists")
```

**第一层是显式的「先查后报错」**（仅当名字变了才查，省一次查询）。**第二层是数据库的 `IntegrityError`：**

```python
except IntegrityError as e:
    self._handle_integrity_error(e, name, server_url, server_identifier)
```

**两层并存说明显式检查是**体验优化**，`IntegrityError` 才是**正确性保证**。** 这与 h01 里「知识库创建只有先查后插」形成对照——这里多一层，那里少一层。而 `_check_provider_exists` 的检查更宽，它同时按 `name` / `server_url_hash` / `server_identifier` 三个字段中的**任意一个**判重：

```python
stmt = select(MCPToolProvider).where(
    MCPToolProvider.tenant_id == tenant_id,
    or_(
        MCPToolProvider.name == name,
        MCPToolProvider.server_url_hash == server_url_hash,
        MCPToolProvider.server_identifier == server_identifier,
    ),
)
```

**`or_` 三条件意味着「三个字段各自要唯一」** ——所以一个租户下不能有两个 MCP 提供方指向同一个 URL。这是合理的（否则同一服务会重复注册），但它让错误消息必须区分命中的是哪个字段，于是有那三条 `if` 逐项判断。

**`list_providers` 里有一处「批量查用户避免 N+1」的显式实现。**

```python
user_ids = {provider.user_id for provider in mcp_providers}
users = self._session.scalars(select(Account).where(Account.id.in_(user_ids))).all()
user_name_map = {user.id: user.name for user in users}

return [
    ToolTransformService.mcp_provider_to_user_provider(
        provider, user_name=user_name_map.get(provider.user_id), include_sensitive=include_sensitive,
    )
    for provider in mcp_providers
]
```

**注释「Batch query all users to avoid N+1 problem」** ——这是一个**装配层（transform service）不该自己查库**的连带后果：因为 `mcp_provider_to_user_provider` 只做纯转换（收 `user_name` 参数），调用方必须自己先把数据凑齐。**这是「转换函数保持纯函数」的正确代价**：一次性查完放在调用方，比让转换函数内部懒加载要可预测得多。

而 `include_sensitive` 参数也值得记：

```python
include_sensitive: bool = True
"""..."""
# If False, skip expensive decryption operations (default: True for backward compatibility)
```

**「默认 `True` 是为向后兼容」** ——即新参数默认保持老行为。而它的语义是「跳过昂贵的解密」，说明**解密是这条列表路径上的主要开销**。一个「列表页不需要看凭据」的调用方传 `False` 能省掉全部解密。

**这一点与 `builtin_tools_manage_service.py` 里的凭据读取形成对照**：那里**总是**解密 + 掩码，没有开关。

```python
decrypted = encrypter.mask_plugin_credentials(encrypter.decrypt(provider.credentials))
```

**同一个仓库里两种做法**：MCP 侧给了跳过解密的开关，内置工具侧没有。后者在「有几十个凭据的列表页」上会做几十次解密，且每次都要构造一个 encrypter（`create_tool_encrypter` 里还带一个缓存对象）。

**`get_builtin_tool_provider_credentials` 的 docstring 有三段，每段都在解释一个非显然的规则。**

```python
"""
get builtin tool provider credentials, filtered by visibility.

``user`` is used to filter the result list by per-credential visibility
(only_me / all_team_members / legacy partial_members). When ``None`` the
query returns every credential for the tenant — meant for internal /
background callers that don't act on behalf of a specific user.

``include_credential_ids`` lets callers request specific credential IDs that should be
returned even if the visibility filter would normally hide them (e.g. an only_me credential
owned by another member which the current workflow/agent node still references). Those
rows are marked with ``from_other_member=True`` so the UI can render them as
borrowed-from-teammate (selectable but not editable).
"""
```

**三段各对应一条规则：**

1. **`user=None` 时返回全部** ——「内部/后台调用方」的语义。**这是一个「参数为 None 含义反转」的设计**：不是「没有用户所以不能判断」，而是「没有用户所以不按用户过滤」。与 h01 里 `get_datasets` 的「无 user + RBAC 开 → `sa.false()`」**方向相反**。

**同一个代码库里，同一个「没有当前用户」的情况，一处 fail-closed（返回空）一处 fail-open（返回全部）。** 判据是「这个方法的调用方是谁」：`get_datasets` 面向外部请求，`get_builtin_tool_provider_credentials` 主要面向内部装配。

2. **`include_credential_ids` 是「可见性过滤的例外通道」** ——让「工作流引用了别人的私有凭据」这件事仍然能读到该凭据。**这是「引用完整性优先于可见性」** 的一次让步：不这么做，用户分享一个工作流给它人后，那个工作流会因为引用不了原作者的私有凭据而不可用。
3. **例外行带 `from_other_member=True` 标记**，用于 UI 区分「可选中但不可编辑」。**把「为什么这一行本该被过滤」编码成一个布尔字段** ——这是一个很好的做法：不隐藏例外，而是标注例外。

**`get_builtin_provider` 里有一段「靠异常做降级」的查询。**

```python
with Session(db.engine, autoflush=False) as session:
    try:
        full_provider_name = provider_name
        provider_id_entity = ToolProviderID(provider_name)
        provider_name = provider_id_entity.provider_name

        if provider_id_entity.organization != "langgenius":
            provider = session.scalar(...)
        else:
            provider = session.scalar(...)

        if provider is None:
            return None

        provider.provider = ToolProviderID(provider.provider).to_string()
        return provider
    except Exception:
        # it's an old provider without organization
        return session.scalar(
            select(BuiltinToolProvider)
            .where(BuiltinToolProvider.tenant_id == tenant_id, BuiltinToolProvider.provider == provider_name)
            .order_by(BuiltinToolProvider.is_default.desc(), BuiltinToolProvider.created_at.asc())
            .limit(1)
        )
```

**五处：**

1. **`except Exception` 捕获一切** ——注释说这是为了「老 provider 没有 organization」。但 `ToolProviderID(...)` 可能因为很多原因抛异常（名字里含 `@` 但格式不对、空串、编码问题），**全部会被当成「老格式」处理**。这是一个**过宽的捕获**。
2. **降级路径里用的 `provider_name` 可能已经被上一行覆盖过** ——`provider_name = provider_id_entity.provider_name` 若成功执行，降级查询用的是**拆出来的短名**而不是原始 `full_provider_name`。若 `ToolProviderID(...)` 在**赋值之前**抛异常，`provider_name` 还是原始值。**两种情况的查询条件不同，而代码没有区分。**
3. **`ToolProviderID(provider.provider).to_string()` 在返回前改写 provider 字段** ——把存储格式转成对外格式。这是一个「读时转换」而不是「存时转换」的选择，代价是每次读都要转。
4. **排序是 `is_default.desc(), created_at.asc()`** ——注释分别说明「default=True first」与「oldest first」。docstring 也写了这两条规则（1. 默认优先；2. 没有默认则最老的）。
5. **它自己开 session（`with Session(db.engine, ...)`），而同类里别的静态方法收 `session` 参数** ——`generate_builtin_tool_provider_name` 收 `*, session: Session`。**同一个类里两种 session 获取方式并存**。

**`save_custom_oauth_client_params` 里的「双 None 早退」是一个明确的契约。**

```python
if client_params is None and enable_oauth_custom_client is None:
    return {"result": "success"}
```

**「两个参数都没给 → 什么都不做，返回成功」** ——这是一个 PATCH-like 语义的显式实现。而它返回 `{"result": "success"}` 这样的字面量而不是一个类型化对象，说明这个方法的返回值没有被消费方严格约束。

**对照 `update_provider` 里那段关于 `identity_mode` 的注释，能看到同一层里另一种「契约在控制器」的做法：**

```python
# Update user-identity forwarding mode. The controller has already
# resolved "leave unchanged" and applied the Enterprise-edition gate,
# so this is always a concrete, vetted value.
mcp_provider.identity_mode = identity_mode
```

**「控制器已经解析了『保持不变』并应用了企业版门禁」** ——于是服务层直接赋值，不做任何判断。**这是一个「把复杂性推给调用方」的取舍**：服务层简单了，但**任何新的调用方都必须重新实现那两件事**，而它只能从这条注释得知。

## 关键取舍

**源码事实：** 权限键以十六个字面量清单手工展开（四个作用域 × 五个角色），`_BUILTIN_MY_PERMISSIONS` 按角色→作用域索引，`_BUILTIN_ROLE_PERMISSION_KEYS` 按角色平铺；两者基于同一批常量但组装不同。注释明确说后者「intentionally models the product-facing role surface for the new RBAC UI instead of the legacy backend's exact hard-authorization checks」。

**源码事实：** `dataset_operator` 在两个映射里的能力集不一致：`_BUILTIN_MY_PERMISSIONS` 它有 `"app": _BUILTIN_APP_DATASET_OPERATOR_KEYS`，而 `_BUILTIN_ROLE_PERMISSION_KEYS` 只展开 workspace + dataset。

**源码事实：** `builtin_permissions` 对非法角色 `except ValueError` 后退化成空权限（fail-closed），并对四处 `.get(key, [])` 兜底；`NORMAL` 在 `_BUILTIN_MY_PERMISSIONS` 里没有 `"dataset"` 键。所有返回都用 `list(...)` 复制以隔离模块级可变列表。

**源码事实：** `builtin_role_permission_keys` 用 `dict.fromkeys` 保序去重（展开顺序 workspace→app→dataset→agent），而 `_BUILTIN_ROLE_PERMISSION_KEYS` 用 `*` 展开**不去重**，同一权限键可能重复出现。

**源码事实：** `builtin_member_roles` 把老角色包成 `RBACRole`（`id`/`name` 都用枚举值、`description=""`、`is_builtin=True`、`type=""`），`role_tag` 对 OWNER 单独判断（而 `OWNER.value` 本就等于 `"owner"`）；无角色时返回空 `roles` 列表。

**源码事实：** 契约模型基类 `_RBACModel` 用 `populate_by_name=True, extra="ignore"`（未知字段静默丢弃）；`Paginated[T]` 用 PEP 695 泛型语法；`pagination` 可选（`None` = 不分页）；`MembersInRole` 两字段默认空串。

**源码事实：** `RBACResourceType` 的 `route` 属性把资源类型映射成 `(segment, id_param)`（apps/app_id 等），每次访问重建三元素字典，未知成员会 `KeyError`；`RBACRoleType` 只剩 `WORKSPACE` 一个值（docstring：the only concrete role type after the access-policy refactor）。

**源码事实：** MCP 侧用 `UNCHANGED_SERVER_URL_PLACEHOLDER = "[__HIDDEN__]"` 作为「URL 未改」的回传哨兵，判定用子串包含而非相等。

**源码事实：** `validate_server_url_standalone` 的 docstring 硬性要求「MUST be called OUTSIDE of any database session to avoid holding locks during network I/O」，配套 `get_provider_for_url_validation` 先读库返回纯数据对象；`update_provider` 接受 `validation_result` 并通过它避免会话内网络 I/O。若调用方漏传，URL 更新被静默跳过（不报错）。

**源码事实：** `ServerUrlValidationResult` 的 `validation_passed` 在唯一能产生 `needs_validation=True` 的路径上是硬编码 `True`（失败改为抛异常），因此该字段冗余；`should_update_server_url` 要求三个条件同时成立。

**源码事实：** `_reconnect_with_url` 三支分派：成功 → `authed=True` + 工具 JSON；`MCPAuthError` → `authed=False` + 空 JSON（合法中间态，需走 OAuth）；其它 `MCPError` → 抛 `ValueError`。

**源码事实：** `OAuthDataType` 四值（tokens / client_info / code_verifier / mixed）；`save_oauth_data` 的 `authed` 用 `data_type == TOKENS or (data_type == MIXED and "access_token" in data) or None` 计算，结果为 `bool | None`，`None` 表示「不改变认证状态」。

**源码事实：** `update_provider` 的重复检测有两层：名字变化时的显式先查，以及捕获 `IntegrityError` 调 `_handle_integrity_error`；`_check_provider_exists` 用 `or_` 同时对 name / server_url_hash / server_identifier 判重并逐项给出不同错误消息。URL 变更用 `sha256` 与库里的 `server_url_hash` 比对而不解密明文。

**源码事实：** `list_providers` 批量查 `Account` 以避免 N+1（转换函数保持纯函数，收 `user_name` 参数）；`include_sensitive=True` 为向后兼容的默认值，注释说明 `False` 会跳过昂贵解密。而内置工具侧的 `get_builtin_tool_provider_credentials` 没有这个开关，总是 `decrypt` 后 `mask`。

**源码事实：** `get_builtin_tool_provider_credentials` 的 `user=None` 表示「返回租户内全部凭据」（面向内部/后台调用方），与 h01 `get_datasets` 的「无 user + RBAC 开 → `sa.false()`」方向相反；`include_credential_ids` 是可见性过滤的例外通道，例外行用 `from_other_member=True` 标注以便 UI 区分「可选中但不可编辑」。

**源码事实：** `get_builtin_provider` 用 `except Exception` 兜「老 provider 没有 organization」，降级查询用的 `provider_name` 可能是已拆解的短名也可能是原始名（取决于异常发生在赋值之前还是之后）；它自建 `Session(db.engine, autoflush=False)` 而同类其它方法收 `session` 参数。

**源码事实：** `save_custom_oauth_client_params` 在两个参数都为 `None` 时早退并返回 `{"result": "success"}`；`update_provider` 对 `identity_mode` 直接赋值，注释说明「控制器已解析『保持不变』并应用企业版门禁」。

**架构推论：** 「权限矩阵用字面量展开」在权限项少、角色稳定时是最可读的写法，但它把**矩阵的语义**变成了不可查询的文本。一旦要回答「admin 比 owner 少哪些权限」，只能人眼比列表。**正确的中间形态是「声明式基线 + 差集」**：定义 owner 全集 + 每个角色的 `-` 与 `+` 集合，既保留可读性又让差集可查询。这里连「admin = owner 减去两条 acl.preview」这件事都没有写下来。

**架构推论：** 「fallback snapshot 必须与外部仓对齐」是一条**无法用代码保证的约束**，只能靠人。这类约束的加固手段是把它变成测试（拉取外部仓的角色定义做对拍），而当前只有注释。**任何「保持同步」的注释都应当被视为一个待办**，否则它会随着时间静默失真。

**架构推论：** 「网络 I/O 必须搬出会话」只能靠两步 API + docstring 表达，因为 Python 里没有「纯函数」的类型标记。**代价是它成了调用方义务**：漏传 `validation_result` 时不是报错而是静默跳过。更好的形状是让 `update_provider` 在「URL 参数含有实际变更但没给 validation_result」时**抛错**——把「静默跳过」变成「显式拒绝」。这是所有「两步 API」的共同加固点。

**架构推论：** 「例外要有标记」这条原则在这里做得很好（`from_other_member=True`），它比「不返回例外」和「无标记地返回例外」都好：前者让分享出去的工作流不可用，后者让用户看到一个自己能点但点不动的凭据却不明白为什么。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/services/rbac/builtin.py` | `builtin_member_roles` | 把老租户角色包成一个 `RBACRole`（id/name 用枚举值，owner 单独定 role_tag） |
| `api/services/rbac/builtin.py` | `builtin_permissions` | 角色 → 四作用域权限快照；非法角色退化成空（fail-closed） |
| `api/services/rbac/builtin.py` | `builtin_role_permission_keys` | 四作用域合并后保序去重成单列表 |
| `api/services/rbac/builtin.py` | `builtin_workspace_roles` | 内置角色以 RBAC 列表形状返回（兼容关闭企业 RBAC 时的 `/rbac/roles`） |
| `api/services/rbac/builtin.py` | `_BUILTIN_MY_PERMISSIONS` | 按角色 → 作用域索引的权限映射（`NORMAL` 无 dataset 键） |
| `api/services/rbac/builtin.py` | `_BUILTIN_ROLE_PERMISSION_KEYS` | 按角色平铺的权限清单（不去重；`dataset_operator` 与上一处不一致） |
| `api/services/rbac/contracts.py` | `Paginated` / `Pagination` | 泛型分页壳（`pagination` 可为 `None` 表示不分页） |
| `api/services/rbac/contracts.py` | `MembersInRole` | 角色成员的最简表示（两个默认空串字段） |
| `api/services/rbac/contracts.py` | `RBACResourceType` | 资源类型 → REST 路径段与参数名的映射 |
| `api/services/rbac/contracts.py` | `RBACRoleType` | 重构后只剩 `workspace` 一种角色类型 |
| `api/services/rbac/contracts.py` | `_RBACModel` | 契约基类（`populate_by_name=True`、`extra="ignore"`） |
| `api/services/tools/builtin_tools_manage_service.py` | `BuiltinToolManageService` | 内置工具提供方与凭据的 CRUD（加密存储、解密+掩码读取） |
| `api/services/tools/builtin_tools_manage_service.py` | `BuiltinToolManageService.create_tool_encrypter` | 按凭据 schema 构造加密器与缓存 |
| `api/services/tools/builtin_tools_manage_service.py` | `BuiltinToolManageService.get_builtin_tool_provider_credentials` | 按可见性过滤凭据；`include_credential_ids` 是例外通道 |
| `api/services/tools/builtin_tools_manage_service.py` | `BuiltinToolManageService.get_builtin_provider` | 默认优先/最老优先的兜底查询（含老格式 provider 的异常降级） |
| `api/services/tools/builtin_tools_manage_service.py` | `BuiltinToolManageService.save_custom_oauth_client_params` | 自定义 OAuth 客户端参数（双 None 早退） |
| `api/services/tools/mcp_tools_manage_service.py` | `OAuthDataType` | OAuth 中间产物四态（tokens / client_info / code_verifier / mixed） |
| `api/services/tools/mcp_tools_manage_service.py` | `ReconnectResult` | 重连结果（authed + 工具 JSON + 加密凭据 JSON） |
| `api/services/tools/mcp_tools_manage_service.py` | `MCPToolManageService.validate_server_url_standalone` | 会话外的 URL 校验（硬性要求） |
| `api/services/tools/mcp_tools_manage_service.py` | `MCPToolManageService._reconnect_with_url` | 纯静态网络连接：成功/需认证/其它错误三支分派 |
| `api/services/tools/mcp_tools_manage_service.py` | `MCPToolManageService.update_provider` | 更新提供方（接受预校验结果以避免会话内网络 I/O） |
| `api/services/tools/mcp_tools_manage_service.py` | `MCPToolManageService.list_providers` | 列表（批量查用户避免 N+1；`include_sensitive` 可跳过解密） |
| `api/services/tools/mcp_tools_manage_service.py` | `MCPToolManageService._check_provider_exists` | name / server_url_hash / server_identifier 三字段任一冲突即报错 |
| `api/services/tools/mcp_tools_manage_service.py` | `UNCHANGED_SERVER_URL_PLACEHOLDER` | URL 未改的回传哨兵 `"[__HIDDEN__]"` |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 文档：工作区成员与权限](https://docs.dify.ai/guides/workspace/members)
- [MCP 规范：授权](https://modelcontextprotocol.io/specification/2025-06-18/basic/authorization)
- [Pydantic v2 文档：模型配置](https://docs.pydantic.dev/latest/api/config/)

## 与主核 / AI 的关系

权限与工具凭据在主核（多模态数据库内核）里分别对应**授权系统**与**外部凭据管理**。四条经验。

**第一，权限矩阵要有「基线 + 差集」的表达，否则语义不可查询。** 这里十六个角色×作用域列表全是字面量，连「admin 比 owner 少哪两条」都只能人眼比对。主核里做授权（表/列/行级 ACL、角色继承）时同理：**用「全集 + 每角色增量」表达，让差集能从数据算出**，而不是让每个角色的能力各自抄一遍。抄写必然漏，而漏掉一条权限的表现是「某个角色多了一个不该有的能力」——这是最难在测试里覆盖的一类缺陷。

**第二，跨系统的「保持同步」不是注释能承担的约束。** 这里权限键必须与外部 RBAC 仓对齐，而唯一的手段是那句注释。主核里凡是有「与另一组件共享枚举/常量」的地方（页大小、错误码、格式版本），都应当有一条**自动对拍**（启动时校验、或 CI 拉取对端定义比对）。否则失真是必然的，只是时间问题。

**第三，跨边界的耗时操作（网络、文件、大计算）必须能从类型上看出「它在事务外」。** 这里靠两步 API 加一句 `MUST`，而漏做的后果是静默跳过而非报错。主核里凡是有「在持有 latch/事务期间做 I/O」风险的路径，正确形状是**把「是否需要外部结果」变成函数签名的必填项**，让「忘记传」变成编译期/调用期错误。Python 里做不到编译期，那就该在运行期断言。

**第四，「例外要带标记」比「不给例外」和「静默给例外」都好。** `include_credential_ids` + `from_other_member=True` 这个组合让「引用了别人私有凭据」这件事既能工作又能被理解。主核里做「跨 schema 引用」「跨租户对象引用」时同理：**当完整性要求突破可见性边界时，突破本身必须带一个可读的原因标记**，否则用户看到的是一个自己点不动却不知道为什么的对象。
