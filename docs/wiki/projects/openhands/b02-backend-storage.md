---
project: openhands
feature: backend-storage
title: 后端清单的本地持久化
page_type: feature
schema_version: 1
refs:
  - path: src/api/backend-registry/storage.ts
    symbols: [BACKENDS_STORAGE_KEY, readStoredBackends, writeStoredBackends]
  - path: src/api/backend-registry/storage.ts
    symbols: [isValidBackend, isLoopbackUrl, parseBackendSelection]
---

# 后端清单的本地持久化

## 一句话职责

`storage` 是后端清单与「当前选择」两层状态的唯一持久层：它负责首次种子的写入、每次读取时的逐条校验、锁定云端模式下的强制覆盖，以及在启动器换了 session key 时同步那条种子后端——读路径因此不是纯读，它会顺手修正存储。

## 机制是什么

这个模块要处理的是「存储里的东西不一定可信」：

- 它可能**不存在**（首次安装）；
- 它可能**存在但内容已经变了**（升级后 schema 变了、被人手改过、被另一个版本的代码写过）；
- 它可能**被别的东西覆盖**（锁定云端部署里，清单只有一个正确答案）。

于是 `readStoredBackends` 的分支顺序不是「读一下、解析一下」，而是一条**修正链**，每个分支都往存储里写回自己认定的事实：

| 优先级 | 条件 | 行为 |
|---|---|---|
| 1 | 锁定到单一 Cloud 主机 | 清单强制为 `[lockedCloud]`，并确保活动选择指向它 |
| 2 | 存储为空（`null`） | 若工厂能造出默认本地后端，种下并写回；否则返回空数组 |
| 3 | 解析后有效条目为 0 | 同上（重新种子） |
| 4 | 有有效条目 | 逐条校验后，做「session key 同步」再返回 |

两个键名是固定的：`BACKENDS_STORAGE_KEY = "openhands-backends"`（清单）与 `ACTIVE_BACKEND_STORAGE_KEY = "openhands-active-backend"`（选择）。

「当前选择」这一层还有一个与清单不同的存储策略：**sessionStorage 优先、localStorage 作为后备**。这个不对称是刻意的，稍后会展开。

## 怎么实现的

**`writeStoredBackends` 是一个「永不抛」的写入。**

```
export function writeStoredBackends(backends: Backend[]): void {
  if (typeof window === "undefined") return;
  try {
    window.localStorage.setItem(BACKENDS_STORAGE_KEY, JSON.stringify(backends));
  } catch {
    /* ignore quota / serialization errors */
  }
}
```

两层保护：`typeof window === "undefined"` 让 SSR / 测试环境不炸；`try/catch` 吞掉配额与序列化错误。后者是必要的——`Backend` 里全是可序列化的字段，但未来加一个非可序列化字段就会抛，而「存不进去」不该让整个界面崩掉。

`readStoredBackends` 同样被一个大 `try/catch` 包住，任何解析错误都返回 `[]`。于是「存储坏了」的后果是「看不到后端」，而不是白屏。

**锁定云端分支在最前，且会重写清单与选择。**

```
const lockedCloudBackend = makeLockedCloudBackend();
if (lockedCloudBackend) {
  writeStoredBackends([lockedCloudBackend]);
  const activeSelection = readStoredActiveBackend();
  if (activeSelection?.backendId !== lockedCloudBackend.id) {
    writeStoredActiveBackend({ backendId: lockedCloudBackend.id });
  }
  return [lockedCloudBackend];
}
```

三件事：强制清单、检查活动选择是否已指向它、不指向则改写。注意第二件事的判据——**没变就不写**。这是同一个模块里反复出现的模式：只有在事实需要修正时才落盘，避免每次读取都触发一次写入。

这条分支在锁定模式下使清单不可增删：用户之前存的东西会被覆盖，每次读取都会重建。这也解释了为什么 `makeDefaultLocalBackend` 要在锁定模式下返回 `null`（见 a03 页）——两个判断必须一致，否则会出现「锁定云端 + 一条本地种子」的混合清单。

**首次安装与「有效条目为零」走同一条重种路径。**

```
const raw = window.localStorage.getItem(BACKENDS_STORAGE_KEY);

if (raw === null) {
  const defaultBackend = makeDefaultLocalBackend();
  if (!defaultBackend) return [];
  writeStoredBackends([defaultBackend]);
  return [defaultBackend];
}

const parsed = JSON.parse(raw);
if (!Array.isArray(parsed)) return [];
const valid = parsed.filter(isValidBackend);

if (valid.length === 0) {
  const defaultBackend = makeDefaultLocalBackend();
  if (!defaultBackend) return [];
  writeStoredBackends([defaultBackend]);
  return [defaultBackend];
}

const synced = syncLauncherDefaultLocalBackend(valid);
return synced;
```

注释在第二处解释了这个合并：「如果存储的数组为空（或者里面全部没通过校验），只在启动器提供了 host 和 API key 时才重新种」。

值得注意的是 `!Array.isArray(parsed)` 直接返回 `[]` 而**不**重种——一个「存在但不是数组」的存储被视为损坏数据，此时不猜测、不覆盖，交给上层（空清单会走到 `NO_BACKEND` 哨兵）。这是「坏数据不静默修正」的选择，与「空数据可以重种」是有区别的。

**`isValidBackend` 是逐字段的白名单校验，且它决定「部分损坏」的处理方式。**

```
export function isValidBackend(value: unknown): value is Backend {
  if (typeof value !== "object" || value === null) return false;
  const v = value as Partial<Backend>;
  return (
    typeof v.id === "string" && v.id.length > 0 &&
    typeof v.name === "string" &&
    typeof v.host === "string" &&
    typeof v.apiKey === "string" &&
    isValidKind(v.kind) &&
    isValidAuthMode(v.authMode) &&
    (v.connectionRevision === undefined ||
      (typeof v.connectionRevision === "number" && Number.isSafeInteger(v.connectionRevision) && v.connectionRevision >= 0))
  );
}
```

它由 `parsed.filter(isValidBackend)` 使用——所以**逐条丢弃**，一条坏了不影响其他条。这个选择很实际：一份十后端的清单里有一条字段缺失，丢掉其余九条是过分惩罚。

几处细节：

- `v.id.length > 0` —— 空 id 会让选择解析、键构造、去重全乱。
- `isValidAuthMode` 接受 `undefined`（缺失等价于 `api-key`），这是类型里 `authMode?` 的运行时对应。
- `connectionRevision` 的校验用了 `Number.isSafeInteger` 且要求 `>= 0`——这与类型注释「凭证变化时递增」一致，负数或小数都是不可能的。
- `isValidKind` 是 `value === "local" || value === "cloud"` 的精确比较，不用 `includes` 也不用 `Array.prototype.some`——两个值的枚举，精确比较最省。

注意它**没有校验** `marsSessionId` / `marsConfigId`。这两个字段是字符串或缺失，但校验函数没管——一个被篡改成数字的 `marsSessionId` 会留到运行时。这是一个小的不一致（丢一条记录比丢一个字段更重），但代价可控：消费它的地方只做字符串比较与「是否真值」判断。

**`isLoopbackUrl` 是「同一台机器」的判据。**

```
export function isLoopbackUrl(value: string): boolean {
  try {
    const { hostname } = new URL(value);
    return hostname === "localhost" || hostname === "127.0.0.1" || hostname === "::1" || hostname === "[::1]";
  } catch {
    return false;
  }
}
```

用 `new URL()` 而不是字符串包含判断，所以 `http://localhost:3000` 与 `https://127.0.0.1` 都能识别，而一个含 `localhost` 子串的路径不会误判。四种写法覆盖了浏览器对 IPv6 字面量的两种表示（`::1` 与 `[::1]`）。

它的唯一用途在 `shouldSyncLauncherDefaultLocalBackend` 里，与 `===` 比较并列：

```
return backend.host === defaultBackend.host || (isLoopbackUrl(backend.host) && isLoopbackUrl(defaultBackend.host));
```

即「同一个 host 字符串，或者两个都指向回环」。后者覆盖了一个真实场景：启动器这次注入的 host 是 `http://127.0.0.1:3000`，而存储里那条是 `http://localhost:3000`——从协议角度它们可能是同一个 agent-server，但字符串不同。若只比较字符串，就永远不会同步 key。

**`syncLauncherDefaultLocalBackend` 是「只有两条都带同一份基础数据才更新」的保守同步。**

```
function syncLauncherDefaultLocalBackend(backends: Backend[]): Backend[] {
  const defaultBackend = makeDefaultLocalBackend();
  if (!defaultBackend) return backends;

  let didSync = false;
  const syncedBackends = backends.map((backend) => {
    if (!shouldSyncLauncherDefaultLocalBackend(backend, defaultBackend)) return backend;
    if (backend.apiKey === defaultBackend.apiKey) return backend;
    didSync = true;
    return { ...backend, apiKey: defaultBackend.apiKey };
  });

  if (!didSync) return backends;
  writeStoredBackends(syncedBackends);
  return syncedBackends;
}
```

它的作用只有一个：**把存储里那条种子后端的 apiKey 更新成启动器这次注入的那一个**。为什么需要：发布二进制每次启动都会生成一个新的 session key（`agent-server-config` 里那段注释说了），但存储里还留着上次的。不同步的话，用户每次重启都看到「后端未连接」，而实际上后端就在这儿、只是 key 变了。

两个前置条件都很严格：

1. `backend.id === SEEDED_DEFAULT_BACKEND_ID && backend.kind === "local"` —— 只动那条种子记录，用户自己加的后端绝不碰。
2. host 匹配（字符串相等或双方都是回环）—— 只动「指向同一台机器的那一条」。

然后 `if (backend.apiKey === defaultBackend.apiKey) return backend` 是第三道：**没变就不改**。三个条件合起来，这个函数是「窄且幂等」的。

`didSync` 标志用于「一个都没变就不写存储」。`map` 会返回新数组，若直接 `writeStoredBackends(syncedBackends)` 就会在每次读取时都触发一次写入——而 `readStoredBackends` 是被 `active-store` 的模块初始化与 `setRegisteredBackends` 都会调的，频率不低。

**活动选择的两层存储，及其不对称。**

```
export function readStoredActiveBackend(): BackendSelection | null {
  if (typeof window === "undefined") return null;

  // 活动后端是标签页作用域的，于是刷新标签 A 不会采用标签 B 的后端。
  // localStorage 保留作为新标签页与旧持久状态的最近使用后备。
  const sessionSelection = parseBackendSelection(readStorageItem(window.sessionStorage, ACTIVE_BACKEND_STORAGE_KEY));
  if (sessionSelection) return sessionSelection;

  return parseBackendSelection(readStorageItem(window.localStorage, ACTIVE_BACKEND_STORAGE_KEY));
}

export function writeStoredActiveBackend(selection: BackendSelection | null): void {
  if (typeof window === "undefined") return;

  if (!selection) {
    removeStorageItem(window.sessionStorage, ACTIVE_BACKEND_STORAGE_KEY);
    removeStorageItem(window.localStorage, ACTIVE_BACKEND_STORAGE_KEY);
    return;
  }

  const serialized = JSON.stringify({ backendId: selection.backendId, orgId: selection.orgId ?? null });
  // 只镜像到 localStorage，作为新标签页/向后兼容的默认值；既有标签页的读取优先 sessionStorage。
  writeStorageItem(window.sessionStorage, ACTIVE_BACKEND_STORAGE_KEY, serialized);
  writeStorageItem(window.localStorage, ACTIVE_BACKEND_STORAGE_KEY, serialized);
}
```

读：sessionStorage 优先，localStorage 兜底。写：**两个都写**。清空：**两个都清**。

这个不对称是这一页最值得想清楚的设计。为什么读要优先 sessionStorage？因为「活动后端」在语义上是**每个标签页自己的**——用户可能在标签 A 里看本地后端，在标签 B 里看云端。若只读 localStorage，两个标签页会互相踩。

那为什么写要两个都写？因为 localStorage 承担「新标签页的默认值」这个角色：一个新开的标签页没有 sessionStorage，此时它应当继承「最近一次使用的后端」，而不是回到某个初始状态。写两份恰好实现「标签内保持、新标签继承」。

于是它还有个必然的副作用：新标签页继承的是「**任意**标签页最后一次选择的后端」。这正是 `url-selection` 那套 URL 参数要解决的问题——一个指向某会话的链接，在新标签里打开时可能落在错误的后端上。那套机制的 docstring 直接点名了这个原因。

`null` 与「没有值」被统一：`writeStoredActiveBackend(null)` 删掉两个键，而 `readStoredActiveBackend` 在没有值或解析失败时都返回 `null`。

**`parseBackendSelection` 对 `orgId` 做了归一。**

```
function parseBackendSelection(raw: string | null): BackendSelection | null {
  if (!raw) return null;
  try {
    const parsed = JSON.parse(raw);
    if (typeof parsed !== "object" || parsed === null || typeof (parsed as BackendSelection).backendId !== "string") return null;
    const orgIdRaw = (parsed as BackendSelection).orgId;
    return {
      backendId: (parsed as BackendSelection).backendId,
      orgId: typeof orgIdRaw === "string" && orgIdRaw.length > 0 ? orgIdRaw : null,
    };
  } catch {
    return null;
  }
}
```

三个决定：`backendId` 必须是字符串（否则整条作废）；`orgId` 只在「非空字符串」时保留，其余（缺失、`null`、空串、数字）一律归一成 `null`；空串被当成缺失，这与 `ResolvedActiveBackend` 里 `orgId: string | null` 的归一一致。

写入端也做了一次归一：`orgId: selection.orgId ?? null`——于是 `undefined` 落盘时变成 `null`，读回时还是 `null`。

**三个 storage 小工具把「Storage 可能抛」的边界收在一处。**

```
function readStorageItem(storage: Storage | undefined, key: string): string | null {
  try { return storage?.getItem(key) ?? null; } catch { return null; }
}
```

`storage` 参数是 `Storage | undefined`——`window.sessionStorage` 在极端情况下（隐私模式、被策略禁用）可能抛或不可用，所以每次访问都包 `try/catch`。写与删同理。这三行重复的函数是「把所有可能抛的 web API 访问归纳到三个地方」的做法，比到处写 `try/catch` 好维护。

## 关键取舍

**源码事实：** `readStoredBackends` 不是纯读：锁定模式会强制重写清单与选择、缺失/全无效会重新种子、种子 key 变了会同步写回。

**源码事实：** 分支顺序是「锁定云端 → 缺失 → 全无效 → 有效条目 + key 同步」；`!Array.isArray` 的损坏数据返回空数组**而不重种**，与「空数组」的处理不同。

**源码事实：** 校验是逐条 `filter`，一条不合规只丢那一条；`isValidBackend` 没有校验 `marsSessionId` / `marsConfigId`。

**源码事实：** 种子 key 同步的三个前置条件都很窄：必须是 `SEEDED_DEFAULT_BACKEND_ID` 且 `kind: "local"`、host 相同或双方都是回环、且 key 确实变了才写。

**源码事实：** 活动选择的读优先 `sessionStorage`、写两个都写、`null` 两个都清。于是「标签内保持」与「新标签继承最近使用」两个诉求同时满足。

**源码事实：** `parseBackendSelection` 把非空字符串以外的 `orgId` 一律归成 `null`，写入端也做同样归一。

**源码事实：** 所有 `window.localStorage` / `sessionStorage` 访问都被收进三个 `try/catch` 小工具，读路径整体再包一层，任何解析失败都退化成空值而非抛错。

**架构推论：** 「读路径顺手修正存储」看起来违背了读写分离，但在这里是必须的：修正依赖运行时信息（当前是哪个 origin、启动器这次注入了什么 key），这些信息存储里没有，也无法在写入时判断。代价是读取有副作用，收益是「用户每次打开都是自洽的」。

**架构推论：** 把「选择」放进 sessionStorage 而「清单」放进 localStorage，是在用两个存储域表达两种生命周期：清单是账号级的、跨标签共享；选择是标签级的、不该互相干扰。这个区分比在同一个域里加标签 id 更省事，也更容易正确处理关闭标签的情况（sessionStorage 会随标签销毁）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/backend-registry/storage.ts` | `BACKENDS_STORAGE_KEY` | 清单的存储键 `openhands-backends` |
| `src/api/backend-registry/storage.ts` | `readStoredBackends` | 修正链：锁定覆盖 → 重种 → 校验 → key 同步 |
| `src/api/backend-registry/storage.ts` | `writeStoredBackends` | 永不抛的写入，吞配额与序列化错误 |
| `src/api/backend-registry/storage.ts` | `isValidBackend` | 逐字段白名单校验，决定「部分损坏」逐条丢弃 |
| `src/api/backend-registry/storage.ts` | `isLoopbackUrl` | 用 URL 解析判回环，覆盖四种写法 |
| `src/api/backend-registry/storage.ts` | `parseBackendSelection` | 选择解析并归一 `orgId` |
| `src/api/backend-registry/storage.ts` | `readStoredActiveBackend` | sessionStorage 优先、localStorage 兜底 |
| `src/api/backend-registry/storage.ts` | `writeStoredActiveBackend` | 双写；`null` 时双清 |
| `src/api/backend-registry/storage.ts` | `syncLauncherDefaultLocalBackend` | 只更新种子后端的 apiKey，且未变则不写 |

## 参考

- [OpenHands 源码：backend-registry/storage.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/backend-registry/storage.ts)
- [MDN：Window.sessionStorage](https://developer.mozilla.org/en-US/docs/Web/API/Window/sessionStorage)
- [MDN：Window.localStorage](https://developer.mozilla.org/en-US/docs/Web/API/Window/localStorage)
- [OpenHands 文档：本地栈与运行时](https://docs.openhands.dev/openhands/usage/local)

## 与主核 / AI 的关系

「读的时候顺手修正」与主核里的「打开数据库时修复元数据」是同一种做法：主核在启动时会检查系统表的一致性、把未提交的事务回滚、必要时重写检查点。这些动作都发生在「读」的路径上，因为它们依赖的运行时事实（上次崩溃点、当前版本）不在数据文件里。

对 AI 工程的实际意义有三条。第一，**校验必须逐条**。一份清单里一条坏掉就全部作废，会让一次字段迁移变成一次「用户丢失所有后端」的事故。逐条 `filter` 把损失限制在最小的那一格。

第二，**同步逻辑的判据要窄，且要幂等**。「只更新那条种子后端、只在 host 指向同一台机器时、只在 key 真的变了时才写」——三个条件让这段逻辑不会误伤用户数据，也不会每次读取都产生一次写入。任何「读路径上写存储」的实现都必须有这两个性质，否则它会把一次读取变成一次性能问题。

第三，**两个存储域可以表达两种生命周期**。清单跨标签共享（localStorage），选择按标签隔离（sessionStorage 优先、localStorage 备用）。这比在一个域里塞标签 id 简单得多，也自然处理了「标签关闭」这件事。代价是必须同时处理「新标签继承哪个选择」这个语义问题——这里选了「最近使用」，而它的副作用（新标签可能落到错误后端）正是 URL 参数机制存在的原因。
