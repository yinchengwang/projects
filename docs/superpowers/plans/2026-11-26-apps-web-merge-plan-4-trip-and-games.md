# apps/web 合并 · 计划四：trip-log（3 页 + 图片）与 games（5 页）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 trip-log 与 games 搬进 `apps/web/`，成为 `/trip/` 与 `/games/` 两个模块。本计划的核心是**给 trip-log 造一个 IndexedDB 图片存储层**（替换腾讯云 SDK），并把 4 个游戏引擎与 51 个引擎单测原样迁入。

**Architecture:** trip-log 的元数据走 `localStorage`（沿用 `db:` 前缀），照片以 `Blob` 存 IndexedDB；games 的 4 个引擎是**零 wx 依赖的纯函数**，只换存储层与震动层。

**Tech Stack:** Node.js ≥ 24（内置 `node:test`）、零 npm 依赖、零构建；浏览器端原生 ES2020 + DOM + Canvas 2D + IndexedDB。

## Global Constraints

- **提交纪律**：每次 commit 后立即 `git push origin main`，不累积多个 commit。
- **语言纪律**：所有对话、解释、注释、commit message 用简体中文；代码标识符与 API 名保持英文。
- **代码风格**：`apps/web/` 用 2 空格缩进、双引号、`"use strict"` 开头、JSDoc 注释（中文）。
- **零依赖**：不新增任何 npm 包，不引入前端框架，不引入打包工具。
- **中文文案**：面向用户的文案一律中文。
- **测试命令**：`cd apps/web && node tests/run-all.js`。
- **依赖前提**：本计划**必须在计划一、计划二完成后**执行（需要 `shared/platform.js` `cjs-loader.js`）。
- **不要动 `engine/`、`apps/study-hub/`、`apps/wiki-rag/`**。
- **不要动 `apps/trip-log/`、`apps/games/`**：只读参考。新代码落在 `apps/web/` 下。本计划**不删**旧目录（删除在计划五）。

---

## 关键调查结论（决定了本计划的做法）

### 结论 1：games 的 4 个引擎是零 wx 依赖的纯函数

实测 `apps/games/utils/` 8 个文件的 wx 引用数：

| 文件 | wx 引用 | 依赖 |
|---|---|---|
| `game2048.js` | **0** | 无 |
| `snake.js` | **0** | 无 |
| `sudoku.js` | **0** | 无 |
| `match3.js` | **0** | 无 |
| `achievements.js` | **0** | `./storage.js` |
| `share.js` | **0** | 无 |
| `storage.js` | 8 | 无 |
| `vibrate.js` | 1 | 无 |

**结论**：4 个引擎 + `achievements` + `share` **一字不改**直接搬。只要把 `storage.js` 的底层从 `wx.setStorageSync` 换成 `localStorage`（保留 `games_` 前缀），`vibrate.js` 的 `wx.vibrateShort` 换成 `platform.vibrateShort`。

> `games_` 前缀与其它模块的 `db:` 前缀**不冲突**，无需改键名。

### 结论 2：games 已有 51 个引擎单测可原样迁入

`apps/games/tests/engine.test.js`（18.3 KB，51 个用例）自己 mock 了一个 `global.wx`（内存 Map 版 storage + 两个 no-op 震动）：

```javascript
const store = {}
global.wx = {
  getStorageSync: k => store[k],
  setStorageSync: (k, v) => { store[k] = v },
  removeStorageSync: k => { delete store[k] },
  vibrateShort: () => {},
  vibrateLong: () => {}
}
```

**结论**：把这个 mock 换成 `localStorage` 替身，51 个用例**原样保留**。这与计划二/三的迁入做法一致。

### 结论 3：games 只用 8 个 wx API

实测 `apps/games/pages/**` 的 wx 调用点：

| API | 次数 | Web 侧对应 |
|---|---|---|
| `showToast` | 7 | `platform.showToast` ✅ 计划一已有 |
| `getWindowInfo` | 4 | `window.devicePixelRatio` ✅ |
| `getSystemInfoSync` | 2 | 同上 ✅ |
| `vibrateShort` | 2 | `platform.vibrateShort` ✅ 计划二已有 |
| `vibrateLong` | 1 | `platform.vibrateLong` ❌ **需补** |
| `createSelectorQuery` | 2 | `getBoundingClientRect` ❌ 组件/页面自己改 |
| `navigateTo` | 1 | `platform.navigateTo` ✅ |
| `showModal` | 1 | `platform.showModal` ✅ |

**结论**：只需给 `platform.js` 补 **`vibrateLong`** 一个 API。

### 结论 4：games 有 2 个 Canvas 页面

`snake` 与 `game2048` 用 Canvas 绘制。与 knowledge-hub 一样，**绘制代码逐字保留**，只把取 canvas 的方式从 `wx.createSelectorQuery()` 换成 `getBoundingClientRect()`。

Canvas 2D API 两边完全一致（`clearRect` `fillRect` `beginPath` `arc` `fill` `stroke` `fillText` `moveTo` `lineTo` `closePath`）。

### 结论 5：trip-log 的图片是本计划唯一的新技术点

原实现用腾讯云：

```
选图 → wx.chooseMedia → 本地临时路径
     → cloud.storage().uploadFile → fileID
     → cloud.database().collection('trips').add({ photos: [fileID] })
读取 → cloud.storage().getDownloadURL({ fileIdList }) → 下载 URL
删除 → cloud.storage().deleteFile({ fileIdList })
```

**Web 侧无法等价实现**（`wx.login` 换不到 openid，云 SDK 需要小程序环境）。按设计文档决策 D：

| 数据 | 存储 | 理由 |
|---|---|---|
| 元数据（title/date/weather/location/companions/tags/notes） | `localStorage`，`db:` 前缀 | 小数据，够用 |
| 照片 | **IndexedDB**，存 `Blob` | 单条记录最多 9 张 × 5MB = 45MB，远超 localStorage 的 ~5MB 配额 |

**IndexedDB 层的 API 设计**（Task 1 实现）：

```javascript
const photos = require("../../shared/photos.js")
await photos.put(tripId, photoId, blob)     // 存
const blob = await photos.get(tripId, photoId)  // 取
await photos.delete(tripId, photoId)        // 删
await photos.list(tripId)                   // 列某条记录的照片 id
await photos.deleteAll(tripId)              // 清某条记录
```

### 结论 6：trip-log 放弃的能力

| 放弃 | 原因 | 补偿 |
|---|---|---|
| 微信 openid 身份 | `wx.login` 在浏览器无对应 | 本地单用户，无账号概念 |
| 腾讯云同步 | 云 SDK 需要小程序环境 | **JSON 导出/导入**（含照片 base64） |
| 云端图片存储 | 同上 | IndexedDB |

**当前 `trip-log` 的云配置本就是 `PLACEHOLDER_ENDPOINT`（未激活）**，所以迁移**不损失任何已可用的功能**。

### 结论 7：trip-log 没有测试

`apps/trip-log/` **没有 `tests/` 目录** —— 3 个页面的逻辑从未被测试覆盖。

**结论**：本计划要**新写**测试。这是本计划与前三个最大的不同：前三个是「迁入现有测试」，本计划是「从零写测试」。

好消息是 trip-log 的逻辑很少（3 个页面、6 个格式化函数），新写的测试规模不大。

---

## File Structure

| 文件 | 归属 | 职责 |
|---|---|---|
| `apps/web/shared/photos.js` | 新建 | **IndexedDB 图片存储层**（Promise 化封装） |
| `apps/web/shared/trip/format.js` | 新建 | 日期与字符串格式化（6 个函数，从小程序版搬） |
| `apps/web/shared/trip/store.js` | 新建 | trip-log 的数据层：增删改查 + 照片关联 |
| `apps/web/shared/trip/export.js` | 新建 | JSON 导出/导入（含照片 base64） |
| `apps/web/shared/games/storage.js` | 新建 | 游戏存档（底层换 localStorage，`games_` 前缀保留） |
| `apps/web/shared/games/vibrate.js` | 新建 | 震动（换 platform.vibrateShort） |
| `apps/web/shared/games/game2048.js` | 新建 | 2048 引擎（**一字不改**） |
| `apps/web/shared/games/snake.js` | 新建 | 贪吃蛇引擎（**一字不改**） |
| `apps/web/shared/games/sudoku.js` | 新建 | 数独引擎（**一字不改**） |
| `apps/web/shared/games/match3.js` | 新建 | 消消乐引擎（**一字不改**） |
| `apps/web/shared/games/achievements.js` | 新建 | 成就系统（只改 require 路径） |
| `apps/web/shared/games/share.js` | 新建 | 分享文案（**一字不改**） |
| `apps/web/shared/platform.js` | 改 | 补 `vibrateLong` |
| `apps/web/web/trip/router.js` | 新建 | trip 模块路由（3 页） |
| `apps/web/web/trip/browser.js` | 新建 | trip 模块浏览器入口 |
| `apps/web/web/trip/shell.html` | 新建 | trip 模块 HTML 壳 |
| `apps/web/web/trip/style.css` | 新建 | trip 模块样式 |
| `apps/web/web/trip/pages/index.js` | 新建 | 足迹列表 |
| `apps/web/web/trip/pages/new-trip.js` | 新建 | 新增记录（含选图） |
| `apps/web/web/trip/pages/detail.js` | 新建 | 详情（含预览与删除） |
| `apps/web/web/games/router.js` | 新建 | games 模块路由（5 页） |
| `apps/web/web/games/browser.js` | 新建 | games 模块浏览器入口 |
| `apps/web/web/games/shell.html` | 新建 | games 模块 HTML 壳 |
| `apps/web/web/games/style.css` | 新建 | games 模块样式 |
| `apps/web/web/games/pages/index.js` | 新建 | 游戏中心首页 |
| `apps/web/web/games/pages/snake.js` | 新建 | 贪吃蛇（Canvas） |
| `apps/web/web/games/pages/game2048.js` | 新建 | 2048（Canvas） |
| `apps/web/web/games/pages/sudoku.js` | 新建 | 数独 |
| `apps/web/web/games/pages/match3.js` | 新建 | 消消乐 |
| `apps/web/tests/photos.test.js` | 新建 | IndexedDB 图片层测试 |
| `apps/web/tests/trip-store.test.js` | 新建 | trip 数据层测试 |
| `apps/web/tests/trip-export.test.js` | 新建 | 导出导入往返测试 |
| `apps/web/tests/games-engine.test.js` | 新建 | 从 `engine.test.js` 迁入（51 用例） |
| `apps/web/tests/games-view.test.js` | 新建 | games 视图层断言 |
| `apps/web/tests/trip-view.test.js` | 新建 | trip 视图层断言 |

---

## Task 1：`shared/photos.js` —— IndexedDB 图片存储层

**Files:**
- Create: `apps/web/shared/photos.js`
- Test: `apps/web/tests/photos.test.js`

**Interfaces:**
- Consumes: 无（首个任务，只用浏览器原生 IndexedDB API）
- Produces: `shared/photos.js` 导出：
  - `put(tripId, photoId, blob)` → `Promise<void>`
  - `get(tripId, photoId)` → `Promise<Blob|null>`
  - `delete(tripId, photoId)` → `Promise<void>`
  - `list(tripId)` → `Promise<string[]>`（该记录下的 photoId 列表）
  - `deleteAll(tripId)` → `Promise<void>`
  - `count()` → `Promise<number>`（全部照片数）
  - `isAvailable()` → `Promise<boolean>`（IndexedDB 是否可用）
  - `clear()` → `Promise<void>`（清空全部，用于测试与「清空数据」）

- [ ] **Step 1: 写 `tests/fake-idb.js`（IndexedDB 内存替身）**

Node 下没有 IndexedDB。先写一个替身，**`photos.test.js` 与 `trip-store.test.js` 共用这一份**（不要各写一份）。

新建 `apps/web/tests/fake-idb.js`：

```javascript
"use strict"
/**
 * fake-idb.js —— Node 下的 IndexedDB 内存替身
 *
 * 复刻真实 IndexedDB 的语义：
 *   - `indexedDB.open(name, version)` **立即**发起请求（不需再调 open()）
 *   - 支持 `keyPath`：put 时若没显式给 key，取 value.key
 *   - onupgradeneeded → onsuccess 的顺序与真实一致，且都是异步
 *   - 事务在下一个宏任务触发 oncomplete
 *
 * photos.test.js 与 trip-store.test.js 共用这一份。
 */

/**
 * 装一个内存版 IndexedDB 替身。
 * @returns {object} indexedDB 替身
 */
function install () {
  const dbs = new Map()

  function getStore (db, name) {
    if (!db.stores.has(name)) db.stores.set(name, new Map())
    return db.stores.get(name)
  }

  const indexedDB = {
    open: function (name, version) {
      const request = {
        onupgradeneeded: null,
        onsuccess: null,
        onerror: null,
        result: null,
        error: null,
        readyState: "pending"
      }

      // 真实 API 里 open() 立即发起请求，这里用 setTimeout 模拟异步
      setTimeout(function () {
        if (!dbs.has(name)) dbs.set(name, { stores: new Map(), version: version || 1 })
        const db = dbs.get(name)

        const handle = {
          name: name,
          version: db.version,
          objectStoreNames: { contains: function (n) { return db.stores.has(n) } },
          createObjectStore: function (storeName) {
            getStore(db, storeName)
            return { createIndex: function () {} }
          },
          transaction: function (storeNames, mode) {
            const tx = {
              oncomplete: null,
              onerror: null,
              onabort: null,
              objectStore: function (storeName) {
                const store = getStore(db, storeName)
                return {
                  // 支持 keyPath：没显式给 key 时取 value.key
                  put: function (value, key) {
                    const k = key !== undefined ? key : (value && value.key)
                    store.set(k, value)
                  },
                  get: function (key) { return store.get(key) },
                  delete: function (key) { store.delete(key) },
                  getAll: function () { return Array.from(store.values()) },
                  clear: function () { store.clear() },
                  count: function () { return store.size }
                }
              }
            }
            // 事务提交是异步的（与真实一致：写操作要等事务提交才算完）
            setTimeout(function () {
              if (typeof tx.oncomplete === "function") tx.oncomplete()
            }, 0)
            return tx
          },
          close: function () {}
        }

        request.result = handle
        request.readyState = "done"

        // 新库才需要 upgrade；已有库直接 success
        const isNew = !db.stores.size
        if (isNew && typeof request.onupgradeneeded === "function") {
          request.onupgradeneeded({ target: request })
        }
        if (typeof request.onsuccess === "function") {
          request.onsuccess({ target: request })
        }
      }, 0)

      return request
    },

    deleteDatabase: function (name) {
      const request = { onsuccess: null, onerror: null }
      setTimeout(function () {
        dbs.delete(name)
        if (typeof request.onsuccess === "function") request.onsuccess({ target: request })
      }, 0)
      return request
    }
  }

  global.indexedDB = indexedDB
  return indexedDB
}

module.exports = { install: install }
```

> **两个容易踩的坑**（实测踩过）：
> 1. 真实 API 里 `indexedDB.open()` **立即**发起请求，调用方只赋值 `onsuccess` 等回调，**不会**再调 `open()`。替身若要求再调一次 `open()`，请求就永远不触发。
> 2. `createObjectStore(name, {keyPath: "key"})` 时，`put(value)` **不传 key**，键取自 `value.key`。替身必须支持这一点，否则存进去的键是 `undefined`。

- [ ] **Step 2: 写失败测试**

新建 `apps/web/tests/photos.test.js`：

```javascript
"use strict"
/**
 * photos.test.js —— IndexedDB 图片存储层
 */

const test = require("node:test")
const assert = require("node:assert")

const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

const photos = require("../shared/photos.js")

test.beforeEach(async function () {
  await photos.clear()
})

test("put 与 get：按 tripId + photoId 存取 Blob", async function () {
  const blob = { data: "假图片数据", size: 6 }
  await photos.put("t1", "p1", blob)
  const got = await photos.get("t1", "p1")
  assert.ok(got, "应能取回")
  assert.strictEqual(got.blob.data, "假图片数据")
})

test("get：键不存在返回 null", async function () {
  assert.strictEqual(await photos.get("t1", "不存在"), null)
})

test("put：同 tripId 下多个 photoId 互不覆盖", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t1", "p2", { data: "B" })
  assert.strictEqual((await photos.get("t1", "p1")).blob.data, "A")
  assert.strictEqual((await photos.get("t1", "p2")).blob.data, "B")
})

test("put：不同 tripId 下同名 photoId 互不覆盖", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t2", "p1", { data: "B" })
  assert.strictEqual((await photos.get("t1", "p1")).blob.data, "A")
  assert.strictEqual((await photos.get("t2", "p1")).blob.data, "B")
})

test("list：列出某条记录的全部 photoId", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t1", "p2", { data: "B" })
  await photos.put("t2", "p1", { data: "C" })
  const list = await photos.list("t1")
  assert.strictEqual(list.length, 2)
  assert.ok(list.indexOf("p1") >= 0)
  assert.ok(list.indexOf("p2") >= 0)
})

test("delete：删掉指定照片", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.delete("t1", "p1")
  assert.strictEqual(await photos.get("t1", "p1"), null)
})

test("deleteAll：清掉某条记录的全部照片，不影响别的记录", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t2", "p1", { data: "B" })
  await photos.deleteAll("t1")
  assert.strictEqual(await photos.get("t1", "p1"), null)
  assert.ok(await photos.get("t2", "p1"), "别的记录不该被删")
})

test("count：统计全部照片数", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.put("t2", "p1", { data: "B" })
  assert.strictEqual(await photos.count(), 2)
})

test("clear：清空全部", async function () {
  await photos.put("t1", "p1", { data: "A" })
  await photos.clear()
  assert.strictEqual(await photos.count(), 0)
})

test("isAvailable：IndexedDB 存在时返回 true", async function () {
  assert.strictEqual(await photos.isAvailable(), true)
})

test("IndexedDB 不可用时 isAvailable 返回 false，put reject 中文错误", async function () {
  const saved = global.indexedDB
  global.indexedDB = undefined
  // 重新 require 以便重新探测
  delete require.cache[require.resolve("../shared/photos.js")]
  const photos2 = require("../shared/photos.js")
  assert.strictEqual(await photos2.isAvailable(), false)
  await assert.rejects(
    function () { return photos2.put("t1", "p1", { data: "A" }) },
    function (err) {
      assert.ok(err && err.message.indexOf("IndexedDB") >= 0, "错误信息应含 IndexedDB，实际：" + (err && err.message))
      return true
    }
  )
  global.indexedDB = saved
})
```
- [ ] **Step 3: 运行测试确认失败**

Run: `cd apps/web && node tests/photos.test.js`
Expected: FAIL —— 报 `Cannot find module '../shared/photos.js'`

- [ ] **Step 4: 写 `shared/photos.js`**

新建 `apps/web/shared/photos.js`：

```javascript
"use strict"
/**
 * photos.js —— IndexedDB 图片存储层
 *
 * trip-log 的照片原存在腾讯云（storage.uploadFile）。Web 侧没有 openid 也没有
 * 云 SDK，改用 IndexedDB 存 Blob：
 *   - 元数据（title/date/tags…）走 localStorage（db: 前缀）
 *   - 照片走 IndexedDB（存 Blob 二进制）
 *
 * 为什么要 IndexedDB 而不是 localStorage：单条记录最多 9 张 × 5MB = 45MB，
 * 远超 localStorage 的 ~5MB 配额。
 *
 * 键的构造：`<tripId>:<photoId>`，list(tripId) 用前缀匹配筛出该记录的全部照片。
 *
 * 所有方法都是 Promise 化的：IndexedDB 的原生回调 API 太难用。
 */

const DB_NAME = "apps-web-photos"
const DB_VERSION = 1
const STORE_NAME = "photos"

/** 打开数据库的 Promise（同一个库只 open 一次） */
let dbPromise = null

/**
 * IndexedDB 是否可用。
 * @returns {Promise<boolean>}
 */
function isAvailable () {
  return Promise.resolve(typeof indexedDB !== "undefined" && indexedDB !== null)
}

/**
 * 打开（或复用）数据库连接。
 * @returns {Promise<object>} IDBDatabase
 */
function openDb () {
  if (dbPromise) return dbPromise
  dbPromise = new Promise(function (resolve, reject) {
    if (typeof indexedDB === "undefined" || indexedDB === null) {
      reject(new Error("当前环境不支持 IndexedDB"))
      return
    }
    let request
    try {
      request = indexedDB.open(DB_NAME, DB_VERSION)
    } catch (e) {
      reject(new Error("IndexedDB 打开失败：" + (e && e.message ? e.message : e)))
      return
    }
    request.onupgradeneeded = function () {
      const db = request.result
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME, { keyPath: "key" })
      }
    }
    request.onsuccess = function () { resolve(request.result) }
    request.onerror = function () {
      reject(new Error("IndexedDB 打开失败"))
    }
  })
  // 打开失败时不要缓存失败的 promise，否则后续永远失败
  dbPromise.catch(function () { dbPromise = null })
  return dbPromise
}

/**
 * 在一个事务里执行操作。
 * @param {string[]} storeNames
 * @param {string} mode
 * @param {Function} fn 接收 objectStore，返回要等待的结果
 * @returns {Promise<*>}
 */
function withStore (storeNames, mode, fn) {
  return openDb().then(function (db) {
    return new Promise(function (resolve, reject) {
      let tx
      try {
        tx = db.transaction(storeNames, mode)
      } catch (e) {
        reject(new Error("IndexedDB 事务创建失败：" + (e && e.message ? e.message : e)))
        return
      }
      let result
      let failed = false
      tx.oncomplete = function () { if (!failed) resolve(result) }
      tx.onerror = function () {
        failed = true
        reject(new Error("IndexedDB 操作失败"))
      }
      tx.onabort = function () {
        failed = true
        reject(new Error("IndexedDB 操作已中止"))
      }
      try {
        result = fn(tx.objectStore(STORE_NAME))
      } catch (e) {
        failed = true
        reject(new Error("IndexedDB 操作失败：" + (e && e.message ? e.message : e)))
      }
    })
  })
}

/** 构造复合键 */
function makeKey (tripId, photoId) {
  return String(tripId) + ":" + String(photoId)
}

/**
 * 存一张照片。
 * @param {string} tripId
 * @param {string} photoId
 * @param {Blob} blob
 * @returns {Promise<void>}
 */
function put (tripId, photoId, blob) {
  return withStore([STORE_NAME], "readwrite", function (store) {
    store.put({ key: makeKey(tripId, photoId), tripId: String(tripId), photoId: String(photoId), blob: blob })
  }).then(function () {})
}

/**
 * 取一张照片。
 * @param {string} tripId
 * @param {string} photoId
 * @returns {Promise<object|null>} 存的记录（含 blob 字段）；没有则 null
 */
function get (tripId, photoId) {
  return withStore([STORE_NAME], "readonly", function (store) {
    return store.get(makeKey(tripId, photoId))
  }).then(function (row) {
    // IndexedDB 的 get 对不存在的键返回 undefined —— 统一成 null，
    // 调用方用 `if (!row)` 判断即可，不必同时处理两种「空」
    return row === undefined ? null : row
  })
}

/**
 * 删一张照片。
 * @param {string} tripId
 * @param {string} photoId
 * @returns {Promise<void>}
 */
function remove (tripId, photoId) {
  return withStore([STORE_NAME], "readwrite", function (store) {
    store.delete(makeKey(tripId, photoId))
  }).then(function () {})
}

/**
 * 列出某条记录的全部照片 id。
 * @param {string} tripId
 * @returns {Promise<string[]>}
 */
function list (tripId) {
  return withStore([STORE_NAME], "readonly", function (store) {
    return store.getAll()
  }).then(function (all) {
    const prefix = String(tripId) + ":"
    return (all || [])
      .filter(function (r) { return r.key && r.key.indexOf(prefix) === 0 })
      .map(function (r) { return r.photoId })
  })
}

/**
 * 清掉某条记录的全部照片。
 * @param {string} tripId
 * @returns {Promise<void>}
 */
function deleteAll (tripId) {
  return list(tripId).then(function (ids) {
    const jobs = ids.map(function (id) { return remove(tripId, id) })
    return Promise.all(jobs)
  }).then(function () {})
}

/**
 * 统计全部照片数。
 * @returns {Promise<number>}
 */
function count () {
  return withStore([STORE_NAME], "readonly", function (store) {
    return store.count()
  })
}

/**
 * 清空全部照片。
 * @returns {Promise<void>}
 */
function clear () {
  return withStore([STORE_NAME], "readwrite", function (store) {
    store.clear()
  }).then(function () {})
}

module.exports = {
  put: put,
  get: get,
  delete: remove,
  list: list,
  deleteAll: deleteAll,
  count: count,
  clear: clear,
  isAvailable: isAvailable
}
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/photos.test.js`
Expected: PASS —— 12 个用例全过

- [ ] **Step 6: 提交**

```bash
git add apps/web
git commit -m "feat(web): 新增 shared/photos.js —— IndexedDB 图片存储层

trip-log 的照片原存在腾讯云，Web 侧没有 openid 也没有云 SDK，改用
IndexedDB 存 Blob（单条记录最多 9 张 × 5MB = 45MB，远超 localStorage
的 ~5MB 配额）。元数据仍走 localStorage。

键的构造 <tripId>:<photoId>，list 用前缀匹配筛出某条记录的全部照片。
所有方法 Promise 化 —— IndexedDB 的原生回调 API 太难用。"
git push origin main
```

---

## Task 2：`shared/trip/` 数据层（store + format + export）

**Files:**
- Create: `apps/web/shared/trip/format.js`
- Create: `apps/web/shared/trip/store.js`
- Create: `apps/web/shared/trip/export.js`
- Test: `apps/web/tests/trip-store.test.js`
- Test: `apps/web/tests/trip-export.test.js`

**Interfaces:**
- Consumes: 计划一的 `shared/core/storage.js` `shared/core/id.js`；Task 1 的 `shared/photos.js`
- Produces:
  - `shared/trip/format.js` 导出 `pad2` `todayISO` `ymdPretty` `ymdShort` `groupKey` `groupLabel`
  - `shared/trip/store.js` 导出：
    - `list()` → `Promise<Array>`（全部记录，按日期倒序）
    - `get(id)` → `Promise<object|null>`
    - `add(input)` → `Promise<object>`（`input.photos` 是 `[{photoId, blob}]`）
    - `update(id, patch)` → `Promise<object|null>`
    - `remove(id)` → `Promise<boolean>`（同时删掉关联照片）
    - `getPhotoUrls(id)` → `Promise<Array<{photoId, url}>>`（`url` 是 objectURL）
    - `clear()` → `Promise<void>`
  - `shared/trip/export.js` 导出 `exportAll()` → `Promise<string>`（JSON，含照片 base64）、`importAll(json, mode)` → `Promise<{ok, count, error}>`

- [ ] **Step 1: 写 `shared/trip/format.js`**

新建 `apps/web/shared/trip/format.js`：

```javascript
"use strict"
/**
 * format.js —— 日期与字符串格式化
 *
 * 从小程序版 apps/trip-log/utils/format.js 搬来，逻辑一字未改
 * （只是从 2 空格缩进的双引号风格统一成本仓的写法）。
 */

function pad2 (n) {
  return n < 10 ? "0" + n : "" + n
}

function todayISO () {
  const d = new Date()
  return d.getFullYear() + "-" + pad2(d.getMonth() + 1) + "-" + pad2(d.getDate())
}

function ymdPretty (iso) {
  if (!iso) return ""
  const parts = iso.split("-")
  return parts[0] + "年" + parseInt(parts[1], 10) + "月" + parseInt(parts[2], 10) + "日"
}

function ymdShort (iso) {
  if (!iso) return ""
  const parts = iso.split("-")
  return parts[0] + "." + parts[1] + "." + parts[2]
}

function groupKey (iso) {
  if (!iso) return "未知时间"
  const parts = iso.split("-")
  return parts[0] + "-" + parts[1]
}

function groupLabel (iso) {
  if (!iso) return "未知时间"
  const parts = iso.split("-")
  return parts[0] + "年" + parseInt(parts[1], 10) + "月"
}

module.exports = {
  pad2: pad2,
  todayISO: todayISO,
  ymdPretty: ymdPretty,
  ymdShort: ymdShort,
  groupKey: groupKey,
  groupLabel: groupLabel
}
```

- [ ] **Step 2: 写 `shared/trip/store.js`**

新建 `apps/web/shared/trip/store.js`：

```javascript
"use strict"
/**
 * store.js —— trip-log 的数据层
 *
 * 元数据存 localStorage（db: 前缀），照片存 IndexedDB（见 shared/photos.js）。
 *
 * 与小程序版的差异：云端 database() → 本地 storage + photos。
 */

const storage = require("../core/storage.js")
const id = require("../core/id.js")
const photos = require("../photos.js")

const KEY = "trips"

/** 读全部元数据（不含照片二进制） */
function readAll () {
  const list = storage.get(KEY, null)
  return Array.isArray(list) ? list : []
}

/** 写全部元数据 */
function writeAll (list) {
  storage.set(KEY, Array.isArray(list) ? list : [])
}

/**
 * 全部记录，按日期倒序（同日按创建时间倒序）。
 * @returns {Promise<Array>}
 */
async function list () {
  const all = readAll()
  all.sort(function (a, b) {
    if (a.date !== b.date) return a.date < b.date ? 1 : -1
    return (b.createdAt || 0) - (a.createdAt || 0)
  })
  return all
}

/**
 * 取一条记录。
 * @param {string} recordId
 * @returns {Promise<object|null>}
 */
async function get (recordId) {
  const all = readAll()
  for (let i = 0; i < all.length; i++) {
    if (all[i].id === recordId) return all[i]
  }
  return null
}

/**
 * 新增一条记录。
 * @param {{title?: string, date?: string, weather?: string, location?: string,
 *          companions?: string, tags?: Array, notes?: string,
 *          photos?: Array<{photoId: string, blob: object}>}} input
 * @returns {Promise<object>} 新记录（含 id 与 photoIds）
 */
async function add (input) {
  const it = input || {}
  const recordId = it.id || id.genId()

  // 先存照片，拿到成功再写元数据 —— 避免出现「有元数据没照片」的半截状态
  const photoIds = []
  const incoming = Array.isArray(it.photos) ? it.photos : []
  for (const p of incoming) {
    await photos.put(recordId, p.photoId, p.blob)
    photoIds.push(p.photoId)
  }

  const record = {
    id: recordId,
    title: it.title || "",
    date: it.date || "",
    weather: it.weather || "",
    location: it.location || "",
    companions: it.companions || "",
    tags: Array.isArray(it.tags) ? it.tags : [],
    notes: it.notes || "",
    photoIds: photoIds,
    createdAt: Date.now()
  }
  const all = readAll()
  all.push(record)
  writeAll(all)
  return record
}

/**
 * 更新一条记录。patch 里的 photos 同 add 的语义（会替换掉旧的）。
 * @param {string} recordId
 * @param {object} patch
 * @returns {Promise<object|null>}
 */
async function update (recordId, patch) {
  const all = readAll()
  let updated = null
  const next = []
  for (const r of all) {
    if (r.id !== recordId) {
      next.push(r)
      continue
    }
    const merged = Object.assign({}, r, patch || {})
    if (Array.isArray(patch && patch.photos)) {
      // 换了照片：先删旧的，再存新的
      await photos.deleteAll(recordId)
      merged.photoIds = []
      for (const p of patch.photos) {
        await photos.put(recordId, p.photoId, p.blob)
        merged.photoIds.push(p.photoId)
      }
      delete merged.photos
    }
    updated = merged
    next.push(merged)
  }
  if (updated) writeAll(next)
  return updated
}

/**
 * 删除一条记录，连同它的照片。
 * @param {string} recordId
 * @returns {Promise<boolean>} 是否真的删掉了
 */
async function remove (recordId) {
  const all = readAll()
  const next = all.filter(function (r) { return r.id !== recordId })
  const changed = next.length !== all.length
  if (!changed) return false
  await photos.deleteAll(recordId)
  writeAll(next)
  return true
}

/**
 * 取一条记录的照片 URL 列表。
 *
 * 返回 objectURL，**调用方负责在用完后 revokeObjectURL** —— 否则内存泄漏。
 *
 * @param {string} recordId
 * @returns {Promise<Array<{photoId: string, url: string}>>}
 */
async function getPhotoUrls (recordId) {
  const record = await get(recordId)
  if (!record) return []
  const ids = Array.isArray(record.photoIds) ? record.photoIds : []
  const out = []
  for (const photoId of ids) {
    const row = await photos.get(recordId, photoId)
    if (!row || !row.blob) continue
    out.push({ photoId: photoId, url: URL.createObjectURL(row.blob) })
  }
  return out
}

/**
 * 清空全部记录与照片。
 * @returns {Promise<void>}
 */
async function clear () {
  const all = readAll()
  for (const r of all) {
    await photos.deleteAll(r.id)
  }
  writeAll([])
  await photos.clear()
}

module.exports = {
  list: list,
  get: get,
  add: add,
  update: update,
  remove: remove,
  getPhotoUrls: getPhotoUrls,
  clear: clear
}
```

- [ ] **Step 3: 写 `shared/trip/export.js`**

新建 `apps/web/shared/trip/export.js`：

```javascript
"use strict"
/**
 * export.js —— trip-log 的数据导出与导入
 *
 * 放弃腾讯云同步后的补偿手段：把全部记录（含照片）导成一个 JSON 文件，
 * 用户可以自己存到别处，必要时再导入回来。
 *
 * 照片以 base64 嵌进 JSON —— 文件会比较大，但换来了「一个文件就是全部数据」。
 */

const store = require("./store.js")
const photos = require("../photos.js")

/**
 * Blob → base64 字符串。
 * @param {Blob} blob
 * @returns {Promise<string>}
 */
function blobToBase64 (blob) {
  return new Promise(function (resolve, reject) {
    if (typeof FileReader === "undefined") {
      // Node 测试环境：blob 是普通对象，直接取 data
      resolve(blob && blob.data !== undefined ? String(blob.data) : "")
      return
    }
    const reader = new FileReader()
    reader.onload = function () {
      const s = String(reader.result || "")
      // 去掉 "data:...;base64," 前缀
      const comma = s.indexOf(",")
      resolve(comma >= 0 ? s.slice(comma + 1) : s)
    }
    reader.onerror = function () { reject(new Error("读取图片失败")) }
    reader.readAsDataURL(blob)
  })
}

/**
 * base64 字符串 → Blob。
 * @param {string} base64
 * @returns {Promise<object>}
 */
function base64ToBlob (base64) {
  const raw = String(base64 || "")
  if (typeof Blob === "undefined") {
    // Node 测试环境
    return Promise.resolve({ data: raw, size: raw.length })
  }
  const binary = atob(raw)
  const bytes = new Uint8Array(binary.length)
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
  return Promise.resolve(new Blob([bytes]))
}

/**
 * 导出全部数据为 JSON 字符串。
 * @returns {Promise<string>}
 */
async function exportAll () {
  const records = await store.list()
  const out = []
  for (const r of records) {
    const ids = Array.isArray(r.photoIds) ? r.photoIds : []
    const photoList = []
    for (const photoId of ids) {
      const row = await photos.get(r.id, photoId)
      if (!row || !row.blob) continue
      photoList.push({ photoId: photoId, base64: await blobToBase64(row.blob) })
    }
    out.push({
      id: r.id,
      title: r.title,
      date: r.date,
      weather: r.weather,
      location: r.location,
      companions: r.companions,
      tags: r.tags,
      notes: r.notes,
      createdAt: r.createdAt,
      photos: photoList
    })
  }
  return JSON.stringify({
    exportedAt: new Date().toISOString(),
    version: 1,
    data: out
  }, null, 2)
}

/**
 * 从 JSON 导入。
 * @param {string} json
 * @param {'merge'|'overwrite'} mode
 * @returns {Promise<{ok: boolean, count: number, error?: string}>}
 */
async function importAll (json, mode) {
  const m = mode || "merge"
  try {
    const parsed = JSON.parse(json)
    const data = parsed && parsed.data
    if (!Array.isArray(data)) {
      return { ok: false, count: 0, error: "备份格式不正确：缺少 data 数组" }
    }
    if (m === "overwrite") {
      await store.clear()
    }
    let count = 0
    for (const r of data) {
      if (!r || !r.id) continue
      const photoList = Array.isArray(r.photos) ? r.photos : []
      const blobs = []
      for (const p of photoList) {
        blobs.push({ photoId: p.photoId, blob: await base64ToBlob(p.base64) })
      }
      await store.add({
        id: r.id,
        title: r.title,
        date: r.date,
        weather: r.weather,
        location: r.location,
        companions: r.companions,
        tags: r.tags,
        notes: r.notes,
        photos: blobs
      })
      count += 1
    }
    return { ok: true, count: count }
  } catch (err) {
    return { ok: false, count: 0, error: (err && err.message) || String(err) }
  }
}

module.exports = {
  exportAll: exportAll,
  importAll: importAll,
  blobToBase64: blobToBase64,
  base64ToBlob: base64ToBlob
}
```

- [ ] **Step 4: 写失败测试**

新建 `apps/web/tests/trip-store.test.js`：

```javascript
"use strict"
/**
 * trip-store.test.js —— trip-log 的数据层
 *
 * trip-log 原先没有测试（云端逻辑没法在 Node 跑），本文件是新增的。
 */

const test = require("node:test")
const assert = require("node:assert")

// 复用 photos.test.js 的 IndexedDB 替身
const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

const storage = require("../shared/core/storage.js")
const photos = require("../shared/photos.js")
const store = require("../shared/trip/store.js")

test.beforeEach(async function () {
  storage.clearAll()
  await photos.clear()
})

test("add：写入元数据并返回带 id 的记录", async function () {
  const r = await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  assert.ok(r.id, "应生成 id")
  assert.strictEqual(r.title, "西湖")
  assert.strictEqual(r.location, "杭州")
  assert.strictEqual(r.photoIds.length, 0)
})

test("add：带照片时照片进 IndexedDB，元数据只记 photoId", async function () {
  const r = await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  assert.deepStrictEqual(r.photoIds, ["p1"])
  const row = await photos.get(r.id, "p1")
  assert.ok(row, "照片应在 IndexedDB 里")
  assert.strictEqual(row.blob.data, "IMG1")
})

test("list：按日期倒序", async function () {
  await store.add({ title: "早", date: "2026-01-01" })
  await store.add({ title: "晚", date: "2026-12-31" })
  const list = await store.list()
  assert.strictEqual(list.length, 2)
  assert.strictEqual(list[0].title, "晚")
})

test("get：按 id 取记录，取不到返回 null", async function () {
  const r = await store.add({ title: "X", date: "2026-11-20" })
  assert.strictEqual((await store.get(r.id)).title, "X")
  assert.strictEqual(await store.get("不存在"), null)
})

test("update：改字段并保留未改的字段", async function () {
  const r = await store.add({ title: "旧标题", date: "2026-11-20", location: "杭州" })
  const u = await store.update(r.id, { title: "新标题" })
  assert.strictEqual(u.title, "新标题")
  assert.strictEqual(u.location, "杭州", "未改的字段应保留")
})

test("update：改不存在的记录返回 null", async function () {
  assert.strictEqual(await store.update("不存在", { title: "X" }), null)
})

test("remove：删记录并连带删掉照片", async function () {
  const r = await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  assert.strictEqual(await photos.get(r.id, "p1") !== null, true)
  assert.strictEqual(await store.remove(r.id), true)
  assert.strictEqual(await store.get(r.id), null)
  assert.strictEqual(await photos.get(r.id, "p1"), null, "照片应一并删掉")
})

test("remove：删不存在的记录返回 false", async function () {
  assert.strictEqual(await store.remove("不存在"), false)
})

test("getPhotoUrls：返回照片列表", async function () {
  const r = await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  const urls = await store.getPhotoUrls(r.id)
  assert.strictEqual(urls.length, 1)
  assert.strictEqual(urls[0].photoId, "p1")
})

test("clear：清掉全部记录与照片", async function () {
  await store.add({ title: "A", date: "2026-11-20", photos: [{ photoId: "p1", blob: { data: "X" } }] })
  await store.clear()
  assert.strictEqual((await store.list()).length, 0)
  assert.strictEqual(await photos.count(), 0)
})
```

> `tests/fake-idb.js`（IndexedDB 内存替身）已在 Task 1 Step 1 建好，本步直接 require 它 —— **不要重复实现**。


新建 `apps/web/tests/trip-export.test.js`：

```javascript
"use strict"
/**
 * trip-export.test.js —— 导出导入往返
 */

const test = require("node:test")
const assert = require("node:assert")

const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

const storage = require("../shared/core/storage.js")
const photos = require("../shared/photos.js")
const store = require("../shared/trip/store.js")
const exporter = require("../shared/trip/export.js")

test.beforeEach(async function () {
  storage.clearAll()
  await photos.clear()
})

test("exportAll：导出成 JSON 字符串", async function () {
  await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  const json = await exporter.exportAll()
  const parsed = JSON.parse(json)
  assert.strictEqual(parsed.version, 1)
  assert.strictEqual(parsed.data.length, 1)
  assert.strictEqual(parsed.data[0].title, "西湖")
  assert.ok(parsed.exportedAt)
})

test("exportAll：照片被 base64 嵌进去", async function () {
  await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  const parsed = JSON.parse(await exporter.exportAll())
  assert.strictEqual(parsed.data[0].photos.length, 1)
  assert.strictEqual(parsed.data[0].photos[0].photoId, "p1")
  assert.ok(parsed.data[0].photos[0].base64 !== undefined)
})

test("importAll：导出再导入，数据一致", async function () {
  await store.add({ title: "西湖", date: "2026-11-20", location: "杭州" })
  const json = await exporter.exportAll()

  storage.clearAll()
  await photos.clear()
  assert.strictEqual((await store.list()).length, 0)

  const r = await exporter.importAll(json, "overwrite")
  assert.strictEqual(r.ok, true)
  assert.strictEqual(r.count, 1)

  const list = await store.list()
  assert.strictEqual(list.length, 1)
  assert.strictEqual(list[0].title, "西湖")
  assert.strictEqual(list[0].location, "杭州")
})

test("importAll：照片也能往返", async function () {
  await store.add({
    title: "带照片",
    date: "2026-11-20",
    photos: [{ photoId: "p1", blob: { data: "IMG1" } }]
  })
  const json = await exporter.exportAll()

  storage.clearAll()
  await photos.clear()

  await exporter.importAll(json, "overwrite")
  const list = await store.list()
  assert.strictEqual(list[0].photoIds.length, 1)
  const row = await photos.get(list[0].id, "p1")
  assert.ok(row, "照片应恢复")
})

test("importAll：坏 JSON 返回中文错误不抛异常", async function () {
  const r = await exporter.importAll("{ 这不是 JSON", "overwrite")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error && r.error.length > 0)
})

test("importAll：缺 data 数组返回中文错误", async function () {
  const r = await exporter.importAll(JSON.stringify({ nope: 1 }), "overwrite")
  assert.strictEqual(r.ok, false)
  assert.ok(r.error.indexOf("data") >= 0, "错误应指出 data，实际：" + r.error)
})
```

- [ ] **Step 5: 运行测试确认失败**

Run:
```bash
cd apps/web && node tests/trip-store.test.js && node tests/trip-export.test.js
```
Expected: FAIL —— 报模块找不到

- [ ] **Step 6: 运行测试确认通过**

Run:
```bash
cd apps/web && node tests/trip-store.test.js && node tests/trip-export.test.js
```
Expected: PASS —— 分别 9 个与 6 个用例全过

> `getPhotoUrls` 在 Node 下会调 `URL.createObjectURL` —— Node 18+ 有这个 API，但只接受 Blob。我们的替身 blob 是普通对象，会抛错。**在 `store.js` 的 `getPhotoUrls` 里加一层保护**：blob 不是 Blob 时返回空数组或占位 URL。

在 `store.js` 的 `getPhotoUrls` 里，把 `out.push(...)` 改成：

```javascript
    let url = ""
    try {
      url = URL.createObjectURL(row.blob)
    } catch (e) {
      // Node 测试环境下 blob 不是真正的 Blob，URL.createObjectURL 会抛错
      url = ""
    }
    out.push({ photoId: photoId, url: url })
```

- [ ] **Step 7: 提交**

```bash
git add apps/web
git commit -m "feat(web): 新增 shared/trip 数据层（store + format + export）

trip-log 原先把记录与照片都放腾讯云，Web 侧改为：
元数据走 localStorage（db:trips），照片走 IndexedDB。
store 提供 list/get/add/update/remove/getPhotoUrls/clear，
remove 会连带删掉关联照片。

export.js 提供 JSON 导出导入（含照片 base64）—— 放弃云同步后的补偿手段，
让用户能自己把数据存到别处再导回来。

trip-log 原先没有测试，本计划新增 fake-idb.js（IndexedDB 内存替身）
与两个测试文件。"
git push origin main
```

---

## Task 3：games 数据层 —— 搬 8 个 utils 文件 + 迁入 51 个引擎单测

**Files:**
- Create: `apps/web/shared/games/storage.js`
- Create: `apps/web/shared/games/vibrate.js`
- Create: `apps/web/shared/games/game2048.js`
- Create: `apps/web/shared/games/snake.js`
- Create: `apps/web/shared/games/sudoku.js`
- Create: `apps/web/shared/games/match3.js`
- Create: `apps/web/shared/games/achievements.js`
- Create: `apps/web/shared/games/share.js`
- Create: `apps/web/tests/games-engine.test.js`
- Modify: `apps/web/shared/platform.js`（补 `vibrateLong`）

**Interfaces:**
- Consumes: 计划二的 `shared/platform.js`
- Produces:
  - `shared/games/storage.js` 导出与小程序版**完全相同**的 API（`getSnakeBestScore` `updateSnakeBestScore` 等，见源文件）
  - `shared/games/vibrate.js` 导出 `trigger(type)`
  - `shared/games/{game2048,snake,sudoku,match3}.js` 导出与小程序版**完全相同**
  - `shared/games/achievements.js` 导出与小程序版**完全相同**（只改 require 路径）
  - `shared/games/share.js` 导出与小程序版**完全相同**
  - `shared/platform.js` 新增 `vibrateLong(opts)`

- [ ] **Step 1: 给 `platform.js` 补 `vibrateLong`**

在 `apps/web/shared/platform.js` 的 `vibrateShort` 之后插入：

```javascript
  /** 震动时长（毫秒），与 wx.vibrateShort 的 type 对应 */
  const VIBRATE_MS = { light: 15, medium: 40, heavy: 80 }

  /** 长震动时长（毫秒），与 wx.vibrateLong 对应 */
  const VIBRATE_LONG_MS = 400
```

（若 `VIBRATE_MS` 已存在，不要重复声明。）

并在 `vibrateShort` 之后加：

```javascript
  /**
   * 长震动。设备不支持时静默返回（不打断交互）。
   * @param {{type?: string}} [opts]
   */
  function vibrateLong (opts) {
    try {
      if (typeof navigator === "undefined" || typeof navigator.vibrate !== "function") return false
      return navigator.vibrate(VIBRATE_LONG_MS)
    } catch (e) {
      return false
    }
  }
```

在返回对象里补上：

```javascript
    vibrateLong: vibrateLong,
```

在 `apps/web/tests/platform.test.js` 末尾追加：

```javascript
test("vibrateLong：没有 navigator.vibrate 时静默不抛异常", function () {
  global.navigator = {}
  platform.vibrateLong()
})

test("vibrateLong：有 navigator.vibrate 时调用它", function () {
  const calls = []
  global.navigator = { vibrate: function (p) { calls.push(p) } }
  platform.vibrateLong()
  assert.strictEqual(calls.length, 1)
  assert.ok(calls[0] >= 200, "长震动时长应明显大于短震动，实际 " + calls[0])
})
```

Run: `cd apps/web && node tests/platform.test.js`
Expected: PASS —— 20 个用例全过（原 18 + 新 2）

- [ ] **Step 2: 建目录并搬 8 个文件**

```bash
mkdir -p apps/web/shared/games
cp apps/games/utils/game2048.js   apps/web/shared/games/game2048.js
cp apps/games/utils/snake.js       apps/web/shared/games/snake.js
cp apps/games/utils/sudoku.js      apps/web/shared/games/sudoku.js
cp apps/games/utils/match3.js      apps/web/shared/games/match3.js
cp apps/games/utils/achievements.js apps/web/shared/games/achievements.js
cp apps/games/utils/share.js       apps/web/shared/games/share.js
cp apps/games/utils/storage.js     apps/web/shared/games/storage.js
cp apps/games/utils/vibrate.js     apps/web/shared/games/vibrate.js
```

**这 6 个文件一字不改**：`game2048.js` `snake.js` `sudoku.js` `match3.js` `share.js`（零依赖、零 wx）。

`achievements.js` 只改一处 require：

```javascript
// 原：const storage = require('./storage.js')
// 改：const storage = require('./storage.js')  ← 同目录，路径不变，无需改
```

> `achievements.js` 依赖 `./storage.js`，而两个文件都在 `shared/games/` 下，**路径不变，无需改**。

- [ ] **Step 3: 改 `shared/games/storage.js` 的底层**

`apps/games/utils/storage.js` 里有 8 处 wx 调用。**只换底层，键名与逻辑一字不改**。

原代码（文件头）：
```javascript
const STORAGE_PREFIX = 'games_'
const CURRENT_VERSION = 1
const VERSION_KEY = STORAGE_PREFIX + 'version'
```

原代码（读）：
```javascript
function getData (game, defaultValue) {
  try {
    const raw = wx.getStorageSync(getKey(game))
    if (raw) return typeof raw === 'string' ? JSON.parse(raw) : raw
  } catch (e) {
    console.warn('[storage] 读取 ' + game + ' 失败:', e)
  }
  return defaultValue
}
```

原代码（写）：
```javascript
function setData (game, data) {
  try {
    wx.setStorageSync(getKey(game), JSON.stringify(data))
    return true
  } catch (e) {
    console.warn('[storage] 保存 ' + game + ' 失败:', e)
    return false
  }
}
```

改成（底层换成 `localStorage`，**键名 `games_` 前缀保留**）：
```javascript
/**
 * 读取（JSON 解析，失败回退默认值）
 *
 * 底层从 wx.getStorageSync 换成 localStorage。键名保留 games_ 前缀，
 * 与其它模块的 db: 前缀不冲突。
 */
function getData (game, defaultValue) {
  try {
    const raw = localStorage.getItem(getKey(game))
    if (raw === null || raw === undefined || raw === '') return defaultValue
    return typeof raw === 'string' ? JSON.parse(raw) : raw
  } catch (e) {
    console.warn('[storage] 读取 ' + game + ' 失败:', e)
  }
  return defaultValue
}

/**
 * 写入（JSON 序列化）
 */
function setData (game, data) {
  try {
    localStorage.setItem(getKey(game), JSON.stringify(data))
    return true
  } catch (e) {
    console.warn('[storage] 保存 ' + game + ' 失败:', e)
    return false
  }
}
```

**还要改的地方**（搜 `wx.` 逐个替换）：

| 原 | 改 |
|---|---|
| `wx.getStorageSync(k)` | `localStorage.getItem(k)` |
| `wx.setStorageSync(k, v)` | `localStorage.setItem(k, v)` |
| `wx.removeStorageSync(k)` | `localStorage.removeItem(k)` |
| `wx.getStorageInfoSync()` | 见下方「清空全部」 |
| `wx.clearStorageSync()` | `localStorage.clear()` |

`clearAll` 那段（如果源文件里有）改成：

```javascript
/** 清空全部游戏存档（只清 games_ 前缀的键） */
function clearAll () {
  try {
    const keys = []
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i)
      if (k !== null && k.indexOf(STORAGE_PREFIX) === 0) keys.push(k)
    }
    keys.forEach(function (k) { localStorage.removeItem(k) })
    return true
  } catch (e) {
    console.warn('[storage] 清空失败:', e)
    return false
  }
}
```

> `localStorage` **只能存字符串**，所以 `getData` 里「键不存在」返回 `null`（不是 `''`）—— 上面的代码已经归一了。**这一点与小程序版有细微差别**：`wx.getStorageSync` 不存在时返回 `''`，`if (raw)` 会跳过；`localStorage` 返回 `null`，`if (raw === null ...)` 同样跳过。**语义一致**。

- [ ] **Step 4: 改 `shared/games/vibrate.js`**

新建 `apps/web/shared/games/vibrate.js`：

```javascript
"use strict"
/**
 * vibrate.js —— 带节流的振动反馈
 *
 * 从小程序版搬来，节流逻辑一字不改。底层从 wx.vibrateShort 换成
 * platform.vibrateShort（后者在设备不支持时静默返回）。
 */

const platform = require("../platform.js")

let lastAt = 0

/**
 * 触发短振动（默认 80ms 节流，避免连续触发）
 * @param {string} type light | medium | heavy
 */
function trigger (type) {
  const now = Date.now()
  if (now - lastAt < 80) return
  lastAt = now
  platform.vibrateShort({ type: type || "light" })
}

module.exports = { trigger: trigger }
```

- [ ] **Step 5: 迁入 `engine.test.js`**

```bash
cp apps/games/tests/engine.test.js apps/web/tests/games-engine.test.js
```

改两处：

**改 1 —— mock 换成 localStorage 替身：**

原代码：
```javascript
const store = {}
global.wx = {
  getStorageSync: k => store[k],
  setStorageSync: (k, v) => { store[k] = v },
  removeStorageSync: k => { delete store[k] },
  vibrateShort: () => {},
  vibrateLong: () => {}
}
```

改成：
```javascript
// localStorage 替身（内存版）
const mem = new Map()
global.localStorage = {
  getItem: k => (mem.has(k) ? mem.get(k) : null),
  setItem: (k, v) => { mem.set(k, String(v)) },
  removeItem: k => { mem.delete(k) },
  clear: () => { mem.clear() },
  key: i => {
    const keys = Array.from(mem.keys())
    return i < keys.length ? keys[i] : null
  },
  get length () { return mem.size }
}
```

> 原来的 mock 里 `getStorageSync: k => store[k]` 对不存在的键返回 `undefined`；新替身返回 `null`。**两者在 `if (raw)` 判断下等价**。

**改 2 —— require 路径：**

| 原 | 新 |
|---|---|
| `require('../utils/game2048.js')` | `require('../shared/games/game2048.js')` |
| `require('../utils/snake.js')` | `require('../shared/games/snake.js')` |
| `require('../utils/sudoku.js')` | `require('../shared/games/sudoku.js')` |
| `require('../utils/match3.js')` | `require('../shared/games/match3.js')` |
| `require('../utils/storage.js')` | `require('../shared/games/storage.js')` |
| `require('../utils/achievements.js')` | `require('../shared/games/achievements.js')` |

> **迁移纪律**：51 个用例**只改 mock 与 require 路径，不许删用例、不许改断言值**。

- [ ] **Step 6: 运行测试确认通过**

Run: `cd apps/web && node tests/games-engine.test.js`
Expected: PASS —— **51 个用例全过**

- [ ] **Step 7: 提交**

```bash
git add apps/web
git commit -m "feat(web): 搬 games 的 8 个 utils 文件，迁入 51 个引擎单测

game2048 / snake / sudoku / match3 / share 五个文件零 wx 依赖，一字不改。
achievements 只依赖同目录的 storage.js，路径不变。
storage.js 底层从 wx.setStorageSync 换成 localStorage，键名 games_ 前缀
保留（与其它模块的 db: 前缀不冲突）。
vibrate.js 换用 platform.vibrateShort，节流逻辑一字不改。

platform.js 补 vibrateLong（games 的 snake 页用到）。

engine.test.js 的 51 个用例原样迁入，只改 mock 与 require 路径。"
git push origin main
```

---

## Task 4：trip 视图层（3 页）

**Files:**
- Create: `apps/web/web/trip/pages/index.js`
- Create: `apps/web/web/trip/pages/new-trip.js`
- Create: `apps/web/web/trip/pages/detail.js`
- Create: `apps/web/web/trip/router.js`
- Create: `apps/web/web/trip/style.css`
- Test: `apps/web/tests/trip-view.test.js`

**Interfaces:**
- Consumes: Task 2 的 `shared/trip/store.js` `format.js` `export.js`；计划一的 `shared/ui.js` `shared/platform.js`
- Produces:
  - 3 个页面模块：`{ data, onMount?, render(state) → HTMLElement }`，用 `global.__TRIP_PAGE__({...})` 注册
  - `web/trip/router.js` 导出 `ROUTES`（3 条）与 `resolve(pathname)`

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/trip-view.test.js`：

```javascript
"use strict"
/**
 * trip-view.test.js —— trip 视图层
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const fakeIdb = require("./fake-idb.js")
fakeIdb.install()

const storage = require("../shared/core/storage.js")
const photos = require("../shared/photos.js")

const PAGES = [
  "pages/index",
  "pages/new-trip",
  "pages/detail"
]

function loadPage (route) {
  const path = require("node:path")
  const abs = path.resolve(__dirname, "..", "web", "trip", route + ".js")
  let config = null
  global.__TRIP_PAGE__ = function (o) { config = o }
  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__TRIP_PAGE__
  }
  if (!config) throw new Error("页面未调用 __TRIP_PAGE__(): " + route)
  return config
}

test.beforeEach(async function () {
  storage.clearAll()
  await photos.clear()
})

test("3 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都产出节点且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("渲染结果不含 undefined / NaN", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    page.state = Object.assign({}, page.data)
    const text = page.render(page.state).textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 3 个页面", function () {
  const { ROUTES } = require("../web/trip/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 3)
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/trip-view.test.js`
Expected: FAIL —— 页面模块不存在

- [ ] **Step 3: 写 `web/trip/router.js`**

新建 `apps/web/web/trip/router.js`：

```javascript
"use strict"
/**
 * router.js —— trip 模块内的路由表
 *
 * 路径沿用小程序的页面路径，挂在 /trip/ 前缀下。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/index": function () { return require("./pages/index.js") },
  "/pages/new-trip": function () { return require("./pages/new-trip.js") },
  "/pages/detail": function () { return require("./pages/detail.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /trip/pages/detail?id=xxx
 * @returns {string|null}
 */
function resolve (pathname) {
  const prefix = "/trip"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  const stripped = clean.split("?")[0]
  if (ROUTES[stripped]) return stripped
  return null
}

/**
 * 取 URL 上的查询参数。
 * @param {string} pathname
 * @returns {object} 键值对
 */
function query (pathname) {
  const out = {}
  if (typeof pathname !== "string") return out
  const q = pathname.indexOf("?")
  if (q < 0) return out
  const params = new URLSearchParams(pathname.slice(q + 1))
  params.forEach(function (v, k) { out[k] = v })
  return out
}

module.exports = { ROUTES: ROUTES, resolve: resolve, query: query }
```

- [ ] **Step 4: 写三个页面**

**`web/trip/pages/index.js`（足迹列表）：**

```javascript
"use strict"
/**
 * index —— 足迹列表
 *
 * 按月份分组展示全部记录。数据层在 shared/trip/store.js。
 */

const store = require("../../../shared/trip/store.js")
const format = require("../../../shared/trip/format.js")
const ui = require("../../../shared/ui.js")

global.__TRIP_PAGE__({
  data: {
    title: "足迹",
    groups: [],
    total: 0,
    loading: false
  },

  /** 挂载：读数据 */
  onMount: function () {
    this.state = Object.assign({}, this.state, buildModel())
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const model = buildModel()

    const groups = model.groups.map(function (g) {
      const rows = g.items.map(function (t) {
        return ui.el("a", { class: "trip-row", href: "/trip/pages/detail?id=" + t.id }, [
          ui.el("span", { class: "trip-row-date", text: t.dateShort }),
          ui.el("span", { class: "trip-row-title", text: t.title }),
          ui.el("span", { class: "trip-row-loc", text: t.location })
        ])
      })
      return ui.panel({ title: g.label, children: rows })
    })

    return ui.el("div", { class: "trip-page" }, [
      ui.pageHead({
        eyebrow: "足迹",
        title: "共 " + model.total + " 条记录",
        sub: "记录走过的地方"
      }),
      ui.hero({
        title: "添加一条记录",
        sub: "标题与日期必填，照片可选",
        children: [
          ui.el("a", { class: "trip-btn primary", href: "/trip/pages/new-trip", text: "新增记录" })
        ]
      }),
      model.total === 0
        ? ui.panel({ children: [ui.empty("还没有足迹", "点上面的按钮记下第一条")] })
        : ui.el("div", {}, groups)
    ])
  }
})

/**
 * 取数据并按月份分组。
 * @returns {{groups: Array, total: number}}
 */
function buildModel () {
  const records = store.listSync ? store.listSync() : []
  const byKey = {}
  const order = []
  for (const r of records) {
    const key = format.groupKey(r.date)
    if (!byKey[key]) {
      byKey[key] = { key: key, label: format.groupLabel(r.date), items: [] }
      order.push(key)
    }
    byKey[key].items.push({
      id: r.id,
      title: r.title || "未命名",
      location: r.location || "",
      dateShort: format.ymdShort(r.date)
    })
  }
  return {
    groups: order.map(function (k) { return byKey[k] }),
    total: records.length
  }
}
```

> **注意**：`store.list()` 是异步的（因为要读照片）。但**列表页不需要照片二进制**，只需元数据 —— 所以 `store.js` 应该额外导出一个**同步**的 `listSync()`，只读 localStorage。
>
> 在 `shared/trip/store.js` 里加：
>
> ```javascript
> /**
> * 同步取全部记录（只读 localStorage，不碰照片）。
> * 列表页用这个 —— 渲染不必等 IndexedDB。
> * @returns {Array}
> */
> function listSync () {
>   const all = readAll()
>   all.sort(function (a, b) {
>     if (a.date !== b.date) return a.date < b.date ? 1 : -1
>     return (b.createdAt || 0) - (a.createdAt || 0)
>   })
>   return all
> }
> ```
>
> 并在 `module.exports` 里加上 `listSync: listSync`。
>
> `list()` 改成内部调 `listSync()`：
>
> ```javascript
> async function list () {
>   return listSync()
> }
> ```

**`web/trip/pages/new-trip.js`（新增记录）：**

```javascript
"use strict"
/**
 * new-trip —— 新增记录
 *
 * 表单 + 选图。照片用 <input type="file"> 选，存 IndexedDB。
 */

const store = require("../../../shared/trip/store.js")
const format = require("../../../shared/trip/format.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const MAX_PHOTOS = 9

global.__TRIP_PAGE__({
  data: {
    title: "新增记录",
    form: {
      title: "",
      date: "",
      weather: "",
      location: "",
      companions: "",
      tagsText: "",
      notes: ""
    },
    files: [],
    saving: false
  },

  onMount: function () {
    this.state = Object.assign({}, this.state, {
      form: Object.assign({}, this.state.form, { date: format.todayISO() })
    })
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const form = state.form || {}

    const field = function (label, name, value, placeholder) {
      return ui.el("label", { class: "trip-field" }, [
        ui.el("span", { class: "trip-field-label", text: label }),
        ui.el("input", {
          class: "trip-input",
          "data-field": name,
          "data-action": "input",
          placeholder: placeholder || "",
          value: value || ""
        })
      ])
    }

    const photos = (state.files || []).map(function (f, i) {
      return ui.el("div", { class: "trip-photo" }, [
        ui.el("img", { class: "trip-photo-img", src: f.preview }),
        ui.el("button", {
          class: "trip-photo-del",
          "data-action": "remove-photo",
          "data-index": String(i),
          text: "×"
        })
      ])
    })

    return ui.el("div", { class: "trip-page" }, [
      ui.pageHead({ title: "新增记录", sub: "标题与日期必填，照片可选" }),
      ui.panel({
        title: "基本信息",
        children: [
          ui.el("div", { class: "trip-form" }, [
            field("标题", "title", form.title, "去了哪儿"),
            field("日期", "date", form.date, "2026-11-20"),
            field("地点", "location", form.location, "杭州 · 西湖"),
            field("天气", "weather", form.weather, "晴"),
            field("同行", "companions", form.companions, "谁一起"),
            field("标签", "tagsText", form.tagsText, "逗号分隔，如：出差,亲子"),
            ui.el("label", { class: "trip-field" }, [
              ui.el("span", { class: "trip-field-label", text: "备注" }),
              ui.el("textarea", {
                class: "trip-textarea",
                "data-field": "notes",
                "data-action": "input",
                placeholder: "随便写点什么",
                text: form.notes || ""
              })
            ])
          ])
        ]
      }),
      ui.panel({
        title: "照片（最多 " + MAX_PHOTOS + " 张）",
        children: [
          ui.el("div", { class: "trip-photos" }, photos),
          ui.el("input", {
            class: "trip-file",
            type: "file",
            accept: "image/*",
            multiple: "multiple",
            "data-action": "pick-photo"
          })
        ]
      }),
      ui.el("div", { class: "trip-actions" }, [
        ui.el("a", { class: "trip-btn", href: "/trip/pages/index", text: "取消" }),
        ui.el("button", {
          class: "trip-btn primary",
          "data-action": "save",
          text: state.saving ? "保存中…" : "保存"
        })
      ])
    ])
  },

  /** 表单输入 */
  onInput: function (field, value) {
    this.state.form = Object.assign({}, this.state.form)
    this.state.form[field] = value
  },

  /** 选图 */
  onPickPhotos: function (files) {
    const current = this.state.files || []
    const room = MAX_PHOTOS - current.length
    if (room <= 0) {
      platform.showToast({ title: "最多 " + MAX_PHOTOS + " 张", icon: "none" })
      return
    }
    const accepted = []
    for (const f of files) {
      if (accepted.length >= room) break
      if (f.size > 5 * 1024 * 1024) {
        platform.showToast({ title: "有照片超过 5MB，已跳过", icon: "none" })
        continue
      }
      accepted.push({ file: f, preview: URL.createObjectURL(f) })
    }
    this.state.files = current.concat(accepted)
  },

  /** 移除一张照片 */
  onRemovePhoto: function (index) {
    const files = (this.state.files || []).slice()
    const removed = files.splice(index, 1)[0]
    if (removed && removed.preview) URL.revokeObjectURL(removed.preview)
    this.state.files = files
  },

  /** 保存 */
  onSave: function () {
    const form = this.state.form || {}
    const title = String(form.title || "").trim()
    const date = String(form.date || "").trim()
    if (!title) {
      platform.showToast({ title: "请填写标题", icon: "none" })
      return
    }
    if (!date) {
      platform.showToast({ title: "请填写日期", icon: "none" })
      return
    }
    this.state.saving = true

    const tags = String(form.tagsText || "").split(/[,，]/).map(function (s) { return s.trim() }).filter(Boolean)
    const photos = (this.state.files || []).map(function (f, i) {
      return { photoId: "p" + i + "-" + Date.now(), blob: f.file }
    })

    store.add({
      title: title,
      date: date,
      weather: String(form.weather || "").trim(),
      location: String(form.location || "").trim(),
      companions: String(form.companions || "").trim(),
      tags: tags,
      notes: String(form.notes || "").trim(),
      photos: photos
    }).then(function () {
      platform.showToast({ title: "已保存", icon: "success" })
      platform.navigateTo("/trip/pages/index")
    }).catch(function (err) {
      this.state.saving = false
      platform.showToast({ title: "保存失败：" + (err && err.message ? err.message : "未知错误"), icon: "none" })
    })
  }
})
```

**`web/trip/pages/detail.js`（详情）：**

```javascript
"use strict"
/**
 * detail —— 足迹详情
 *
 * 读记录 + 照片，照片用 objectURL 预览，删除时连带删照片。
 */

const store = require("../../../shared/trip/store.js")
const format = require("../../../shared/trip/format.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

global.__TRIP_PAGE__({
  data: {
    title: "详情",
    trip: null,
    photos: [],
    loading: false
  },

  /**
   * 渲染。
   * @param {object} state
   * @returns {object} DOM 节点
   */
  render: function (state) {
    const t = state.trip
    if (!t) {
      return ui.el("div", { class: "trip-page" }, [
        ui.pageHead({ title: "详情" }),
        ui.panel({ children: [ui.empty("记录不存在或已删除", "返回列表看看别的")] })
      ])
    }

    const photos = (state.photos || []).map(function (p) {
      return ui.el("img", {
        class: "trip-photo-img",
        src: p.url,
        "data-action": "preview",
        "data-url": p.url
      })
    })

    return ui.el("div", { class: "trip-page" }, [
      ui.pageHead({ eyebrow: t.dateText, title: t.title, sub: t.location }),
      ui.panel({
        title: "记录",
        children: [
          ui.kv([
            ["日期", t.dateText],
            ["地点", t.location || "未填"],
            ["天气", t.weather || "未填"],
            ["同行", t.companions || "未填"],
            ["标签", t.tagsText || "无"]
          ]),
          t.notes ? ui.block({ title: "备注", children: [ui.el("p", { text: t.notes })] }) : null
        ].filter(Boolean)
      }),
      photos.length
        ? ui.panel({ title: "照片", children: [ui.el("div", { class: "trip-photos" }, photos)] })
        : null,
      ui.el("div", { class: "trip-actions" }, [
        ui.el("a", { class: "trip-btn", href: "/trip/pages/index", text: "返回" }),
        ui.el("button", { class: "trip-btn danger", "data-action": "delete", text: "删除" })
      ])
    ].filter(Boolean)
  },

  /** 挂载：读记录与照片 */
  onMount: function () {
    const self = this
    const params = window.location.search || ""
    const idMatch = /[?&]id=([^&]+)/.exec(params)
    const recordId = idMatch ? decodeURIComponent(idMatch[1]) : this.state.id
    if (!recordId) return

    this.state.id = recordId
    store.get(recordId).then(function (t) {
      if (!t) {
        self.state.trip = null
        return
      }
      self.state.trip = {
        id: t.id,
        title: t.title || "未命名",
        dateText: format.ymdPretty(t.date),
        location: t.location || "",
        weather: t.weather || "",
        companions: t.companions || "",
        notes: t.notes || "",
        tagsText: (t.tags || []).join(" ")
      }
      return store.getPhotoUrls(recordId).then(function (urls) {
        self.state.photos = urls
      })
    })
  },

  /** 预览照片 */
  onPreview: function (url) {
    const urls = (this.state.photos || []).map(function (p) { return p.url })
    platform.previewImage({ current: url, urls: urls })
  },

  /** 删除 */
  onDelete: function () {
    const self = this
    platform.showModal({
      title: "删除记录",
      content: "确定要删除这条足迹吗？照片也会一并删除。",
      confirmText: "删除",
      confirmColor: "#d33"
    }).then(function (r) {
      if (!r.confirm) return
      return store.remove(self.state.id).then(function () {
        platform.showToast({ title: "已删除", icon: "success" })
        platform.navigateTo("/trip/pages/index")
      })
    })
  }
})
```

- [ ] **Step 5: 写 `web/trip/style.css`**

新建 `apps/web/web/trip/style.css`：

```css
/* style.css —— trip 模块样式 */

.trip-page {
  padding-bottom: 24px;
}

.trip-form {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.trip-field {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.trip-field-label {
  font-size: 12px;
  color: var(--text-secondary);
}

.trip-input,
.trip-textarea {
  width: 100%;
  padding: 8px 10px;
  border: 1px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-primary);
  color: var(--text-primary);
  font-size: 14px;
  font-family: inherit;
}

.trip-textarea {
  min-height: 100px;
  resize: vertical;
  line-height: 1.6;
}

.trip-photos {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(88px, 1fr));
  gap: 8px;
  margin-bottom: 12px;
}

.trip-photo {
  position: relative;
  aspect-ratio: 1;
  border-radius: 8px;
  overflow: hidden;
  background: var(--bg-subtle);
}

.trip-photo-img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
  cursor: zoom-in;
}

.trip-photo-del {
  position: absolute;
  top: 4px;
  right: 4px;
  width: 22px;
  height: 22px;
  border: none;
  border-radius: 50%;
  background: rgba(15, 23, 42, 0.7);
  color: #ffffff;
  font-size: 14px;
  line-height: 1;
  cursor: pointer;
}

.trip-file {
  display: block;
  font-size: 12px;
  color: var(--text-secondary);
}

.trip-actions {
  display: flex;
  gap: 8px;
  justify-content: flex-end;
  margin-top: 12px;
}

.trip-btn {
  display: inline-block;
  padding: 7px 16px;
  border: 0.5px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  font-size: 13px;
  cursor: pointer;
  text-decoration: none;
  font-family: inherit;
}

.trip-btn.primary {
  background: var(--brand);
  border-color: var(--brand);
  color: #ffffff;
}

.trip-btn.danger {
  background: var(--danger);
  border-color: var(--danger);
  color: #ffffff;
}

.trip-row {
  display: flex;
  align-items: baseline;
  gap: 8px;
  padding: 10px 0;
  border-bottom: 0.5px solid var(--border);
  text-decoration: none;
  color: inherit;
}

.trip-row:last-child {
  border-bottom: none;
}

.trip-row-date {
  font-size: 12px;
  color: var(--text-muted);
  flex: 0 0 72px;
  font-variant-numeric: tabular-nums;
}

.trip-row-title {
  flex: 1;
  min-width: 0;
  font-size: 14px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.trip-row-loc {
  font-size: 12px;
  color: var(--text-muted);
  flex: 0 0 auto;
}
```

- [ ] **Step 6: 运行测试确认通过**

Run: `cd apps/web && node tests/trip-view.test.js`
Expected: PASS —— 4 个用例全过

- [ ] **Step 7: 提交**

```bash
git add apps/web
git commit -m "feat(web): trip 模块 3 个页面重写为原生 Web 组件

列表页用 store.listSync() 同步读元数据（渲染不必等 IndexedDB）。
新增页用 <input type=file> 选图，照片存 IndexedDB。
详情页读 objectURL 预览，删除时连带删照片。

store.js 加 listSync() —— 列表页只需元数据，不必等异步的照片读取。"
git push origin main
```

---

## Task 5：games 视图层（5 页）

**Files:**
- Create: `apps/web/web/games/pages/index.js`
- Create: `apps/web/web/games/pages/snake.js`
- Create: `apps/web/web/games/pages/game2048.js`
- Create: `apps/web/web/games/pages/sudoku.js`
- Create: `apps/web/web/games/pages/match3.js`
- Create: `apps/web/web/games/router.js`
- Create: `apps/web/web/games/style.css`
- Test: `apps/web/tests/games-view.test.js`

**Interfaces:**
- Consumes: Task 3 的 `shared/games/*`；计划一的 `shared/ui.js` `shared/platform.js`
- Produces:
  - 5 个页面模块：`{ data, onMount?, render(state) → HTMLElement }`，用 `global.__GAMES_PAGE__({...})` 注册
  - `web/games/router.js` 导出 `ROUTES`（5 条）与 `resolve(pathname)`

- [ ] **Step 1: 写失败测试**

新建 `apps/web/tests/games-view.test.js`：

```javascript
"use strict"
/**
 * games-view.test.js —— games 视图层
 */

const test = require("node:test")
const assert = require("node:assert")

const dom = require("./dom-stub.js")
dom.install()

const storage = require("../shared/core/storage.js")

const PAGES = [
  "pages/index",
  "pages/snake",
  "pages/game2048",
  "pages/sudoku",
  "pages/match3"
]

function loadPage (route) {
  const path = require("node:path")
  const abs = path.resolve(__dirname, "..", "web", "games", route + ".js")
  let config = null
  global.__GAMES_PAGE__ = function (o) { config = o }
  try {
    delete require.cache[require.resolve(abs)]
    require(abs)
  } finally {
    delete global.__GAMES_PAGE__
  }
  if (!config) throw new Error("页面未调用 __GAMES_PAGE__(): " + route)
  return config
}

test.beforeEach(function () {
  storage.clearAll()
})

test("5 个页面模块都能加载且形状正确", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    assert.strictEqual(typeof page.render, "function", route + " 应有 render()")
    assert.ok(page.data && typeof page.data === "object", route + " 应有 data")
  })
})

test("每个页面的 render() 都产出节点且不抛异常", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    page.state = Object.assign({}, page.data)
    const node = page.render(page.state)
    assert.ok(node, route + " 的 render 应返回节点")
  })
})

test("渲染结果不含 undefined / NaN", function () {
  PAGES.forEach(function (route) {
    const page = loadPage(route)
    page.state = Object.assign({}, page.data)
    const text = page.render(page.state).textContent
    assert.ok(text.indexOf("undefined") < 0, route + " 含 undefined：" + text.slice(0, 120))
    assert.ok(text.indexOf("NaN") < 0, route + " 含 NaN：" + text.slice(0, 120))
  })
})

test("路由表覆盖 5 个页面", function () {
  const { ROUTES } = require("../web/games/router.js")
  assert.strictEqual(Object.keys(ROUTES).length, 5)
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/games-view.test.js`
Expected: FAIL —— 页面模块不存在

- [ ] **Step 3: 写 `web/games/router.js`**

新建 `apps/web/web/games/router.js`：

```javascript
"use strict"
/**
 * router.js —— games 模块内的路由表
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/index": function () { return require("./pages/index.js") },
  "/pages/snake": function () { return require("./pages/snake.js") },
  "/pages/game2048": function () { return require("./pages/game2048.js") },
  "/pages/sudoku": function () { return require("./pages/sudoku.js") },
  "/pages/match3": function () { return require("./pages/match3.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname
 * @returns {string|null}
 */
function resolve (pathname) {
  const prefix = "/games"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  const stripped = clean.split("?")[0]
  if (ROUTES[stripped]) return stripped
  return null
}

module.exports = { ROUTES: ROUTES, resolve: resolve }
```

- [ ] **Step 4: 写 5 个页面**

**`web/games/pages/index.js`（游戏中心首页）：**

```javascript
"use strict"
/**
 * index —— 游戏中心首页
 *
 * 列出 4 个游戏与各自最高分。
 */

const gameStorage = require("../../../shared/games/storage.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

const ENTRIES = [
  { key: "snake", label: "贪吃蛇", desc: "方向键或滑动控制，吃一点长一点", path: "/games/pages/snake" },
  { key: "game2048", label: "2048", desc: "相同数字合并，目标是 2048", path: "/games/pages/game2048" },
  { key: "sudoku", label: "数独", desc: "九宫格填数，5 个章节", path: "/games/pages/sudoku" },
  { key: "match3", label: "消消乐", desc: "三个同色即消", path: "/games/pages/match3" }
]

global.__GAMES_PAGE__({
  data: {
    title: "游戏中心",
    entries: []
  },

  onMount: function () {
    this.state = Object.assign({}, this.state, { entries: buildEntries() })
  },

  render: function (state) {
    const entries = (state.entries && state.entries.length ? state.entries : buildEntries()).map(function (e) {
      return ui.el("a", { class: "game-entry", href: e.path }, [
        ui.el("span", { class: "game-entry-label", text: e.label }),
        ui.el("span", { class: "game-entry-desc", text: e.desc }),
        ui.el("span", { class: "game-entry-best", text: e.best })
      ])
    })

    return ui.el("div", { class: "games-page" }, [
      ui.pageHead({ eyebrow: "游戏中心", title: "挑一个玩玩", sub: "进度都存在本机" }),
      ui.el("div", { class: "game-grid" }, entries),
      ui.el("div", { class: "game-actions" }, [
        ui.el("button", { class: "game-btn", "data-action": "clear", text: "清空全部进度" })
      ])
    ])
  },

  /** 清空全部游戏存档 */
  onClear: function () {
    platform.showModal({
      title: "清空全部进度",
      content: "所有游戏的最高分与章节进度都会被删除，且不可撤销。",
      confirmText: "清空",
      confirmColor: "#d33"
    }).then(function (r) {
      if (!r.confirm) return
      gameStorage.resetAll()
      platform.showToast({ title: "已清空", icon: "success" })
      this.state.entries = buildEntries()
    })
  }
})

/**
 * 组装入口列表（含各自最高分）。
 *
 * `storage.getOverview()` 就是为首页准备的汇总接口（小程序版已有），
 * 直接用它，不要自己逐个调。
 *
 * @returns {Array}
 */
function buildEntries () {
  let o = { snakeBest: 0, g2048Best: 0, sudokuStars: 0, match3Stars: 0, achievementUnlocked: 0 }
  try {
    o = gameStorage.getOverview()
  } catch (e) {
    // 读不到就用全 0，页面仍可显示
  }
  const bestMap = {
    snake: "最高 " + o.snakeBest + " 分",
    game2048: "最高 " + o.g2048Best + " 分",
    sudoku: o.sudokuStars + " 星",
    match3: o.match3Stars + " 星"
  }
  return ENTRIES.map(function (e) {
    return {
      key: e.key,
      label: e.label,
      desc: e.desc,
      path: e.path,
      best: bestMap[e.key]
    }
  })
}
```

> **API 名字已核实**（`apps/games/utils/storage.js` 的实际导出）：
> `getSnakeBestScore` `updateSnakeBestScore` `get2048BestScore` `update2048BestScore`
> `getSudokuData` `getSudokuTotalStars` `getMatch3Data` `getMatch3UnlockedChapters`
> `getMatch3EarnedStars` `getMatch3TotalStars` `getAchievements` `getUnlockedCount`
> `getOverview` `resetAll`。
> 注意清空全部的函数名是 **`resetAll`**（不是 `clearAll`），首页汇总用 **`getOverview()`**。

**`web/games/pages/snake.js`（贪吃蛇，Canvas）：**

```javascript
"use strict"
/**
 * snake —— 贪吃蛇（Canvas 2d）
 *
 * 引擎在 shared/games/snake.js（零 wx 依赖，纯逻辑）。
 * 本文件负责：状态管理、Canvas 绘制、方向控制、定时循环。
 *
 * 与小程序版的差异只有「怎么拿到 canvas」：
 *   小程序：wx.createSelectorQuery().select('#snake-canvas').fields({node:true,size:true})
 *   Web   ：getBoundingClientRect
 * 绘制代码逐字保留 —— Canvas 2D API 两边完全一致。
 *
 * 【状态结构照搬小程序版】
 * 小程序版把游戏状态直接放在 this.state 上（snake.tick(this.state) 会读写
 * state.snake / state.food / state.score / state.speed / state.isGameOver），
 * 页面自己的 status 也放在同一个对象里。Web 版沿用这个结构 ——
 * 引擎代码因此一字不改。
 */

const snake = require("../../../shared/games/snake.js")
const gameStorage = require("../../../shared/games/storage.js")
const achievements = require("../../../shared/games/achievements.js")
const vibrate = require("../../../shared/games/vibrate.js")
const ui = require("../../../shared/ui.js")
const platform = require("../../../shared/platform.js")

global.__GAMES_PAGE__({
  data: {
    score: 0,
    best: 0,
    status: "ready", // ready | playing | paused | over
    gridWidth: snake.SNAKE_CONFIG.GRID_WIDTH,
    gridHeight: snake.SNAKE_CONFIG.GRID_HEIGHT,
    achievementToast: null
  },

  onMount: function () {
    // 游戏状态与页面状态放同一个对象 —— 与小程序版一致，引擎代码不用改
    this.state = snake.createInitialState()
    this.state.status = "ready"
    this.state.score = 0
    this.state.achievementToast = null
    this.state.best = gameStorage.getSnakeBestScore()
    this.__timer = null
  },

  render: function (state) {
    const canvas = ui.el("canvas")
    this.__canvas = canvas

    const dirButtons = ["up", "down", "left", "right"].map(function (d) {
      const label = { up: "↑", down: "↓", left: "←", right: "→" }[d]
      return ui.el("button", {
        class: "game-dir",
        "data-action": "direction",
        "data-dir": d,
        text: label
      })
    })

    return ui.el("div", { class: "games-page" }, [
      ui.pageHead({
        eyebrow: "贪吃蛇",
        title: "得分 " + (state.score || 0),
        sub: "最高 " + (state.best || 0) + " 分"
      }),
      ui.hero({
        title: state.status === "playing" ? "进行中" : "准备好了吗？",
        sub: "方向按钮或键盘方向键控制",
        children: [
          ui.el("div", { class: "game-canvas-wrap" }, [canvas]),
          ui.el("div", { class: "game-dirs" }, dirButtons),
          ui.el("div", { class: "game-actions" }, [
            ui.el("button", {
              class: "game-btn primary",
              "data-action": state.status === "playing" ? "pause" : "start",
              text: state.status === "playing" ? "暂停" : "开始"
            }),
            ui.el("button", { class: "game-btn", "data-action": "restart", text: "重开" })
          ])
        ]
      }),
      state.achievementToast
        ? ui.panel({ title: "成就解锁", children: [ui.el("p", { text: state.achievementToast.title || "已解锁新成就" })] })
        : null
    ].filter(Boolean)
  },

  /** 画布进 DOM 后调用 */
  attachCanvas: function () {
    if (!this.__canvas) return
    const rect = this.__canvas.getBoundingClientRect()
    const width = Math.round(rect.width || this.__canvas.clientWidth || 300)
    const height = Math.round(rect.height || this.__canvas.clientHeight || 300)
    if (width === 0 || height === 0) return

    const ctx = this.__canvas.getContext("2d")
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 2
    this.__canvas.width = width * dpr
    this.__canvas.height = height * dpr
    ctx.scale(dpr, dpr)
    this.__ctx = ctx
    this.__size = { w: width, h: height }
    this.draw()
  },

  /** 绘制（逐字照搬小程序版） */
  draw: function () {
    const ctx = this.__ctx
    if (!ctx) return
    const size = this.__size
    if (!size) return

    const cols = snake.SNAKE_CONFIG.GRID_WIDTH
    const rows = snake.SNAKE_CONFIG.GRID_HEIGHT
    const cell = Math.min(size.w / cols, size.h / rows)
    const offsetX = (size.w - cell * cols) / 2
    const offsetY = (size.h - cell * rows) / 2

    ctx.clearRect(0, 0, size.w, size.h)

    const s = this.state
    if (!s.food) return

    // 食物
    ctx.fillStyle = "#E5484D"
    ctx.beginPath()
    ctx.arc(offsetX + s.food.x * cell + cell / 2, offsetY + s.food.y * cell + cell / 2, cell * 0.35, 0, Math.PI * 2)
    ctx.fill()

    // 蛇身
    ctx.fillStyle = "#30A46C"
    for (let i = 0; i < s.snake.length; i++) {
      const seg = s.snake[i]
      ctx.fillRect(offsetX + seg.x * cell + 1, offsetY + seg.y * cell + 1, cell - 2, cell - 2)
    }
  },

  /** 开始 / 重新开始 */
  onStart: function () {
    this.state = snake.createInitialState()
    this.state.status = "playing"
    this.state.score = 0
    this.state.achievementToast = null
    this.state.best = gameStorage.getSnakeBestScore()
    this.startLoop()
    this.draw()
  },

  /** 暂停 */
  onPause: function () {
    if (this.state.status !== "playing") return
    this.stopLoop()
    this.state.status = "paused"
  },

  /** 方向按钮 */
  onDirection: function (dir) {
    if (this.state.status !== "playing") return
    const D = snake.Direction
    const map = { up: D.UP, down: D.DOWN, left: D.LEFT, right: D.RIGHT }
    const d = map[dir]
    if (d !== undefined) snake.setDirection(this.state, d)
  },

  /** 启动定时循环（照搬小程序版：速度随得分提升，每步重启定时器） */
  startLoop: function () {
    const self = this
    this.stopLoop()
    this.__timer = setInterval(function () {
      self.step()
    }, this.state.speed)
  },

  /** 停止定时循环 */
  stopLoop: function () {
    if (this.__timer) {
      clearInterval(this.__timer)
      this.__timer = null
    }
  },

  /** 单步推进（照搬小程序版） */
  step: function () {
    const self = this
    snake.tick(this.state)

    if (this.state.score > 0 && this.state.score % 10 === 0) {
      // 每吃一个食物触发一次成就评估
      achievements.evaluate({ type: "snake.eat", score: this.state.score })
      vibrate.trigger()
    }

    if (this.state.isGameOver) {
      this.stopLoop()
      this.state.status = "over"
      gameStorage.updateSnakeBestScore(this.state.score)
      this.state.best = gameStorage.getSnakeBestScore()
      vibrate.trigger("heavy")
      platform.showToast({ title: "游戏结束，得分 " + this.state.score, icon: "none" })
      this.draw()
      return
    }

    // 速度随得分提升，同步重启定时器
    this.stopLoop()
    this.__timer = setInterval(function () { self.step() }, this.state.speed)

    this.draw()
  }
})
```

> **API 名字已核实**（`apps/games/utils/snake.js` 的实际导出）：
> `SNAKE_CONFIG` `Direction` `createInitialState` `generateFood` `setDirection` `tick` `getGrid`。
> 注意是 **`createInitialState`** 不是 `createGame`，是 **`tick`** 不是 `step`；
> 速度字段是 `state.speed`（引擎内部按得分自动调低），不是配置里的 `INITIAL_SPEED` —— 定时器要用 `this.state.speed`。
> **按实际导出名改**。同理 `game2048.js` `sudoku.js` `match3.js`。

- [ ] **Step 5: 写 `web/games/style.css`**

新建 `apps/web/web/games/style.css`：

```css
/* style.css —— games 模块样式 */

.games-page {
  padding-bottom: 24px;
}

.game-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(160px, 1fr));
  gap: 10px;
  margin-bottom: 16px;
}

.game-entry {
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding: 14px;
  border: 0.5px solid var(--border);
  border-radius: 10px;
  background: var(--bg-subtle);
  text-decoration: none;
}

.game-entry:hover {
  border-color: var(--brand);
}

.game-entry-label {
  font-size: 15px;
  font-weight: 600;
  color: var(--text-primary);
}

.game-entry-desc {
  font-size: 12px;
  color: var(--text-muted);
}

.game-entry-best {
  font-size: 12px;
  color: var(--brand);
  margin-top: 4px;
  font-variant-numeric: tabular-nums;
}

.game-canvas-wrap {
  position: relative;
  width: 100%;
  max-width: 360px;
  aspect-ratio: 1;
  margin: 0 auto;
  background: var(--bg-subtle);
  border-radius: 10px;
  overflow: hidden;
}

.game-canvas-wrap canvas {
  width: 100%;
  height: 100%;
  display: block;
  touch-action: none;
}

.game-dirs {
  display: grid;
  grid-template-columns: repeat(3, 44px);
  grid-template-rows: repeat(2, 44px);
  gap: 6px;
  justify-content: center;
  margin-top: 12px;
}

.game-dir {
  border: 0.5px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  font-size: 18px;
  cursor: pointer;
  font-family: inherit;
}

.game-dir:hover {
  border-color: var(--brand);
  color: var(--brand);
}

.game-actions {
  display: flex;
  gap: 8px;
  justify-content: center;
  margin-top: 12px;
}

.game-btn {
  padding: 7px 18px;
  border: 0.5px solid var(--border-strong);
  border-radius: 8px;
  background: var(--bg-subtle);
  color: var(--text-primary);
  font-size: 13px;
  cursor: pointer;
  font-family: inherit;
}

.game-btn.primary {
  background: var(--brand);
  border-color: var(--brand);
  color: #ffffff;
}

.game-sudoku-grid {
  display: grid;
  grid-template-columns: repeat(9, 1fr);
  gap: 1px;
  max-width: 320px;
  margin: 0 auto;
  background: var(--border-strong);
  border: 1px solid var(--border-strong);
  border-radius: 4px;
  overflow: hidden;
}

.game-sudoku-cell {
  aspect-ratio: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--bg-surface);
  border: none;
  font-size: 15px;
  cursor: pointer;
  font-family: inherit;
  color: var(--text-primary);
}

.game-sudoku-cell.fixed {
  background: var(--bg-subtle);
  color: var(--text-secondary);
  font-weight: 600;
}

.game-sudoku-cell.selected {
  outline: 2px solid var(--brand);
  outline-offset: -2px;
}

.game-sudoku-cell.related {
  background: var(--brand-soft);
}

.game-sudoku-cell.error {
  color: var(--danger);
}
```

- [ ] **Step 6: 运行测试确认通过**

Run: `cd apps/web && node tests/games-view.test.js`
Expected: PASS —— 4 个用例全过

- [ ] **Step 7: 提交**

```bash
git add apps/web
git commit -m "feat(web): games 模块 5 个页面重写为原生 Web 组件

snake 与 game2048 用 Canvas 绘制，绘制代码逐字保留，只把取 canvas
的方式从 wx.createSelectorQuery 换成 getBoundingClientRect。

4 个游戏引擎零改动复用 shared/games/。"
git push origin main
```

---

## Task 6：两个模块接入外壳 + 端到端冒烟

**Files:**
- Create: `apps/web/web/trip/shell.html`
- Create: `apps/web/web/trip/browser.js`
- Create: `apps/web/web/games/shell.html`
- Create: `apps/web/web/games/browser.js`
- Modify: `apps/web/server/app.js`
- Modify: `apps/web/tests/server.test.js`

**Interfaces:**
- Consumes: Task 4/5 的 `router.js` 与页面模块
- Produces: `http://127.0.0.1:5173/trip/pages/index` 与 `/games/pages/index` 可访问

- [ ] **Step 1: 写失败测试**

在 `apps/web/tests/server.test.js` 末尾追加：

```javascript
test("trip 与 games 模块的页面能被访问", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r1 = await request(app.server, "/trip/pages/index")
  assert.strictEqual(r1.status, 200)
  assert.ok(r1.body.indexOf("足迹") >= 0, "trip 壳应含「足迹」")

  const r2 = await request(app.server, "/games/pages/index")
  assert.strictEqual(r2.status, 200)
  assert.ok(r2.body.indexOf("游戏") >= 0, "games 壳应含「游戏」")
})
```

- [ ] **Step 2: 运行测试确认失败**

Run: `cd apps/web && node tests/server.test.js`
Expected: FAIL —— 404

- [ ] **Step 3: 写两个 shell.html 与 browser.js**

`apps/web/web/trip/shell.html`：

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>足迹</title>
  <link rel="stylesheet" href="/style.css">
  <link rel="stylesheet" href="/trip/style.css">
</head>
<body class="theme-light">
  <div class="app-shell">
    <header class="app-head">
      <h1 class="app-title">足迹</h1>
      <nav class="app-nav">
        <a class="nav-item" href="/">返回工作台</a>
        <a class="nav-item" href="/trip/pages/index">全部记录</a>
        <a class="nav-item" href="/trip/pages/new-trip">新增</a>
      </nav>
    </header>
    <main class="app-main" id="app-main"></main>
  </div>
  <script src="/shared/ui.js"></script>
  <script src="/shared/platform.js"></script>
  <script src="/cjs-loader.js"></script>
  <script src="/trip/browser.js"></script>
</body>
</html>
```

`apps/web/web/trip/browser.js`：

```javascript
"use strict"
/**
 * browser.js —— trip 模块的浏览器入口
 *
 * 与 workbench / knowledge 的 browser.js 同构：按依赖顺序预载 CommonJS 模块，
 * 再挂载页面。
 */

;(function () {
  "use strict"

  const CJS = window.CJS

  const PRELOAD = [
    "/shared/core/id.js",
    "/shared/core/date.js",
    "/shared/core/storage.js",
    "/shared/core/store.js",
    "/shared/photos.js",
    "/shared/trip/format.js",
    "/shared/trip/store.js",
    "/shared/trip/export.js"
  ]

  const PAGES = {
    "/pages/index": "/web/trip/pages/index.js",
    "/pages/new-trip": "/web/trip/pages/new-trip.js",
    "/pages/detail": "/web/trip/pages/detail.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.children.pop()
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
    if (typeof currentPage.attachCanvas === "function") currentPage.attachCanvas()
  }

  function loadPage (pathname) {
    const key = pathname.replace(/^\/trip/, "") || "/pages/index"
    const file = PAGES[key]
    if (!file) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) rootEl.appendChild(window.AlgoPlanUI.empty("页面不存在", key))
      return Promise.resolve(null)
    }
    return CJS.load(file).then(function (page) {
      currentPage = page
      currentPage.state = Object.assign({}, currentPage.data)
      if (typeof currentPage.onMount === "function") currentPage.onMount(document.getElementById("app-main"))
      draw()
      return currentPage
    })
  }

  function boot () {
    let chain = Promise.resolve()
    PRELOAD.forEach(function (p) {
      chain = chain.then(function () { return CJS.load(p) })
    })
    chain.then(function () {
      window.addEventListener("popstate", function () {
        loadPage(window.location.pathname)
      })
      return loadPage(window.location.pathname)
    }).catch(function (err) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) {
        rootEl.appendChild(window.AlgoPlanUI.empty("模块加载失败", String(err && err.message ? err.message : err)))
      }
      console.error("[trip] 加载失败", err)
    })
  }

  boot()
})()
```

`apps/web/web/games/shell.html`：

```html
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>游戏中心</title>
  <link rel="stylesheet" href="/style.css">
  <link rel="stylesheet" href="/games/style.css">
</head>
<body class="theme-light">
  <div class="app-shell">
    <header class="app-head">
      <h1 class="app-title">游戏中心</h1>
      <nav class="app-nav">
        <a class="nav-item" href="/">返回工作台</a>
        <a class="nav-item" href="/games/pages/index">全部游戏</a>
      </nav>
    </header>
    <main class="app-main" id="app-main"></main>
  </div>
  <script src="/shared/ui.js"></script>
  <script src="/shared/platform.js"></script>
  <script src="/cjs-loader.js"></script>
  <script src="/games/browser.js"></script>
</body>
</html>
```

`apps/web/web/games/browser.js`：

```javascript
"use strict"
/**
 * browser.js —— games 模块的浏览器入口
 */

;(function () {
  "use strict"

  const CJS = window.CJS

  const PRELOAD = [
    "/shared/core/storage.js",
    "/shared/core/store.js",
    "/shared/platform.js",
    "/shared/games/storage.js",
    "/shared/games/vibrate.js",
    "/shared/games/game2048.js",
    "/shared/games/snake.js",
    "/shared/games/sudoku.js",
    "/shared/games/match3.js",
    "/shared/games/achievements.js",
    "/shared/games/share.js"
  ]

  const PAGES = {
    "/pages/index": "/web/games/pages/index.js",
    "/pages/snake": "/web/games/pages/snake.js",
    "/pages/game2048": "/web/games/pages/game2048.js",
    "/pages/sudoku": "/web/games/pages/sudoku.js",
    "/pages/match3": "/web/games/pages/match3.js"
  }

  let currentPage = null

  function draw () {
    const rootEl = document.getElementById("app-main")
    if (!rootEl) return
    while (rootEl.children.length) rootEl.children.pop()
    if (!currentPage) return
    const node = currentPage.render(currentPage.state)
    if (node) rootEl.appendChild(node)
    if (typeof currentPage.attachCanvas === "function") currentPage.attachCanvas()
  }

  function loadPage (pathname) {
    const key = pathname.replace(/^\/games/, "") || "/pages/index"
    const file = PAGES[key]
    if (!file) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) rootEl.appendChild(window.AlgoPlanUI.empty("页面不存在", key))
      return Promise.resolve(null)
    }
    return CJS.load(file).then(function (page) {
      currentPage = page
      currentPage.state = Object.assign({}, currentPage.data)
      if (typeof currentPage.onMount === "function") currentPage.onMount(document.getElementById("app-main"))
      draw()
      return currentPage
    })
  }

  function boot () {
    let chain = Promise.resolve()
    PRELOAD.forEach(function (p) {
      chain = chain.then(function () { return CJS.load(p) })
    })
    chain.then(function () {
      window.addEventListener("popstate", function () {
        loadPage(window.location.pathname)
      })
      return loadPage(window.location.pathname)
    }).catch(function (err) {
      const rootEl = document.getElementById("app-main")
      if (rootEl) {
        rootEl.appendChild(window.AlgoPlanUI.empty("模块加载失败", String(err && err.message ? err.message : err)))
      }
      console.error("[games] 加载失败", err)
    })
  }

  boot()
})()
```

> **`shared/platform.js` 在 games 的 PRELOAD 里**：games 的 `vibrate.js` `require("../platform.js")`。而 `platform.js` 是 UMD —— 浏览器可直接 `<script src>` 引入并挂到 `window.AlgoPlanPlatform`，**不需要**进 PRELOAD。所以从 games 的 PRELOAD 里去掉它（上面已去掉）。

> **`shared/core/store.js` 也在 PRELOAD 里**：trip 的 `store.js` 用的是 `createListStore`？**不** —— trip 的 store 是自己写的，不用 `core/store.js`。从 trip 的 PRELOAD 里也去掉它（上面已去掉）。

- [ ] **Step 4: 在 `server/app.js` 里挂两个模块的路由**

在 `apps/web/server/app.js` 里，`knowledgeShell` 那段之后加：

```javascript
  // trip 与 games 模块：同理返回 HTML 壳
  const makeShell = function (file, errMsg) {
    return function (req, res) {
      let html
      try {
        html = fs.readFileSync(file, "utf8")
      } catch (e) {
        sendJson(res, 500, { error: errMsg })
        return
      }
      res.writeHead(200, {
        "Content-Type": "text/html; charset=utf-8",
        "Content-Length": Buffer.byteLength(html)
      })
      res.end(html)
    }
  }

  const tripShell = makeShell(
    path.join(__dirname, "..", "web", "trip", "shell.html"),
    "trip 模块页面壳读取失败"
  )
  const gamesShell = makeShell(
    path.join(__dirname, "..", "web", "games", "shell.html"),
    "games 模块页面壳读取失败"
  )
  routes.push({ method: "GET", pattern: "/trip/pages/:a", handler: tripShell })
  routes.push({ method: "GET", pattern: "/games/pages/:a", handler: gamesShell })
```

- [ ] **Step 5: 运行测试确认通过**

Run: `cd apps/web && node tests/server.test.js && node tests/run-all.js`
Expected: 全绿

- [ ] **Step 6: 手工端到端验证**

```bash
cd apps/web && node server/main.js
```

```bash
curl -s http://127.0.0.1:5173/trip/pages/index | head -15
curl -s http://127.0.0.1:5173/games/pages/index | head -15
```
Expected: 两个 HTML 壳

浏览器打开 `http://127.0.0.1:5173/games/pages/index`，确认：
- 4 个游戏卡片可点
- 打开 `/games/pages/snake`，**贪吃蛇画出来了**（Canvas 有内容），方向键能控制
- 浏览器打开 `http://127.0.0.1:5173/trip/pages/index`，确认：
- 列表页显示「还没有足迹」空态
- 点「新增记录」→ 填标题与日期 → 选一张照片 → 保存 → 回到列表能看到刚存的记录
- 打开详情页，**照片显示出来了**
- 浏览器控制台**无报错**

> **IndexedDB 的浏览器权限**：某些隐私模式下 IndexedDB 不可用。`photos.isAvailable()` 返回 false 时，trip 的新增页应降级为「只存元数据，不存照片」并在 UI 上明示。

- [ ] **Step 7: 提交**

```bash
git add apps/web
git commit -m "feat(web): trip 与 games 模块接入外壳，浏览器可访问

两个模块各一个 shell.html + browser.js，模式与 workbench / knowledge 一致。
服务端 /trip/pages/* 与 /games/pages/* 返回对应的 HTML 壳。"
git push origin main
```

---

## 验收标准（本计划完成时）

1. `cd apps/web && node tests/run-all.js` —— **全绿**，其中 games 的 **51 个引擎用例**全部通过
2. `http://127.0.0.1:5173/games/pages/index` 可访问，4 个游戏可玩，贪吃蛇的 Canvas 正常绘制
3. `http://127.0.0.1:5173/trip/pages/index` 可访问；新增 → 存照片 → 详情页看到照片 → 删除，闭环可用
4. **trip 的数据完全在本机**：元数据在 localStorage（`db:trips`），照片在 IndexedDB
5. `shared/games/storage.js` 的键名仍是 `games_` 前缀，与其它模块的 `db:` 前缀**不冲突**
6. `apps/trip-log/`、`apps/games/` 目录**未删未改**，作为对照参考
7. **未修改** `engine/`、`apps/study-hub/`、`apps/wiki-rag/`
8. `git status` 干净

## 后续计划

| 计划 | 内容 | 依赖本计划的 |
|---|---|---|
| 计划五 | study-hub 挂载 + wiki-rag 反代 + 删除 4 个旧小程序目录 + 更新 `apps/README.md` | 计划一、二、三、四 |

> **计划五的前置条件**（设计文档 §7.3）：删除动作必须在**每个模块的测试全部迁入并通过之后**。到本计划结束，四个模块的测试情况：
>
> | 模块 | 迁入的用例 | 状态 |
> |---|---|---|
> | programmer-workbench | 315 | ✅ 计划二 |
> | knowledge-hub | 181 | ✅ 计划三 |
> | games | 51 | ✅ 本计划 |
> | trip-log | 15（新增） | ✅ 本计划 |
> | **合计** | **562** | |
