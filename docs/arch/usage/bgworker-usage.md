> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# 后台任务调度器使用指南

**版本**: v1.0
**日期**: 2026-07-16
**状态**: 已实现

---

## 1. 概述

后台任务调度器（BGWorker）是一个通用的后台线程框架，用于执行数据库系统的后台任务。本指南介绍如何在项目中使用 BGWorker 调度器，包括快速开始、配置参数和完整示例。

---

## 2. 快速开始

### 2.1 头文件包含

```c
#include <db/bgworker.h>
```

### 2.2 启动调度器

```c
/* 启动后台任务调度器 */
db_bgworker_start();
```

### 2.3 注册自定义任务

```c
/**
 * @brief 任务初始化回调（可选）
 * 在任务首次运行时调用一次，用于分配上下文数据。
 */
static int my_task_init(db_bg_task_context_t *ctx) {
    ctx->user_data = my_alloc_context();
    return 0;
}

/**
 * @brief 任务检查回调（必选）
 * 每隔 interval_ms 调用一次，判断是否需要触发 execute。
 * @return > 0 满足触发条件
 *         = 0 不满足条件
 *         < 0 错误
 */
static int my_task_check(db_bg_task_context_t *ctx) {
    /* 返回 > 0 表示满足触发条件 */
    return check_condition(ctx->user_data) ? 1 : 0;
}

/**
 * @brief 任务执行回调（必选）
 * 仅当连续 time_window 次 check 返回 > 0 时才会被调用。
 */
static int my_task_execute(db_bg_task_context_t *ctx) {
    return perform_action(ctx->user_data);
}

/**
 * @brief 任务清理回调（可选）
 * 在调度器停止时调用，用于释放 user_data。
 */
static void my_task_cleanup(db_bg_task_context_t *ctx) {
    my_free_context(ctx->user_data);
}

/* 注册任务 */
db_bg_task_t task = {
    .name = "my_custom_task",
    .interval_ms = 10000,  /* 10 秒检查一次 */
    .init = my_task_init,
    .check = my_task_check,
    .execute = my_task_execute,
    .cleanup = my_task_cleanup,
    .ctx = NULL
};

int slot;
db_bgworker_register(&task, &slot);
```

### 2.4 停止调度器

```c
db_bgworker_stop();
```

---

## 3. 配置参数

### 3.1 调度器全局参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `bgworker.enabled` | bool | `true` | 是否启用后台任务调度器 |
| `bgworker.tick_ms` | int | `1000` | 主循环检查间隔（毫秒） |
| `bgworker.max_tasks` | int | `16` | 最大任务槽位数（编译期常量） |

### 3.2 CCEH 缩容任务参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `bgworker.cceh_shrink.enable` | bool | `true` | 是否启用 CCEH 缩容任务 |
| `bgworker.cceh_shrink.interval_ms` | int | `5000` | CCEH 缩容检查间隔（毫秒） |
| `bgworker.cceh_shrink.load_factor_threshold` | float | `0.25` | 负载因子阈值 |
| `bgworker.cceh_shrink.time_window` | int | `3` | 时间窗口大小（连续满足条件次数） |
| `bgworker.cceh_shrink.min_global_depth` | int | `1` | 最小全局深度（防止过度缩容） |

### 3.3 postgresql.conf 格式示例

```ini
# 后台任务调度器
bgworker.enabled = on
bgworker.tick_ms = 1000

# CCEH 缩容任务
bgworker.cceh_shrink.enable = on
bgworker.cceh_shrink.interval_ms = 5000
bgworker.cceh_shrink.load_factor_threshold = 0.25
bgworker.cceh_shrink.time_window = 3
bgworker.cceh_shrink.min_global_depth = 1
```

---

## 4. 公共 API 参考

### 4.1 生命周期管理

| 函数 | 说明 |
|------|------|
| `db_bgworker_start()` | 启动后台任务调度器（幂等） |
| `db_bgworker_stop()` | 停止后台任务调度器（幂等） |

### 4.2 任务管理

| 函数 | 说明 |
|------|------|
| `db_bgworker_register(task, slot_out)` | 注册任务，返回分配的槽位索引 |
| `db_bgworker_unregister(name)` | 按名称注销任务 |
| `db_bgworker_pause(name)` | 暂停指定任务（状态置为 PAUSED） |
| `db_bgworker_resume(name)` | 恢复指定任务（状态置为 IDLE） |

### 4.3 状态查询

```c
/**
 * @brief 获取任务状态
 * @param name 任务名称
 * @param status_out 输出状态（IDLE/CHECKING/EXECUTING/PAUSED/STOPPED）
 * @return 0 成功，-1 未找到
 */
int db_bgworker_get_status(const char *name, unsigned int *status_out);

/**
 * @brief 获取调度器统计信息
 */
typedef struct db_bgworker_stats {
    uint32_t  task_count;         /* 已注册任务数 */
    uint64_t  running_time_ms;    /* 运行时间（毫秒） */
    uint64_t  total_cycles;       /* 总检查周期数 */
    uint64_t  total_executions;   /* 总执行次数 */
} db_bgworker_stats_t;

int db_bgworker_get_stats(db_bgworker_stats_t *stats_out);
```

---

## 5. 任务状态机

```
        ┌────────────┐
        │    IDLE    │◀──────────────────────────┐
        └─────┬──────┘                            │
              │ tick 到达                          │
              ▼                                   │
        ┌────────────┐     check == 0            │
        │  CHECKING  │──────────────────────────▶│
        └─────┬──────┘                            │
              │ check > 0 且连续 time_window 次   │
              ▼                                   │
        ┌────────────┐                            │
        │ EXECUTING  │──────────────────────────▶│
        └────────────┘                            │
              │ pause()                           │
              ▼                                   │
        ┌────────────┐                            │
        │   PAUSED   │──────── resume() ─────────▶┘
        └────────────┘
```

状态转换说明：

| 起始状态 | 触发条件 | 目标状态 |
|----------|----------|----------|
| IDLE | 到达检查时间 | CHECKING |
| CHECKING | check 返回 0 | IDLE |
| CHECKING | 连续 N 次 check > 0 | EXECUTING |
| EXECUTING | execute 完成 | IDLE |
| 任意 | `db_bgworker_pause()` | PAUSED |
| PAUSED | `db_bgworker_resume()` | IDLE |

---

## 6. 完整示例

### 6.1 简单示例：周期性日志任务

```c
#include <db/bgworker.h>
#include <stdio.h>
#include <time.h>

/**
 * @brief 简单的"心跳"任务，每 5 秒打印一次
 */
typedef struct heartbeat_ctx {
    int beat_count;
} heartbeat_ctx_t;

static int heartbeat_init(db_bg_task_context_t *ctx) {
    heartbeat_ctx_t *hc = calloc(1, sizeof(*hc));
    if (hc == NULL) return -1;
    ctx->user_data = hc;
    return 0;
}

static int heartbeat_check(db_bg_task_context_t *ctx) {
    /* 始终满足条件（每次都触发） */
    return 1;
}

static int heartbeat_execute(db_bg_task_context_t *ctx) {
    heartbeat_ctx_t *hc = (heartbeat_ctx_t *)ctx->user_data;
    hc->beat_count++;
    printf("[heartbeat] beat #%d at %ld\n", hc->beat_count, time(NULL));
    return 0;
}

static void heartbeat_cleanup(db_bg_task_context_t *ctx) {
    free(ctx->user_data);
}

int main(void) {
    /* 启动调度器 */
    if (db_bgworker_start() != 0) {
        fprintf(stderr, "Failed to start bgworker\n");
        return 1;
    }

    /* 注册心跳任务 */
    db_bg_task_t task = {
        .name = "heartbeat",
        .interval_ms = 5000,
        .init = heartbeat_init,
        .check = heartbeat_check,
        .execute = heartbeat_execute,
        .cleanup = heartbeat_cleanup,
        .ctx = NULL
    };

    int slot;
    if (db_bgworker_register(&task, &slot) != 0) {
        fprintf(stderr, "Failed to register heartbeat task\n");
        return 1;
    }

    printf("bgworker running, press Enter to stop...\n");
    getchar();

    /* 停止调度器 */
    db_bgworker_stop();
    return 0;
}
```

### 6.2 CCEH 缩容任务示例

CCEH 缩容任务已内置于调度器中，启用方式为：

```c
/* 启动调度器（自动加载 postgresql.conf 中的配置） */
db_bgworker_start();

/* 缩容任务已自动注册，配置项示例：
 * bgworker.cceh_shrink.enable = on
 * bgworker.cceh_shrink.interval_ms = 5000
 * bgworker.cceh_shrink.load_factor_threshold = 0.25
 * bgworker.cceh_shrink.time_window = 3
 */
```

缩容触发逻辑：

```
当 load_factor < 0.25 连续出现 3 次时，触发一次缩容操作。
每次缩容仅合并一对相邻 segment，避免长时间阻塞。
```

---

## 7. 错误处理

### 7.1 错误码

| 返回值 | 含义 |
|--------|------|
| 0 | 成功 |
| -1 | 通用错误（参数无效、内存不足等） |
| -2 | 任务未找到（注销/暂停时） |
| -3 | 任务已存在（注册时） |
| -4 | 调度器未运行 |
| -5 | 内存分配失败 |
| -6 | 线程创建失败 |

### 7.2 最佳实践

1. **幂等性**：所有 API 都是幂等的，重复调用 `start()` 或 `stop()` 不会出错。
2. **状态查询**：长时间运行的任务，建议定期通过 `db_bgworker_get_stats()` 查看统计信息。
3. **资源管理**：`init()` 中分配的资源必须在 `cleanup()` 中释放，避免内存泄漏。
4. **避免阻塞**：`check()` 和 `execute()` 回调应尽量短小，避免阻塞调度器主循环。
5. **时间窗口**：合理设置 `time_window`，过小容易抖动，过大会延迟响应。

---

## 8. 平台兼容性

| 平台 | 线程实现 | 状态 |
|------|----------|------|
| Linux | `pthread_create` + `pthread_timedjoin_np` | 已支持 |
| Windows | `CreateThread` + `WaitForSingleObject` | 已支持 |
| macOS | `pthread_create` + `pthread_join` | 已支持 |

跨平台差异通过条件编译自动处理，无需业务代码适配。

---

## 9. 测试参考

完整的使用示例和单元测试可参考：

- `engine/src/db/bgworker/bworker.c` — 调度器核心实现
- `engine/src/db/bgworker/tasks/task_cceh_shrink.c` — CCEH 缩容任务示例
- `engine/src/db/bgworker/tasks/task_stats.c` — 统计收集任务示例

---

## 10. 相关文档

- 设计文档：`docs/process/specs/2026-07-16-bgworker-design.md`
- 任务接口定义：`engine/src/db/bgworker/tasks/task_iface.h`
- 内部数据结构：`engine/src/db/bgworker/bgworker_internal.h`
- 公共 API 头文件：`engine/include/db/bgworker.h`