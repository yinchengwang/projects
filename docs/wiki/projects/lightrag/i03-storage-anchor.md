---
project: lightrag
feature: storage-anchor
title: 存储锚点与排他锁
page_type: feature
schema_version: 1
refs:
  - path: lightrag/config_anchor.py
    symbols: [StorageAnchor, read_anchor, _write_temp]
  - path: lightrag/config_anchor.py
    symbols: [canonical_storage_uuid, new_storage_uuid, parse_anchor_payload]
  - path: lightrag/kg/anchor_lock.py
    symbols: [ExclusiveAnchorLock, acquire_anchor_lock_exclusive, anchor_bind_lock]
---

# 存储锚点与排他锁

## 一句话职责

用一个**放在所有工作区之外**的小文件声明「这个 `WORKING_DIR` 属于哪个配置容器的哪个 UUID」，使两个部署误用同一目录这件事从「静默损坏」变成「启动失败」；再配三把职责不同的文件锁，让「绑定身份」「离线迁移」「同一目录多服务器」三种并发各自有明确的准入规则。

## 机制是什么

锚点文件的路径与格式都极简，而这两点都是刻意的：

- **路径**：`<working_dir>/config_storage_anchor.json`，对每个后端、每个工作区都一样，直接放在 `working_dir` 下。「因为它绑定整个部署，所以它住在每个工作区之外，**空工作区也不例外**。没有设置挪动它」。
- **格式**：`schema_version`（整数 `1`）、`backend`、`storage_uuid`；JSON 后端额外有 `layout` 与 `members`。而 docstring 有一句关键词：**「没有 host、凭据、路径或连接串：身份被比较，连接细节不被比较」**。

第二条是这个设计最重要的取舍：锚点记录的是「身份」而不是「地址」。所以把一个部署从 `localhost:5432` 换到 `db.internal:5432` 而容器内容没变时，锚点仍然匹配——这正是想要的，因为「同一个容器」这个事实不因连接方式改变。

而 `members` 只在 JSON 后端存在，理由是 JSON 后端的容器就是一组文件，所以「容器包含哪些工作区」必须显式记录；数据库后端的容器天然知道自己有什么。

三把锁的分工写在 `anchor_lock.py` 的模块 docstring 里，而它先澄清了一个容易混淆的点：

> `<working_dir>/.lightrag_anchor.lock`，在它保护的锚点文件旁边。**它是与 JSON 配置存储在工作区快照旁边取的那把独占锁 `.lightrag_storage.lock` 不同的文件**——对空工作区两者都直接在 `working_dir` 里——而两者回答不同的问题：
>
> - 快照认领让第二个**服务器**离开一个 JSON 配置文件——**启动者之间互斥**；
> - 这把锁让**迁移**离开一个正在运行的部署——启动者彼此共享它，只与迁移互斥。

所以「两把锁都在同一目录里」不等于「它们做同一件事」。第二条那句「启动者彼此共享它，只与迁移互斥」把共享/独占的语义与业务意图绑定了：共享模式存在的唯一目的就是「让多个启动者共存，而迁移能看见它们全部」。

## 怎么实现的

**`canonical_storage_uuid` 的判据是「解析回来的字符串逐字等于输入」。**

```python
if not isinstance(value, str): return None
try: parsed = uuid.UUID(value)
except (ValueError, AttributeError, TypeError): return None
return value if str(parsed) == value else None
```

docstring：「`value` 当它是规范小写连字符形式的 UUID，否则 `None`。**只有规范形式，所以一个 UUID 的两种拼写永远不可能比较为不等**」。

这里的关键是它**不接受**非规范形式（大写、无连字符、带 `urn:uuid:` 前缀）。`uuid.UUID()` 能解析这些形式，而 `str(parsed)` 会归一化回规范形式——所以「`str(parsed) == value`」这个比较是「输入已经是规范形式」的判据。

为什么必须拒绝非规范形式？因为「两个 UUID 是否相等」这个比较若在两边各用不同拼写，字符串比较会说「不等」，而语义上它们是同一个容器。所以这里采取的是「**在入口处强制规范形式，于是之后的比较可以直接用字符串相等**」——把归一化成本从每个比较点挪到一个校验点。

这与 i02 里 `caseless_alias` 处理「比较用归一化形式」是相反的策略（那边容忍多种拼写），差别在于数据来源的信任度：UUID 由本程序生成（`new_storage_uuid` 直接返回 `str(uuid.uuid4())`），所以要求规范形式是合理的；工作区名来自用户，所以必须容忍。

**`StorageAnchor` 的 `__post_init__` 用一条等价式约束两个字段的关系。**

```python
if (self.backend == JSON_CONFIG_BACKEND) != (self.members is not None):
    raise ValueError(f"a {self.backend} anchor " + ("must carry a member tuple" if self.backend == JSON_CONFIG_BACKEND else "carries no members"))
if self.members is not None:
    object.__setattr__(self, "members", tuple(sorted(set(self.members))))
```

第一行用 `!=` 比较两个布尔：**「是 JSON 后端」与「有成员元组」必须同真同假**。这是一个很紧凑的不变量表达——它同时挡住「JSON 锚点没有成员列表」（不完整）与「数据库锚点带了成员列表」（矛盾）。

第二行在 `frozen=True` 的 dataclass 里用 `object.__setattr__` 强制排序去重。所以 `StorageAnchor.backend` 与 `.storage_uuid` 不可改，而 `.members` 在构造时被归一化成「排序去重的元组」——这让两个成员集合相同的锚点在**字节层面**相等，因此可以直接比较。

`with_member` 返回一个新实例而不是修改：

```python
return StorageAnchor(backend=self.backend, storage_uuid=self.storage_uuid, members=(*self.members, workspace))
```

而因为它会走 `__post_init__`，所以扩张后仍然自动排序去重。所以「加一个已存在的成员」不会产生重复——这是一个由构造器保证的幂等性。

**`parse_anchor_payload` 的校验顺序是按「能不能给出好错误」排的。**

第一步是形状：

```python
keys = set(payload)
expected = _JSON_ANCHOR_FIELDS if payload.get("backend") == JSON_CONFIG_BACKEND else _ANCHOR_FIELDS
if keys != expected:
    missing = sorted(expected - keys); extra = sorted(keys - expected)
    raise _unreadable(path, f"unexpected structure (missing {missing}, unexpected {extra})")
```

三个细节：

1. **`keys != expected` 是精确相等** ——既报缺失也报多余。所以一个多写了 `host` 字段的锚点会被拒，错误消息会具体说「unexpected ['host']」。这与 i01 里 `{"inputs", "outputs"} != example.keys()` 是同一种严格。
2. **期望的形状由 `payload.get("backend")` 决定** ——注释说明：「后端决定期望哪个形状；一个未准入或缺失的后端在结构检查之后被拒，好让结构检查先说出哪里不对」。所以顺序是「先按声明的后端校验形状，再校验后端本身合法」。
3. **缺失与多余分别排序列出** ——所以错误不只是「结构不对」，而是「缺了 X、多了 Y」。

然后版本、后端、UUID 依次校验，每条都有专属消息。版本那条用了 `type(version) is not int` 而不是 `isinstance`：

```python
if type(version) is not int or version != ANCHOR_SCHEMA_VERSION:
    raise _unreadable(path, f"unsupported schema_version {version!r}; expected integer {ANCHOR_SCHEMA_VERSION}")
```

`type(...) is not int` 排除了 `True`（`bool` 是 `int` 子类）与 `1.0`（`float` 不等于 int 类型）。所以一个 `schema_version: true` 的锚点会被拒。这与 h03 里 `top_k` 校验显式排除 `bool` 是同一个坑。

后端校验从注册表取准入列表：

```python
def _admitted_backends() -> tuple[str, ...]:
    from lightrag.kg import STORAGE_IMPLEMENTATIONS
    return tuple(STORAGE_IMPLEMENTATIONS["CONFIG_STORAGE"]["implementations"])
```

所以「哪些后端能出现在锚点里」这一事实只有一个来源（存储注册表），锚点模块不维护第二份。而 `publish_anchor` 也用同一个函数校验——所以「能写进去的」与「能读出来的」是同一个集合。

**`read_anchor` 的返回契约是「只有文件不存在才 `None`」，而这一条在整页里被重复强调。**

```python
try: info = os.lstat(path)
except FileNotFoundError: return None
except OSError as e: raise _unreadable(path, ...) from e
if stat.S_ISLNK(info.st_mode): raise _unreadable(path, "it is a symlink, which the next atomic publish would replace with a regular file (leaving its target stale); replace it with a copy of the file it points to")
if not stat.S_ISREG(info.st_mode): raise _unreadable(path, "it is not a regular file")
try:
    with open(path, "rb") as f: raw = f.read()
except FileNotFoundError: return None
except OSError as e: raise _unreadable(path, ...) from e
```

`FileNotFoundError` 出现了**两次**——一次在 `lstat`、一次在 `open`。第二次是必需的：文件可能在两次调用之间被删（另一个进程正在做「删除即重绑」）。所以两处都返回 `None` 让「读完之前被删」与「一开始就不存在」得到相同结果。

而符号链接的拒绝理由与 i02 里 `probe_snapshot` 完全一致：**下一次原子发布会把链接换成常规文件，让它的目标停在旧内容上**。错误消息也一样给出「换成副本」的修法。两处独立实现但结论一致，说明这是一个被认真推导过的结论而不是复制粘贴。

**`_unreadable` 的错误消息里写进了三条操作指引。**

```python
f"The configuration storage anchor {path} could not be read: {detail}. "
f"It is never treated as absent. Repair the file or restore it from "
f"backup; if this deployment is meant to rebind to the container its "
f"current configuration selects, delete the file and restart."
```

三条：**「它从不被当作缺失」**（说明这为什么是错误而不是降级）、「修复或从备份恢复」、「如果你想重绑到当前配置选择的容器，删掉这个文件并重启」。

第三条尤其重要，因为「删除锚点」正是官方认可的重绑方式（模块 docstring 最后一条规则）。所以错误消息把出路写出来了，运维不需要查文档就知道怎么办。

**`_write_temp` 是「写到临时文件并 fsync」的标准三步，而 `"x"` 模式是关键。**

```python
def _write_temp(tmp: str, anchor: StorageAnchor) -> None:
    data = json.dumps(anchor.to_payload(), indent=2, sort_keys=True) + "\n"
    with open(tmp, "x", encoding="utf-8") as f:
        f.write(data); f.flush(); os.fsync(f.fileno())
```

三点：

1. **`open(tmp, "x")`** ——独占创建。若临时文件名撞了（同一进程内几乎不可能，因为 `tmp_path_for` 含 PID/线程/纳秒），这里会抛而不是覆盖别人的临时文件。
2. **`f.flush()` 然后 `os.fsync(f.fileno())`** ——先把 Python 缓冲刷到 OS，再让 OS 刷到盘。少了 `flush` 的话 `fsync` 刷的是空缓冲。
3. **`sort_keys=True`** ——让同一份锚点的序列化结果确定。这使得「锚点文件内容」可比较（例如排障时 `diff` 两个备份）。

**`_publish_no_clobber` 是整页最复杂的一段，而它要解决的问题是「不覆盖已存在的文件」这个操作在 POSIX 上不存在。**

先把机制列出来：

| 平台 | 手段 | 为什么 |
|---|---|---|
| POSIX | `os.link(tmp, path)` 然后 `os.unlink(tmp)` | `link(2)` **从不替换**它的目标 |
| Windows | `os.rename(tmp, path)` | rename **原生拒绝**已存在的目标 |
| 无硬链接的文件系统 | `O_CREAT\|O_EXCL` 占名，再 `os.replace` 覆盖空占位 | 回退路径自身就是 no-clobber |

第一行是关键：`os.rename` / `os.replace` 都会**静默覆盖**，所以 POSIX 上没有「原子地创建并失败于已存在」的重命名原语。而 `link(2)` 有这个性质——若目标存在，它返回 `EEXIST` 而不动目标。

而 `os.link` 之后必须 `os.unlink(tmp)`，注释说明：「tmp 名是同一文件的第二个链接；删不掉它只留下一个无害的孤儿，不是一次失败的发布」。所以清理失败不升级为错误——这与 g01 里 `atomic_write` 的 `finally` 尽力删除是同一态度，但这里更宽松（连警告都没有），因为「多一个硬链接」比「多一个 tmp 文件」无害。

**无硬链接时的回退路径是这一段里最讲究的，注释把每一步的理由都写了。**

```python
os.close(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644))
try:
    os.replace(tmp, path)
except OSError as replace_error:
    try:
        if os.path.getsize(path) == 0:
            os.unlink(path)
    except OSError as cleanup_error:
        raise _EmptyClaimLeft(replace_error.errno, f"{replace_error}; the empty claim could not be removed either (...)",) from replace_error
    raise
```

注释：

> 一个没有硬链接的文件系统用 `O_CREAT | O_EXCL` 认领这个名字，**然后才**用临时文件替换这个空占位，所以回退路径自身就是 no-clobber 的，且**不依赖任何锁**（绑定锁 fail open，而键控锁看不见另一个进程树）。在替换落地之前锚点是一个空文件，而**每个读者都把它拒为不可读**——响亮，绝不是「缺失」——而在这个窗口里崩溃会把它留给运维去删，即那个被认可的重绑。

三句话各自是个设计决定：

1. **回退不依赖锁** ——这是一个必要的性质，因为锁在这个环境里可能根本拿不到（这正是走上回退路径的原因之一）。若回退依赖锁，那它就不可靠。
2. **中间状态是一个空文件，而空文件被所有读者拒为不可读** ——所以「崩溃在窗口里」不会产生「一个看起来不存在的锚点」，而是产生「一个明确损坏的锚点」。这就是「响亮而不是相反」的意思。
3. **空占位被留给运维删** ——而删掉正是官方认可的重绑路径。所以最坏情况的恢复动作与常规重绑是同一个动作。

而清理那一步有个细心之处：**先检查 `getsize(path) == 0` 再删**。因为若 `os.replace` 其实部分成功了（或者别的进程做了什么），删一个非空锚点就是数据损坏。所以「只在我能证明它还是我的空占位时才删」。

这引出了 `_EmptyClaimLeft` 这个异常的存在理由：**若连清理都失败，就必须明确报告「目标位置留着一个空文件」**。而 `publish_anchor` 里对它的处理专门给了一条消息：

```python
elif isinstance(e, _EmptyClaimLeft):
    outcome = f"No anchor was published, but an EMPTY file remains at {path}: every start refuses it as unreadable until it is deleted. Delete it, then start again."
```

所以三种失败被区分开：已发布但可能不持久、空占位残留、什么都没发布。每种给不同的 `outcome` 说明——这是「失败要能指导下一步」的具体实现。

**`publish_anchor` 用 `stage` 变量把「在哪一步失败」编码进错误消息。**

```python
stage = "create the anchor directory"
try:
    os.makedirs(directory, exist_ok=True)
    stage = "write the temporary anchor file"
    tmp = tmp_path_for(path)
    _write_temp(tmp, anchor)
    stage = "publish the anchor"
    if replace: os.replace(tmp, path)
    else: _publish_no_clobber(tmp, path)
    tmp = None
    stage = "fsync the anchor directory"
    _fsync_dir(directory)
```

五步依次推进 `stage`。而 `except OSError` 里用 `published = stage == "fsync the anchor directory"` 判断「是否已经发布了」：

```python
if published:
    outcome = "The anchor was published but may not be durable; the next start re-checks it against the container."
```

所以「目录 fsync 失败」被归类为「已发布但可能不持久」而不是「发布失败」。这个区分是对的：`os.replace` 已经返回，文件内容在页缓存里，只是没保证落盘。而 docstring 说下一次启动会「拿它与容器重检」——所以即使断电丢了这次写入，下次启动会重新走绑定流程。

这条是「部分成功」的处理范例：不谎报成功，也不谎报失败，而是报告确切的中间态并说明后果。

**`_fsync_dir` 把「不支持」与「失败」分开，而判据是一张 errno 白名单。**

```python
if os.name == "nt": return
flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
fd = os.open(directory, flags)
try: os.fsync(fd)
except OSError as e:
    if e.errno not in _DIR_FSYNC_UNSUPPORTED: raise
finally: os.close(fd)
```

`_DIR_FSYNC_UNSUPPORTED` 是 `{EINVAL, ENOTSUP, EOPNOTSUPP, EBADF}` ——这些 errno 表示「这个文件系统不能 fsync 一个目录」而不是「fsync 失败了」。

而 Windows 直接返回，因为「Windows 不能打开一个目录来 fsync」。注意 `O_DIRECTORY` 用了 `getattr(..., 0)` 兜底——这个标志在某些平台上不存在，用 0 意味着「不要求它是目录」。

这个分层的价值在于：目录 fsync 是「让重命名持久」的必要步骤，但它不是所有平台都支持。若把不支持当失败，那这个功能在所有不支持的平台上都不可用；若把所有失败都当不支持，那就丢掉了真正的错误。白名单是唯一正确的做法，而它的正确性依赖对每种文件系统的 errno 行为的了解。

**`anchor_lock.py` 的核心是 `_shared_holds` 这个模块级字典，而它的作用被注释点明为「跨 fork 继承」。**

```python
_shared_holds: dict[str, _SharedHold] = {}
```

注释：

> 锁路径 → 这个进程树的共享持有。**跨 `fork` 继承，而这正是重点**：一个 Gunicorn worker 在这里找到它 master 的持有并把自己计入，而不是打开自己的描述符。master 的计数也被继承，所以一个 worker 的释放永远不会到零，也永远不会解锁它与 master 共享的那个描述。

这两句解释了一个非显然的机制：`flock` 的锁是绑在**打开的文件描述（open file description）**上的，而 `fork` 出的子进程继承那个描述。所以如果 worker 各自打开文件并 `flock`，它们拿到的是**不同的描述**——虽然同一进程树仍能拿到共享锁（`LOCK_SH` 之间兼容），但在 worker 释放时可能会解锁整个描述。

而这里的做法是「计数共享」：

```python
hold = _shared_holds.get(path)
if hold is not None:
    hold.holders += 1
    return
```

所以第一个调用者真的取锁，后续调用者只加计数。而 `_SharedHold.holders` 默认 1。

这不只是优化，而是**正确性**：若每个 worker 各自 `flock`，那 Gunicorn 的 worker 会被重启（`max_requests` 或崩溃），而重启期间 master 的锁可能因为 worker 退出而被释放——于是迁移可以进来。用「master 取锁、worker 计数」把这个窗口消掉。

**三把锁的「fail open / fail closed」方向是这一层最有意思的设计，而每个方向都有明确理由。**

| 锁 | 拿不到时 | 理由 |
|---|---|---|
| 锚点共享锁（启动者） | **fail open**（警告后继续） | 「拒绝会停掉一个今天就在那里跑的单服务器」 |
| 锚点独占锁（迁移） | **fail closed**（除非 `--assume-exclusive`） | 迁移必须有排他证据 |
| 绑定锁 | 竞争 **等待**（有超时）；其他失败 fail open | 「被识别的竞争从不被绕过」 |

第一行的理由写在模块 docstring 里：「启动者在文件系统或平台无法加锁的地方（无 lockd 的 NFSv3、SMB/CIFS、只读目录、或 Windows——它的 `msvcrt.locking` 没有共享模式）**fail open**：一条警告，然后继续——这正是快照认领已经采取的姿态」。

所以方向由「失败时哪边更糟」决定：启动失败是**确定的服务不可用**，而失去互斥是**可能的**数据风险。所以启动者 fail open。

第二行反向：迁移的目标就是「证明没有别人在用」，所以拿不到锁就无法证明——必须 fail closed，除非运维显式声明「我已手工停掉所有」。

而第三行更精细：**区分「被识别的竞争」与「无法加锁」**。

```python
except BlockingIOError:
    pass            # 继续轮询，直到超时
except OSError as e:
    if fcntl is not None:
        _warn_bind_unlocked(path, ...); break    # 其他 errno：fail open
    # msvcrt: "held by another process" 与 "cannot lock here" 是同一个错误；
    # 安全的读法是 "held"
```

docstring 解释了为什么这样做：「因为**没有 errno 列表能在 NFS 与 SMB 上可靠地区分『不支持』**，而拒绝会停掉一个今天就在那里跑的单服务器；一次丢失的成员追加随后由迁移的交叉检查发现」。

所以这里承认了一个技术限制（errno 不可靠），选择了「宁可漏掉互斥也不能停服务」，并把兜底交给了另一个机制（迁移的交叉检查）。这是一个成熟的权衡：**互斥缺失的风险被另一个检查覆盖了，所以不必在这里冒险**。

而 Windows 那句注释很实在：`msvcrt` 对「被别的进程持有」与「这个文件系统不支持」返回同一个错误，所以只能按「被持有」解读——即等待到超时。方向是保守的（多等），而超时后的错误消息给出指引。

**`acquire_anchor_lock_exclusive` 有一条「本进程已经持有共享锁」的前置检查。**

```python
if path in _shared_holds:
    raise ConfigurationAnchorLockError("This process already holds the configuration anchor lock {path} as a starter; a migration cannot run inside a running instance.")
```

这条挡的是「在一个运行中的实例里跑迁移」——而 `flock` 本身不会拦它（同一个文件描述上的升级行为不确定），所以必须显式拒绝。

**`anchor_bind_lock` 是一个 `@asynccontextmanager`，而它的轮询设计有一个不得不这样做的理由。**

```python
while not locked:
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        locked = True
    except BlockingIOError: pass
    ...
    if time.monotonic() >= deadline:
        raise ConfigurationAnchorLockError(...)
    await asyncio.sleep(0.05)
```

用「非阻塞尝试 + 50ms 睡眠」而不是阻塞的 `flock`，因为**这是一个 async 上下文管理器**——阻塞调用会卡住整个事件循环。docstring 明说「等待（轮询，**从不阻塞事件循环**）直到 `timeout`」。

而超时的错误消息给出「什么都没写」这个关键信息：「另一个进程已经持有配置锚点绑定锁超过 {timeout} 秒……**什么都没写**。等它启动完成后重试」。所以调用方知道可以安全重试。

`DEFAULT_BIND_LOCK_TIMEOUT_SECONDS = 120.0` 的注释给了量级依据：「一个绑定是一次身份写、flush 与回读再加上一次文件发布；这足够了」。

**最后一条规则是一个明确的能力边界声明。**

模块 docstring 结尾：

> 一个本地目录锁**不覆盖**一个用另一个 `working_dir` 访问同一远端配置容器的部署；那些必须由运维停止并协调。**不提供分布式锁**。

这条很重要，因为它是唯一一个「系统不解决问题」的诚实声明。三把锁全部基于**本地文件**，所以它们只能协调共享同一文件系统的进程。而「两个不同的 `working_dir` 指向同一个 PostgreSQL 配置容器」这个场景下，它们互相看不见——而那种情况下的唯一屏障是容器的 UUID 身份检查（`IDENTITY_UUID_MISMATCH`）。

所以整页的两半是互补的：**锁管本地进程，身份管跨机部署**。锁管不了的地方，身份检查会在启动时发现不一致并拒绝。

## 关键取舍

**源码事实：** 锚点固定在 `<working_dir>/config_storage_anchor.json`，「对每个后端每个工作区都一样」，因为「它绑定整个部署，所以住在每个工作区之外，空工作区也不例外」。

**源码事实：** 锚点只记 `schema_version` / `backend` / `storage_uuid`（JSON 后端加 `layout` / `members`），**没有 host、凭据、路径或连接串**——「身份被比较，连接细节不被比较」。

**源码事实：** `canonical_storage_uuid` 只接受规范小写连字符形式（`str(parsed) == value`），所以「一个 UUID 的两种拼写永远不可能比较为不等」——归一化在入口做，比较点直接用字符串相等。

**源码事实：** `StorageAnchor.__post_init__` 用 `(backend == JSON) != (members is not None)` 表达「两个字段必须同真同假」，并 `object.__setattr__` 把 `members` 归一化为排序去重元组，所以两个成员集合相同的锚点字节层面相等。

**源码事实：** `parse_anchor_payload` 先按声明的后端校验**精确的键集合**（缺失与多余分别报出），再校验版本（`type(...) is not int` 排除 `bool` 与 `float`）、后端（从存储注册表取准入列表）、UUID（规范形式）。

**源码事实：** `read_anchor` 只有 `FileNotFoundError` 返回 `None`，且**两处**都这样处理（`lstat` 与 `open`），因为文件可能在两次调用之间被删——「删除即重绑」是官方路径。

**源码事实：** 符号链接与非常规文件被拒，理由与 i02 的 `probe_snapshot` 一致：「下一次原子发布会把它换成常规文件，让它的目标停在旧内容上」。

**源码事实：** `_write_temp` 用 `open(tmp, "x")` 独占创建、`flush` 后 `fsync`、`sort_keys=True` 让序列化确定。

**源码事实：** `_publish_no_clobber` 三平台三手段：POSIX 用 `os.link`（从不替换目标）、Windows 用 `os.rename`（原生拒绝已存在）、无硬链接时用 `O_CREAT|O_EXCL` 占名再替换——「回退路径自身就是 no-clobber 的，**且不依赖任何锁**」。

**源码事实：** 回退路径的中间态是一个**空文件**，而「每个读者都把它拒为不可读——响亮，绝不是『缺失』」；崩溃在窗口里留待运维删除，而那正是被认可的重绑动作。清理时**先确认 `getsize == 0`** 才删；连清理都失败则抛 `_EmptyClaimLeft` 并明确报告「目标位置留着一个空文件」。

**源码事实：** `publish_anchor` 用 `stage` 变量把失败定位到五步之一，并把「目录 fsync 失败」归类为「**已发布但可能不持久**」（不是失败），因为「下一次启动会拿它与容器重检」。

**源码事实：** `_fsync_dir` 用 errno 白名单（`EINVAL` / `ENOTSUP` / `EOPNOTSUPP` / `EBADF`）区分「文件系统不支持」与「fsync 失败」；Windows 直接返回。

**源码事实：** `_shared_holds` 跨 `fork` 继承，所以 Gunicorn worker「找到 master 的持有并把自己计入」——「worker 的释放永远不会到零，也永远不会解锁它与 master 共享的那个描述」。

**源码事实：** 三把锁的失败方向不同：启动者共享锁 **fail open**（「拒绝会停掉一个今天就在那里跑的单服务器」）；迁移独占锁 **fail closed**（除非 `--assume-exclusive`）；绑定锁对**被识别的竞争等待到超时**，对其他失败 fail open。

**源码事实：** 绑定锁用「非阻塞尝试 + 50ms 睡眠」轮询而不是阻塞 `flock`，因为它是 async 上下文管理器；Windows 上 `msvcrt` 对「被持有」与「不支持」返回同一错误，所以一律按「被持有」等待。

**源码事实：** `acquire_anchor_lock_exclusive` 显式拒绝「本进程已持共享锁」的情形——「一个迁移不能在运行中的实例里跑」。

**源码事实：** 模块明确声明能力边界：「一个本地目录锁**不覆盖**一个用另一个 `working_dir` 访问同一远端配置容器的部署；那些必须由运维停止并协调。**不提供分布式锁**」。

**架构推论：** 锚点文件记录的「身份而非地址」是这一层最关键的判断，因为它决定了锚点能容忍哪些变化。换连接地址、换凭据、换网络路径都不该让它失效；而换容器（哪怕同名）必须让它失效。「容器 UUID」恰好是那个不随部署方式改变的常量，所以它是正确的身份。

**架构推论：** 三个「不覆盖」的失败模式（本地锁不管跨机、绑定锁 fail open 可能丢成员、空占位需人工清理）都被显式声明并各自配了下游兜底：跨机由 UUID 身份检查兜、丢失的成员追加由迁移的交叉检查兜、空占位由「删除即重绑」这条官方路径兜。**能力边界写清楚并接上兜底，比声称「覆盖所有情况」有用得多**——因为前者可被验证，后者只会在生产里发现反例。

**架构推论：** 「无硬链接时的回退不依赖锁」这条要求看起来是细节，但它揭示了一个原则：**降级路径不能依赖它正在降级绕开的那个机制**。既然走上回退是因为「原子 no-clobber 不可用」，那回退就不能靠「锁来保证互斥」——否则在最需要它的时候它恰好不可靠。`O_EXCL` 提供的正是「不需要外部协调的互斥」，这是唯一自洽的选择。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/config_anchor.py` | `StorageAnchor` | 冻结的绑定记录，`members` 构造时排序去重 |
| `lightrag/config_anchor.py` | `StorageAnchor.with_member` | 返回新实例，自动去重，加已有成员是幂等 |
| `lightrag/config_anchor.py` | `read_anchor` | 只有不存在返 `None`，链接与非常规文件拒 |
| `lightrag/config_anchor.py` | `parse_anchor_payload` | 精确键集合 + 版本 + 后端 + UUID 四段校验 |
| `lightrag/config_anchor.py` | `canonical_storage_uuid` | 只接受规范形式，归一化在入口做 |
| `lightrag/config_anchor.py` | `new_storage_uuid` | 生成容器身份 |
| `lightrag/config_anchor.py` | `_write_temp` | `"x"` 独占创建，flush 后 fsync |
| `lightrag/config_anchor.py` | `publish_anchor` | 五段 `stage` 定位，三种失败分别说明 |
| `lightrag/config_anchor.py` | `_publish_no_clobber` | 三平台三手段，回退不依赖锁 |
| `lightrag/config_anchor.py` | `_fsync_dir` | errno 白名单区分不支持与失败 |
| `lightrag/config_anchor.py` | `_EmptyClaimLeft` | 空占位清理失败时明确报告残留 |
| `lightrag/config_anchor.py` | `anchor_path` | `<working_dir>/config_storage_anchor.json` |
| `lightrag/kg/anchor_lock.py` | `_shared_holds` | 跨 fork 继承的共享持有计数 |
| `lightrag/kg/anchor_lock.py` | `acquire_anchor_lock_shared` | 启动者共享锁，竞争时拒绝、其他失败 fail open |
| `lightrag/kg/anchor_lock.py` | `ExclusiveAnchorLock` | 迁移的独占持有，`release` 幂等 |
| `lightrag/kg/anchor_lock.py` | `acquire_anchor_lock_exclusive` | 迁移独占锁，拿不到就拒绝除非运维声明 |
| `lightrag/kg/anchor_lock.py` | `anchor_bind_lock` | async 上下文管理器，轮询不阻塞事件循环 |
| `lightrag/kg/anchor_lock.py` | `_warn_unlocked` | fail open 时说明「迁移看不见本进程」 |
| `lightrag/kg/anchor_lock.py` | `anchor_lock_path` | `realpath` 让同一目录的两种拼写成一个键 |

## 参考

- [lightrag 源码：config_anchor.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/config_anchor.py)
- [lightrag 源码：kg/anchor_lock.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/kg/anchor_lock.py)
- [link(2) 手册页](https://man7.org/linux/man-pages/man2/link.2.html)
- [flock(2) 手册页](https://man7.org/linux/man-pages/man2/flock.2.html)

## 与主核 / AI 的关系

主核里最接近的是**数据库的 system identifier 与数据目录锁**：一个 PostgreSQL 数据目录里有一个 `PG_VERSION` 与 `global/pg_control` 记录着集群身份，而 `postmaster.pid` 是一个「这个目录正在被某个服务器使用」的互斥标记。把一个数据目录指给两个实例会立刻失败而不是静默损坏——这正是锚点文件在做的事。

**差别在于身份的用途。** 主核的 system identifier 主要用于复制（判断主备是否同源），锚点则专门用于「防止误绑」。而两者的手段一致：把「这个目录属于谁」写进目录本身，让任何「以为它属于自己」的进程立刻发现不对。

**锁的方向选择与主核的 `postmaster.pid` 正好相反。** 主核在主进程启动时**独占**拿到数据目录锁，第二个实例启动**直接失败**——fail closed。而这里的启动者用**共享**锁（多个服务器可以共存），只有迁移才独占。这个反向来自业务差异：主核里「同一数据目录两个实例」永远是错的；这里「同一 `WORKING_DIR` 上有多个服务器/SDK/维护工具」是正常用法。

与 i01（配置存储）/ i02（配置分片）的关系：那两页定义「配置存在哪、键长什么样、快照文件在哪」，这一页定义「怎么证明你没搞错目录」。三者合起来构成配置层的完整自保护。

与 g04（嵌入基线）的关系：基线的可信度依赖「容器的身份没变」，而这个身份正是锚点记录的 `storage_uuid`。所以锚点的 UUID 校验是基线校验的前置条件——若容器被换了，那基线说的「已采纳的空间」就已经不是当前容器的空间了。

对 AI 工程最实用的一条是**「用身份而不是地址来做绑定校验」**。任何需要「这个目录/容器是不是我上次用的那个」的场景，记录地址是错的（地址会变），记录身份才对。而身份必须是「不随部署方式改变的常量」——UUID 满足，容器名不满足（可能被复用）。

另一条是**「降级路径不能依赖它正在绕开的机制」**。这里的例子是「无硬链接时的回退不依赖锁」。类似的情形在别处也常见：「配置读不出来时用默认值」不该依赖「默认值文件能读」；「网络失败时用本地缓存」不该依赖「缓存服务可用」。判据是：**这条路径被走到的时候，它依赖的东西恰好是坏的。**

最后一条是 `publish_anchor` 那种「三种失败分别说明」。写文件失败不是一种情况而是三种：已发布但可能不持久、留了个空占位、什么都没做。每种的正确下一步不同（等下次启动 / 删了重试 / 修权限重试）。把它们合并成「写入失败」会让运维做错动作——而错误的消息比没有消息更糟，因为它会引导人去修一个不存在的问题。
