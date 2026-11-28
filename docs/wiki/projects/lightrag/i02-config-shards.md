---
project: lightrag
feature: config-shards
title: 配置分片与工作区发现
page_type: feature
schema_version: 1
refs:
  - path: lightrag/config_shards.py
    symbols: [validate_config_workspace, validate_deployment_workspace, DiscoveredShard]
  - path: lightrag/config_shards.py
    symbols: [probe_snapshot, _physical_id, _differ_only_in_normalization]
---

# 配置分片与工作区发现

## 一句话职责

JSON 后端的配置不是「一个文件」而是「每个工作区一份快照文件」，所以需要回答两个问题：**每个工作区的快照该放在哪**（固定路径，任何设置挪不动），以及**磁盘上现有哪些工作区**（有界扫描，且必须能识别「两个名字其实是同一个目录」这种会毁掉数据的布局）。

## 机制是什么

模块 docstring 用四条规则把它讲清了，而每条都指向一个具体的破坏场景：

**第一条：固定路径。** 工作区 `w` 的快照在 `WORKING_DIR/w/kv_workspace_config.json`，它的生命周期认领文件在旁边；空工作区直接用 `WORKING_DIR` 本身。「没有任何设置挪动它们。**用 `json_config_dir` 解析，绝不用第二条规则**」。

「绝不用第二条规则」这句话是这一层最重要的纪律——因为一旦有两条推导路径（例如某处用 `os.path.join` 自己拼），它们必然在某个边界（空工作区、末尾斜杠、Windows 分隔符）分叉，而分叉的后果是「找不到自己的配置」并把它当成「没有配置」。

**第二条：五个保留名。** 一个工作区不能与 `WORKING_DIR` 下那五个部署级文件同名（锚点、它的两把锁、空工作区的快照与认领），否则它的**目录**会与那个**文件**撞车。

而这一条的实际范围比「五个名字」大得多，docstring 列了四种别名化方式：

1. **Unicode 大小写折叠与规范化**（`caseless_alias`）；
2. **Windows 的尾部点/空格剥离**；
3. **只由点和空格组成的名字**（Windows 会把它剥成 `WORKING_DIR` 本身）；
4. **带盘符的名字**（如 `C:`，Windows 会把它拼到 `WORKING_DIR` 之外）。

而且这四条「**在每个平台上都适用**，好让数据保持可移植」。这一点很关键：如果只在 Windows 上检查，那一个在 Linux 上创建的叫 `con` 或 `foo.` 的工作区搬到 Windows 就会出问题——而那时数据已经存在了。

**第三条：扫描有界。** `discover_shards` 只探测根快照与每个直接子目录里的一个文件，**从不递归，从不读业务文件**。

**第四条：离线读是严格的。** `read_shard_file` **只在文件不存在时返回 `None`**；无法解码的内容或「不是映射的映射」的载荷都抛。

## 怎么实现的

**`validate_config_workspace` 是三道检查的串联，而三道检查各有独立的破坏场景。**

```python
validate_workspace(workspace)                                  # 基础：路径穿越、$ 前缀
if ntpath.splitdrive(workspace)[0]: raise ...                  # 盘符
if workspace and not caseless_alias(workspace): raise ...       # 只剩点和空格
if caseless_alias(workspace) in _RESERVED_CASELESS: raise ...   # 保留名
```

第二道用 `ntpath.splitdrive` 而不是 `os.path.splitdrive` —— 这一点很重要：`ntpath` 是 Windows 语义的实现在任何平台上都可用，所以在 Linux 上也能识别 `C:` 这种名字。若用 `os.path.splitdrive`，在 POSIX 上它什么都不拆，检查就失效了。而这正是「在每个平台上都适用」这条规则的具体实现。

错误消息也写得很有针对性：「**它的第二个字符是 `:`**，所以 Windows 把它读作盘符限定并把它拼到 `WORKING_DIR` 之外」。指出「第二个字符」是因为 `splitdrive` 的判据就是那个位置——这让读者能立刻对上代码。

第三道 `if workspace and not caseless_alias(workspace)` ——注意 `caseless_alias` 做了 `name.rstrip(" .")`，所以一个叫 `"..."` 或 `"   "` 的名字会变成空串（falsy），于是被拒。而 `if workspace` 前置条件让空工作区（合法）不被这道检查拦下。

错误消息解释了后果：「它只由点和空格组成，**Windows 会从一个路径组件里剥掉这些，所以它的目录会是 `WORKING_DIR` 本身——空工作区的那个**」。

**`caseless_alias` 是整页最精巧的一个函数，而它的构造是「剥尾 → 规范化 → 折叠 → 再规范化」。**

```python
def caseless_alias(name: str) -> str:
    stripped = unicodedata.normalize("NFD", name.rstrip(" ."))
    return unicodedata.normalize("NFD", stripped.casefold())
```

四步各有理由：

1. **`rstrip(" .")` 先做** ——这是 Windows 的行为（路径组件的尾部点与空格被剥离），而且必须在规范化**之前**做，因为它们可能出现在任何位置。
2. **`normalize("NFD")` 在折叠前** ——因为 `casefold` 在合成形式上可能给出不同结果，先分解让折叠可预测。
3. **`casefold()` 而不是 `lower()`** ——`casefold` 是「大小写无关比较」的正确选择，它能处理 `ß` → `ss`、`K`（开尔文符号）→ `k` 这类折叠。
4. **`normalize("NFD")` 在折叠后** ——因为 `casefold` 可能产生需要再分解的字符。

而 docstring 有一句跨模块的警告：

> 设置向导无法解码 `\u` 转义，所以它通过映射**仅有的那些折叠后是纯 ASCII 的非 ASCII 字符**（`_ASCII_FOLDING_NON_ASCII`）来匹配这条规则；**保持两者同步**。

`_ASCII_FOLDING_NON_ASCII` 是一张 13 项的硬编码表：

```python
{"\u00df": "ss", "\u017f": "s", "\u037e": ";", "\u1e9e": "ss", "\u1fef": "`",
 "\u212a": "k", "\ufb00": "ff", "\ufb01": "fi", "\ufb02": "fl", "\ufb03": "ffi",
 "\ufb04": "ffl", "\ufb05": "st", "\ufb06": "st"}
```

注释解释了这张表的完备性：「**每一个折叠后是纯 ASCII 的非 ASCII 字符**：一个名字能用它们中的任何一个来别名一个 ASCII 保留名」。所以这张表是「攻击面」的完整枚举——一个名叫 `kevin` 用开尔文符号 `K` 开头（`\u212a`）会折叠成 `k`，于是别名了 `kevin`。

而「一个测试把这张表钉到 `caseless_alias`」——**用测试维护两处实现的一致性**。这是被迫的：设置向导不能 import 这个模块（它可能在不同环境跑），所以只能复制逻辑；而复制就会漂移。用测试钉住是这种情况下唯一可靠的办法。

`_RESERVED_CASELESS = frozenset(caseless_alias(n) for n in RESERVED_WORKSPACE_NAMES)` —— 保留名集合也在**别名形式**上比较，所以 `KV_WORKSPACE_CONFIG.JSON`（大写）也会被拒。

**`validate_deployment_workspace` 把「目录规则」与「名字规则」按后端分层。**

```python
from lightrag.kg.working_dir_lock import uses_working_dir
if uses_working_dir(*storages):
    return validate_config_workspace(workspace)
return validate_workspace(workspace)
```

docstring：「`validate_workspace`（路径穿越、保留的 `$` 前缀）对每个后端都成立。`validate_config_workspace` 的目录规则——盘符限定名、只剩点/空格的名、五个保留根名——存在是因为**一个工作区目录会被创建在 `WORKING_DIR` 下**，所以它们只在 `storages` 里有一个把数据放在那里时才适用。**一个完全在数据库后端上的部署不创建这样的目录，并保留它此前能用的每个名字**」。

这是一个很成熟的兼容性判断：新加的校验只在「新校验所假设的前提成立」时生效。若一个纯 PG 部署的用户有一个叫 `foo.` 的工作区，升级后不该突然启动失败——因为那个名字从未变成目录。

而 `uses_working_dir(*storages)` 的检查包含了配置存储在自身（docstring 明说「配置存储也在内」），因为 JSON 配置存储确实会在 `WORKING_DIR` 下建目录。

**`json_config_dir` 与 `json_config_path` 两行代码，但「唯一推导入口」的地位是它们全部的价值。**

```python
def json_config_dir(working_dir: str, workspace: str) -> str:
    validate_config_workspace(workspace)
    root = os.path.abspath(working_dir)
    return os.path.join(root, workspace) if workspace else root

def json_config_path(working_dir: str, workspace: str) -> str:
    return os.path.join(json_config_dir(working_dir, workspace), CONFIG_JSON_FILE_NAME)
```

三个细节：

1. **每次调用都重新 `validate_config_workspace`** ——不是冗余，而是「不可信输入在这里被挡住」。`workspace` 可能来自 HTTP 头（`LIGHTRAG-WORKSPACE`）、环境变量、或配置文件，所以每次使用都该校验。
2. **`os.path.abspath(working_dir)`** ——把相对路径固化，否则同一个 `working_dir` 在不同 CWD 下解析出不同结果，而那是「找不到配置」的经典来源。
3. **空工作区返回 `root` 本身** ——`os.path.join(root, "")` 其实也返回 `root`，但显式写成三元式让意图可读（并且避免 `os.path.join` 在末尾加分隔符的行为差异）。

**`probe_snapshot` 的三档返回与三档异常，界线在于「存不存在」与「能不能用」。**

```python
try:
    info = os.lstat(path)
except FileNotFoundError:
    return False
except OSError as e:
    raise _shard_error(path, f"{type(e).__name__}: {e}") from e
if stat.S_ISLNK(info.st_mode):
    raise _shard_error(path, "it is a symlink, which the first atomic save would replace with a regular file (leaving its target stale); replace it with a copy of the file it points to")
if not stat.S_ISREG(info.st_mode):
    raise _shard_error(path, "it is not a regular file")
return True
```

docstring：「`False` 只当它不存在。任何存在的、不是常规文件的东西——一个目录、一个符号链接——**被拒绝而不是跟随或跳过**」。

符号链接被单独拎出来是因为它有一个具体的破坏机制：**第一次原子保存会把它替换成一个常规文件**（`os.replace` 换掉 inode），于是链接指向的那个目标文件永远停在旧内容上——而运维以为自己改的是被链接的那份。

而错误消息给出修法：「用一份它指向的文件的**副本**替换它」。所以修法不是「删掉链接」而是「换成真文件」。

`os.lstat` 而不是 `os.stat` 是关键：`lstat` 不跟随链接，所以能看到「这是个链接」这个事实。

**`_physical_id` 返回 `(st_dev, st_ino)`，而它是「两个路径是不是同一个目录」的唯一判据。**

```python
def _physical_id(path: str) -> tuple[int, int] | None:
    try:
        info = os.stat(path)
    except FileNotFoundError:
        return None
    except OSError as e:
        raise _shard_error(path, f"{type(e).__name__}: {e}") from e
    return (info.st_dev, info.st_ino)
```

这里用 `os.stat`（跟随链接）而不是 `lstat` ——因为要判断的是「链接指向的目标」。所以同一个函数在处理 `probe_snapshot`（要看链接本身）与这里（要看目标）时用法相反，而两者的选择都是对的。

`(st_dev, st_ino)` 这个组合是 POSIX 上的「物理文件标识」标准做法：单独用 inode 号会在跨设备时撞车，加上设备号才唯一。

**`_differ_only_in_normalization` 是一行，但它的方向选择（NFC 比较）值得看。**

```python
return a != b and unicodedata.normalize("NFC", a) == unicodedata.normalize("NFC", b)
```

判据是「字符串不同但 NFC 形式相同」——所以它是「这些是两个拼写方式，但语义上是同一个名字」的判据。

而 `a != b` 前置是必需的：若两个名字完全相同，`normalize` 也相同，但那不是「只在规范化上不同」——它就是同一个名字。这个区分在下游很重要，因为「同一拼写」与「不同拼写映射到同一处」需要不同的处理。

**`physical_directory_aliases` 是整页最复杂的函数，而它要解决的是「HFS+ 与 APFS 两种相反的行为」。**

先看它算法的骨架：

```python
root = os.path.abspath(working_dir)
entries = {entry.name: entry.path for entry in _child_directories(root)}
requested = set(names)
candidates = requested | {""} | set(entries)
physical: dict[tuple[int, int], set[str]] = {}
for name in candidates:
    identity = _physical_id(os.path.join(root, name) if name else root)
    if identity is not None:
        physical.setdefault(identity, set()).add(name)
```

所以它把「请求的名字 + 空工作区 + 所有实际存在的子目录」放进一组候选，按物理身份分组。**同一个物理身份下有多个名字 = 别名组。**

然后是一个精细的过滤：

```python
for group in physical.values():
    kept = {
        name for name in group
        if not (
            name in entries and name not in requested
            and any(
                other in group and other not in entries
                and _differ_only_in_normalization(name, other)
                and _snapshot_owner(entries[name]) in (None, other)
                for other in requested
            )
        )
    }
    if len(kept) > 1: groups.append(sorted(kept))
```

这个过滤处理的是一个非对称情形，而 docstring 解释得很清楚：

> 当一个**列出的名字**与一个**被请求的名字**只在 Unicode 规范化上不同、被请求的拼写自己没被列出、且该目录的快照命名了被请求的工作区或完全没命名时，**那个列出的名字不是被请求名字的第二个名字**：那是**一个分解型文件系统（HFS+）列出被请求工作区自己的目录**的方式。

所以场景是：用户请求 `café`（NFC 形式），而 HFS+ 把目录列成 `cafe\u0301`（NFD 形式）。这时「列出的名字」与「请求的名字」是同一个目录的两种拼写，**不该**报「一个目录两个工作区」。而判据是「快照的 owner 是请求的那个名字或没有 owner」——因为快照的 owner 行是权威，它说的是哪个工作区。

反过来：

> 一个**被列出的拼写所拥有**的快照是另一个工作区，而一个规范化不敏感的文件系统（APFS）会把两种拼写都解析到它上面——**一个别名**。

所以同样是「NFC 相同、拼写不同」，两种情况的处理相反，而区分靠**快照里的 owner 行**。这个设计很实在：文件系统层面无法区分「同一个工作区的两种拼写」与「两个工作区的两种拼写」，唯一能区分的是快照自己声明它属于谁。

**`_snapshot_owner` 的「从不抛异常」是一条刻意的容错。**

```python
try:
    if not probe_snapshot(path): return None
    rows = read_shard_file(path) or {}
except ConfigurationIdentityError:
    return None
```

docstring：「只用于分辨一个仅因 Unicode 规范化而不同的列表拼写代表哪个工作区。**从不抛**：快照在它被打开的地方被完整校验，并且先被探测所以 FIFO 永远不会被读」。

两点：

1. **它只服务于「拼写归一化」这一个判断**，所以它不需要完整校验——若快照坏了，`discover_shards` 会在真正打开它时报错，这里只需要「读不出 owner」这个较弱的信息。
2. **`probe_snapshot` 前置**是为了「一个 FIFO 永远不会被读」——`read_shard_file` 会 `open` 一个 FIFO 并永远阻塞，所以必须先确认它是常规文件。

第 2 点是一个具体的挂死风险，而它的防护是「先 probe 再 read」这个顺序。

**`discover_shards` 的三处「不递归、不读业务文件、不跟随链接循环」体现在代码里。**

```python
children = _child_directories(root)
for entry in children:
    # Only the one snapshot file inside is probed, so a link loop cannot recurse.
    path = os.path.join(entry.path, CONFIG_JSON_FILE_NAME)
```

注释点明了「只探测那一个文件，所以一个链接环无法递归」。而 `_child_directories` 用 `entry.is_dir(follow_symlinks=True)` —— 它**跟随**符号链接（与运行中的存储一致）：

> 每一个 `root` 的直接子目录，**包括指向目录的符号链接（被跟随，与运行中的存储跟随它一致）**。

所以「发现」与「使用」用同一套规则，两者不会分歧——这是 `check_snapshot_location` 那句「启动对同一个检查应用」的原则。

而权限不足的子目录被跳过并警告：

```python
except ConfigurationIdentityError as e:
    if not isinstance(e.__cause__, PermissionError): raise
    logger.warning(f"Skipping {entry.path} ... A workspace kept there is not listed; restore access before starting it.")
```

判据是「**这个用户无法搜索该目录**（挂载卷上的 `lost+found`、另一个服务的目录），所以它既不能在那里写过快照也不能服务一个」。所以跳过是安全的：一个读不了的目录不可能有「这个部署写的」快照。

而警告里给了解法：「在那里保留的工作区不会被列出；在启动它之前恢复访问」。所以这不是静默忽略——它明确说明「有一类工作区现在看不见」。

**`_discovered_name` 处理 HFS+ 的另一种表现，而它的早退很关键。**

```python
name = entry.name
if unicodedata.normalize("NFC", name) == unicodedata.normalize("NFD", name):
    return name  # Nothing normalization could change, e.g. pure ASCII.
```

这一行判断「这个名字在两种规范化下相同」——那就没什么可归一的，直接返回。这是一个纯 ASCII 名字的快速路径（绝大多数情况）。

否则才去读快照的 owner 并做三重判断。而它返回的是 **owner 的拼写**而不是列出的拼写：

> owner 的拼写随后被保留如存储的，**所以发现报告的是锚点成员携带的那个名字**。

所以「磁盘上怎么列的」与「快照说它属于谁」冲突时，以快照为准。这让发现结果与锚点成员名单一致——否则两者会永远不匹配，导致启动时反复报「成员未注册」。

**`check_snapshot_location` 是「启动前的自检」，而它的判据是「发现会不会也把我的快照当成别人的」。**

```python
probe_snapshot(json_config_path(working_dir, workspace))
if _physical_id(json_config_dir(working_dir, workspace)) is None: return
for group in physical_directory_aliases(working_dir, [workspace]):
    if workspace in group:
        raise _one_directory_error(json_config_path(working_dir, workspace), group)
```

docstring：「它的目录必须不与 `WORKING_DIR` 或另一个直接子目录是同一个物理目录——**否则 `discover_shards` 也会按那个名字找到这个快照并拒绝它**」。

所以这个检查的目的不是「阻止用户」，而是**让启动期就暴露一个会在下次扫描时爆炸的布局**。若不做，用户可能成功启动、写入数据，然后某次重启时 `discover_shards` 报错——而那时数据已经在一个歧义的位置上了。

而 `_one_directory_error` 的消息给出了具体修法：「**用单独一个目录替换那个符号链接**」。

**`read_shard_file` 的严格度是「读不出来就抛，只有不存在才返回 `None`」，而两处细节值得记。**

```python
with open(path, encoding="utf-8-sig") as f: content = f.read()
...
if not content.strip(): return {}
```

**`utf-8-sig`** ——容忍 BOM。这与 g01 里 `load_json` 用的是同一个编码，因为两者读同一类文件。

**空文件返回 `{}` 而不是 `None`** ——docstring：「一个存在的空文件读作没有行，正如存储本身加载它的方式」。所以「文件在但是空的」与「文件不在」被区分开：前者是「一个空配置」，后者是「从未创建」。

而两处抛：

```python
if not isinstance(payload, dict):
    raise _shard_error(path, f"expected a JSON object, got {type(payload).__name__}")
for key, row in payload.items():
    if not isinstance(row, dict):
        raise _shard_error(path, f"record {key!r} is not a mapping")
```

第二处是逐行检查——「不是映射的映射」会抛。所以一个值被写成字符串的行不会被静默接受。

而 `_shard_error` 的措辞把「不可跳过」写进去了：「**它从不被跳过或被当作缺失；修复或恢复它**」。这是一条给运维的指令，而不是一句错误描述。

## 关键取舍

**源码事实：** 每个工作区的快照路径固定为 `WORKING_DIR/<workspace>/kv_workspace_config.json`，空工作区用 `WORKING_DIR` 本身；docstring 要求「用 `json_config_dir` 解析，**绝不用第二条规则**」。

**源码事实：** 五个保留名（配置快照、锚点、配置认领、锚点锁、锚点绑定锁）**在任何平台上**都被拒绝，包括它们的 Unicode 大小写/规范化别名、Windows 尾部点空格剥离后、只剩点与空格的名、以及盘符限定名。

**源码事实：** `validate_config_workspace` 用 `ntpath.splitdrive` 而不是 `os.path.splitdrive`，所以在 POSIX 上也能识别 `C:` 这种名字。

**源码事实：** `caseless_alias` 的顺序是「剥尾点空格 → NFD → `casefold` → NFD」，用 `casefold` 而非 `lower` 才能折叠 `ß`→`ss`、开尔文 `K`→`k`。

**源码事实：** `_ASCII_FOLDING_NON_ASCII` 是一张 13 项的**完备枚举**——「每一个折叠后是纯 ASCII 的非 ASCII 字符」，即一个名字能用来别名 ASCII 保留名的全部手段；设置向导无法 import 本模块所以复制了这条规则，**用一个测试把两处钉住**。

**源码事实：** `validate_deployment_workspace` 只在「有存储把数据放在 `WORKING_DIR` 下」时才施加目录规则——纯数据库部署「不创建这样的目录，并保留它此前能用的每个名字」。

**源码事实：** `probe_snapshot` 用 `os.lstat` 不跟随链接；符号链接被拒是因为「第一次原子保存会把它替换成常规文件（留下它的目标陈旧）」，修法是「换成它指向文件的副本」。

**源码事实：** `_physical_id` 返回 `(st_dev, st_ino)` 作物理标识，用 `os.stat` 跟随链接——与 `probe_snapshot` 的 `lstat` 相反，因为两者要看的东西不同。

**源码事实：** `physical_directory_aliases` 处理 HFS+ 与 APFS 两种**相反**的行为，「同一拼写 vs 不同拼写」的区分靠**快照的 owner 行**：owner 是请求的名字（或没有 owner）→ 是同一个工作区的另一种拼写；owner 是列出的拼写 → 是另一个工作区，构成别名。

**源码事实：** `_discovered_name` 在「两种规范化下相同」时早退（纯 ASCII 快速路径）；否则以快照 owner 的拼写为准，让发现结果与锚点成员名单一致。

**源码事实：** `discover_shards` 只探测每个子目录里的那一个快照文件（所以链接环无法递归），跟随指向目录的符号链接（与运行中的存储一致），权限不足的子目录**跳过并警告**（含「恢复访问后再启动它」的指令）。

**源码事实：** `check_snapshot_location` 的目的是「让启动期暴露一个会在下次扫描时爆炸的布局」——判据是「`discover_shards` 会不会也按那个名字找到这个快照并拒绝它」。

**源码事实：** `read_shard_file` 用 `utf-8-sig`（容忍 BOM），空文件返回 `{}` 而**只有不存在才返回 `None`**；非对象载荷与「不是映射的行」都抛，错误消息写明「它从不被跳过或被当作缺失；修复或恢复它」。

**架构推论：** 「固定路径 + 单一推导函数」这条纪律的价值不在代码量而在**一致性**。任何有两条路径推导的地方，两条会在某个边界分叉，而分叉的症状是「找不到」——而「找不到」在一个「没有配置也能启动」的系统里是不可见的，它会静默地用默认值跑起来。所以正确的做法是把推导收成一个函数并明确禁止第二条。

**架构推论：** 这份文件里大半篇幅在处理 **Unicode 与文件系统的大小写/规范化不敏感**，而这看起来像是过度谨慎，实际上是必要的：一个「两个名字指向同一目录」的布局会让两个工作区互相覆盖对方的配置——而配置里存的是嵌入基线，覆盖的后果是**所有检索结果都错**且没有报错。所以这里必须穷举别名化的手段（`casefold` + 那张 13 项表 + 尾部点空格 + 盘符），因为漏掉一种就是一个静默的数据损坏路径。

**架构推论：** 「发现」与「使用」必须用同一套规则，这条在代码里出现了两次（跟随符号链接的选择、`check_snapshot_location` 与 `discover_shards` 共享 `physical_directory_aliases`）。理由是：若两者分歧，就会出现「能写入但重启后发现不了」或「发现得到但打不开」——而这两种都在重启之后才暴露，那时数据已经写进去了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/config_shards.py` | `validate_config_workspace` | 五保留名 + 盘符 + 点空格名，任何平台都适用 |
| `lightrag/config_shards.py` | `validate_deployment_workspace` | 只在有存储用 `WORKING_DIR` 时才施加目录规则 |
| `lightrag/config_shards.py` | `caseless_alias` | 剥尾 → NFD → casefold → NFD 的别名形式 |
| `lightrag/config_shards.py` | `_ASCII_FOLDING_NON_ASCII` | 折叠成 ASCII 的非 ASCII 字符完备枚举 |
| `lightrag/config_shards.py` | `RESERVED_WORKSPACE_NAMES` | 五个部署级文件名 |
| `lightrag/config_shards.py` | `json_config_dir` | 唯一的快照路径推导入口 |
| `lightrag/config_shards.py` | `probe_snapshot` | 三档：不存在 / 可用 / 拒绝（链接、非常规文件） |
| `lightrag/config_shards.py` | `_physical_id` | `(st_dev, st_ino)` 物理目录标识，跟随链接 |
| `lightrag/config_shards.py` | `DiscoveredShard` | 发现结果的冻结载体 |
| `lightrag/config_shards.py` | `discover_shards` | 有界扫描，一目录一快照，重复即拒 |
| `lightrag/config_shards.py` | `physical_directory_aliases` | 按物理身份分组并排除「同名异拼写」的假别名 |
| `lightrag/config_shards.py` | `_differ_only_in_normalization` | 「拼写不同但 NFC 相同」的判据 |
| `lightrag/config_shards.py` | `_discovered_name` | 以快照 owner 的拼写为准，对齐锚点成员 |
| `lightrag/config_shards.py` | `_snapshot_owner` | 从不抛，先 probe 所以 FIFO 不会被读 |
| `lightrag/config_shards.py` | `check_snapshot_location` | 启动前自检，暴露会在扫描时爆炸的布局 |
| `lightrag/config_shards.py` | `read_shard_file` | 严格读：只有不存在返 `None`，空文件返 `{}` |
| `lightrag/config_shards.py` | `_child_directories` | 跟随符号链接，与运行中的存储一致 |

## 参考

- [lightrag 源码：config_shards.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/config_shards.py)
- [Unicode 规范附件 #15：规范化形式](https://www.unicode.org/reports/tr15/)
- [Python 文档：unicodedata](https://docs.python.org/3/library/unicodedata.html)
- [lightrag 源码仓库](https://github.com/HKUDS/LightRAG)

## 与主核 / AI 的关系

主核里最接近的是**表空间与数据文件路径的绑定**：一个表空间名映射到一个确定的文件路径，而「两个表空间指向同一个文件」是必须拒绝的——否则两个表的写入会互相覆盖。

这里的问题在结构上完全一样，但多了**文件系统语义的不确定性**：主核面对的是自己控制的目录布局（名字由引擎生成，无 Unicode 歧义），而这里的名字来自用户（HTTP 头、环境变量），并且要跑在大小写不敏感（NTFS / APFS）与规范化不敏感（HFS+）的文件系统上。所以这一层必须自己实现「两个名字是否可能指向同一目录」的判定。

与 i01（配置存储）的关系：那一页定义键与作用域，这一页定义**物理布局**——即「一个 JSON 后端部署的若干快照文件怎么组织成一个组」。

与 i03（存储锚点）的关系：锚点文件是那五个保留名之一，而锚点里的成员列表正是本页 `discover_shards` 的输出所要匹配的对象。两页共享 `physical_directory_aliases` 这条规则。

对 AI 工程最实用的一条是**「路径推导只允许一个入口」**。这件事在单机小项目里无所谓，但只要有两个地方各自拼路径，它们一定在某天分叉——分叉点是空字符串、尾部分隔符、Windows 反斜杠、相对路径之 CWD 变化，或者 symlink。而分叉后的症状是「找不到自己的配置」，如果系统在找不到时用默认值启动，那就是一次静默的数据错乱。

另一条是**「用户可控名字必须做 Unicode 归一化检查」**。这一条在跨平台部署时尤其重要：Linux 上创建的工作区叫 `Café`（NFC），搬到 macOS（HFS+ 会把名字存成 NFD）后，按 NFC 去查找就找不到。这里的做法是「用 NFC 比较判定等价，但以快照里记录的拼写为准」——**比较用归一化形式，存取用原始字节**。这个分工值得照抄。

最后一条是 `_snapshot_owner` 的「先 probe 再 read」。`open()` 一个 FIFO 会永久阻塞，而「一个意外的 FIFO 出现在配置目录里」是完全可能的（某个工具的错误输出重定向到了那里）。所以任何「遍历用户目录并读文件」的代码都该先确认「这是常规文件」再读，否则一次挂死就足够让服务起不来。
