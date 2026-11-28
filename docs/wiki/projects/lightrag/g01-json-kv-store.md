---
project: lightrag
feature: json-kv-store
title: JSON 键值存储与文档状态实现
page_type: feature
schema_version: 1
refs:
  - path: lightrag/kg/json_kv_impl.py
    symbols: [JsonKVStorage]
  - path: lightrag/kg/json_doc_status_impl.py
    symbols: [JsonDocStatusStorage]
  - path: lightrag/file_atomic.py
    symbols: [atomic_write, _replace_file, reap_orphan_tmp_files]
---

# JSON 键值存储与文档状态实现

## 一句话职责

把「文档正文块」与「文档处理进度」两类数据落成两个 JSON 文件，但用一个**全进程共享的 `Manager().dict()`** 当写缓冲——于是跨进程同步不靠「重载文件」而靠「共享内存」，文件只在提交时被整体重写一遍；代价是 `storage_updated` 这个标志的语义在这里**与所有文件型后端相反**。

## 机制是什么

lightrag 里有两类后端，跨进程模型**根本不同**：

| 后端 | 内存状态 | 跨进程同步方式 | `storage_updated=True` 的含义 |
|---|---|---|---|
| `NetworkXStorage` / `FaissVectorDBStorage` / `NanoVectorDBStorage` | 每进程一份私有副本 | 文件重载（reload） | **磁盘上有更新，去重载** |
| `JsonKVStorage` / `JsonDocStatusStorage` | 全进程共享同一份 | 共享内存（无重载） | **内存里有脏数据，待刷盘** |

`JsonKVStorage` 的类 docstring 把这句话说得很直白：`self._data` **不是**每进程的 dict，而是 `shared_storage._shared_dicts` 里一个 `Manager().dict()` 代理的引用——每个 worker 看到的是同一个实例。任一进程的修改立刻对所有进程可见，**没有重载这回事**，所以「加一个 `_get_*` 入口方法去重读」在这里是错的。磁盘文件只负责持久化：`initialize` 里读一次，`index_done_callback` 里写一次，不在稳态路径上。

`storage_updated` 因此被翻转：「`True` 是『还有脏数据待刷盘』，永远不是『磁盘上有更新鲜的数据要重载』」。

这个翻转不是笔误，而是「共享内存 + 单文件」这个组合的必然结果：既然所有进程的内存已经一致，那么「磁盘变了」这件事对它们毫无意义（谁改的？就是自己人），唯一需要传播的信号是「内存脏了，该有人刷盘」。

而 `JsonDocStatusStorage` 把同一套协议**重新实现了一遍而不是继承**。类 docstring 解释了理由并给出维护指示：「像 `JsonKVStorage` 一样跑共享内存 + 脏标志协议，是重新实现而非继承……这里的机制与理由见设计文档；**在这里做改动几乎总是 `JsonKVStorage` 也需要同样的改动**」。两份实现是同一份协议的两个实例，靠人工同步——这是这一组里明确标注的维护债。

## 怎么实现的

**`_storage_lock` 覆盖每一次 `self._data` 访问，读也一样。** docstring 的理由是：「`Manager` 代理在跨进程时不是自由线程的」。这一点与文件型后端形成对照——那些后端只在重载/保存时加锁，中间的读不需要，因为读的是自己的内存副本。这里没有副本，每一次读都是一次跨进程 RPC，所以锁的范围覆盖全部访问。

**`__post_init__` 里配置命名空间是一个完全特殊的分支。** 普通分支很短：

```python
validate_workspace(self.workspace)
if self.workspace:
    workspace_dir = os.path.join(working_dir, self.workspace)
file_name = f"kv_store_{self.namespace}.json"
self._shared_workspace = self.workspace
```

而配置分支把三件事都改掉了：

```python
workspace_dir = json_config_dir(working_dir, self.global_config.get("workspace") or "")
self.workspace = CONFIG_CONTAINER_TAG
file_name = CONFIG_JSON_FILE_NAME
self._shared_workspace = f"{CONFIG_CONTAINER_TAG}@{os.path.realpath(workspace_dir)}"
```

注释逐条解释了每一步：路径由 `working_dir` 与实例自己的 workspace 推导出来，**任何设置都挪不动它**；`self.workspace` 保持容器标签要求的取值但「它不寻址任何东西」；而 `_shared_workspace` 加上 `realpath` 是因为「共享命名空间的记账（初始化认领、命名空间锁、更新标志、清理）是按**物理文件**键的……一个常量键会把它们别名到同一个命名空间」。

这句是这一页最实在的坑：共享记账的键必须能区分**物理文件**。若两个不同的 `working_dir` 都用常量 `_lightrag_config` 当键，它们的共享 dict 会被合并成一个，然后互相覆盖对方的文件。

**`finalize` 里「最后释放持有」这件事被写成了两段长注释，因为它失败的后果很重。**

```python
try:
    if self.namespace.endswith("_cache"):
        await self.index_done_callback()
finally:
    if self._holds_namespace:
        self._holds_namespace = False
        await leave_namespace_init(self.namespace, workspace=self._shared_workspace)
        self._storage_lock = None
```

三个决定：

1. **只有 `*_cache` 命名空间在关闭时刷盘。** 理由在 docstring 里：「缓存命名空间在 query/extract 期间经常被写而不触发即时的 `index_done_callback`（缓存变动快，流水线也不总在自然的提交点结束）」。其余命名空间的写入都已经过流水线驱动的 `_insert_done()` 提交。

2. **释放放在 `finally` 里。** 注释解释了为什么必需：「因为上面的刷盘可能失败（磁盘满、只读挂载），而 `_finalize_storages_impl` 会吸收那个失败并继续：**持有权会静默泄漏**」。

3. **泄漏的后果被逐条列出来了**：「泄漏的持有不是小事——它被计数，所以这个进程树里任何后来的实例都无法再成为最后离开的那个，命名空间永远不会被清空，而第二个 `working_dir` 会被 `SharedNamespaceBackingConflictError` 拒绝，**在整个进程生命周期内**」。所以「丢掉缓存」是两者中较小的损失。

释放之后还有一步 `self._storage_lock = None`，注释说明它是必要的：「最后一次释放清空了共享 dict。通过这个实例的写会落进那个空 dict，而它的刷盘会把它发布到文件之上，所以之后每次调用都抛 `StorageNotInitializedError` 直到 `initialize()` 跑过」。

**`index_done_callback` 是这一页最长的方法，而它的结构是为了「写文件」与「发布」这两步不能拆开。**

顺序：在锁内检查 `storage_updated.value` → 快照 `self._data.copy()` → 用 `commit_in_storage_io(_write, _committed)` 把写文件+发布包成一个不可取消的单元 → 结束后清理脏标志。

三个细节：

**第一，`self._data.copy()` 而不是 `dict(self._data)`。** 注释给出了确切理由：「`DictProxy.copy()` 是单个 `Manager` RPC，在服务端整体编组整个映射；`dict(proxy)` 会走映射协议并**为每个值单独做一次 RPC**」。所以对 10 万条记录的命名空间，两者差 10 万次往返。而浅拷贝是安全的，因为「`write_json` 只读它的参数」。

**第二，写文件被移出事件循环，但快照在循环内、锁内完成。** 注释：「这在大型语料上重写整个文件要几秒，期间服务 HTTP 的单个 worker 否则会被阻塞。**只有写动作被移走** —— `data_dict` 在上面、在循环内、在锁下被快照，所以工作线程不碰任何共享的东西」。

**第三，`_committed` 里的「sanitize 后重载」必须在写的不可取消区内。** 这是全篇最精细的一处：

```python
async def _committed() -> None:
    if write_outcome.get("needs_reload"):
        cleaned_data = load_json(self._file_name)
        if cleaned_data is not None:
            try:
                self._data.clear()
                self._data.update(cleaned_data)
            except Exception as exc:
                reconcile_failure.append(exc)
                raise
    await clear_all_update_flags(...)
```

注释解释：「对账属于写的不可取消区**内部**。此时净化后的文件已经发布，所以落在两者之间的取消会让共享 dict 持有那些编码失败的行——与磁盘分叉，直到某次后续刷盘碰巧再净化一次」。而 `clear()` + `update()` 两步必须失败即报，因为「在共享 `Manager().dict()` 上这是两次独立的 RPC，所以它们之间失败会让共享 dict 变**空**，而磁盘上是正确的净化快照——且脏标志还设着，所以下次刷盘会把这个空 dict 直接写回去，丢掉命名空间的每一行」。

这就是为什么失败要记进 `reconcile_failure` 并重抛：上层按 `CommitBookkeepingError` 处理时会读这个列表，决定是否升级成原始异常。

**`CommitBookkeepingError` 的两个分支是这一层的判断核心。**

```python
except CommitBookkeepingError as e:
    if reconcile_failure:
        raise reconcile_failure[0]
    log_without_raising(logger.error, "...written to ..., but its post-write bookkeeping failed...")
```

**有对账失败时重抛**，理由：「现在不可靠的是共享内存视图，而后续刷盘治不好它——**后续刷盘正是会发布它的那个动作**。磁盘上的文件是正确的快照，所以恢复办法是停止向这个工作区写并重启 worker」。而且刻意**重抛原始异常而不是 `CommitBookkeepingError`**，因为「调用方把那个类型读作『已提交，只是发布延迟了』，有些会刻意吸收它」。

**没有对账失败时只记日志不抛**，理由同样具体：「越过上面的守卫，唯一可能失败的是脏标志清除——文件已发布、共享 dict 与它一致。这在下次刷盘时自愈……**重抛反而会把一次持久写报告成从未发生**，而每个调用方都继承那个错误：`_insert_done` 会把一个行已经在磁盘上的文档标成 FAILED，`utils_graph` 的删除路径会把它已经完成的 chunk 追踪清理变成 fail/500」。

这两段合起来是一条很成熟的错误分类原则：**只有当失败让状态「不可能」自愈时才抛；能靠下一次重试自愈的失败只能记为日志**。而「报告成失败」的危害在这里被具体化为「上层会因此做错事」，不只是「日志不好看」。

**注意这里没有 `set_all_update_flags`。** docstring 明确点出：「与文件型后端的提交有**语义差异**：这里没有 `set_all_update_flags`。共享 dict 在进程间已经一致；`index_done_callback` 全局唯一做的事是**清除**脏标志」。

所以同一个方法名在两个后端上做的事情正好互补：文件型后端是「我写完了，你们去重载」（设标志），共享内存后端是「我写完了，大家都干净了」（清标志）。

**`_as_row` 是一个只有五行但值得单独说的守卫。** 它拒收非 mapping 的载荷：

```python
if value is not None and not isinstance(value, dict):
    raise CorruptStorageRecordError(f"[{self.workspace}] {self.namespace} record {key!r} in {self._file_name} is {type(value).__name__}, not a mapping: {value!r}")
```

docstring 说明了为什么必需：「下面每个读都会归一化它交回的行（`create_time`、`update_time`、`_id`），所以它会对文件持有的任何东西调用映射方法。一个映射到字符串或数字的键——手工改过的文件、手工修过的截断文件、外来写入者——因此会从本类内部逃出 `AttributeError: 'str' object has no attribute 'setdefault'`，而上层把它读作**存储失败**而不是一行损坏。改为按名字报出损坏，让懂 schema 的人决定它意味着什么」。

这条是「错误归因」的范例：同一个事实（这行坏了）在两种表达下会触发两条完全不同的处理路径——一层会说「存储挂了」并可能触发隔离/降级，另一层会说「这行坏了」并可能跳过它。**把模糊的错误变成具体的错误，本身就是可靠性设计。**

**`JsonDocStatusStorage` 在协议之上加了三条规则，每条都指向「它是恢复锚点」这个定位。**

docstring 的分节很整齐：

- 「`upsert`、`update_doc_status_fields` 与源冲突修复的刷盘是**同步**的——它们在返回前 `await index_done_callback()`。**文档状态是摄入流水线的恢复锚点，所以崩溃不能丢掉『一个文档已被排队』这个事实**。`delete` 仍是延迟的；`drop` 像其他几个一样刷盘」。
- 「读在返回前把行**拷贝或转换**出 `self._data`——绝不把指向共享代理的活动引用交给调用方」。
- 「`drop` 是破坏性的、**不串行化**；调用方必须持有流水线的 `busy` 预留（`/documents/clear` 端点这么做）」。

第一条是最重要的：同一份代码里，写入有的同步有的延迟，判据是「这个写丢了会不会让恢复出问题」。文档状态记的是「这个文档在什么阶段」，丢一条就意味着重启后不知道要重做它——所以必须持久。而 `delete` 丢了只是少删一条，下次还会删。

**`_CONFLICT_SAMPLE_CAP = 32` 是「有界采样」的显式声明。** 注释：「源冲突列示/修复 API 暴露的冲突文档 id 样本的有界上限——**绝不物化整个集合**」。这与 a01 里「大图谱必须流式扫」是同一条纪律在另一个维度的应用。

**内存边界被单独声明成「能力边界」而不是缺陷。** docstring 写：「第一阶段的内存边界能力：下面的分页调度 API 是真正的 keyset 扫描（通过有界堆实现有界页内存），但**这个后端本质上把整个存储常驻在共享内存里，并在刷盘时重写整个文件**——那个常驻是独立的、有文档的限制，不是分页 API 能修的东西。文档数非常大的部署应使用服务端后端（Redis/PG/…）」。

这段话的价值在于它**明确区分了「API 形状」与「资源模型」**：分页 API 保证的是「调用方不必一次拿到全部」，而不是「后端不必持有全部」。两者常被混淆，而在这一层它们分别是 True 与 False。

**`file_atomic.py` 的三个函数是所有这些文件后端的公共地基，而它的位置本身有个理由。**

模块 docstring 第一段：「为什么它在包根而不是 `lightrag/kg/` 下：`lightrag.utils.write_json` 需要 `atomic_write` 来获得崩溃安全，而几个 `lightrag/kg/*` 模块两者都需要。把辅助函数放在 `lightrag/kg/` 下会造成 `utils -> kg -> utils` 的 import 环。保持这个模块无依赖（只用标准库）避免这一点」。

`atomic_write` 的机制是「每写入者一个 tmp 兄弟文件 + `os.replace`」：

```python
tmp = tmp_path_for(file_name)
try:
    write_fn(tmp)
    _preserve_mode(tmp, file_name, workspace)
    _replace_file(tmp, file_name, workspace)
except BaseException:
    try:
        if os.path.exists(tmp):
            os.remove(tmp)
    except OSError as exc:
        logger.warning(...)
    raise
```

`tmp_path_for` 后缀含 PID、线程 id、纳秒时间戳，docstring 解释：「这样多个并发写入者——共享同一工作目录的不同进程，或同一进程内的多个线程——不会踩坏对方正在写的 tmp 并留下『no such file』的重命名错误」。

**两种失败模式被区别对待，这是模块 docstring 的核心。**

- **Python 异常**（`write_fn` 抛了、`os.replace` 失败）：`finally` 会跑，在飞的 tmp 被尽力删除，异常继续传播。**磁盘上的目标文件是先前那份快照**。
- **进程级被杀**（SIGKILL、OOM、硬重启）在写 tmp 与重命名之间：`finally` 不跑，tmp 作为孤儿留下，由 `reap_orphan_tmp_files` 在下次启动时、超过阈值后清掉。

所以「原子」在这里的确切含义是：**目标文件要么是旧的完整快照，要么是新的完整快照，不会是半份**。而代价是可能留下垃圾文件——这部分垃圾被显式的回收机制接管。

**`reap_orphan_tmp_files` 有一处容易忽略但很关键的正确性细节：`glob.escape`。**

```python
patterns = [glob.escape(file_name) + ".tmp.*", *extra_patterns]
```

docstring 写：「`glob.escape` 是必需的，因为 `file_name` 由 `working_dir + namespace` 组成，可以合法包含 glob 元字符（workspace `[v2]`、`*`、`?`）。朴素拼接会静默漏掉真正的孤儿，或者把匹配扩大到不相关存储类型的 tmp 文件」。

这里两种失败方向都写出来了：漏掉（垃圾不清）与扩大（删了别人的 tmp）。扩大更危险——那是在删一个可能正在被写的文件。

`extra_patterns` 的存在理由是历史迁移：「接受已构建好的 glob 模式，用于从旧命名方案迁移（例如 Faiss 早先固定的 `<meta>.tmp` 后缀，默认模式的尾部 `.*` 匹配不到它）」。

回收阈值 `TMP_REAP_AGE_SECONDS = 3600` 的理由是「足够大，使另一个活进程的在飞写不可能还在跑（数百万节点的 graphml 写几分钟就完成，不是几小时）」。

**`_replace_file` 是一段只在 Windows 上执行的退避重试，而它的注释解释了「为什么退避总额必须小」。**

```python
if sys.platform != "win32":
    os.replace(src, dst)
    return
```

非 Windows 直接替换——注释：「重试循环会给一条永不重试的路径加上一个分支和一句误导性的『after 1 attempts』日志」。

Windows 分支：`WINDOWS_REPLACE_MAX_ATTEMPTS = 8`，初始 0.05 秒，每次 ×1.5，上限 0.2 秒，共约 1.0 秒。触发条件是 `PermissionError`（`[WinError 5]` ACCESS_DENIED 或 `[WinError 32]` SHARING_VIOLATION），因为「另一个线程、协程或系统服务（例如搜索索引器、杀毒软件）暂时持有 dst 的打开句柄」。

而「为什么总额只能 1 秒」这段注释是这一页最有分量的设计判断：

「退避的 sleep 跑在进程**全局的单 worker storage-io 执行器**上，所以总额是对**每个**文件后端刷盘施加的最坏停顿，不只是正在重试的那个。保持在一秒左右：索引器或杀毒软件持有的句柄在几十到几百毫秒内清除，**而更长的持有更适合以失败暴露出来，而不是让排在那个 worker 后面的每个命名空间一起付账**」。

所以这是一个明确的资源权衡：用一个共享的单 worker 执行器换取「所有文件写串行、不争抢磁盘」，代价是任何一次退避都会阻塞所有后端。作者据此把退避总额压到 1 秒，并明说「超过就让它失败」。

**`_preserve_mode` 修的是一个静默的安全退化。**

```python
if not os.path.exists(dst): return
os.chmod(tmp, stat.S_IMODE(os.stat(dst).st_mode))
```

docstring：「没有这个，`os.replace` 交换 inode 后新文件会继承 umask 默认值——**先前快照上任何有意的限制（例如 `chmod 0600`）都会被静默放宽**」。

而模块 docstring 把 `os.replace` 不保留的东西列全了：「owner、group、ACL、xattr、硬链接关系，以及任何符号链接目标身份。**mode 位（rwx）被显式保留**——见 `_preserve_mode`」。

这张清单本身是知识：`rename(2)` 保持 inode 不变，所以「替换」这个动作在 POSIX 上其实不换 inode——但很多实现（以及 Windows 的 `MoveFileEx`）会，于是元数据会随旧 inode 一起消失。这里把能救的（mode）救了，救不了的写进文档。

**`write_json` 的两段式写与 `sanitized` 返回值闭环了整个协议。**

```python
try:
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(json_obj, f, indent=2, ensure_ascii=False)
except (UnicodeEncodeError, UnicodeDecodeError) as e:
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(json_obj, f, indent=2, ensure_ascii=False, cls=SanitizingJSONEncoder)
    sanitized = True
```

两点：**慢路径复用同一个 tmp 路径**，注释说「让操作在慢路径上也是单次重命名」；**返回 `sanitized` 布尔**，让调用方知道「磁盘上的内容已经与内存不同」，从而触发那个「把净化后的数据重载回共享 dict」的动作。

而 `SanitizingJSONEncoder` 剥的是孤立代理项（lone surrogate）——`_sanitize_string_for_json` 用正则检测，干净字符串**零拷贝返回**（注释：「99% 的情况」）。

`load_json` 有一个小但重要的边界处理：

```python
if not content.strip():
    logger.warning("Empty JSON file treated as missing: %s", file_name)
    return None
```

「已存在但空白」的文件被当作缺失——因为所有调用方都写成 `load_json(...) or {}`。若不这样，一个空文件会在 `json.loads` 处抛异常，而那是一个「用户已经遇过、但语义上等于没数据」的状态。

## 关键取舍

**源码事实：** `self._data` 是 `Manager().dict()` 代理的引用，全进程共享，**没有重载**；`storage_updated=True` 在这里表示「内存脏、待刷盘」，与文件型后端相反。

**源码事实：** `_storage_lock` 覆盖每一次 `self._data` 访问（读也一样），因为 Manager 代理跨进程不是自由线程的。

**源码事实：** 配置命名空间的 `_shared_workspace` 用 `f"{CONFIG_CONTAINER_TAG}@{realpath(workspace_dir)}"`，因为「一个常量键会把它们别名到同一个命名空间」。

**源码事实：** `index_done_callback` 用 `self._data.copy()` 而非 `dict(...)`，因为前者是单个 Manager RPC 整体编组，后者为每个值单独 RPC。

**源码事实：** 写文件移出事件循环，但快照在循环内、锁内完成；sanitize 后的重载在写的不可取消区内，且 `clear()`+`update()` 之间失败会重抛（否则空 dict 会被下次刷盘发布到磁盘上）。

**源码事实：** `CommitBookkeepingError` 分两路：有对账失败则重抛原始异常（共享视图不可靠且下次刷盘会发布它）；否则只记日志（文件已发布，重抛会把持久写报成失败，导致文档被标 FAILED 而其实行在磁盘上）。

**源码事实：** 共享内存版的提交**没有** `set_all_update_flags`，只有 `clear_all_update_flags` ——与文件型后端正好互补。

**源码事实：** `_as_row` 用 `CorruptStorageRecordError` 报出损坏行，避免它从本类内部逃成 `AttributeError` 而被上层误读为「存储失败」。

**源码事实：** `finalize` 在 `finally` 里释放共享命名空间持有，因为刷盘失败会被上层吸收、持有权会静默泄漏；泄漏导致「无人能成为最后离开者」+「第二个 `working_dir` 永久被拒」。

**源码事实：** `atomic_write` 用 per-writer tmp + `os.replace`；Python 异常时 finally 清 tmp 且目标保持旧快照，进程被杀时留孤儿由 `reap_orphan_tmp_files` 在一小时后回收。

**源码事实：** `reap_orphan_tmp_files` 必须 `glob.escape(file_name)`，因为工作区名可合法含 `*`、`?`、`[v2]`；朴素拼接会漏掉真孤儿或删掉别人的 tmp。

**源码事实：** Windows 的 `os.replace` 退避总额刻意压到约 1 秒，因为 sleep 跑在进程全局**单** worker 执行器上，更长的持有会阻塞所有文件后端。

**源码事实：** `_preserve_mode` 显式搬运 mode 位，否则 `os.replace` 后新文件继承 umask、把 `chmod 0600` 静默放宽。owner/group/ACL/xattr/硬链接/符号链接身份都**不**被保留，且已在模块 docstring 列明。

**源码事实：** `JsonDocStatusStorage` 的 `upsert` / `update_doc_status_fields` 同步刷盘，`delete` 延迟；分页 API 是有界 keyset 扫描，但整个存储仍常驻共享内存、刷盘仍重写整个文件——这是被显式声明的独立限制。

**架构推论：** 「共享内存 + 单文件」与「私有副本 + 重载」是两种自洽但互斥的设计，而这套代码同时存在两者——所以同一个字段名（`storage_updated`）与同一个方法名（`index_done_callback`）在两边含义相反。这不是命名混乱，而是**同一个抽象契约（`StorageNameSpace`）被两种截然不同的并发模型实现**的必然结果。代价是任何跨后端的通用代码都不能假设标志的方向，任何维护者读错一次就会写反向的逻辑。

**架构推论：** 「提交是整个命名空间的整体重写」这件事改变了「写入」的语义。在共享内存模型下，**任何一个写入者的刷盘都会把其他所有写入者的待定修改一起持久化**。docstring 为此专门点出「这对 chunk 追踪命名空间最重要，因为它们的行携带 purge 归属」。所以「我提交了」在这里不表示「我的修改持久了」，而表示「到目前为止所有修改都持久了」——这是一个更强也更模糊的保证，而依赖精确提交边界的逻辑不能用它。

**架构推论：** `file_atomic` 的设计把「原子」精确定义成「目标文件不会半份」，而把「可能留垃圾」显式接受并配套回收。这个分工是必要的：文件系统不提供「写临时文件并清理」的原子原语，只能靠 `rename` 的原子性 + 事后的孤儿回收。而回收必须带年龄阈值与精确 glob，否则会在两个方向上出错——这两点都不是可选项。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage` | 共享内存 + 单文件的键值存储，标志语义反转 |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage.__post_init__` | 配置命名空间的特殊路径与 `_shared_workspace` 键 |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage.initialize` | 单进程认领后读文件，其余进程共享 dict |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage.index_done_callback` | 快照 + 循环外写 + 对账 + 清脏标志 |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage.finalize` | 缓存刷盘后于 `finally` 释放持有 |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage._as_row` | 按名字报出损坏行，避免误归因为存储失败 |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage.upsert` | 打时间戳、写共享 dict、翻转全进程脏标志 |
| `lightrag/kg/json_kv_impl.py` | `JsonKVStorage.filter_keys` | 差集语义的存在性检查 |
| `lightrag/kg/json_doc_status_impl.py` | `JsonDocStatusStorage` | 同一协议的第二份实现（不继承） |
| `lightrag/kg/json_doc_status_impl.py` | `JsonDocStatusStorage.upsert` | 同步刷盘，因为它是摄入的恢复锚点 |
| `lightrag/kg/json_doc_status_impl.py` | `JsonDocStatusStorage.drop` | 破坏性、不串行化，要求调用方持 busy 预留 |
| `lightrag/kg/json_doc_status_impl.py` | `JsonDocStatusStorage.index_done_callback` | 三个文件后端里最高频的写入者 |
| `lightrag/file_atomic.py` | `atomic_write` | per-writer tmp + 重命名，异常时清 tmp |
| `lightrag/file_atomic.py` | `tmp_path_for` | 后缀含 PID/线程/纳秒，避免并发踩踏 |
| `lightrag/file_atomic.py` | `_replace_file` | 仅 Windows 的退避重试，总额约 1 秒 |
| `lightrag/file_atomic.py` | `_preserve_mode` | 搬运 mode 位，防止权限被 umask 放宽 |
| `lightrag/file_atomic.py` | `reap_orphan_tmp_files` | 按年龄回收硬杀留下的孤儿，须 `glob.escape` |
| `lightrag/utils.py` | `write_json` | 两段式写，返回「是否发生过净化」 |
| `lightrag/utils.py` | `load_json` | `utf-8-sig`，空文件等同缺失 |
| `lightrag/utils.py` | `commit_in_storage_io` | 把「发布 + 记账」包成不可取消单元 |

## 参考

- [lightrag 源码：kg/json_kv_impl.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/kg/json_kv_impl.py)
- [lightrag 源码：file_atomic.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/file_atomic.py)
- [Python 文档：multiprocessing.Manager](https://docs.python.org/3/library/multiprocessing.html#multiprocessing.Manager)
- [rename(2) 手册页](https://man7.org/linux/man-pages/man2/rename.2.html)

## 与主核 / AI 的关系

主核里最接近的是**共享缓冲区管理器（shared buffer pool）**：所有进程看到同一份页缓存，因此不需要「重载文件」这个概念——磁盘只是持久化目标，一致性由内存中的共享结构保证。这一页的 `Manager().dict()` 就是那个共享页缓存在 Python 层的退化版本。

**差别在于失效的责任方。** 主核的缓存有明确的淘汰与失效协议（脏页回写、pin/unpin、版本号），因为它必须支持随机小写；这里没有淘汰（内存常驻），失效问题被消解成「根本不存在副本」。而 `storage_updated` 的反转正是这一点的体现：在共享模型下，「失效」这个信号变成了「待刷盘」这个信号。

**「提交发布整个命名空间」与数据库的 checkpoint 同构。** 一次 checkpoint 把整个脏页集合刷到磁盘并更新控制文件，任何一个事务的提交都推进了全局水位。这里任何一个写入者的 `index_done_callback` 也是如此。所以「我的修改持久了吗」这个问题在两种系统里都不能靠「我调用了提交」来回答——必须问「在哪个水位之后」。

与 a01（存储契约）的分工很直接：那一页讲四类契约与「无事务」，这一页讲这个后端如何用共享内存把「无事务」的残留窗口压到最小——它至少保证了内存视图的强一致，代价是完全放弃多进程的内存隔离。

与 g02/g03 的对比是整组 G 的核心：**图与向量后端用「私有副本 + 文件重载」，键值后端用「共享内存」。所以面对「另一个进程改了磁盘」这件事，前者要决策「重载还是拒绝」，后者根本不需要决策。** 两个选择各自的代价都很清楚：共享内存换来无重载，换掉的是「每进程独立内存」和「写入可并行」。

对 AI 工程最实用的一条是**「原子写必须区分异常与硬杀」**。同一个 `atomic_write`，Python 异常路径能保证「目标文件是旧快照且不留垃圾」，硬杀路径只能保证「目标文件是旧快照」——垃圾要靠事后回收。任何号称「原子写」的实现都该问一句：它覆盖的是哪种失败？只覆盖前者的话，生产环境（OOM killer、容器重启）会留一地 tmp 文件。

另一条是 **`_as_row` 那条错误归因**。一行数据损坏与整个存储不可用，在异常层面都表现成「读失败了」，但正确处理完全不同：前者该跳过这一行，后者该熔断。把前者包成后者（这里是让 `AttributeError` 逃出去）会让一次局部损坏触发全局降级。做法很便宜：在数据边界上显式检查类型并抛一个**特指的**异常。

最后一条是 `_preserve_mode` 那个静默的安全退化。任何「写临时文件再替换」的实现（配置文件、密钥文件、缓存）都会遇到它：`os.replace` 不保留权限，`chmod 0600` 变成 umask 默认。这在凭据文件上就是安全漏洞，而在普通数据文件上只是「权限变宽了没人注意」。修法只有一处一行，但不修就没人会知道。
