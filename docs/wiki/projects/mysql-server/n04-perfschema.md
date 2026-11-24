---
project: mysql-server
feature: perfschema
page_type: feature
schema_version: 1
refs:
  - path: storage/perfschema/pfs.cc
    symbols: [pfs]
  - path: storage/perfschema/pfs_instr_class.cc
    symbols: [PFS_instr_class]
---

# 性能监控

## 一句话职责

性能监控（Performance Schema）负责**在运行期采集并暴露 MySQL 服务器的内部指标**——等待事件（mutex / rwlock / cond / file / socket / table I/O / table lock / idle）、语句摘要（statement digest）、表与索引统计、内存分配、事务与错误计数；它通过 `performance_schema` 库里的 SQL 表对外暴露这些数据。核心实现落在 `storage/perfschema/pfs.cc` 的 PSI（Performance Schema Instrumentation）接口实现上（`pfs` 前缀的一族函数），而 `storage/perfschema/pfs_instr_class.cc` 的 `PFS_instr_class` 是所有「仪表类别（class）」的公共基类——每一类被监控对象（mutex、rwlock、cond、thread、file、stage、statement、socket、memory）都有一个 `PFS_*_class` 结构体继承它，共享「名字 / 是否启用 / 是否计时 / 事件序号」这套元数据。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责采集与暴露运行期指标（等待事件、语句摘要、表与索引统计）；**不负责慢查询日志（属于日志体系）**、**不负责语句执行本身（D 组）**。

## 机制是什么

### 它是「插桩 + 存储引擎」而不是「日志」

`storage/perfschema/pfs.cc` 的文件头注释写明了它的定位：从用户视角，Performance Schema 由一个名为 `performance_schema` 的专用数据库 schema 加上若干 SQL 表组成；从实现视角，它是一个**专用的存储引擎（Storage Engine）**，负责暴露散落在服务器代码各处的「Instrumentation Points（插桩点）」采集到的数据。

源码里对插桩接口给出了一份完整的清单（`PAGE_PFS_PSI` 章节的表格），覆盖的插桩种类包括：mutex（高层 `mysql_mutex_lock()` 与低层 `PSI_MUTEX_CALL(start_mutex_wait)`）、rwlock、conditions、errors、file、idle、metadata locks、memory、socket、stage、statement、table I/O、system、thread、transaction、data locks、TLS Channels。

### 三层接入路径

`pfs.cc` 的文档把插桩接口分成三层，这是理解整个机制的关键：

1. **服务器代码 / 插件**：直接调用 `pfs_start_mutex_wait_v1()` / `pfs_end_mutex_wait_v1()` 这样的静态函数（`PSI_MUTEX_CALL` 展开为直接函数调用）。
2. **插件**：同一个 `PSI_MUTEX_CALL` 宏展开为对 `psi_mutex_service` 服务结构体的**函数指针调用**，以避免插件二进制硬引用 `pfs_start_mutex_wait_v1` 符号。
3. **组件（component）**：通过 `REQUIRES_SERVICE_PLACEHOLDER(mysql_mutex_v1)` + `mysql_service_mysql_mutex_v1->lock(...)` 走组件服务框架，组件对服务器代码**零链接期依赖**。

三层调用的是同一份插桩实现，区别只在链接方式。

### 三类接口功能

`pfs.cc` 的 `PAGE_PFS` 章节把插桩点分为三段，这是理解整个机制的关键：

- **起始插桩**：通知 Performance Schema 即将执行某个操作（`pfs_start_mutex_wait_v1`）。
- **实际操作**：被监控的代码本体。
- **结束插桩**：通知 Performance Schema 操作已完成（`pfs_end_mutex_wait_v1`）。

起始插桩返回一个不透明的 `locker` 指针（`PSI_mutex_locker`），结束插桩拿它把结果写回。**这个 locker 携带了本次采集所需的全部上下文**（线程、对象、起始时间戳、开关标志），使得结束插桩不需要再去查全局状态。

### 「仪表类别（class）」与「仪表实例（instance）」

`PFS_instr_class` 是所有 class 的公共基类。`pfs_instr_class.cc` 里为每类对象各维护一张**预分配数组**：

| 类别 | 数组全局变量 | 元素类型 | 注册函数 | 查找函数 |
|---|---|---|---|---|
| mutex | `mutex_class_array` | `PFS_mutex_class` | `register_mutex_class` | `find_mutex_class` |
| rwlock | `rwlock_class_array` | `PFS_rwlock_class` | `register_rwlock_class` | `find_rwlock_class` |
| cond | `cond_class_array` | `PFS_cond_class` | `register_cond_class` | `find_cond_class` |
| thread | `thread_class_array` | `PFS_thread_class` | `register_thread_class` | `find_thread_class` |
| file | `file_class_array` | `PFS_file_class` | `register_file_class` | `find_file_class` |
| stage | `stage_class_array` | `PFS_stage_class` | `register_stage_class` | `find_stage_class` |
| statement | `statement_class_array` | `PFS_statement_class` | `register_statement_class` | `find_statement_class` |
| socket | `socket_class_array` | `PFS_socket_class` | `register_socket_class` | `find_socket_class` |
| memory | `memory_class_array` | `PFS_memory_class` | `register_memory_class` | `find_memory_class` |

**class 是永久的**：源码文档说明「instrument 的生命周期是永久的，它们在服务器启动时加载、之后永不移除」。

### 三种聚合层级

`pfs.cc` 的 `PAGE_PFS_AGGREGATES` 章节把聚合表分成三种实现模型：

- **On the fly（实时聚合）**：写入线程做完全部计算，读线程直接读结果。`pfs_end_mutex_wait_v1` 属于这一类——它同时写 `mutex->m_mutex_stat.m_wait_stat`（实例级）、`thread->write_instr_class_waits_stats()`（线程级）和全局槽位。
- **Higher level（按需聚合）**：读者线程在执行 SELECT 时才从多个内部缓冲区收集数据算结果，写线程零开销。
- **Mixed（混合）**：内部缓冲区不是永久的时采用——对象销毁前把统计写进寿命更长的「父缓冲区」，此时按 on-the-fly 模型；对象在时按 higher-level 模型。

`PFS_table_share::aggregate_io()` / `aggregate_lock()` 属于 mixed 模型：把索引级统计汇总到全局 `global_table_io_stat` / `global_table_lock_stat` 后 `reset()`。

## 怎么实现的

### 第一步：启动期分配各类仪表的缓冲区

`storage/perfschema/pfs_instr_class.cc` 里每个类别都有一对 `init_*_class` / `cleanup_*_class` 函数，签名形态一致：

```cpp
int init_sync_class(uint mutex_class_sizing, uint rwlock_class_sizing,
                    uint cond_class_sizing) {
  mutex_class_dirty_count = mutex_class_allocated_count = 0;
  rwlock_class_dirty_count = rwlock_class_allocated_count = 0;
  cond_class_dirty_count = cond_class_allocated_count = 0;
  mutex_class_max = mutex_class_sizing;
  rwlock_class_max = rwlock_class_sizing;
  cond_class_max = cond_class_sizing;
  mutex_class_lost = rwlock_class_lost = cond_class_lost = 0;

  mutex_class_array = nullptr;
  ...
  if (mutex_class_max > 0) {
    mutex_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_mutex_class, mutex_class_max, sizeof(PFS_mutex_class),
        PFS_mutex_class, MYF(MY_ZEROFILL));
    if (unlikely(mutex_class_array == nullptr)) {
      return 1;
    }
  }
  ...
}
```

要点：

- **容量来自启动参数**：`mutex_class_sizing` / `rwlock_class_sizing` / `cond_class_sizing` 由调用方传入，它们最终来自 Performance Schema 的系统变量（`performance_schema_mutex_class_size` 一类）。
- **分配失败即返回 1**，由调用方决定启动是否失败。
- **`MYF(MY_ZEROFILL)`** 保证数组内容全零——这是后面「注册过程中读者可能看到半初始化条目」不崩的前提（源码注释明确说了这一点）。

同族函数还包括 `init_thread_class` / `init_file_class` / `init_stage_class` / `init_statement_class` / `init_socket_class` / `init_memory_class`，形态一致：置 `*_class_dirty_count = *_class_allocated_count = 0`、`*_class_max = 传入值`、`*_class_lost = 0`，然后 `PFS_MALLOC_ARRAY`。

### 第二步：为「全局类别」注册固定的仪表

有一批 class 是**内建的、永远存在的**，不走 `register_*_class` 路径，而是在 `register_global_classes()` 里直接 `init_instr_class` + `configure_instr_class`：

```cpp
void register_global_classes() {
  /* Table I/O class */
  init_instr_class(&global_table_io_class, table_io_class_name.str,
                   (uint)table_io_class_name.length, 0, 0, PSI_DOCUMENT_ME,
                   PFS_CLASS_TABLE_IO);
  global_table_io_class.m_event_name_index = GLOBAL_TABLE_IO_EVENT_INDEX;
  configure_instr_class(&global_table_io_class);

  /* Table lock class */
  ...
  /* Idle class */
  init_instr_class(&global_idle_class, idle_class_name.str,
                   (uint)idle_class_name.length, PSI_FLAG_USER,
                   0, /* no volatility */
                   PSI_DOCUMENT_ME, PFS_CLASS_IDLE);
  global_idle_class.m_event_name_index = GLOBAL_IDLE_EVENT_INDEX;
  ...
  /* Error class */
  ...
  global_error_class.m_enabled = true; /* Enabled by default */
  configure_instr_class(&global_error_class);
  global_error_class.m_timed = false; /* Not applicable */
  error_class_max = 1;                /* only one error class as of now. */

  /* Transaction class */
  ...
  transaction_class_max = 1; /* used for sizing by other event classes */
}
```

可读出的硬事实：

- **error class 与 transaction class 各只有 1 个**——`error_class_max = 1` 的注释写着 "only one error class as of now"，`transaction_class_max = 1` 的注释写着 "used for sizing by other event classes"。这两行注释还解释了为什么「只有一个」却仍然保留 `*_class_max` 这个变量：它是被别的事件类别拿来定尺寸的。
- **error class 显式 `m_timed = false`**，注释是 "Not applicable"。
- **table I/O / table lock / metadata 三个全局 class 没有设置 `m_enabled`**——它们沿用 `init_instr_class` 里的默认值。

### 第三步：`init_instr_class` 建立一个 class 的初始状态

```cpp
static void init_instr_class(PFS_instr_class *klass, const char *name,
                             uint name_length, int flags, int volatility,
                             const char *documentation,
                             PFS_class_type class_type) {
  assert(name_length <= PFS_MAX_INFO_NAME_LENGTH);
  memset(klass, 0, sizeof(PFS_instr_class));
  klass->m_name.set(class_type, name, name_length);
  klass->m_flags = flags;
  klass->m_enforced_flags = 0;

  /*
    For memory instruments,
    if memory quotas are suggested by default
    by the instrumentation (in the code),
    then enforce memory quotas (in runtime)
  */
  if (klass->m_flags & PSI_FLAG_MEM_COLLECT) {
    klass->m_enforced_flags |= PSI_FLAG_MEM_COLLECT;
  }

  klass->m_volatility = volatility;
  klass->m_enabled = true;
  klass->m_timed = true;
  klass->m_type = class_type;

  klass->m_documentation = nullptr;
  if (documentation != nullptr) {
    /* PSI_DOCUMENT_ME is an empty string. */
    if (documentation[0] != '\0') {
      klass->m_documentation =
          my_strdup(PSI_NOT_INSTRUMENTED, documentation, 0);
    }
  }
}
```

要点：

- **`m_enabled` 与 `m_timed` 的默认值都是 `true`**，但这个默认值只在 `init_instr_class` 里成立——各类 `register_*_class` 会在之后按类别覆盖它们（见下）。
- **`PSI_FLAG_MEM_COLLECT` 会被同时写进 `m_flags` 与 `m_enforced_flags`**：注释说这是「instrumentation 在代码里建议了内存配额，就在运行期强制执行内存配额」。
- **`PSI_DOCUMENT_ME` 是空字符串**，所以它不会被 `my_strdup` 复制——注释专门点明了这一点。
- **名字长度断言 `name_length <= PFS_MAX_INFO_NAME_LENGTH`**。

### 第四步：`PFS_instr_name::set` 建立名字与「旧名」映射

```cpp
void PFS_instr_name::set(PFS_class_type class_type, const char *name,
                         uint max_length_arg) {
  // Copy the given name to the member.
  const uint length = safe_strlen(name, std::min(max_length, max_length_arg));
  memcpy(m_private_name, name, length);
  m_private_name[length] = '\0';
  m_private_name_length = length;

  // Check if there is an alternative name to use when
  // @@terminology_use_previous is enabled.
  auto compatible_name =
      terminology_use_previous::lookup(class_type, std::string{name, length});
  m_private_old_name = compatible_name.old_name;
  m_private_old_name_length =
      compatible_name.old_name ? strlen(compatible_name.old_name) : 0;
  m_private_version = compatible_name.version;
}
```

配套的读取端：

```cpp
const char *PFS_instr_name::str() const {
  DBUG_TRACE;
  if (m_private_old_name != nullptr &&
      terminology_use_previous::is_older_required(m_private_version))
    return m_private_old_name;
  return m_private_name;
}
```

即：**`@@terminology_use_previous` 打开时，仪表会显示旧名**——`terminology_use_previous::lookup` 按 class_type 与新名查出旧名，`is_older_required` 判断是否该用旧名。

`safe_strlen` 是本页唯一一处「不读越界」的辅助函数：

```cpp
static uint safe_strlen(const char *s, uint max_len) {
  const char *end =
      static_cast<const char *>(memchr(s, '\0', static_cast<size_t>(max_len)));
  return end == nullptr ? max_len : static_cast<uint>(end - s);
}
```

用 `memchr` 而不是 `strlen`，因为字符串可能在缓冲区末尾没有 `\0` 终止——注释说「这在字符串可能没有 `\0` 结尾就终止时有用」。找不到 `\0` 时**返回 `max_len` 而非报错**，即名字被截断。

### 第五步：`configure_instr_class` 应用用户配置

```cpp
static void configure_instr_class(PFS_instr_class *entry) {
  uint match_length = 0; /* length of matching pattern */

  // May be NULL in unit tests
  if (pfs_instr_config_array == nullptr) {
    return;
  }
  Pfs_instr_config_array::iterator it = pfs_instr_config_array->begin();
  for (; it != pfs_instr_config_array->end(); ++it) {
    PFS_instr_config *e = *it;
    /**
      Compare class name to all configuration entries. In case of multiple
      matches, the longer specification wins. For example, the pattern
      'ABC/DEF/GHI=ON' has precedence over 'ABC/DEF/%=OFF' regardless of
      position within the configuration file or command line.
      Consecutive wildcards affect the count.
    */
    if (!my_wildcmp(&my_charset_latin1, entry->m_name.str(),
                    entry->m_name.str() + entry->m_name.length(), e->m_name,
                    e->m_name + e->m_name_length, '\\', '?', '%')) {
      if (e->m_name_length >= match_length) {
        entry->m_enabled = e->m_enabled;
        entry->m_timed = e->m_timed;
        match_length = std::max(e->m_name_length, match_length);
      }
    }
  }
}
```

可读出的规则：

- **最长匹配优先**：`e->m_name_length >= match_length` 才覆盖，且 `match_length` 取 `std::max`。注释给的例子是 `'ABC/DEF/GHI=ON'` 优先于 `'ABC/DEF/%=OFF'`，**与它在配置文件或命令行里的位置无关**。
- **通配符是 `%`（任意长）与 `?`（单字符）**，转义符是 `\\`。
- **配置数组可能为 `nullptr`**——注释说 "May be NULL in unit tests"，此时直接返回，不报错。
- **配置只覆盖 `m_enabled` 与 `m_timed` 两个字段**。

### 第六步：`register_*_class` 的「先查重、再占位、后填充」三段式

`register_mutex_class` 是这一族的样板：

```cpp
PFS_sync_key register_mutex_class(const char *name, uint name_length,
                                  PSI_mutex_info *info) {
  uint32 index;
  PFS_mutex_class *entry;

  /*
    This is a full array scan, which is not optimal.
    This is acceptable since this code is only used at startup,
    or when a plugin is loaded.
  */
  REGISTER_CLASS_BODY_PART(index, mutex_class_array, mutex_class_max, name,
                           name_length)
  /*
    Note that:
    mutex_class_dirty_count is incremented *before* an entry is added
    mutex_class_allocated_count is incremented *after* an entry is added
  */
  index = mutex_class_dirty_count++;

  if (index < mutex_class_max) {
    entry = &mutex_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation,
                     PFS_CLASS_MUTEX);
    entry->m_mutex_stat.reset();
    entry->m_event_name_index = mutex_class_start + index;
    entry->m_singleton = nullptr;
    entry->m_enabled = false; /* disabled by default */
    entry->m_timed = false;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON);

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++mutex_class_allocated_count;
    return (index + 1);
  }

  /*
    Out of space, report to SHOW STATUS that
    the allocated memory was too small.
  */
  if (pfs_enabled) {
    mutex_class_lost++;
  }
  return 0;
}
```

逐条拆解：

- **查重是全数组线性扫描**：`REGISTER_CLASS_BODY_PART` 宏展开为 `for (INDEX = 0; INDEX < MAX; ++INDEX)`，比对长度与 `strncmp`。源码注释明说「这是全数组扫描，不是最优的。这可以接受，因为这段代码只在启动时或插件加载时使用」。
- **返回的 key 是 `index + 1`**——**从 1 开始，0 保留作「无效 key」**。这一点在 `FIND_CLASS_BODY` 宏里被反向印证。
- **dirty_count 先加、allocated_count 后加**：注释专门解释这个顺序。**中间存在一个窗口**，源码注释对此有完整描述：读者可能短暂看到一条**不完整**的记录（名字/统计暂时为空或零），但**不会崩**，因为数组是用 `MY_ZEROFILL` 分配的。
- **mutex 类默认 `m_enabled = false; m_timed = false;`**——注释是 "disabled by default"。这与 `init_instr_class` 里的默认 `true` 不同，说明「先设 true 再按类别覆盖」是普遍模式。
- **超容量时不报错，只 `mutex_class_lost++`**——但**有 `if (pfs_enabled)` 前置判断**：`pfs_enabled` 这个全局布尔是「Performance Schema 是否启用」，在 `pfs_instr_class.cc` 里定义为 `bool pfs_enabled = true;`，注释说「这个标志在启动时设置，之后不再改变」。即**性能监控整体关闭时，连「丢失计数」都不累加**。
- **返回 0 表示注册失败**，调用方据此判定 key 无效。

各 `register_*_class` 的默认开关状态**逐类不同**，这是必须逐个核对的地方：

| 注册函数 | 默认 `m_enabled` | 默认 `m_timed` | 依据 |
|---|---|---|---|
| `register_mutex_class` | `false` | `false` | 源码注释 "disabled by default" |
| `register_rwlock_class` | `false` | `false` | 同上 |
| `register_cond_class` | `false` | `false` | 同上 |
| `register_thread_class` | 继承 `init_instr_class` 的 `true` | 继承 `true` | 该函数体内未覆盖这两个字段 |
| `register_file_class` | `true` | `true` | 源码注释 "enabled by default" |
| `register_stage_class` | 取决于 `is_progress()` | 取决于 `is_progress()` | 见下 |
| `register_statement_class` | 取决于 `is_disabled()` | 取决于 `is_untimed()` | 见下 |
| `register_socket_class` | `false` | `false` | 源码注释 "disabled by default" |
| `register_memory_class` | 继承 `true` | **强制 `false`** | 源码注释 "Immutable" |

**stage 的分支**：

```cpp
entry->enforce_valid_flags(PSI_FLAG_STAGE_PROGRESS);

if (entry->is_progress()) {
  /* Stages with progress information are enabled and timed by default */
  entry->m_enabled = true;
  entry->m_timed = true;
} else {
  /* Stages without progress information are disabled by default */
  entry->m_enabled = false;
  entry->m_timed = false;
}
```

**statement 的分支**：

```cpp
entry->enforce_valid_flags(PSI_FLAG_MUTABLE | PSI_FLAG_DISABLED |
                           PSI_FLAG_UNTIMED);

if (entry->is_disabled()) {
  /*
    The instrumentation is PSI_FLAG_DISABLED.
  */
  entry->m_enabled = false;
} else {
  entry->m_enabled = true;
}

if (entry->is_untimed()) {
  /*
    The instrumentation is PSI_FLAG_UNTIMED.
  */
  entry->m_timed = false;
} else {
  entry->m_timed = true;
}
```

**memory 的特殊处理**：

```cpp
entry->enforce_valid_flags(
    (PSI_FLAG_ONLY_GLOBAL_STAT | PSI_FLAG_MEM_COLLECT));

/* Set user-defined configuration options for this instrument */
configure_instr_class(entry);
entry->m_timed = false; /* Immutable */
```

注意 memory 的 `m_timed = false` 是在 `configure_instr_class` **之后**设置的，所以**配置无法把 memory 仪表变成计时的**——注释用 "Immutable" 点明了这一点。

**thread 的额外逻辑**（`register_thread_class`）：

```cpp
entry->m_singleton = nullptr;
entry->m_history = true;

entry->enforce_valid_flags(PSI_FLAG_SINGLETON | PSI_FLAG_USER |
                           PSI_FLAG_THREAD_SYSTEM | PSI_FLAG_AUTO_SEQNUM |
                           PSI_FLAG_NO_SEQNUM);

configure_instr_class(entry);
++thread_class_allocated_count;

entry->m_seqnum.store(1);

if (entry->has_seqnum()) {
  /*
    Ensure room for "-%d" suffix with 2 digits minimum,
    so that:
    - the "-%d" format fits into the class
    - the "-NN" suffix fits into the instance
  */
  assert(strlen(info->m_os_name) < PFS_MAX_OS_NAME_LENGTH - 3);

  (void)snprintf(entry->m_os_name, PFS_MAX_OS_NAME_LENGTH, "%s-%%d",
                 info->m_os_name);
} else {
  assert(strlen(info->m_os_name) < PFS_MAX_OS_NAME_LENGTH);
  strncpy(entry->m_os_name, info->m_os_name, PFS_MAX_OS_NAME_LENGTH);
}
entry->m_os_name[PFS_MAX_OS_NAME_LENGTH - 1] = '\0';
```

即：**带序号的线程 class，其 OS 名会预留 `-NN` 后缀空间**，断言要求 `strlen(info->m_os_name) < PFS_MAX_OS_NAME_LENGTH - 3`（减 3 是为 `-` 加至少两位数字留位）。

### 第七步：按 key 反查 class

```cpp
#define FIND_CLASS_BODY(KEY, COUNT, ARRAY) \
  if ((KEY == 0) || (KEY > COUNT)) {       \
    return NULL;                           \
  }                                        \
  return &ARRAY[KEY - 1]
```

`find_mutex_class` / `find_rwlock_class` / `find_cond_class` / `find_thread_class` / `find_file_class` / `find_stage_class` / `find_statement_class` / `find_socket_class` / `find_memory_class` 全部用这个宏。

**两个关键点**：

1. **`KEY == 0` 直接返回 NULL**——印证了「key 从 1 开始，0 是无效值」。
2. **边界是 `KEY > COUNT` 而非 `KEY > MAX`**——`COUNT` 传的是 `*_class_allocated_count`（已分配数），不是 `*_class_max`（容量）。即**只有真正分配完成的条目才可查**，正好对应「dirty_count 与 allocated_count 之间的窗口不可见」的设计。

`find_statement_class` 的参数类型是 `PFS_stage_key`（源码里就是这么写的），而它查的是 `statement_class_array`——这是一处类型复用，本页如实记录。

### 第八步：`sanitize_*_class` 做指针合法性校验

每个 `find_*_class` 旁边都配一个 `sanitize_*_class`，形如：

```cpp
PFS_mutex_class *sanitize_mutex_class(PFS_mutex_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_mutex_class, mutex_class_array, mutex_class_max,
                      unsafe);
}
```

它们的作用是**校验一个「来路不明」的指针是否真的落在自己的数组里**——这是 Performance Schema 面对用户可写指针（instrumentation 接口把 `PFS_*` 指针暴露给插件）时的防线：插件可能传回一个已被释放或伪造的指针。

对全局 class，`sanitize` 是显式指针比较：

```cpp
PFS_instr_class *sanitize_table_class(PFS_instr_class *unsafe) {
  if (likely((&global_table_io_class == unsafe) ||
             (&global_table_lock_class == unsafe))) {
    return unsafe;
  }
  return nullptr;
}
```

`find_table_class` / `find_idle_class` / `find_metadata_class` / `find_error_class` / `find_transaction_class` 都是同一形态——**index 必须等于 1**，否则返回 `nullptr`（对应「error class 只有 1 个、transaction class 只有 1 个」）。

### 第九步：`pfs.cc` 侧的注册入口——前缀拼接与名字长度约束

`pfs.cc` 里 `pfs_register_mutex_v1` 等函数通过 `REGISTER_BODY_V1` 宏完成「拼前缀 + 调 register」：

```cpp
#define REGISTER_BODY_V1(KEY_T, PREFIX, REGISTER_FUNC)                         \
  KEY_T key;                                                                   \
  char formatted_name[PFS_MAX_INFO_NAME_LENGTH];                               \
  size_t prefix_length;                                                        \
  size_t len;                                                                  \
  size_t full_length;                                                          \
                                                                                \
  assert(category != nullptr);                                                 \
  assert(info != nullptr);                                                     \
  if (unlikely(                                                                \
          build_prefix(&PREFIX, category, formatted_name, &prefix_length)) ||  \
      !pfs_initialized) {                                                      \
    for (; count > 0; count--, info++) *(info->m_key) = 0;                     \
    return;                                                                    \
  }                                                                            \
                                                                                \
  for (; count > 0; count--, info++) {                                         \
    assert(info->m_key != nullptr);                                            \
    assert(info->m_name != nullptr);                                           \
    len = strlen(info->m_name);                                                \
    full_length = prefix_length + len;                                         \
    if (likely(full_length <= PFS_MAX_INFO_NAME_LENGTH)) {                     \
      memcpy(formatted_name + prefix_length, info->m_name, len);               \
      key = REGISTER_FUNC(formatted_name, (uint)full_length, info);            \
    } else {                                                                   \
      pfs_print_error("REGISTER_BODY_V1: name too long <%s> <%s>\n", category, \
                      info->m_name);                                           \
      key = 0;                                                                 \
    }                                                                          \
                                                                                \
    *(info->m_key) = key;                                                      \
  }                                                                            \
  return
```

**这条宏是「注册失败」的唯一处理点**：名字超长时打印错误并把 key 置 0；`build_prefix` 失败或 `pfs_initialized` 为假时**把所有 key 全部置 0 并直接返回**——即**性能监控未初始化时，插桩注册整体退化为「全部无效」**，被监控代码照常运行（这正是源码文档里「instrumentation point 全部返回 void、没有错误码」的兑现）。

前缀拼接由 `build_prefix` 完成：

```cpp
static int build_prefix(const LEX_CSTRING *prefix, const char *category,
                        char *output, size_t *output_length) {
  const size_t len = strlen(category);
  char *out_ptr = output;
  const size_t prefix_length = prefix->length;

  if (unlikely((prefix_length + len + 2) >= PFS_MAX_FULL_PREFIX_NAME_LENGTH)) {
    pfs_print_error("build_prefix: prefix+category is too long <%s> <%s>\n",
                    prefix->str, category);
    return 1;
  }

  if (unlikely(strchr(category, '/') != nullptr)) {
    pfs_print_error("build_prefix: invalid category <%s>\n", category);
    return 1;
  }

  /* output = prefix + '/' + category + '/' */
  memcpy(out_ptr, prefix->str, prefix_length);
  out_ptr += prefix_length;
  if (len > 0) {
    *out_ptr = '/';
    out_ptr++;
    memcpy(out_ptr, category, len);
    out_ptr += len;
    *out_ptr = '/';
    out_ptr++;
  }
  *output_length = int(out_ptr - output);

  return 0;
}
```

两条硬约束：

- **`prefix_length + len + 2 >= PFS_MAX_FULL_PREFIX_NAME_LENGTH` 即失败**——那个 `2` 是拼进去的两个 `/`。
- **category 里不能含 `/`**——否则失败。注释里的例子说明前缀形态是 `'wait/sync/mutex/sql/'`，即 `prefix + '/' + category + '/'`。

`pfs_register_rwlock_v2` 是唯一不走 `REGISTER_BODY_V1` 宏的注册函数，因为它要按 `PSI_FLAG_RWLOCK_SX` / `PSI_FLAG_RWLOCK_PR` 分别用**三个不同前缀**（`prlock_instrument_prefix` / `rwlock_instrument_prefix` / `sxlock_instrument_prefix`）注册成三类。

`pfs_register_thread_v1` 则是**兼容旧 ABI 的适配层**：它把 `PSI_thread_info_v1` 转成 `PSI_thread_info` 再逐条调 `pfs_register_thread_vc`，其中 `m_os_name` 由 `m_name` 截断而来——注释说 "No os name in PSI_thread_info_v1, truncate the thread name."。

### 第十步：事件名索引的分段布局

多个 class 共享一张「按事件名索引」的数组，必须靠**分段偏移**区分。`init_event_name_sizing` 负责算这些偏移：

```cpp
void init_event_name_sizing(const PFS_global_param *param) {
  /* global table I/O, table lock, idle, metadata */
  mutex_class_start = COUNT_GLOBAL_EVENT_INDEX;
  rwlock_class_start = mutex_class_start + param->m_mutex_class_sizing;
  cond_class_start = rwlock_class_start + param->m_rwlock_class_sizing;
  file_class_start = cond_class_start + param->m_cond_class_sizing;
  socket_class_start = file_class_start + param->m_file_class_sizing;
  wait_class_max = socket_class_start + param->m_socket_class_sizing;
}
```

**布局是固定的、可推的**：

1. **头部**：`COUNT_GLOBAL_EVENT_INDEX` 个槽位给全局类别（table I/O、table lock、idle、metadata、error、transaction）——注释列出了这四类「global table I/O, table lock, idle, metadata」，error 与 transaction 另有专用的 `GLOBAL_ERROR_INDEX` / `GLOBAL_TRANSACTION_INDEX`。
2. **随后依次**是 mutex → rwlock → cond → file → socket 各自的 class 段。
3. **每一段的起点等于上一段起点 + 上一段的 sizing 参数**。
4. **`wait_class_max` 是整个等待类事件名数组的上界**（socket 段末尾）。

对应地，`register_mutex_class` 里 `entry->m_event_name_index = mutex_class_start + index;`、`register_rwlock_class` 里 `= rwlock_class_start + index;`、`register_cond_class` 里 `= cond_class_start + index;`、`register_file_class` 里 `= file_class_start + index;`、`register_socket_class` 里 `= socket_class_start + index;`——**全部是「段起点 + 段内下标」**。

而 stage、statement、memory 三类**不带段起点**，直接用段内下标：`register_stage_class` 里 `entry->m_event_name_index = index;`、`register_statement_class` 里同样 `= index;`、`register_memory_class` 里同样 `= index;`。

`pfs_end_mutex_wait_v1` 里有一条断言把这个布局钉死：

```cpp
const uint index = mutex->m_class->m_event_name_index;
assert(index <= wait_class_max);
```

### 第十一步：采集路径——起始插桩建立上下文

以 `pfs_start_mutex_wait_v1` 为例：

```cpp
PSI_mutex_locker *pfs_start_mutex_wait_v1(PSI_mutex_locker_state *state,
                                          PSI_mutex *mutex,
                                          PSI_mutex_operation op,
                                          const char *src_file, uint src_line) {
  ...
  uint flags;
  ulonglong timer_start = 0;

  if (flag_thread_instrumentation) {
    PFS_thread *pfs_thread = my_thread_get_THR_PFS();
    if (unlikely(pfs_thread == nullptr)) {
      return nullptr;
    }
    if (!pfs_thread->m_enabled) {
      return nullptr;
    }
    state->m_thread = reinterpret_cast<PSI_thread *>(pfs_thread);
    flags = STATE_FLAG_THREAD;

    if (pfs_mutex->m_timed) {
      timer_start = get_wait_timer();
      state->m_timer_start = timer_start;
      flags |= STATE_FLAG_TIMED;
    }

    if (flag_events_waits_current) {
      if (unlikely(pfs_thread->m_events_waits_current >=
                   &pfs_thread->m_events_waits_stack[WAIT_STACK_SIZE])) {
        locker_lost++;
        return nullptr;
      }
      PFS_events_waits *wait = pfs_thread->m_events_waits_current;
      state->m_wait = wait;
      flags |= STATE_FLAG_EVENT;

      PFS_events_waits *parent_event = wait - 1;
      wait->m_event_type = EVENT_TYPE_WAIT;
      wait->m_nesting_event_id = parent_event->m_event_id;
      wait->m_nesting_event_type = parent_event->m_event_type;

      wait->m_thread_internal_id = pfs_thread->m_thread_internal_id;
      wait->m_class = pfs_mutex->m_class;
      wait->m_timer_start = timer_start;
      wait->m_timer_end = 0;
      wait->m_object_instance_addr = pfs_mutex->m_identity;
      wait->m_event_id = pfs_thread->m_event_id++;
      wait->m_end_event_id = 0;
      wait->m_operation = mutex_operation_map[(int)op];
      wait->m_source_file = src_file;
      wait->m_source_line = src_line;
      wait->m_wait_class = WAIT_CLASS_MUTEX;

      pfs_thread->m_events_waits_current++;
    }
  } else {
    if (pfs_mutex->m_timed) {
      timer_start = get_wait_timer();
      state->m_timer_start = timer_start;
      flags = STATE_FLAG_TIMED;
      state->m_thread = nullptr;
    } else {
      /*
        Complete shortcut.
      */
      /* Aggregate to EVENTS_WAITS_SUMMARY_BY_INSTANCE (counted) */
      pfs_mutex->m_mutex_stat.m_wait_stat.aggregate_counted();
      return nullptr;
    }
  }

  state->m_flags = flags;
  state->m_mutex = mutex;
  return reinterpret_cast<PSI_mutex_locker *>(state);
}
```

可读出的机制细节：

- **flags 是「位集合」**：`STATE_FLAG_THREAD`（写线程级表）、`STATE_FLAG_TIMED`（采时间）、`STATE_FLAG_EVENT`（写当前事件表）。后面每一步都按 flags 分支，避免无谓开销。
- **`flag_thread_instrumentation` 为假时走「只有全局统计」的路径**——此时 `state->m_thread = nullptr`。
- **「Complete shortcut」是真正的快速路径**：线程级插桩关闭、且该仪表不计时时，只做一次 `aggregate_counted()` 就返回 `nullptr`——**连 locker 都不创建**。这是源码文档说的「optimize primarily the critical path (data collection)」的具体落地。
- **等待栈溢出是硬丢弃**：判据是 `pfs_thread->m_events_waits_current >= &pfs_thread->m_events_waits_stack[WAIT_STACK_SIZE]`，即当前等待数达到 `WAIT_STACK_SIZE` 上限；此时 `locker_lost++` 并返回 `nullptr`。**这是一个可统计的丢弃计数，不是错误**。
- **`mutex_operation_map` 是操作码到事件名的映射表**，索引由 `op` 给出。
- **`wait - 1` 是父事件**：等待事件是栈式的，父事件 id 与类型被继承。

### 第十二步：结束插桩做三向聚合

`pfs_end_mutex_wait_v1` 是三向聚合的样板：

```cpp
void pfs_end_mutex_wait_v1(PSI_mutex_locker *locker, int rc) {
  auto *state = reinterpret_cast<PSI_mutex_locker_state *>(locker);
  ...
  auto *mutex = reinterpret_cast<PFS_mutex *>(state->m_mutex);
  auto *thread = reinterpret_cast<PFS_thread *>(state->m_thread);

  const uint flags = state->m_flags;

  if (flags & STATE_FLAG_TIMED) {
    timer_end = get_wait_timer();
    wait_time = timer_end - state->m_timer_start;
    /* Aggregate to EVENTS_WAITS_SUMMARY_BY_INSTANCE (timed) */
    mutex->m_mutex_stat.m_wait_stat.aggregate_value(wait_time);
  } else {
    /* Aggregate to EVENTS_WAITS_SUMMARY_BY_INSTANCE (counted) */
    mutex->m_mutex_stat.m_wait_stat.aggregate_counted();
  }

  if (likely(rc == 0)) {
    mutex->m_owner = thread;
  }

  if (flags & STATE_FLAG_THREAD) {
    PFS_single_stat *event_name_array;
    event_name_array = thread->write_instr_class_waits_stats();
    const uint index = mutex->m_class->m_event_name_index;

    assert(index <= wait_class_max);
    assert(sanitize_thread(thread) != nullptr);

    if (flags & STATE_FLAG_TIMED) {
      /* Aggregate to EVENTS_WAITS_SUMMARY_BY_THREAD_BY_EVENT_NAME (timed) */
      event_name_array[index].aggregate_value(wait_time);
    } else {
      /* Aggregate to EVENTS_WAITS_SUMMARY_BY_THREAD_BY_EVENT_NAME (counted) */
      event_name_array[index].aggregate_counted();
    }

    if (flags & STATE_FLAG_EVENT) {
      auto *wait = reinterpret_cast<PFS_events_waits *>(state->m_wait);
      assert(wait != nullptr);

      wait->m_timer_end = timer_end;
      wait->m_end_event_id = thread->m_event_id;
      if (thread->m_flag_events_waits_history) {
        insert_events_waits_history(thread, wait);
      }
      if (thread->m_flag_events_waits_history_long) {
        insert_events_waits_history_long(wait);
      }
      thread->m_events_waits_current--;

      assert(wait == thread->m_events_waits_current);
    }
  }
}
```

**三个聚合目标**：

1. **实例级**：`mutex->m_mutex_stat.m_wait_stat` —— 对应 `EVENTS_WAITS_SUMMARY_BY_INSTANCE`。
2. **线程级**：`thread->write_instr_class_waits_stats()[index]` —— 对应 `EVENTS_WAITS_SUMMARY_BY_THREAD_BY_EVENT_NAME`，下标就是 `m_event_name_index`。
3. **当前事件**：`PFS_events_waits` 记录 —— 对应 `EVENTS_WAITS_CURRENT`，并按 `m_flag_events_waits_history` / `m_flag_events_waits_history_long` 分别写入历史表。

**「timed」与「counted」是两条完全不同的数据形态**：计时仪表累加 `aggregate_value(wait_time)`（时间总和），不计时仪表累加 `aggregate_counted()`（只累加次数）。这两者在 SQL 层对应不同的列。

`rc == 0` 才写 `mutex->m_owner = thread`——**加锁失败不记录持有者**。

`pfs_end_idle_wait_v1` 展示了同一套模式的另一个变体：它没有实例级聚合，只做线程级与全局级（`global_idle_stat`），因为 idle 没有「实例」概念。

### 第十三步：文件 I/O 按操作类型分三桶

`pfs_end_file_wait_vc` 的分桶逻辑：

```cpp
const size_t bytes = ((int)byte_count > -1 ? byte_count : 0);

PFS_file_stat *file_stat;

if (file != nullptr) {
  file_stat = &file->m_file_stat;
} else {
  file_stat = &klass->m_file_stat;
}

switch (state->m_operation) {
  /* Group read operations */
  case PSI_FILE_READ:
    byte_stat = &file_stat->m_io_stat.m_read;
    break;
  /* Group write operations */
  case PSI_FILE_WRITE:
    byte_stat = &file_stat->m_io_stat.m_write;
    break;
  /* Group remaining operations as miscellaneous */
  case PSI_FILE_CREATE:
  case PSI_FILE_CREATE_TMP:
  case PSI_FILE_OPEN:
  case PSI_FILE_STREAM_OPEN:
  case PSI_FILE_STREAM_CLOSE:
  case PSI_FILE_SEEK:
  case PSI_FILE_TELL:
  case PSI_FILE_FLUSH:
  case PSI_FILE_FSTAT:
  case PSI_FILE_CHSIZE:
  case PSI_FILE_DELETE:
  case PSI_FILE_RENAME:
  case PSI_FILE_SYNC:
  case PSI_FILE_STAT:
  case PSI_FILE_CLOSE:
    byte_stat = &file_stat->m_io_stat.m_misc;
    break;
  default:
    assert(false);
    byte_stat = nullptr;
    break;
}
```

三个要点：

- **字节数为负时归零**：`((int)byte_count > -1 ? byte_count : 0)`。
- **「没有实例时归到 class」**：`file != nullptr` 用实例的统计槽，否则用 class 的——这样文件对象尚未创建时（打开文件的过程中）也能统计。
- **分三桶：读 / 写 / 其余（misc）**。`pfs_end_socket_wait_v1` 用的是完全相同的三桶结构（`PSI_SOCKET_RECV`/`RECVFROM`/`RECVMSG` → 读；`SEND`/`SENDTO`/`SENDMSG` → 写；`CONNECT`/`CREATE`/`BIND`/`SEEK`/`OPT`/`STAT`/`SHUTDOWN`/`SELECT`/`CLOSE` → misc）。

### 第十四步：表 I/O 统计按操作分四类

`pfs_end_table_io_wait_v1`：

```cpp
table_io_stat = &table->m_table_stat.m_index_stat[state->m_index];
table_io_stat->m_has_data = true;

switch (state->m_io_operation) {
  case PSI_TABLE_FETCH_ROW:
    stat = &table_io_stat->m_fetch;
    break;
  case PSI_TABLE_WRITE_ROW:
    stat = &table_io_stat->m_insert;
    break;
  case PSI_TABLE_UPDATE_ROW:
    stat = &table_io_stat->m_update;
    break;
  case PSI_TABLE_DELETE_ROW:
    stat = &table_io_stat->m_delete;
    break;
  default:
    assert(false);
    stat = nullptr;
    break;
}
```

四个计数槽：**fetch / insert / update / delete**。聚合用 `aggregate_many_value(wait_time, numrows)`（计时）或 `aggregate_counted(numrows)`（不计时）——**行数与时间一起累加**。

断言 `state->m_index < table->m_share->m_key_count || state->m_index == MAX_INDEXES` 表明：`MAX_INDEXES` 这个特殊下标代表「整表汇总」（不是某个具体索引）。

### 第十五步：语句采集——`pfs_start_statement_vc` 的多层开关判定

语句插桩的开关判定最复杂，因为它要同时考虑「全局开关 / 线程开关 / 仪表开关 / 计时开关 / 摘要开关 / CPU 开关 / 遥测开关」：

```cpp
uint pfs_flags = 0;

/*
  IF to collect PFS data.
*/
if (flag_global_instrumentation) {
  if (klass->m_enabled) {
    /*
      WHERE to collect PFS data.
    */
    if (flag_thread_instrumentation) {
      if (pfs_thread != nullptr) {
        if (pfs_thread->m_enabled) {
          /* Update per thread PFS tables only, no global table. */
          pfs_flags |= STATE_FLAG_BASE | STATE_FLAG_THREAD;

          if (flag_events_statements_current) {
            pfs_flags |= STATE_FLAG_EVENT;
          }
        }
      }
    } else {
      /* Update global PFS tables only, no per thread tables. */
      pfs_flags |= STATE_FLAG_BASE;
    }

    /*
      WHAT PFS data to collect.
    */
    if (pfs_flags != 0) {
      if (klass->m_timed) {
        pfs_flags |= STATE_FLAG_TIMED;

        if (flag_events_statements_cpu) {
          pfs_flags |= STATE_FLAG_CPU;
        }
      }

      if (flag_statements_digest) {
        pfs_flags |= STATE_FLAG_DIGEST;
      }
    }
  }
}
```

源码用三个注释把判定分成 **IF（要不要采）/ WHERE（采到哪一层）/ WHAT（采什么）** 三段。这套「三层判定」是 Performance Schema 的核心可观测性开关模型。

**注意一个容易被忽略的依赖**：`STATE_FLAG_CPU`（采集 CPU 时间）**嵌套在 `klass->m_timed` 之内**——仪表不计时就拿不到 CPU 时间。

随后 `adjust_collect_flags` 把 PFS 的 flags 与遥测（telemetry）的 flags 合并：

```cpp
static void adjust_collect_flags(PSI_statement_locker_state *state) {
  assert(state != nullptr);

  const uint64_t pfs_flags = state->m_pfs_flags;

  uint64_t tel_flags = 0;

#ifdef HAVE_PSI_SERVER_TELEMETRY_TRACES_INTERFACE
  uint64_t tel_required_flags = 0;

  if (state->m_telemetry_scope & TRACE_STATEMENTS) {
    /*
      m_tel_stmt_start() does not expose fine grained flags to the
      telemetry component.
      TRACE_STATEMENTS means all statements attributes.
    */
    tel_required_flags = STATE_FLAG_STATEMENT_TELEMETRY;
  }

  /* Effective telemetry flags */
  tel_flags = pfs_flags | tel_required_flags;

  state->m_tel_flags = tel_flags;
#endif /* HAVE_PSI_SERVER_TELEMETRY_TRACES_INTERFACE */

  /*
    If
    - the performance schema
    - or telemetry
    needs some data, collect it.
  */
  state->m_collect_flags = pfs_flags | tel_flags;
}
```

**遥测可以单独触发采集**：即使 Performance Schema 自己的 flags 为 0，只要遥测需要 `TRACE_STATEMENTS`，`STATE_FLAG_STATEMENT_TELEMETRY` 就会被并进来。

而 `STATE_FLAG_STATEMENT_TELEMETRY` 这个常量本身就说明了遥测需要语句的哪些属性：

```cpp
static constexpr uint STATE_FLAG_STATEMENT_TELEMETRY =
    STATE_FLAG_BASE     /* statement metrics */
    | STATE_FLAG_THREAD /* thread memory statistics */
    | STATE_FLAG_EVENT  /* query text, error message */
    | STATE_FLAG_DIGEST /* query digest */
    | STATE_FLAG_TIMED  /* timing data */
    | STATE_FLAG_CPU;   /* CPU data */
```

**语句栈溢出同样是被统计的丢弃**：

```cpp
if (pfs_thread->m_events_statements_count >= statement_stack_max) {
  nested_statement_lost++;
  pfs_flags &= ~STATE_FLAG_EVENT;
  state->m_pfs_flags = pfs_flags;
}
```

注意这里的处理与等待栈不同：**语句栈溢出不是放弃整条语句，而是只摘掉 `STATE_FLAG_EVENT`**——语句级的汇总统计仍然照做，只是不再产生 `EVENTS_STATEMENTS_CURRENT` 这一行。

`statement_stack_max` 来自 `init_instruments`（`storage/perfschema/pfs_instr.cc`）里的 `statement_stack_max = param->m_statement_stack_sizing;`。

### 第十六步：语句结束——`pfs_end_statement_vc` 按诊断状态填结果

`pfs_end_statement_vc` 的诊断区处理是「错误信息也进监控」的落点：

```cpp
switch (da->status()) {
  case Diagnostics_area::DA_EMPTY:
    break;
  case Diagnostics_area::DA_OK:
    message_text_length = da->message_text_length();
    if (message_text_length > 0) {
      memcpy(pfs->m_message_text, da->message_text(), message_text_length);
    }
    pfs->m_message_text[message_text_length] = '\0';
    pfs->m_message_text_length = message_text_length;

    pfs->m_rows_affected = da->affected_rows();
    pfs->m_warning_count = da->last_statement_cond_count();
    memcpy(pfs->m_sqlstate, "00000", SQLSTATE_LENGTH);
    break;
  case Diagnostics_area::DA_EOF:
    pfs->m_warning_count = da->last_statement_cond_count();
    break;
  case Diagnostics_area::DA_ERROR:
    message_text_length = da->message_text_length();
    if (message_text_length > 0) {
      memcpy(pfs->m_message_text, da->message_text(), message_text_length);
    }
    pfs->m_message_text[message_text_length] = '\0';
    pfs->m_message_text_length = message_text_length;

    pfs->m_sql_errno = da->mysql_errno();
    memcpy(pfs->m_sqlstate, da->returned_sqlstate(), SQLSTATE_LENGTH);
    pfs->m_error_count++;
    break;
  case Diagnostics_area::DA_DISABLED:
    break;
}
```

可读出的规则：

- **成功时 SQLSTATE 写死 `"00000"`**（不是从诊断区取）。
- **错误时才取 `mysql_errno()` 与 `returned_sqlstate()`，并 `m_error_count++`**。
- **成功与错误都会复制 `message_text`**（成功时是提示信息，错误时是错误文本）。
- **`DA_EOF` 只记警告数**。

内存统计在语句结束时结算：

```cpp
size_t stmt_controlled_size = 0;
size_t stmt_total_size = 0;

if (thread != nullptr) {
  if (state->m_parent_sp_share == nullptr) {
    thread->m_session_all_memory_stat.end_top_statement(&stmt_controlled_size,
                                                        &stmt_total_size);
  } else {
    thread->m_session_all_memory_stat.end_nested_statement(
        state->m_controlled_local_size_start,
        state->m_controlled_stmt_size_start, state->m_total_local_size_start,
        state->m_total_stmt_size_start, &stmt_controlled_size,
        &stmt_total_size);
  }
}
```

**顶层语句与嵌套语句走两个不同的结算入口**——`end_top_statement` 与 `end_nested_statement`。嵌套语句的起始侧对应 `start_top_statement` / `start_nested_statement`（见 `pfs_start_statement_vc`）。

摘要（digest）只在「线程级 + 摘要开关」都成立时才建：

```cpp
if (pfs_flags & STATE_FLAG_DIGEST) {
  digest_storage = state->m_digest;

  if (digest_storage != nullptr) {
    /* Populate PFS_statements_digest_stat with computed digest
     * information.*/
    digest_stat = find_or_create_digest(thread, digest_storage,
                                        state->m_schema_name,
                                        state->m_schema_name_length);
  }
}
```

### 第十七步：内存采集——`pfs_memory_alloc_vc` 的「受控 / 非受控」二分

内存插桩的每次分配会决定这份额外内存**算不算进「受控内存」配额**：

```cpp
PSI_memory_key pfs_memory_alloc_vc(PSI_memory_key key, size_t size,
                                   PSI_thread **owner) {
  PSI_memory_key result_key = key;
  ...
  if (!flag_global_instrumentation) {
    *owner_thread = nullptr;
    return PSI_NOT_INSTRUMENTED;
  }

  PFS_memory_class *klass = find_memory_class(key);
  if (klass == nullptr) {
    *owner_thread = nullptr;
    return PSI_NOT_INSTRUMENTED;
  }

  if (!klass->m_enabled) {
    *owner_thread = nullptr;
    return PSI_NOT_INSTRUMENTED;
  }

  const uint index = klass->m_event_name_index;

  if (flag_thread_instrumentation && !klass->is_global()) {
    PFS_thread *pfs_thread = my_thread_get_THR_PFS();
    if (unlikely(pfs_thread == nullptr)) {
      *owner_thread = nullptr;
      return PSI_NOT_INSTRUMENTED;
    }
    if (!pfs_thread->m_enabled) {
      *owner_thread = nullptr;
      return PSI_NOT_INSTRUMENTED;
    }

    if (klass->has_enforced_memory_cnt()) {
      result_key |= PSI_MEM_CNT_BIT;
    }

    /* Adjust session memory for this thread. */
    if (result_key & PSI_MEM_CNT_BIT) {
      if (pfs_thread->m_cnt_thd != nullptr) {
        pfs_thread->mem_cnt_alloc(size);
      }
      pfs_thread->m_session_all_memory_stat.count_controlled_alloc(size);
    } else {
      pfs_thread->m_session_all_memory_stat.count_uncontrolled_alloc(size);
    }
    ...
```

三个要点：

1. **三层早退**：全局插桩关闭 / class 找不到 / class 未启用——都返回 `PSI_NOT_INSTRUMENTED` 并把 owner 置空。
2. **`PSI_MEM_CNT_BIT` 决定「受控 vs 非受控」**：`has_enforced_memory_cnt()` 为真（来自 `init_instr_class` 里把 `PSI_FLAG_MEM_COLLECT` 同时写进 `m_enforced_flags`）则置位该 bit，走 `count_controlled_alloc`；否则走 `count_uncontrolled_alloc`。**返回的 key 被修改**（`result_key |= PSI_MEM_CNT_BIT`），所以**释放时必须用返回的 key**，这正是 `pfs_memory_free_vc` 里用 `PSI_REAL_MEM_KEY(key)` 还原的原因。
3. **`klass->is_global()` 为真的内存只统计全局槽位**（`global_instr_class_memory_array`），不记到线程上。

`pfs_memory_free_vc` 的注释把一条重要规则写死了：

```cpp
/*
  Do not check klass->m_enabled.
  Do not check flag_global_instrumentation.
  If a memory alloc was instrumented,
  the corresponding free must be instrumented.
*/
```

**即：释放路径故意不做「是否启用 / 是否插桩」的检查**——因为分配时若被插桩了，对应的释放就必须被插桩，否则计数会失衡。

### 第十八步：错误统计的索引折叠

`pfs_log_error_v1` 展示了一种「有限槽位 + 溢出折叠」的统计方式：

```cpp
void pfs_log_error_v1(uint error_num, PSI_error_operation error_operation) {
  ...
  if (!flag_global_instrumentation) {
    return;
  }
  if (!global_error_class.m_enabled) {
    return;
  }
  if (!max_global_server_errors) {
    return;
  }

  /* Find the index of this particular error in array of error stats. */
  error_stat_index = lookup_error_stat_index(error_num);

  /*
    If this error goes beyond max_global_server_errors, OR
    If it's (RE)SIGNALED error with custom error number
    collect its stats at NULL row.
  */
  if (error_stat_index >= max_global_server_errors) {
    error_stat_index = 0;
  }

  if (flag_thread_instrumentation &&
      (error_stat_index < max_session_server_errors)) {
    PFS_thread *pfs_thread = my_thread_get_THR_PFS();
    if (unlikely(pfs_thread == nullptr)) {
      return;
    }
    if (!pfs_thread->m_enabled) {
      return;
    }

    /* Aggregate to EVENTS_ERRORS_SUMMARY_BY_THREAD_BY_ERROR */
    stat = &pfs_thread->write_instr_class_errors_stats()[GLOBAL_ERROR_INDEX];
  } else {
    /* Aggregate to EVENTS_ERRORS_SUMMARY_GLOBAL_BY_ERROR */
    stat = &global_error_stat;
  }

  /* Aggregate to EVENTS_ERRORS_SUMMARY_..._BY_ERROR (counted) */
  stat->aggregate_count(error_stat_index, error_operation);
}
```

要点：

- **错误槽位数由 `max_global_server_errors` 限定**，超出的一律折叠到下标 0——注释说这是「NULL row」，即「无法归类的错误都归到这里」。同样，**`(RE)SIGNAL` 的自定义错误号也归到 0**。
- **`max_global_server_errors` 为 0 时整个函数直接返回**——即「不收集错误」。
- **会话级槽位更少**（`max_session_server_errors`），超过的落到全局。

### 第十九步：插件卸载时清空源码文件指针

`pfs_unload_plugin_v1` 处理的是一个真实的安全问题：

```cpp
void pfs_unload_plugin_v1(const char *plugin_name [[maybe_unused]]) {
  /*
    A plugin or component is being unloaded. Events that originated from the
    plugin contain string pointers set by the __FILE__ macro. These source file
    strings are used in the SOURCE column in some tables, and will become
    invalid when the plugin memory is freed.
    For safety, all file string pointers in all events will be set to NULL.
  */

  /* Temporarily disable the SOURCE column in all tables. */
  pfs_unload_plugin_ref_count++;

  /* Find the relevant events, set source file strings to NULL. */
  reset_source_file_pointers();

  /* Re-enable the SOURCE column in all tables. */
  pfs_unload_plugin_ref_count--;
}
```

**引用计数 `pfs_unload_plugin_ref_count` 是 `std::atomic<uint32>`**（定义在 `pfs_instr_class.cc`），初值 0，注释说「当共享库被卸载时递增，当性能监控处理完该事件时递减」。`reset_source_file_pointers` 在 `storage/perfschema/pfs_instr.cc` 中实现。

### 第二十步：事务统计的读只 / 读写二分

`pfs_end_transaction_v1`：

```cpp
if (flags & STATE_FLAG_TIMED) {
  /* Aggregate to EVENTS_TRANSACTIONS_SUMMARY_..._BY_EVENT_NAME (timed) */
  if (state->m_read_only) {
    stat->m_read_only_stat.aggregate_value(wait_time);
  } else {
    stat->m_read_write_stat.aggregate_value(wait_time);
  }
} else {
  /* Aggregate to EVENTS_TRANSACTIONS_SUMMARY_..._BY_EVENT_NAME (counted) */
  if (state->m_read_only) {
    stat->m_read_only_stat.aggregate_counted();
  } else {
    stat->m_read_write_stat.aggregate_counted();
  }
}

stat->m_savepoint_count += state->m_savepoint_count;
stat->m_rollback_to_savepoint_count += state->m_rollback_to_savepoint_count;
stat->m_release_savepoint_count += state->m_release_savepoint_count;
```

**事务统计把「只读事务」与「读写事务」分开计数**，保存点相关的三个计数是直接累加。

还有一个「事件已被清掉」的防御分支：

```cpp
if (flags & STATE_FLAG_EVENT) {
  auto *pfs = reinterpret_cast<PFS_events_transactions *>(state->m_transaction);
  assert(pfs != nullptr);

  /* events_transactions_current may have been cleared while the transaction
   * was active */
  if (unlikely(pfs->m_class == nullptr)) {
    return;
  }
```

**注意这个 `return` 会跳过后面的保存点计数**——因为它直接返回了整个函数。这是一处真实的行为细节。

提交 / 回滚状态由参数决定：

```cpp
pfs->m_state = (commit ? TRANS_STATE_COMMITTED : TRANS_STATE_ROLLED_BACK);

if (pfs->m_xa) {
  pfs->m_xa_state =
      (commit ? TRANS_STATE_XA_COMMITTED : TRANS_STATE_XA_ROLLBACK_ONLY);
}
```

### 第二十一步：遥测组件的安装 / 卸载检测

`pfs_detect_telemetry_vc` 用「指针不等 + 加锁复查」的双检模式检测遥测组件的装卸：

```cpp
void pfs_detect_telemetry_vc(PSI_thread *thread [[maybe_unused]]) {
#ifdef HAVE_PSI_SERVER_TELEMETRY_TRACES_INTERFACE
  auto *pfs_thread = reinterpret_cast<PFS_thread *>(thread);
  assert(pfs_thread != nullptr);

  // Dirty read
  telemetry_t *actual_telemetry = g_telemetry.m_ptr.load();

  telemetry_t *expected_telemetry = pfs_thread->m_telemetry;

  if (actual_telemetry != expected_telemetry) {
    server_telemetry_tracing_lock();
    // Safe read
    actual_telemetry = g_telemetry.m_ptr.load();
    if (actual_telemetry != expected_telemetry) {
      if (expected_telemetry == nullptr) {
        pfs_thread->m_telemetry = actual_telemetry;
        // We detected that a telemetry component just got installed.
        // Create a telemetry session, and save it in pfs_thread.
        pfs_thread->m_telemetry_session =
            actual_telemetry->m_tel_session_create();
      } else {
        // We detected that a telemetry component just got un installed.
        // Destroy the telemetry session from pfs_thread.
        expected_telemetry->m_tel_session_destroy(
            pfs_thread->m_telemetry_session);
        pfs_thread->m_telemetry = nullptr;
        pfs_thread->m_telemetry_session = nullptr;
      }
    }
    server_telemetry_tracing_unlock();
  }
#endif /* HAVE_PSI_SERVER_TELEMETRY_TRACES_INTERFACE */
}
```

**「先脏读指针、发现不等再上锁复查」** 是标准的双检模式：只有指针变化时才付出锁的代价。

### 第二十二步：表与索引统计——`find_or_create_table_share` 与 DDL 检测

`pfs_instr_class.cc` 里最复杂的一段是表共享（TABLE_SHARE）的插桩。它用 `LF_HASH`（无锁哈希表）按「类型 + 库名 + 表名」做键：

```cpp
PFS_table_share *find_or_create_table_share(PFS_thread *thread, bool temporary,
                                            const TABLE_SHARE *share) {
  PFS_table_share_key key;

  LF_PINS *pins = get_table_share_hash_pins(thread);
  if (unlikely(pins == nullptr)) {
    global_table_share_container.m_lost++;
    return nullptr;
  }
  ...
search:
  entry = reinterpret_cast<PFS_table_share **>(
      lf_hash_search(&table_share_hash, pins, &key, sizeof(key)));
  if (entry && (entry != MY_LF_ERRPTR)) {
    pfs = *entry;
    pfs->inc_refcount();
    if (compare_keys(pfs, share) != 0) {
      /*
        Some DDL was detected.
        - keep the lock stats, they are unaffected
        - destroy the index stats, indexes changed.
        - adjust the expected key count
        - recreate index stats
      */
      pfs->destroy_index_stats();
      pfs->m_key_count = share->keys;
      for (uint index = 0; index < pfs->m_key_count; index++) {
        (void)pfs->find_or_create_index_stat(share, index);
      }
    }
    lf_hash_search_unpin(pins);
    return pfs;
  }

  lf_hash_search_unpin(pins);

  if (retry_count == 0) {
    lookup_setup_object_table(thread, key.m_type, &key.m_schema_name,
                              &key.m_schema_name, &enabled, &timed);
    /*
      Even when enabled is false, a record is added in the dictionary:
      - It makes enabling a table already in the table cache possible,
      - It improves performances for the next time a TABLE_SHARE is reloaded
        in the table cache.
    */
  }
  ...
```

**DDL 检测是这里的核心**：`compare_keys` 比对索引数量与索引名：

```cpp
static int compare_keys(PFS_table_share *pfs, const TABLE_SHARE *share) {
  if (pfs->m_key_count != share->keys) {
    return 1;
  }

  size_t len;
  uint index = 0;
  const uint key_count = share->keys;
  KEY *key_info = share->key_info;
  PFS_table_share_index *index_stat;

  for (; index < key_count; key_info++, index++) {
    index_stat = pfs->find_index_stat(index);
    if (index_stat != nullptr) {
      len = strlen(key_info->name);
      if (len != index_stat->m_key.m_name_length) {
        return 1;
      }
      if (memcmp(index_stat->m_key.m_name, key_info->name, len) != 0) {
        return 1;
      }
    }
  }

  return 0;
}
```

**发现 DDL 后的处理是「保留锁统计、销毁索引统计、重建索引统计」**——注释把这四步写得很清楚。锁统计不受影响，因为表锁的统计与索引无关。

**「即使 enabled 为 false 也要建记录」** 这条注释很关键，理由有两条：让「已在表缓存里的表」后续能被启用；以及提升同一 TABLE_SHARE 再次加载时的性能。

**重试上限是一个硬编码常量**：

```cpp
uint retry_count = 0;
const uint retry_max = 3;
```

并发插入冲突时 `++retry_count > retry_max` 就放弃并 `global_table_share_container.m_lost++`——注释写 "Avoid infinite loops"。`lf_hash_insert` 返回负值是 OOM，同样 `m_lost++`。

**索引统计的创建**：

```cpp
PFS_table_share_index *create_table_share_index_stat(
    const TABLE_SHARE *server_share, uint server_index) {
  assert((server_share != nullptr) || (server_index == MAX_INDEXES));

  PFS_table_share_index *pfs = nullptr;
  pfs_dirty_state dirty_state;

  /* Create a new record in index stat array. */
  pfs = global_table_share_index_container.allocate(&dirty_state);
  if (pfs != nullptr) {
    if (server_index == MAX_INDEXES) {
      pfs->m_key.m_name_length = 0;
    } else {
      KEY *key_info = server_share->key_info + server_index;
      const size_t len = strlen(key_info->name);

      memcpy(pfs->m_key.m_name, key_info->name, len);
      pfs->m_key.m_name_length = len;
    }

    /* Reset the stats. */
    pfs->m_stat.reset();

    /* Use this stat buffer. */
    pfs->m_lock.dirty_to_allocated(&dirty_state);
  }

  return pfs;
}
```

**`MAX_INDEXES` 这个特殊下标代表「整表汇总项」**，它的名字长度为 0。

**锁统计与索引统计的创建都用「原子 CAS」**：

```cpp
PFS_table_share_lock *PFS_table_share::find_or_create_lock_stat() {
  PFS_table_share_lock *pfs = this->m_race_lock_stat.load();
  if (pfs != nullptr) {
    return pfs;
  }

  /* (2) Create a lock stat */
  PFS_table_share_lock *new_pfs = create_table_share_lock_stat();
  if (new_pfs == nullptr) {
    return nullptr;
  }
  new_pfs->m_owner = this;

  /* (3) Atomic CAS */
  if (atomic_compare_exchange_strong(&this->m_race_lock_stat, &pfs, new_pfs)) {
    /* Ok. */
    return new_pfs;
  }

  /* Collision with another thread that also executed (2) and (3). */
  release_table_share_lock_stat(new_pfs);

  return pfs;
}
```

**「先原子读、失败再创建、最后 CAS」** 三段式，CAS 失败说明别的线程抢先创建了，把自己刚建的那个释放掉、用别人建的那个。

### 第二十三步：`setup_object` 派生的开关刷新

`SETUP_OBJECTS` 表的修改需要传导到已存在的对象上。`PFS_table_share::refresh_setup_object_flags`：

```cpp
void PFS_table_share::refresh_setup_object_flags(PFS_thread *thread) {
  lookup_setup_object_table(thread, OBJECT_TYPE_TABLE, &m_key.m_schema_name,
                            &m_key.m_table_name, &m_enabled, &m_timed);
}
```

`update_table_share_derived_flags` 用一个 `PFS_buffer_processor` 回调遍历所有已注册的表共享：

```cpp
class Proc_table_share_derived_flags
    : public PFS_buffer_processor<PFS_table_share> {
 public:
  explicit Proc_table_share_derived_flags(PFS_thread *thread)
      : m_thread(thread) {}

  void operator()(PFS_table_share *pfs) override {
    pfs->refresh_setup_object_flags(m_thread);
  }

 private:
  PFS_thread *m_thread;
};

void update_table_share_derived_flags(PFS_thread *thread) {
  Proc_table_share_derived_flags proc(thread);
  global_table_share_container.apply(proc);
}
```

`update_program_share_derived_flags` 是同一形态的另一个实现（作用于 `global_program_container`）。

### 第二十四步：重置统计

`reset_events_waits_by_class` 展示「重置」是分桶进行的：

```cpp
void reset_events_waits_by_class() {
  reset_file_class_io();
  reset_socket_class_io();
  global_idle_stat.reset();
  global_table_io_stat.reset();
  global_table_lock_stat.reset();
  global_metadata_stat.reset();
}

void reset_file_class_io() {
  PFS_file_class *pfs = file_class_array;
  PFS_file_class *pfs_last = file_class_array + file_class_max;

  for (; pfs < pfs_last; pfs++) {
    pfs->m_file_stat.m_io_stat.reset();
  }
}
```

**注意 `reset_file_class_io` 遍历的是 `file_class_max`（容量）而不是 `file_class_allocated_count`（已分配数）**——这意味着未分配的槽位也会被访问。这是本页如实记录的一处细节。

## 关键取舍

### 取舍一：插桩点不分配内存、不加锁

源码文档把这条列为首要设计原则：「所有 instrumentation point 返回 void，没有错误码。即使性能监控内部失败，服务器代码也会继续执行。」以及「instrumentation point 不分配内存」——内存一般在启动时预分配，部分插桩按块在运行期增量预分配，「两种情况下内存都被视为服务器生命周期内的静态内存」。

**取舍：监控永远不能影响被监控对象的行为。** 代价是**采集失败只能被计数、不能被上报**——所以 `locker_lost`、`nested_statement_lost`、`*_class_lost`、`m_lost` 这一系列计数器是唯一的事故通道。

### 取舍二：优先优化采集路径，把复杂度推给读取路径

源码文档的「No performance hit」原则说得很直白：「当在『记录时处理』和『取数据时处理』之间做选择时，优先让插桩更快，把复杂度推到数据读取侧。」

**取舍：写入侧做最少的活。** 代价是 setup 路径与查询路径「看起来次优」——文档明确承认了这一点。

### 取舍三：采集粒度由「仪表」决定，而不是由「对象」决定

`PFS_instr_class` 是所有类别的公共基类，每个类别一张预分配数组、每个条目一个 key。

**取舍：新增一种被监控对象需要新增一整套数组 + 注册 + 查找 + sanitize 函数。** 代价是扩展成本高——但换来的是**不同类别之间互不干扰**，可以各自独立地设容量上限（`mutex_class_max` / `file_class_max` / `statement_class_max` …）。

### 取舍四：class 永久，instance 有生命周期

源码文档说「instrument 的生命周期是永久的，它们在服务器启动时加载、之后永不移除」；而对象实例的生命周期取决于 `_init` / `_destroy` 调用。

**取舍：保证「事件名」这个维度稳定。** 代价是**插件卸载后仪表条目仍在**——所以 `register_*_class` 的查重步骤（`REGISTER_CLASS_BODY_PART`）专门用来识别「之前加载过又卸载的插件」并复用原槽位（注释："The instrument was not found (from a possible previous load / unload of a plugin), allocate it."）。

### 取舍五：注册冲突不报错，只累加 lost 计数

`register_mutex_class` 超容量时只 `mutex_class_lost++` 并返回 0（无效 key），注释是 "Out of space, report to SHOW STATUS that the allocated memory was too small."

**取舍：容量不足降级为「这一类仪表不再注册」，服务器照常运行。** 代价是**问题被推迟到 SHOW STATUS 才可见**，而且由于 `if (pfs_enabled)` 的前置判断，性能监控整体关闭时连这个计数都没有。

### 取舍六：最长匹配优先，与配置顺序无关

`configure_instr_class` 的注释给了明确例子：`'ABC/DEF/GHI=ON'` 优先于 `'ABC/DEF/%=OFF'`，**regardless of position within the configuration file or command line**。

**取舍：规则可预测。** 代价是**用户必须知道「更具体的模式优先」这条规则**，否则会与「后者覆盖前者」的直觉相反。

### 取舍七：内存释放路径故意不做启用检查

`pfs_memory_free_vc` 的注释：「Do not check klass->m_enabled. Do not check flag_global_instrumentation. If a memory alloc was instrumented, the corresponding free must be instrumented.」

**取舍：保证内存计数不失衡。** 代价是**即使该内存类当前是关闭的，释放路径仍会执行计数逻辑**（只是不写受控配额）。

### 取舍八：语句栈溢出只摘事件、不摘语句

`pfs_start_statement_vc` 里语句栈溢出时执行的是 `pfs_flags &= ~STATE_FLAG_EVENT;`，**不是整个放弃**。

**取舍：保住语句级汇总统计的完整性。** 代价是**这一刻的 `EVENTS_STATEMENTS_CURRENT` 会缺一行**，用户看到的当前语句数与实际不符。

### 取舍九：等待栈溢出则整条丢弃

`pfs_start_mutex_wait_v1` 里等待栈溢出时是 `locker_lost++; return nullptr;`——**整条等待事件丢失**。

**取舍：等待事件的嵌套结构必须完整，宁可丢一条也不能写坏栈。** 代价是**深层嵌套的等待（超过 `WAIT_STACK_SIZE`）会完全不被记录**，且这一丢失只体现在 `locker_lost` 计数上。

### 取舍十：内存统计用「受控 / 非受控」二分

`PSI_MEM_CNT_BIT` 由 `has_enforced_memory_cnt()` 决定，后者来自 `PSI_FLAG_MEM_COLLECT` 被同时写入 `m_flags` 与 `m_enforced_flags`。

**取舍：只有「明确声明要管内存配额」的内存类才进入配额统计。** 代价是**同一段代码的内存可能一部分受控、一部分不受控**——所以 `pfs_memory_alloc_vc` 的返回 key 被 `|= PSI_MEM_CNT_BIT` 修改过，释放时必须用这个被修改过的 key。

### 取舍十一：错误槽位有限，超出折叠到 NULL 行

`pfs_log_error_v1` 里 `if (error_stat_index >= max_global_server_errors) { error_stat_index = 0; }`，注释说超出范围的错误「以及 `(RE)SIGNAL` 的自定义错误号」都归到 NULL 行。

**取舍：内存占用有界。** 代价是**自定义错误号在统计上无法区分**。

### 取舍十二：错误统计的会话槽位比全局更少

`if (flag_thread_instrumentation && (error_stat_index < max_session_server_errors))`——会话级槽位更少，超出的错误**落到全局统计**而非丢弃。

**取舍：会话级视图省内存，全局视图保完整性。**

### 取舍十三：插件卸载时清空所有源码文件指针

`pfs_unload_plugin_v1` 调 `reset_source_file_pointers()` 把所有事件里的 `__FILE__` 字符串指针置空，代价是这段时间内 SOURCE 列不可用。

**取舍：避免悬垂指针导致崩溃。** 代价是**插件卸载后残留的历史事件丢失 SOURCE 信息**——这是安全优先于完整性的取舍。

### 取舍十四：脏计数与已分配计数分离，容忍短暂的中间态

`register_mutex_class` 的注释详细描述了 dirty/allocated 之间的窗口及其后果：读者会短暂看到不完整记录（名字/统计暂时为空或零），但不会崩。

**取舍：用「短暂的空值」换取「无锁的并行注册」。** 代价是**读者可能在极短窗口内读到全零的统计行**——但由于数组是 `MY_ZEROFILL` 分配的，安全性有保证。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/perfschema/pfs.cc | `pfs` | PSI 接口实现族的统一前缀：`pfs_register_*`（注册仪表）、`pfs_init_*` / `pfs_destroy_*`（对象生命周期）、`pfs_start_*_wait` / `pfs_end_*_wait`（采集起止）、`pfs_start_statement_vc` / `pfs_end_statement_vc`、`pfs_memory_alloc_vc` / `pfs_memory_free_vc` / `pfs_memory_realloc_vc`、`pfs_log_error_v1`、`pfs_unload_plugin_v1` |
| storage/perfschema/pfs.cc | `build_prefix` | 拼仪表名前缀 `prefix + '/' + category + '/'`；前缀+category+2 长度超 `PFS_MAX_FULL_PREFIX_NAME_LENGTH` 或 category 含 `/` 即返回 1 |
| storage/perfschema/pfs.cc | `REGISTER_BODY_V1` | 注册宏：`build_prefix` 失败或 `!pfs_initialized` 时把所有 key 置 0 并返回；名字超 `PFS_MAX_INFO_NAME_LENGTH` 时报错并把 key 置 0 |
| storage/perfschema/pfs.cc | `adjust_collect_flags` | 把 PFS 的 `m_pfs_flags` 与遥测的 `m_tel_flags` 按位或成 `m_collect_flags`；遥测 `TRACE_STATEMENTS` 会引入 `STATE_FLAG_STATEMENT_TELEMETRY` |
| storage/perfschema/pfs.cc | `STATE_FLAG_STATEMENT_TELEMETRY` | 遥测需要的语句属性集合：BASE / THREAD / EVENT / DIGEST / TIMED / CPU |
| storage/perfschema/pfs.cc | `pfs_register_mutex_v1` / `pfs_register_rwlock_v2` / `pfs_register_cond_v1` / `pfs_register_thread_vc` / `pfs_register_file_vc` / `pfs_register_stage_v1` / `pfs_register_statement_vc` / `pfs_register_socket_v1` / `pfs_register_memory_vc` | 各类仪表的注册入口；`pfs_register_rwlock_v2` 按 `PSI_FLAG_RWLOCK_SX` / `PSI_FLAG_RWLOCK_PR` 分别用 sx / pr / rw 三个前缀 |
| storage/perfschema/pfs.cc | `pfs_register_thread_v1` | 旧 ABI 适配：把 `PSI_thread_info_v1` 转成 `PSI_thread_info`，`m_os_name` 由 `m_name` 截断 |
| storage/perfschema/pfs.cc | `pfs_start_mutex_wait_v1` | 等待采集起始：按 `flag_thread_instrumentation` / `m_timed` / `flag_events_waits_current` 组装 flags；等待栈满 `WAIT_STACK_SIZE` 时 `locker_lost++` 并返回 nullptr；不计时且无线程插桩时走「Complete shortcut」只做一次 `aggregate_counted()` |
| storage/perfschema/pfs.cc | `pfs_end_mutex_wait_v1` | 三向聚合：实例级 `m_mutex_stat.m_wait_stat`、线程级 `write_instr_class_waits_stats()[index]`、事件级 `PFS_events_waits` + 历史表；断言 `index <= wait_class_max` |
| storage/perfschema/pfs.cc | `pfs_end_idle_wait_v1` | idle 只有线程级与全局级聚合（`global_idle_stat`），无实例级 |
| storage/perfschema/pfs.cc | `pfs_end_file_wait_vc` | 文件 I/O 按操作分三桶（读 / 写 / misc）；字节数为负时归零；无实例时归到 class 的 `m_file_stat` |
| storage/perfschema/pfs.cc | `pfs_end_socket_wait_v1` | socket I/O 同样分三桶（RECV* / SEND* / 其余 misc） |
| storage/perfschema/pfs.cc | `pfs_end_table_io_wait_v1` | 表 I/O 四个计数槽：fetch / insert / update / delete；用 `aggregate_many_value(wait_time, numrows)`；断言 index 合法或等于 `MAX_INDEXES` |
| storage/perfschema/pfs.cc | `pfs_end_table_lock_wait_v1` | 表锁统计写入 `m_table_stat.m_lock_stat.m_stat[state->m_index]`，线程级用 `GLOBAL_TABLE_LOCK_EVENT_INDEX` |
| storage/perfschema/pfs.cc | `pfs_start_statement_vc` | 语句采集起始：IF/WHERE/WHAT 三层判定；`STATE_FLAG_CPU` 嵌套在 `m_timed` 内；语句栈满 `statement_stack_max` 时 `nested_statement_lost++` 且只清 `STATE_FLAG_EVENT` |
| storage/perfschema/pfs.cc | `pfs_end_statement_vc` | 语句结束：按 `Diagnostics_area::status()` 分支填错误信息、SQLSTATE（成功时写死 `"00000"`）、`m_error_count`；顶层/嵌套语句分别用 `end_top_statement` / `end_nested_statement` 结算内存 |
| storage/perfschema/pfs.cc | `pfs_memory_alloc_vc` | 内存分配：三层早退返回 `PSI_NOT_INSTRUMENTED`；`has_enforced_memory_cnt()` 决定置 `PSI_MEM_CNT_BIT`，从而走 `count_controlled_alloc` 或 `count_uncontrolled_alloc` |
| storage/perfschema/pfs.cc | `pfs_memory_free_vc` | 内存释放：**故意不检查** `m_enabled` 与 `flag_global_instrumentation`；用 `PSI_REAL_MEM_KEY(key)` 还原 key |
| storage/perfschema/pfs.cc | `pfs_log_error_v1` | 错误统计：`max_global_server_errors` 为 0 则不收集；超出范围的错误号折叠到下标 0（NULL 行）；会话槽位 `max_session_server_errors` 更小，超出落到全局 |
| storage/perfschema/pfs.cc | `pfs_end_transaction_v1` | 事务结束：只读 / 读写分开计数；`pfs->m_class == nullptr` 时直接 return（会跳过保存点计数）；`commit` 决定 `TRANS_STATE_COMMITTED` / `TRANS_STATE_ROLLED_BACK` |
| storage/perfschema/pfs.cc | `pfs_detect_telemetry_vc` | 遥测装卸检测：先脏读 `g_telemetry.m_ptr`，不等时上 `server_telemetry_tracing_lock()` 复查；安装则 `m_tel_session_create()`，卸载则 `m_tel_session_destroy()` |
| storage/perfschema/pfs.cc | `pfs_abort_telemetry_vc` | 组件卸载时销毁本线程的遥测会话并清空指针 |
| storage/perfschema/pfs.cc | `pfs_unload_plugin_v1` | 插件卸载：`pfs_unload_plugin_ref_count++` → `reset_source_file_pointers()` → `--`，避免 `__FILE__` 字符串悬垂 |
| storage/perfschema/pfs_instr_class.cc | `PFS_instr_class` | 所有仪表类别的公共基类：名字（`PFS_instr_name`）、`m_flags` / `m_enforced_flags`、`m_volatility`、`m_enabled`、`m_timed`、`m_type`、`m_documentation`、`m_event_name_index` |
| storage/perfschema/pfs_instr_class.cc | `pfs_enabled` | 全局布尔，启动时设定、之后不再改变；各 `register_*_class` 的 `*_class_lost++` 都以它为前置条件 |
| storage/perfschema/pfs_instr_class.cc | `pfs_unload_plugin_ref_count` | `std::atomic<uint32>`，插件/组件事件引用计数 |
| storage/perfschema/pfs_instr_class.cc | `PFS_instr_name::set` | 复制名字并查 `terminology_use_previous::lookup` 得到旧名与版本 |
| storage/perfschema/pfs_instr_class.cc | `PFS_instr_name::str` / `PFS_instr_name::length` | `@@terminology_use_previous` 生效时返回旧名与旧长度 |
| storage/perfschema/pfs_instr_class.cc | `safe_strlen` | 用 `memchr` 找 `\0`，找不到时返回 `max_len`（不读越界） |
| storage/perfschema/pfs_instr_class.cc | `init_instr_class` | 建立 class 初始状态：`m_enabled = true`、`m_timed = true`；`PSI_FLAG_MEM_COLLECT` 同时写入 `m_flags` 与 `m_enforced_flags`；`PSI_DOCUMENT_ME` 是空串故不复制 |
| storage/perfschema/pfs_instr_class.cc | `configure_instr_class` | 应用用户配置：通配符 `%` / `?` / 转义 `\\`；**最长匹配优先**，与配置顺序无关；只覆盖 `m_enabled` 与 `m_timed`；配置数组为 `nullptr` 时直接返回 |
| storage/perfschema/pfs_instr_class.cc | `register_global_classes` | 注册内建的全局 class：table I/O、table lock、idle、metadata、error、transaction；`error_class_max = 1`、`transaction_class_max = 1`；error class 显式 `m_timed = false` |
| storage/perfschema/pfs_instr_class.cc | `init_event_name_sizing` | 计算事件名索引的分段起点：mutex → rwlock → cond → file → socket，起点 = 上一段起点 + 上一段 sizing；`wait_class_max` 为数组上界 |
| storage/perfschema/pfs_instr_class.cc | `init_sync_class` / `cleanup_sync_class` | 分配 mutex / rwlock / cond 三张 class 数组（`PFS_MALLOC_ARRAY` + `MY_ZEROFILL`），分配失败返回 1；清理时逐个 `my_free` 文档串 |
| storage/perfschema/pfs_instr_class.cc | `init_thread_class` / `init_file_class` / `init_stage_class` / `init_statement_class` / `init_socket_class` / `init_memory_class` | 其余各类 class 数组的分配，形态一致：置 dirty/allocated 为 0、`*_class_max` 为传入值、`*_class_lost` 为 0 |
| storage/perfschema/pfs_instr_class.cc | `register_mutex_class` | mutex 仪表注册：全数组线性查重 → `mutex_class_dirty_count++` → `init_instr_class` → 默认 `m_enabled = false` / `m_timed = false` → `configure_instr_class` → `++mutex_class_allocated_count`；超容量时 `mutex_class_lost++` 并返回 0 |
| storage/perfschema/pfs_instr_class.cc | `register_rwlock_class` / `register_cond_class` / `register_socket_class` | 与 mutex 版同构，默认同样 `m_enabled = false` / `m_timed = false` |
| storage/perfschema/pfs_instr_class.cc | `register_thread_class` | thread 仪表注册：设 `m_history = true`；带序号时 `m_seqnum.store(1)` 且 OS 名预留 `-NN` 后缀，断言 `strlen(info->m_os_name) < PFS_MAX_OS_NAME_LENGTH - 3` |
| storage/perfschema/pfs_instr_class.cc | `register_file_class` | file 仪表注册：默认 `m_enabled = true` / `m_timed = true`（注释 "enabled by default"） |
| storage/perfschema/pfs_instr_class.cc | `register_stage_class` | stage 仪表注册：`is_progress()` 为真则默认启用且计时，否则默认关闭且不计时；`m_event_name_index = index` |
| storage/perfschema/pfs_instr_class.cc | `register_statement_class` | statement 仪表注册：`is_disabled()` / `is_untimed()` 决定 `m_enabled` / `m_timed`；`m_event_name_index = index` |
| storage/perfschema/pfs_instr_class.cc | `register_memory_class` | memory 仪表注册：`configure_instr_class` 之后强制 `m_timed = false`（注释 "Immutable"），配置无法覆盖 |
| storage/perfschema/pfs_instr_class.cc | `REGISTER_CLASS_BODY_PART` | 注册查重宏：全数组扫描比长度与 `strncmp`，命中即返回 `INDEX + 1`；注释说明只在启动时或插件加载时调用 |
| storage/perfschema/pfs_instr_class.cc | `FIND_CLASS_BODY` | 反查宏：`KEY == 0` 或 `KEY > COUNT` 返回 NULL，否则返回 `&ARRAY[KEY - 1]`；`COUNT` 传的是 `*_class_allocated_count` |
| storage/perfschema/pfs_instr_class.cc | `find_mutex_class` / `find_rwlock_class` / `find_cond_class` / `find_thread_class` / `find_file_class` / `find_stage_class` / `find_statement_class` / `find_socket_class` / `find_memory_class` | 按 key 反查 class 的九个入口，全部基于 `FIND_CLASS_BODY`；`find_statement_class` 的参数类型是 `PFS_stage_key` |
| storage/perfschema/pfs_instr_class.cc | `sanitize_mutex_class` / `sanitize_rwlock_class` / `sanitize_cond_class` / `sanitize_thread_class` / `sanitize_file_class` / `sanitize_stage_class` / `sanitize_statement_class` / `sanitize_socket_class` / `sanitize_memory_class` | 校验外部传入的 class 指针是否落在本数组内 |
| storage/perfschema/pfs_instr_class.cc | `find_table_class` / `find_idle_class` / `find_metadata_class` / `find_error_class` / `find_transaction_class` | 全局 class 的反查：index 必须为 1，否则返回 `nullptr` |
| storage/perfschema/pfs_instr_class.cc | `sanitize_table_class` / `sanitize_idle_class` / `sanitize_metadata_class` / `sanitize_error_class` / `sanitize_transaction_class` | 全局 class 的指针校验：与对应全局对象做显式指针比较 |
| storage/perfschema/pfs_instr_class.cc | `find_or_create_table_share` | 表共享插桩：`LF_HASH` 按「类型 + 库名 + 表名」查找；`compare_keys` 检测到 DDL 则保留锁统计、销毁并重建索引统计；`retry_max = 3` 防无限循环；`lf_hash_insert` OOM 或超重试时 `global_table_share_container.m_lost++` |
| storage/perfschema/pfs_instr_class.cc | `compare_keys` | DDL 检测：比对索引数量与每个索引名（长度 + `memcmp`），不一致返回 1 |
| storage/perfschema/pfs_instr_class.cc | `PFS_table_share::find_or_create_lock_stat` / `PFS_table_share::find_or_create_index_stat` | 表锁统计与索引统计的「原子读 → 创建 → CAS」三段式创建；CAS 失败释放自己创建的对象 |
| storage/perfschema/pfs_instr_class.cc | `create_table_share_index_stat` | 索引统计创建：`server_index == MAX_INDEXES` 时名字长度为 0（整表汇总项），否则拷贝 `key_info[server_index].name` |
| storage/perfschema/pfs_instr_class.cc | `PFS_table_share::aggregate_io` / `aggregate_lock` | 混合模型聚合：把各索引统计汇总到 `global_table_io_stat` / `global_table_lock_stat` 后 `reset()` |
| storage/perfschema/pfs_instr_class.cc | `PFS_table_share::refresh_setup_object_flags` / `update_table_share_derived_flags` / `update_program_share_derived_flags` | `SETUP_OBJECTS` 改动传导到已注册对象：用 `PFS_buffer_processor` 回调批量刷新 `m_enabled` / `m_timed` |
| storage/perfschema/pfs_instr_class.cc | `reset_events_waits_by_class` / `reset_file_class_io` / `reset_socket_class_io` | 按类重置等待统计；`reset_file_class_io` 遍历范围是 `file_class_max`（容量）而非已分配数 |
| storage/perfschema/pfs_instr_class.cc | `init_table_share_hash` / `cleanup_table_share_hash` | `LF_HASH` 表共享哈希的初始化（`LF_HASH_UNIQUE`）与销毁，仅在 `param->m_table_share_sizing != 0` 时初始化 |
| storage/perfschema/pfs_instr_class.cc | `set_table_share_key` | 构造哈希键：`m_type` 为 `OBJECT_TYPE_TABLE` 或 `OBJECT_TYPE_TEMPORARY_TABLE`，外加库名与表名 |
| storage/perfschema/pfs_instr_class.cc | `drop_table_share` | 按键删除表共享插桩：从哈希删条目、`destroy_lock_stat()`、`destroy_index_stats()`、归还容器 |

## 与主核 / AI 的关系

- **PFS 是「旁路观测」，不是「主链路的一部分」。** 源码文档明确写：instrumentation point 全部返回 void、没有错误码，监控内部失败时服务器代码照常执行；`pfs_memory_free_vc` 甚至故意不检查开关以保证计数不失衡。**架构推论：** 主核（`engine/db/`）若要引入类似机制，最容易犯的错是让观测路径返回错误或参与事务语义——MySQL 这套设计把「观测失败」彻底降级为计数器（`locker_lost` / `nested_statement_lost` / `*_class_lost` / `m_lost`），这个取舍是可以照搬的。
- **PFS 的内存是「预分配 + 静态」的，不是「按需申请」的。** `pfs.cc` 文档写明内存「一般在启动时预分配」，`init_sync_class` 一类的函数用 `PFS_MALLOC_ARRAY` 一次性按 sizing 参数分配，失败即启动失败。**架构推论：** 主核若照搬「按事件类别分数组 + 启动期 sizing」的形态，需要为每一类可观测对象单独设计容量参数与溢出策略——MySQL 这里已经有 9 类 class 数组各自的 `*_class_max` 与 `*_class_lost`，复制这个形态时容量参数的数量会线性增长。
- **「丢弃」在 PFS 里是一等公民，且被显式计数。** 等待栈满 `WAIT_STACK_SIZE` → `locker_lost++` 并丢弃整条事件；语句栈满 `statement_stack_max` → `nested_statement_lost++` 并只摘掉 `STATE_FLAG_EVENT`；仪表数组满 → `*_class_lost++`；表共享哈希插入失败 → `m_lost++`。**架构推论：** AI 侧若把「采集完整率」当作可信度依据，这四个计数器就是必须先看的四个数字——它们的存在说明 MySQL 侧本来就预期采样会丢。
- **PFS 与遥测（telemetry）是两条并行的采集需求，共享同一份采集结果。** `adjust_collect_flags` 把 PFS 的 flags 与遥测的 flags 按位或；`STATE_FLAG_STATEMENT_TELEMETRY` 定义了遥测需要的语句属性全集；`pfs_detect_telemetry_vc` 用双检模式检测遥测组件的装卸。**架构推论：** 这意味着「AI 观测链路」在 MySQL 里的形态不是「另起一套采集」，而是「在同一份采集结果上再开一个消费者」——AI 侧若要采集同样的指标，复用同一批插桩点比另埋一套更划算。
- **PFS 的接口分层（服务器直调 / 插件服务指针 / 组件服务）是为「二进制兼容」服务的，不是为「性能」服务的。** `pfs.cc` 文档的「Versioned instrumentation」与「Easy deployment」两节说明：插桩接口会随时间扩展，老插件不能被迫重新发布，因此同一部署里必须同时支持多个接口版本。**架构推论：** 主核若要做「可插拔观测」，最容易被忽略的成本是**旧扩展的二进制兼容**——MySQL 用「PSI 服务结构体 + 组件服务占位符」把这个问题解决了，但这套机制的前提是插件与服务器**同版本部署**。
- **PFS 明确不承担慢查询日志与语句执行本身。** 这是清单条目写死的边界。**架构推论：** 对 AI / CDC 链路而言，PFS 提供的是「这次执行发生了什么」的**结构化聚合**（按仪表、按对象实例、按线程），而慢查询日志提供的是「这次执行的文本」。两者不可互相替代——PFS 的 statement digest 是**摘要**（按 `find_or_create_digest` 归一化后的形态），不是原始 SQL 文本。

## 参考

- [storage/perfschema/pfs.cc（MySQL Server 8.0.40 源码，GitHub）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/perfschema/pfs.cc) —— 本页 refs 的权威来源之一：PSI 接口实现族（`pfs_register_*` / `pfs_init_*` / `pfs_start_*_wait` / `pfs_end_*_wait` / `pfs_start_statement_vc` / `pfs_end_statement_vc` / `pfs_memory_*` / `pfs_log_error_v1` / `pfs_unload_plugin_v1`）、`build_prefix`、`REGISTER_BODY_V1`、`adjust_collect_flags`、`STATE_FLAG_STATEMENT_TELEMETRY`，以及 `PAGE_PFS` / `PAGE_PFS_PSI` / `PAGE_PFS_AGGREGATES` 三段设计文档（设计原则、插桩接口分层、聚合层级模型）。
- [storage/perfschema/pfs_instr_class.cc（MySQL Server 8.0.40 源码，GitHub）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/perfschema/pfs_instr_class.cc) —— 本页 refs 的权威来源之二：`PFS_instr_class` 基类与 `PFS_instr_name`（含 `terminology_use_previous` 旧名映射）、`init_instr_class` / `configure_instr_class` / `register_global_classes` / `init_event_name_sizing`、九个 `register_*_class` 与九个 `find_*_class` / `sanitize_*_class`、`find_or_create_table_share` 的 DDL 检测与 CAS 创建路径。
- [storage/perfschema/pfs_instr.cc（MySQL Server 8.0.40 源码，GitHub）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/perfschema/pfs_instr.cc) —— 实例级缓冲区的初始化：`init_instruments` 把 `param` 里的各 sizing 参数落到 `file_handle_max` / `events_waits_history_per_thread` / `statement_stack_max` 等全局变量，并按类别 `init` 各容器；`reset_source_file_pointers` 是 `pfs_unload_plugin_v1` 的实际执行者。
- [storage/perfschema/pfs_account.cc（MySQL Server 8.0.40 源码，GitHub）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/perfschema/pfs_account.cc) —— 账号维度的聚合：`find_or_create_account` 与 `PFS_account::aggregate_waits` / `aggregate_statements` / `aggregate_memory` 等，说明线程级统计如何被上卷到账号级。
- [storage/perfschema 目录（MySQL Server 8.0.40 源码，GitHub）](https://github.com/mysql/mysql-server/tree/mysql-8.0.40/storage/perfschema) —— Performance Schema 存储引擎的完整文件清单，本页涉及的 `pfs.cc` / `pfs_instr.cc` / `pfs_instr_class.cc` / `pfs_account.cc` / `pfs_host.cc` 均在此目录。
- [The Performance Schema（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/performance-schema.html) —— Performance Schema 的官方总览：专用 `performance_schema` schema 与 SQL 表、存储引擎定位、以及「no behavior changes / no performance hit / unintrusive / extendable / versioned / easy deployment」六条设计原则的官方表述。[Oracle 官方手册镜像（同一手册的官方托管副本）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/performance-schema.html)
- [Performance Schema Configuration（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/performance-schema-configuration.html) —— `SETUP_INSTRUMENTS` / `SETUP_OBJECTS` 等配置表的官方说明，对应本页 `configure_instr_class` 的通配符与最长匹配规则、以及 `lookup_setup_object_table` 刷新已注册对象的路径。
- [Performance Schema Instrumentation（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/performance-schema-instrumentation.html) —— 插桩点的官方清单与各仪表类别的启用方式，对应本页九个 `register_*_class` 的默认 `m_enabled` / `m_timed` 状态与各 `*_class_max` sizing 参数。
- [InnoDB Redo Log（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/innodb-redo-log.html) —— 官方手册中与本页同属一个引擎族（InnoDB）的机制文档页，可用于对照「事务边界」与「可观测性」的职责划分；本页不引用其具体实现细节。
