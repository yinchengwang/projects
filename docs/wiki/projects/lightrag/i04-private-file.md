---
project: lightrag
feature: private-file
title: 私有文件写入与原子替换
page_type: feature
schema_version: 1
refs:
  - path: lightrag/private_file.py
    symbols: [open_private_file, _windows_verify_dacl, PrivateFileError]
  - path: lightrag/private_file.py
    symbols: [_windows_current_user_sid, windows_create_exclusive]
---

# 私有文件写入与原子替换

## 一句话职责

创建一个**从存在的那一刻起就只有本进程身份能读**的文件，供那些「把库里的文档复制到一个新路径、且要长期保留」的场景使用（当前的调用方是离线重建工具对损坏向量容器的备份）——而「私有」在两个平台上要两种完全不同的机制，所以「先建再收紧」不是一个可移植的形状，在 Windows 上甚至不是一个正确的形状。

## 机制是什么

模块 docstring 的第一段把「为什么不能先建再收紧」讲透了：

> "私有"意味着：除了这个进程**实际运行身份**以外的任何用户都不能读这个文件，而这一点**从文件存在的那一刻**成立，而不是从某个后续调用收紧它那一刻成立。这个区别在 Windows 上不是学究式的，那里访问是在**打开一个句柄时**检查的——一个在后续收紧之前就打开了文件的进程，此后通过那个句柄继续读，**包括之后写进去的一切**。

所以问题的实质是「权限检查的时机」：POSIX 每次 `read` 都检查，Windows 在 `CreateFile` 时检查一次并把结果绑到句柄上。因此 Windows 上的「先建再 chmod」有一个**不可关闭的窗口**：另一个进程可以在这个窗口里拿到句柄，然后永久有效。

两个平台的手段：

| 平台 | 机制 | 为什么它够 |
|---|---|---|
| POSIX | `os.open(..., O_CREAT \| O_EXCL, 0o600)` | 创建时就带 0600，umask 只能变窄 |
| Windows | `CreateFileW` + 受保护 DACL + `dwShareMode=0` | DACL 在创建时进去，共享模式禁掉第二个句柄 |

而 Windows 那一行还有第二个机制的必要性：`dwShareMode=0` 与 DACL **覆盖不同的窗口，互不替代**。docstring：

> 两个设置覆盖不同的窗口，互不替代：**DACL 管这次之后每一次打开**，包括句柄关闭之后；而 `dwShareMode=0` 阻止在复制进行中打开第二个句柄。

所以 DACL 管「长期」，共享模式管「当下」。若只有 DACL，另一个进程仍能在写入期间打开文件读到半份内容（DACL 允许本用户读）；若只有共享模式，句柄关闭后就没保护了。

## 怎么实现的

**`open_private_file` 是 `@contextmanager`，而它的异常分类被明确声明为「故意不合并成一种」。**

docstring 列了三种：

1. **建不成私有、或证不出私有** → `PrivateFileError`。「什么都没写，磁盘上已有的东西也没被碰过——**特别是名字冲突抛 `FileExistsError` 并留下已存在的文件**」。
2. **调用方自己 body 里的异常**（源读不出来、磁盘满）→ **原样传播**。「这不重新分类它；它只是清理」。
3. 两种情况下都会尝试删掉自己建的文件，「所以没有半份副本看起来像完整副本」。而这个删除是**尽力而为**：失败就记日志，原始异常仍然胜出，「这可能留下一个零长度或部分文件，在一个 `.<corrupt>-<token>` 名下，后续运行不会复用」。

第 1 条里那个「名字冲突」的强调是有来由的——后面 `_invalid_handle_value` 的注释会说，早期版本因为句柄比较写错，把「名字冲突」当成了「创建成功」，然后清理时**删掉了已经存在的那个文件**。所以「冲突时不动既有文件」这条纪律是被 bug 教出来的。

**POSIX 路径的校验核心是「创建时的 mode 就是证据，所以它必须第一个读」。**

```python
mode = stat.S_IMODE(os.fstat(destination.fileno()).st_mode)
# The mode the file was CREATED with is the evidence, and it is read first: a umask only ever
# subtracts, so a group or other bit here means the filesystem ignored the requested 0600 outright,
# and someone may already hold a descriptor on it. Restoring anything before this is asked would
# overwrite the proof and accept the file.
if mode & (stat.S_IRWXG | stat.S_IRWXO):
    raise PrivateFileError(f"{path} was created mode {oct(mode)}, which grants group or other access. The filesystem did not honour the requested 0600.")
```

注释里的推理链值得逐句读：

1. **umask 只能减** ——所以如果创建后的 mode 里有 group 或 other 位，那不是 umask 干的，而是**文件系统根本没理会请求的 0600**。
2. **而那时可能已经有人持有句柄了** ——所以必须拒绝。
3. **所以「先读再修」不是顺序偏好而是证据保全** ——「在问这个问题之前恢复任何东西都会覆盖证据并接受这个文件」。

第 2 点是最关键的：如果一个文件系统（例如某些网络挂载、某些 FAT 实现）不理会 POSIX 权限位，那么文件从创建那一瞬起就是宽的，而**这个窗口已经发生过了**——无法补救，只能拒绝。

**而 owner 自己的读位被 umask 减掉时，代码选择「恢复」而不是拒绝——理由写得很实在。**

```python
if not mode & stat.S_IRUSR:
    # What the umask CAN do is subtract the owner's own read bit, which leaves a file this process
    # still finishes writing through the open descriptor and nobody can read afterwards -- including
    # the operator whose only copy of the data this is once recovery drops the originals. Restoring it
    # on the descriptor can only ADD back what the umask took; it is not the "create then tighten"
    # shape this module refuses, because the file has never been wider than 0600 and both checks are
    # re-asked below.
    os.fchmod(destination.fileno(), 0o600)
    mode = stat.S_IMODE(os.fstat(destination.fileno()).st_mode)
```

这段区分了两种「mode 不对」：

- **有 group/other 位** → 文件曾经宽过 → **不可补救 → 拒绝**。
- **缺 owner 读位** → 文件从未宽过（只是窄了）→ **可以补 → 恢复**。

而后面的 `if mode & (...) or not mode & S_IRUSR:` 是**重新问一遍**——所以 `fchmod` 若没生效（某些文件系统会忽略它），仍然会被拒。这与前面「恢复前先读证据」一起构成了一个「试图修复但保留证明」的顺序。

而「owner 读位缺失」的后果被具体化了：「一个本进程仍能通过打开的描述符写完、而之后没有人能读的文件——**包括那个运维，一旦恢复流程丢掉了原件，这是它们唯一的副本**」。

这是这一页最有分量的动机说明：这个模块服务的是「灾难恢复」路径，所以「文件存在但读不出」等于「数据彻底丢失」。

**`os.O_EXCL` 与 `FileExistsError` 一起构成了「绝不截断别人的文件」这条保证。**

```python
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
```

`O_EXCL` 配合 `O_CREAT` 是 POSIX 上唯一的「原子地创建并失败于已存在」。没有它的话，`O_CREAT` 会打开已有文件并从偏移 0 开始写——**静默覆盖别人的数据**。

而 docstring 明说：「`path` 必须不存在；创建是独占的，一个已存在的文件抛 `FileExistsError` 而不是被复用或截断」。

**`os.fdopen` 失败时那段清理注释很短但很精确。**

```python
try:
    destination = os.fdopen(fd, "wb")
except BaseException:
    # fdopen did not take ownership, so the descriptor is still ours, and it still says which file this name meant.
    try: identity = os.fstat(fd)
    except OSError: identity = None
    os.close(fd)
    _remove_quietly(path, identity)
    raise
```

两点：

1. **`fdopen` 失败意味着它没有接管描述符** ——所以描述符还是我们的，必须自己关。若不关就是泄漏；若关了两次则可能关掉别人新分配的同号描述符。
2. **在关之前先 `fstat` 拿身份** ——因为「删除是按名字的」，而名字只有在没人重排目录时才是我们的。所以先记下 `(st_dev, st_ino)`，删除时用它确认。

第 2 点是 `_remove_quietly` 里那段检查的来源。

**`_remove_quietly` 的身份检查是「宁可留下垃圾也不删别人的文件」。**

```python
if identity is not None:
    try: on_disk = os.lstat(path)
    except OSError as stat_error:
        logger.warning(...); return
    if (on_disk.st_dev, on_disk.st_ino) != (identity.st_dev, identity.st_ino):
        logger.warning(f"Not removing {path}: it is no longer the file this process created, so deleting it would destroy somebody else's")
        return
```

docstring 把这条原则说得极清楚：

> 删除是按**名字**的，而名字只有在没人重排目录时才属于我们，所以当身份已知时先把名字与它核对，**而一个陌生者被留下不动**——与 `windows_create_exclusive` 对碰撞情形声明的是同一条规则：**删掉别人的文件是值得避免的失败，而留下自己的半成品只是不整洁**。

最后那句是整段的判断核心，也是这一页最重要的取舍表述：「不整洁」与「数据损坏」之间，永远选前者。

而它同时承认了这道检查的局限：「这个检查**缩小**窗口而不是关闭它：这里没有什么是原子的，残留是『一个别的进程能写的目录仍然是清理会输掉竞争的那个目录』」。

**`_discard` 先 `fstat` 再 `close` 再删——顺序不能反。**

```python
try: identity = os.fstat(destination.fileno())
except (OSError, ValueError): pass
try: destination.close()
except OSError as close_error: logger.warning(...)
_remove_quietly(path, identity)
```

必须在 `close` 之前 `fstat`，因为描述符一关就 `fstat` 不了。而 `(OSError, ValueError)` 两个都捕：`OSError` 是系统层面的失败，`ValueError` 是「已关闭的文件对象」。

**Windows 路径的三个决定被 docstring 归纳成一句「顺序就是要点」。**

```python
"""Ordering is the point. The security descriptor goes IN to the creation call, so the file is never
briefly inheritable; the read-back happens on the handle that call returned, so it describes the
object created and not whatever a later path lookup would find; and the handle becomes Python's only
at the very end, so every failure before that has exactly one owner to clean up."""
```

三句：

1. **安全描述符在创建调用里传进去** → 「文件从不短暂可继承」。这就是模块开头那个「窗口」的关闭方式。
2. **读回在创建返回的句柄上做** → 描述的是**被创建的那个对象**，而不是「之后某次路径查找找到的东西」。这个区别在「另一个进程在两次操作之间把该名字换成符号链接」时决定正确性。
3. **句柄到最后才变成 Python 的** → 此前每个失败都只有一个所有者要清理。

第 3 点是资源所有权的纪律：`msvcrt.open_osfhandle(handle, ...)` 之后，fd **和** handle 都指向同一个内核对象，**两个都关会关掉 OS 在这期间重新分配的号**。所以代码里那句注释：

```python
# Past this point the fd owns the handle: closing both would close whatever the OS reassigned the number to in between.
fd = msvcrt.open_osfhandle(handle, os.O_WRONLY | os.O_BINARY)
```

**安全描述符用 SDDL 字符串而不是手工拼 ACL，而理由是可审阅性。**

```python
descriptor = _windows_security_descriptor(kernel32, advapi32, ctypes, f"O:{sid}D:P(A;;FA;;;{sid})")
```

`_windows_security_descriptor` 的 docstring：

> 用 SDDL 而不是手工拼装 ACL，因为整个描述符随后是**一个可审阅的字符串**：`D:P` 保护 DACL，那正是阻止父目录条目被继承的东西，而单个 `A` 条目把 `FA`（完全访问）授予**一个** SID 而不给其他任何人。给完全访问而不是读：运维必须能读、归档、最终删除这些文件。

三件事：

1. **`D:P` 是「保护」标志** ——没有它，DACL 里的条目只是「父目录今天规定的那些」，而它们**明天可能变**。这一点在 `assert_dacl_grants_only` 的第一条检查里被重复。
2. **`A;;FA;;;{sid}`** ——一个允许条目、完全访问、给这一个 SID。
3. **为什么是 `FA` 而不是 `GR`（generic read）** ——因为运维需要**归档与删除**。若只给读，那「读回来」这个动作永远做不了，而「一个没人能读的备份不是备份」这条逻辑与 POSIX 侧「owner 读位必须恢复」完全一致。

**`_windows_current_user_sid` 从访问令牌读身份，而不是从环境变量。**

docstring：

> 从访问令牌读，**绝不从 `USERNAME` / `USERDOMAIN` 读**：那些是继承的环境字符串，调用方可以把它们设成任何东西，而授权必须命名那个真正会拥有句柄的身份。**先试线程令牌**，好让一个运行在模拟身份下的线程授权给那个身份，而不是进程的。

三条：

1. **不用环境变量** ——因为它们可被伪造，而伪造的后果是「授权给一个不存在的身份」或「授权给另一个真实身份」。
2. **线程令牌优先** ——Windows 支持线程级模拟（impersonation），所以「这个线程实际以谁的身份运行」可能与进程不同。
3. **回退只允许一种情形**：

```python
if code != _ERROR_NO_TOKEN:
    raise PrivateFileError("Could not read this thread's impersonation token, so the identity that would own the file cannot be established: ...")
```

docstring：「那个回退只因为**一个**理由被允许——`ERROR_NO_TOKEN`，它意味着线程根本没有在模拟，于是进程令牌是正确答案。任何其他失败（例如拒绝访问）都表示**存在一个读不出的令牌**，而在那里回退会**把文件授权给一个与将要拥有它的身份不同的身份，静默地**」。

这是「错误分类决定正确性」的范例：`ERROR_NO_TOKEN` 是「没有模拟」这个**事实的答案**，而其他错误是「问不到」。把后者也当「没有模拟」会静默产出权限错的文件。

**`windows_create_exclusive` 被单独拆出来、并且「给普通参数」，理由是可测试性。**

docstring：

> 拆出来并给普通参数，**好让失败分类能在任何平台上被演练**：它是「错了会毁掉文件而不只是拒绝一个」的那部分。

而它对调用方的契约极其明确：

> 一个调用方**只能在这个函数返回时**才删 `path`。这里的每次 raise 都把文件系统留在它找到的样子——这对碰撞情形最重要，**那里存在的文件是别人的，删掉它是这个签名存在要防止的 bug**。

所以这个函数的签名设计服务于「谁能删」这个问题：返回句柄 = 文件是我建的 = 我可以删；抛异常 = 不是我建的 = 我不许删。

**`_invalid_handle_value()` 那段注释记录了一次真实的数据损坏。**

```python
def _invalid_handle_value() -> int:
    """``INVALID_HANDLE_VALUE`` as ctypes hands it back, not as C writes it.
    The SDK defines it ``(HANDLE)-1``, but a function whose ``restype`` is a pointer returns a Python int
    holding the unsigned bit pattern, so on 64-bit it arrives as 18446744073709551615 and
    ``handle == -1`` is FALSE. Comparing against the literal therefore let every failed creation through
    as a success -- including a name collision, whose cleanup then deleted the file that was already there.
    """
    import ctypes
    return ctypes.c_void_p(-1).value
```

这条值得完整引用，因为它是一条完整的因果链：

1. C 里 `INVALID_HANDLE_VALUE` 是 `(HANDLE)-1`。
2. 但 `restype` 是指针时，`ctypes` 返回的是**无符号位模式**的 Python int，64 位上就是 `18446744073709551615`。
3. 所以 `handle == -1` 是 `False`。
4. 于是**每一次创建失败都被当成成功**。
5. 而其中「名字冲突」那次的清理**删掉了已经存在的文件**。

第 5 步就是前面多处强调「碰撞时不许删」的来源。这是一个「类型层面的小错导致数据销毁」的真实案例，而它的修法是用 `ctypes.c_void_p(-1).value` 让 ctypes 自己算那个值，而不是手写 `-1`。

**`windows_create_exclusive` 把「三种无句柄形态」都算作失败。**

```python
if handle is None or handle in (0, _invalid_handle_value()):
    code = ctypes.get_last_error()
    if code in (_ERROR_FILE_EXISTS, _ERROR_ALREADY_EXISTS):
        raise FileExistsError(errno.EEXIST, os.strerror(errno.EEXIST), path)
    raise PrivateFileError(f"Could not create {path} with a private security descriptor: {ctypes.WinError(code)}")
```

三种：`None`（`restype` 是 `c_void_p` 时的 NULL）、`0`（NULL）、以及 `INVALID_HANDLE_VALUE` 的无符号形式。注释：「`None`（一个 NULL 指针 restype）、`0`、以及 `(HANDLE)-1` 的无符号形式都是『没有句柄』」。

而错误分类只有两种：**名字已存在**（映射到 `FileExistsError`，与 POSIX 路径一致）与**其他失败**（`PrivateFileError`）。这个双分类保证了「调用方用一个 `except FileExistsError` 就能同时处理两个平台」。

**`_declare_windows_signatures` 的注释解释了为什么每个调用都必须声明签名。**

> `ctypes` 默认一个未命名的参数为 C 的 `int`，那会把一个 64 位 `HANDLE` 或指针截到 32 位，**并把一个能工作的调用变成一个失败或静默损坏的调用。下面没有一处是可选的。**

「静默损坏」这半句是关键：截断 32 位不一定立刻报错，它可能变成一个看似有效但指向别处的句柄。所以 `argtypes` 不是「最佳实践」而是正确性要求。

**`_windows_verify_dacl` 的核心判断是「读回用 SDDL 往返，而不是遍历 ACE 数组」。**

```python
status = advapi32.GetSecurityInfo(handle, _SE_FILE_OBJECT, _DACL_SECURITY_INFORMATION, None, None, None, None, ctypes.byref(descriptor))
if status != 0:
    raise PrivateFileError(f"Could not read back the security descriptor of {path}, so its privacy cannot be confirmed (a filesystem that does not persist ACLs answers this way): {ctypes.WinError(status)}")
...
assert_dacl_grants_only(actual, sid, path, sid_matches=lambda field: _windows_sids_equal(kernel32, advapi32, ctypes, field, sid))
```

两点：

1. **`GetSecurityInfo` 用 `handle` 而不是路径** ——docstring：「从**句柄**读回，所以它描述的是**被创建的那个对象**」。这与前面「顺序就是要点」的第二句一致。
2. **`status != 0` 的错误消息里点名了 FAT32** ——「一个不持久化 ACL 的文件系统是这样回答的」。所以「不支持」不会被当成「通过」，而是「证不出私有 → 拒绝」。这正是 docstring 那句：「要么调用失败，要么回来的 DACL 不是请求的那个，**两者都拒绝**」。

**`assert_dacl_grants_only` 是「什么是私有」的可执行定义，五条条件各有一个理由，而 docstring 把它们写成了递进的层次。**

第一层（谁被挡在外面）：

1. **`D:P`** ——「没有这个保护标志，下面的条目就只是父目录今天规定的东西，而它**明天可能在文件底下改变**」。
2. **恰好一个条目** ——「第二个条目就是第二个受众，无论它授予什么」。
3. **那个条目必须是对这个 SID 的允许** ——「一个给别人的条目就是这个守卫要防的全部失败」。

第二层（那个条目真的生效）：

4. **它必须适用于这个文件** ——「一个继承专用（inherit-only）条目通过了上面每一条条件，同时把它的权限授予一个文件不可能拥有的**子对象**」。

第三层（它还真的授予了必要的东西）：

5. **它的掩码必须仍然赋予 owner 读与删除** ——「前三条都是关于谁被挡在外面，而一个被文件系统降级过的掩码满足它们每一条，同时留下一个它的 owner 读不回或删不掉的文件——**这些副本是无限期保留的，而一旦它们存在那个工具就丢掉原件，所以不可用本身就是一种损失**」。

这个三层结构很清晰：**先证明没有别人能进，再证明这个条目不是空转，最后证明自己还能用**。而第三层最容易被忽略，因为「私有」这个词让人只想到「挡住别人」——而这一层的失败方向是「挡住了所有人」，那对一个备份文件就等于数据丢失。

**按值比较而不是按文本比较，理由是一个只在本机测试时通过的 bug。**

docstring 那段值得完整引用：

> 一个 SDDL SID 字段不是稳定的拼写：这个进程写下去的字符串，回来时**只要那个账户有别名就会被缩写成两字母别名**，所以一个由内置 Administrator（RID 500）创建的存储写成 `S-1-5-21-...-500`，读回来是 `LA`。比较文本会**拒绝**每一个这样的文件——同时对普通用户通过，因为他们的 SID 没有别名，**而那正是那样一种 bug 的形状：通过本机测试，在别人机器上失败**。

这段话点出了一个具体的「测试环境偏差」：内置 Administrator 账户有别名 `LA`（Language/Administrator），普通用户没有。所以开发机上测试通过，部署到以 Administrator 运行的机器上全部失败。而修法是按值比较：

```python
def to_sid(text: str):
    sid = ctypes.c_void_p()
    if not advapi32.ConvertStringSidToSidW(text, ctypes.byref(sid)):
        raise PrivateFileError(...)
    return sid
...
return bool(advapi32.EqualSid(first, second))
```

`ConvertStringSidToSidW` 接受两种形式（完整 `S-1-...` 串与别名如 `LA` / `BA`），而 `EqualSid` 按**值**比较。

**掩码同理：「一个掩码有几种拼写，所以它被解析而不是当文本比较」。**

```python
def _sddl_access_mask(field: str) -> int | None:
    text = field.strip()
    if not text: return None
    if text[:2].lower() == "0x":
        try: return int(text, 16)
        except ValueError: return None
    if text.isdigit(): return int(text, 10)
    if len(text) % 2: return None
    mask = 0
    for index in range(0, len(text), 2):
        value = _SDDL_RIGHTS.get(text[index : index + 2].upper())
        if value is None: return None
        mask |= value
    return mask
```

四种形态：`0x` 前缀的十六进制、纯数字、两字母记号的串联、以及无法解析（返回 `None`）。

而 `None` 的语义被明确定义：「`None` 是『这无法解释』，**调用方必须把它当作拒绝**：一个未知拼写不是权限存在的证据」。

这与 `_ace_applies_to_this_object` 里「一个未知拼写不是 ACE 生效的证据」是同一条规则——**面对不认识的东西，答案必须是拒绝**。这与 g04 里「不确定就是不确定」方向相反：那边不确定要退回宽松（因为拒绝会停机），这边不确定要拒绝（因为宽松会泄露数据）。方向由「哪个错误更严重」决定。

**`_map_generic_file_rights` 处理「通用权限位」。**

```python
for generic, specific in ((_GENERIC_ALL, _FILE_ALL_ACCESS), (_GENERIC_READ, _FILE_GENERIC_READ), ...):
    if mask & generic:
        mask |= specific
return mask
```

docstring：「一个读回的文件 DACL 通常携带具体权限，因为通用权限在 ACE 被应用时就被映射了。一个**仍然回答 `GA` 的文件系统是在给完全访问，不是更少**，所以在检查权限之前先应用映射，而不是拒绝这个拼写」。

所以这个函数的方向是**放宽而不是收紧**：`GA`（generic all）意味着「全部」，所以映射成 `FILE_ALL_ACCESS` 后必然满足 `_REQUIRED_RIGHTS`。这与上一条「认不出就拒绝」不矛盾——`GA` 是**认得出的**，只是用了通用形式。

**`_file_generic_read | _DELETE` 是「owner 必须有什么」的最小集。**

```python
_REQUIRED_RIGHTS = _FILE_GENERIC_READ | _DELETE
```

与 POSIX 侧「owner 读位必须存在」对应，但多了 `_DELETE`。理由是同一个：这些备份是**无限期保留**的，而工具在它们存在后会丢掉原件——所以 owner 必须能读回（归档）也能删掉（清理）。若缺 `DELETE`，文件会变成永久占位。

## 关键取舍

**源码事实：** 「私有」的定义是「从文件存在那一刻起，除本进程实际运行身份外无人可读」，而不是「某个后续调用收紧之后」。在 Windows 上这个区别不是学究式的，因为「访问在打开句柄时检查，之后通过该句柄继续读包括此后写入的一切」。

**源码事实：** POSIX 用 `os.open(..., O_CREAT | O_EXCL, 0o600)`；Windows 用 `CreateFileW` + 受保护 DACL（`D:P`）+ `dwShareMode=0`，两个设置「覆盖不同的窗口，互不替代」（DACL 管长期，共享模式管复制进行中）。

**源码事实：** 三种失败故意不合并：建不成/证不出私有 → `PrivateFileError`；名字冲突 → `FileExistsError` 且**不动既有文件**；调用方 body 的异常 → 原样传播。所有情形都尽力删除自己建的文件，失败只记日志且原始异常胜出。

**源码事实：** POSIX 侧「创建时的 mode 就是证据，必须第一个读」——umask 只能减，所以 group/other 位意味着文件系统没理会 0600，且**可能已有人持有句柄**，因此不可补救只能拒绝。

**源码事实：** owner 自己的读位被 umask 减掉时**恢复而不是拒绝**，因为它「从未宽过 0600」；恢复后**重新检查两遍**。缺 owner 读位的后果是「一个没人能读的文件」，而在恢复流程丢弃原件之后那是数据的唯一副本。

**源码事实：** `_remove_quietly` 在删除前核对 `(st_dev, st_ino)`，「一个陌生者被留下不动」——「删掉别人的文件是值得避免的失败，而留下自己的半成品只是不整洁」；它承认这道检查「缩小窗口而不是关闭它」。

**源码事实：** Windows 的三条顺序纪律：描述符在创建调用里传入（文件从不短暂可继承）、读回在创建返回的句柄上做（描述被创建的对象而非某次路径查找的结果）、句柄到最后才交给 Python（此前每个失败只有一个所有者要清理）。

**源码事实：** `open_osfhandle` 之后 fd 与 handle 指向同一内核对象，**两个都关会关掉 OS 在此期间重新分配的号**。

**源码事实：** 安全描述符用 SDDL 字符串（`O:{sid}D:P(A;;FA;;;{sid})`）因为「整个描述符随后是一个可审阅的字符串」；用 `FA` 而非只读，因为运维必须能读、归档、最终删除。

**源码事实：** 身份从**访问令牌**读而绝不从 `USERNAME` / `USERDOMAIN` 读（后者可被伪造）；线程令牌优先以便在模拟身份下授权给被模拟者；回退到进程令牌**只允许 `ERROR_NO_TOKEN`**，其他失败表示「存在一个读不出的令牌」，回退会「把文件授权给一个与所有者不同的身份，静默地」。

**源码事实：** `windows_create_exclusive` 被拆出来以「让失败分类能在任何平台上演练」，且它的契约是「调用方只能在这个函数返回时才删 `path`」——因为碰撞情形下那个文件是别人的。

**源码事实：** `_invalid_handle_value()` 用一个真实的 bug 命名：`restype` 是指针时 `(HANDLE)-1` 回来是 `18446744073709551615`，所以 `handle == -1` 为假，**每次创建失败都被当成功**，而其中碰撞那次的清理**删掉了已存在的文件**。

**源码事实：** `_declare_windows_signatures` 是正确性要求而非最佳实践：「`ctypes` 默认参数为 C `int`，会把 64 位 HANDLE 或指针截到 32 位，把一个能工作的调用变成失败或**静默损坏**的调用」。

**源码事实：** `assert_dacl_grants_only` 五条条件分三层：`D:P` 保护 + 恰好一个条目 + 是对本 SID 的允许（谁被挡）；条目**适用于这个文件**（继承专用条目会通过前三条却把权限给不存在的子对象）；掩码**仍然赋予** owner 读与删除（防「挡住了所有人」）。

**源码事实：** SID 与掩码都**按值解析而非按文本比较**：SDDL 会把有別名的账户缩写成 `LA`，所以文本比较「通过本机测试（普通用户无别名），在别人机器上失败（内置 Administrator）」；掩码有 `FA` / `0x1f01ff` / 两字母串联三种拼写。

**源码事实：** 未知拼写的处理是**拒绝**（`_sddl_access_mask` 返回 `None`，`_ace_applies_to_this_object` 返回 `False`）：「一个未知拼写不是权限存在的证据」。唯一例外是通用权限位，那是**认得出**的形态，映射后必然满足要求。

**源码事实：** `_REQUIRED_RIGHTS = FILE_GENERIC_READ | DELETE` ——读回（归档）与删除（清理）都是必需的，因为「这些副本无限期保留，而工具一旦它们存在就丢掉原件」。

**架构推论：** 这一页的全部复杂度来自一条不可通约的差异：**POSIX 每次访问都检查权限，Windows 只在打开时检查一次**。所以「创建后收紧」在 POSIX 上有意义（窗口内读到的是空文件），在 Windows 上无意义（窗口内打开的句柄永久有效）。任何跨平台的「安全创建」都必须放弃「创建后收紧」这个形状，改为「在创建调用里带上权限」——而后者在两个平台上是两个完全不同的 API。

**架构推论：** 「按值比较而不是按文本比较」这条在两个地方出现（SID 与掩码），而理由都是「同一个答案有多种拼写」。这提示了一条通用判据：**只要一个值可能是「同一个事物的不同表示」，任何相等判断都必须先归一化**。而最危险的情况是「归一化在某些环境下总是恒等」——因为那样本机测试会全绿，而部署环境会全红。SID 别名就是这种情形。

**架构推论：** 这一层对「认不出就拒绝」与「认不出就继续」的分野给了两个相反的例子：未知 SID/掩码拼写 → 拒绝（宽松会泄露）；`ERROR_NO_TOKEN` → 回退（因为它是「没有模拟」这个**事实的答案**而不是「问不到」）。判据是：**这个错误是「答案是 X」还是「问题没得到答案」？** 前者可以按 X 处理，后者必须谨慎——而这个区分只在错误码语义清晰时成立，所以 `msvcrt` 那种「两种情形同一错误」的 API 就只能一律按更保守的一侧处理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/private_file.py` | `open_private_file` | 跨平台「创建即私有」的上下文管理器 |
| `lightrag/private_file.py` | `PrivateFileError` | 建不成或证不出私有，**写入之前**抛 |
| `lightrag/private_file.py` | `windows_create_exclusive` | `CreateFileW` + 唯一正确的返回值判读 |
| `lightrag/private_file.py` | `_invalid_handle_value` | `ctypes` 下 `(HANDLE)-1` 的无符号值，防数据销毁 |
| `lightrag/private_file.py` | `_open_private_file_windows` | 创建 → 读回验证 → 交给 Python 的三段顺序 |
| `lightrag/private_file.py` | `_windows_current_user_sid` | 从访问令牌取身份，线程令牌优先 |
| `lightrag/private_file.py` | `_windows_verify_dacl` | 从句柄读回 DACL 并交结构规则判定 |
| `lightrag/private_file.py` | `_windows_sids_equal` | `ConvertStringSidToSidW` + `EqualSid` 按值比较 |
| `lightrag/private_file.py` | `assert_dacl_grants_only` | 「私有」的五条可执行定义，分三层 |
| `lightrag/private_file.py` | `_ace_applies_to_this_object` | 继承专用条目不算生效，未知拼写不算生效 |
| `lightrag/private_file.py` | `_sddl_access_mask` | 三种掩码拼写 + 无法解析返回 `None` |
| `lightrag/private_file.py` | `_map_generic_file_rights` | 通用位展开成文件具体权限 |
| `lightrag/private_file.py` | `_REQUIRED_RIGHTS` | 读回与删除都是必需（备份要能归档与清理） |
| `lightrag/private_file.py` | `_windows_security_descriptor` | SDDL `D:P` 单 SID 完全访问 |
| `lightrag/private_file.py` | `_declare_windows_signatures` | 每个 Win32 调用必须显式签名否则截断 |
| `lightrag/private_file.py` | `_remove_quietly` | 按 `(dev, ino)` 核对后才删，陌生者留下 |
| `lightrag/private_file.py` | `_discard` | 先 `fstat` 再 `close` 再删 |

## 参考

- [lightrag 源码：private_file.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/private_file.py)
- [CreateFileW 文档](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [SDDL 字符串格式](https://learn.microsoft.com/en-us/windows/win32/secauthz/security-descriptor-definition-language)
- [open(2) 手册页](https://man7.org/linux/man-pages/man2/open.2.html)

## 与主核 / AI 的关系

主核里最接近的是**创建数据文件时的权限继承与保护**：一个新建的表空间文件必须从创建那一刻起就有正确的权限，因为事后 `chmod` 会留下一个窗口，而那个窗口里文件已经存在了。差别在于主核通常只跑在 POSIX 上，所以 `open(..., 0600)` 就够了；这里必须跨平台，于是被迫面对 Windows 的「打开时检查」语义。

**这条差异的普适性值得强调**：任何「先创建、再设置安全属性」的代码在 Windows 上都是错的，而它看起来对——因为在 POSIX 上测不出问题。同类的还有：创建临时文件后 chmod、创建目录后 chown、写配置文件后加密。正确做法都是「在一次系统调用里带上最终属性」。

与 i03（存储锚点）的关系：锚点文件本身**不是**私有的（它只有 backend 与 UUID，没有敏感信息），所以它走普通的原子写。而这一页服务的对象是「向量容器的备份」，那些备份含有文档内容——所以需要私有。这个对比说明了「什么时候需要私有」：**文件内容是否包含源数据**。

与 g01（JSON 键值存储）的关系是基础设施的共享：`file_atomic` 提供「不毁旧数据」的原子替换，这一页提供「新文件从出生就是私有的」。两者解决不同阶段的暴露：前者防「写一半」，后者防「写对了但谁都读得到」。而 `_write_temp` 用的是普通 `open(..., "x")`，所以若锚点将来需要私有，那一处也得改——这是一个跨页的依赖。

对 AI 工程最实用的一条是**「安全属性必须在创建调用里带上」**。这不只适用于文件权限，也适用于任何「创建资源再设访问控制」的场景：创建数据库表再 `GRANT`、创建 S3 对象再改 ACL、创建云实例再配安全组。所有这类操作都有一个「已存在但未保护」的窗口，而窗口里发生的访问是永久的（尤其在 Windows 这类「打开时检查一次」的模型里）。

另一条是**「按值比较，不按文本比较」**。SID 别名那个例子很有教育意义：`S-1-5-21-...-500` 与 `LA` 是同一个账户，而文本比较会说「不等」。而这类 bug 的特征是「本机全绿、部署全红」——因为别名只在特定账户上存在。判据是：**这个字符串是否有多个合法拼写？** 有的话就解析成值再比。

最后一条是 `_remove_quietly` 的「宁可留下垃圾也不删别人的」。清理代码在异常路径上运行，此时目录可能已经被别人动过，而「按名字删除」是一个危险操作。正确做法是先核对身份（`dev`/`ino`），确认是自己的才删——即使这样就可能留下垃圾。因为**留下垃圾是可逆的（手工删），删掉别人的数据是不可逆的**。
