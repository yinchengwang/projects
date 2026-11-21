/**
 * @file memctx.h
 * @brief 内存上下文子系统
 *
 * 实现 AllocSet 内存分配器，支持 palloc/pfree/reset API。
 * 为 SQL 执行引擎提供内存生命周期管理，避免每次小分配都直接调用 malloc/free。
 *
 * AllocSet 设计要点：
 * 1. 按块（Block）批量向系统申请内存，再在块内做小对象分配
 * 2. 父子层级：reset 时只释放子上下文块，保留当前块；delete 时释放全部块
 * 3. 8 字节对齐：所有分配地址按 8 字节对齐
 * 4. pfree 为空操作：释放由 reset/delete 统一完成（典型 PostgreSQL 风格）
 */

#ifndef SDK_IMPL_MEMCTX_H
#define SDK_IMPL_MEMCTX_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* 前向声明 */
typedef struct MemoryContextData *MemoryContext;

typedef enum {
    T_MemoryContext = 100,
    T_AllocSetContext = 101
} MmdbNodeTag;

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * 基本类型定义
 * ======================================================================== */

/** 字节大小类型 */
typedef unsigned long Size;

/** 默认块大小（8KB） */
#define ALLOCSET_DEFAULT_BLOCK_SIZE 8192

/** 最小块大小（含 AllocBlock 头） */
#define ALLOCSET_MIN_BLOCK_SIZE 1024

/* ========================================================================
 * AllocSet 预设配置
 *
 * 为不同使用场景提供经过调优的块大小参数。
 * 块大小按指数增长（当前块 * 2），直到 maxBlockSize 上限。
 * ======================================================================== */

/**
 * @brief AllocSet 预设配置枚举
 */
typedef enum AllocSetPreset {
    ALLOCSET_PRESET_DEFAULT = 0,    /**< 默认：8KB init, 8KB max（通用场景） */
    ALLOCSET_PRESET_SMALL高频 = 1,  /**< 小对象高频：1KB init, 64KB max（SQL executor, request scope） */
    ALLOCSET_PRESET_LARGE = 2,      /**< 大对象：64KB init, 1MB max（集合创建, 索引构建） */
    ALLOCSET_PRESET_BULK = 3,       /**< 批量导入：1MB init, 16MB max（bulk insert, 数据导入） */
} AllocSetPreset;

/** 预设 0：默认 */
#define ALLOCSET_PRESET0_INIT  8192
#define ALLOCSET_PRESET0_MAX   8192

/** 预设 1：小对象高频（1KB → 64KB 指数增长） */
#define ALLOCSET_PRESET1_INIT  1024
#define ALLOCSET_PRESET1_MAX   65536

/** 预设 2：大对象（64KB → 1MB 指数增长） */
#define ALLOCSET_PRESET2_INIT  65536
#define ALLOCSET_PRESET2_MAX   1048576

/** 预设 3：批量导入（1MB → 16MB 指数增长） */
#define ALLOCSET_PRESET3_INIT  1048576
#define ALLOCSET_PRESET3_MAX   16777216

/** 内存对齐基数 */
#define ALLOCSET_ALIGNMENT 8

/* ========================================================================
 * 严格释放模式（MMDB_MEMCTX_STRICT_FREE）
 *
 * 启用后在 pfree()、MemoryContextReset()、MemoryContextDelete() 中执行：
 * - 魔数校验（检测已释放内存）
 * - 双重释放检测（MEMORY_ALLOCATION_FLAG_FREED 标记）
 * - 跨上下文释放检测（owner 字段校验）
 *
 * 仅在 Debug 构建或显式定义 MMDB_MEMCTX_STRICT_FREE=1 时启用。
 * Release 构建默认关闭（性能优先）。
 * ======================================================================== */

#ifndef MMDB_MEMCTX_STRICT_FREE
  #ifdef DEBUG
    #define MMDB_MEMCTX_STRICT_FREE 1
  #else
    #define MMDB_MEMCTX_STRICT_FREE 0
  #endif
#endif

/** 对齐宏：向上对齐到 8 字节 */
#define ALLOCSET_ALIGN(size) \
    (((size) + (ALLOCSET_ALIGNMENT - 1)) & ~(Size)(ALLOCSET_ALIGNMENT - 1))

/** Size 类型可表示的最大值（用于溢出检查） */
#define ALLOCSET_MAX_SIZE (~(Size)0)

/** 获取用户指针之前的分配头（必须在 palloc 返回的指针上调用） */
#define GET_ALLOCATION_HEADER(ptr) \
    ((MemoryAllocationHeader *)((char *)(ptr) - ALLOCSET_ALIGN(sizeof(MemoryAllocationHeader))))

/* 前向声明 */
typedef struct MemoryContextData *MemoryContext;

/* ========================================================================
 * 内存分配头结构
 * ======================================================================== */

/** 分配头魔数 */
#define MEMORY_ALLOCATION_HEADER_MAGIC  0x4D454D435458ULL  /* "MEMCTX" */

/** 分配头标志：已释放 */
#define MEMORY_ALLOCATION_FLAG_FREED    0x1

/* ========================================================================
 * 全局删除哨兵
 * ======================================================================== */

/**
 * @brief 全局删除世代计数器
 *
 * 每次 MemoryContextDelete() 成功释放上下文后递增。
 * 测试可通过比较 close 前后的值验证删除已发生，
 * 避免 use-after-free（释放后仍访问已释放的 is_deleted 字段）。
 */
extern uint64_t g_memctx_delete_generation;

/**
 * @brief 内存分配头
 *
 * 每次用户分配前插入隐藏头部，用于校验和调试。
 */
typedef struct MemoryAllocationHeader {
    uint64_t           magic;           /**< 魔数校验 */
    Size               requested_size;  /**< 用户请求大小 */
    Size               allocated_size;  /**< 实际分配大小（含对齐） */
    MemoryContext      owner;           /**< 所属上下文 */
    uint64_t           generation;      /**< 分配时的 generation */
    uint32_t           flags;           /**< 标志位 */
} MemoryAllocationHeader;

/* ========================================================================
 * 内存上下文统计结构
 * ======================================================================== */

/**
 * @brief 内存上下文统计信息
 */
typedef struct MemoryContextStats {
    Size               current_bytes;       /**< 当前已分配字节数 */
    Size               peak_bytes;          /**< 历史峰值分配字节数 */
    Size               total_allocated;     /**< 累计分配字节数 */
    Size               total_freed;         /**< 累计释放字节数 */
    Size               allocation_count;    /**< 累计分配次数 */
    Size               free_count;          /**< 累计释放次数 */
    Size               reset_count;         /**< reset 次数 */
    Size               oom_count;           /**< OOM 次数 */
    Size               invalid_free_count;  /**< 无效释放次数 */
    Size               double_free_count;   /**< 双重释放次数 */
    Size               resource_count;      /**< 资源析构回调数量 */
    Size               child_count;         /**< 子上下文数量 */
} MemoryContextStats;

/* ========================================================================
 * 内存上下文错误码
 * ======================================================================== */

/**
 * @brief 内存上下文错误码
 */
typedef enum MemoryContextError {
    MMDB_MEMCTX_OK = 0,                  /**< 成功 */
    MMDB_MEMCTX_INVALID_CONTEXT,         /**< 无效上下文 */
    MMDB_MEMCTX_INVALID_POINTER,         /**< 无效指针 */
    MMDB_MEMCTX_CROSS_CONTEXT_FREE,      /**< 跨上下文释放 */
    MMDB_MEMCTX_DOUBLE_FREE,             /**< 双重释放 */
    MMDB_MEMCTX_LIMIT_EXCEEDED,          /**< 超出限额 */
    MMDB_MEMCTX_OVERFLOW,                /**< 溢出 */
    MMDB_MEMCTX_OOM,                     /**< 内存不足 */
    MMDB_MEMCTX_WRONG_THREAD,            /**< 线程不匹配 */
    MMDB_MEMCTX_ALREADY_DELETED          /**< 已删除 */
} MemoryContextError;

/* ========================================================================
 * 资源析构节点
 * ======================================================================== */

/**
 * @brief 资源析构回调节点
 *
 * 用于注册需要在上下文销毁时释放的外部资源。
 */
typedef struct MemoryResource {
    void            *resource;       /**< 资源指针 */
    void (*destructor)(void *resource, void *arg); /**< 析构回调 */
    void            *arg;            /**< 析构回调参数 */
    const char      *name;           /**< 资源名称（调试用） */
    struct MemoryResource *next;     /**< 下一个节点 */
} MemoryResource;

/* ========================================================================
 * 内存上下文方法表
 * ======================================================================== */

/**
 * @brief 内存上下文方法表
 *
 * 多态分派表：不同的分配器实现可注册自己的方法。
 */
typedef struct MemoryContextMethods {
    void *(*alloc)(MemoryContext ctx, Size size);  /**< 分配内存 */
    void  (*free_p)(MemoryContext ctx, void *ptr); /**< 释放单个指针（可为 noop） */
    void  (*reset)(MemoryContext ctx);             /**< 重置上下文（释放子块） */
    void  (*delete_ctx)(MemoryContext ctx);        /**< 删除上下文（释放全部） */
} MemoryContextMethods;

/* ========================================================================
 * 内存上下文数据结构
 * ======================================================================== */

/**
 * @brief 内存上下文数据结构
 */
typedef struct MemoryContextData {
    MmdbNodeTag              type;          /**< 节点类型（复用公共 NodeTag，值为 T_MemoryContext/T_AllocSetContext） */
    MemoryContext            parent;        /**< 父上下文 */
    MemoryContext            firstchild;    /**< 第一个子上下文 */
    MemoryContext            prevchild;     /**< 前一个兄弟 */
    MemoryContext            nextchild;     /**< 后一个兄弟 */
    const MemoryContextMethods *methods;    /**< 方法表 */
    const char              *name;          /**< 上下文名称（调试用） */

    /* 统计字段 */
    Size                     current_bytes;     /**< 当前已分配字节数 */
    Size                     peak_bytes;        /**< 历史峰值分配字节数 */
    Size                     total_allocated;   /**< 累计分配字节数 */
    Size                     total_freed;       /**< 累计释放字节数 */
    Size                     allocation_count;  /**< 累计分配次数 */
    Size                     free_count;        /**< 累计释放次数 */
    Size                     reset_count;       /**< reset 次数 */
    Size                     oom_count;         /**< OOM 次数 */
    Size                     invalid_free_count;/**< 无效释放次数 */
    Size                     double_free_count; /**< 双重释放次数 */

    /* 限额 */
    Size                     max_bytes;         /**< 内存限额（0 表示无限制） */

    /* 校验与生命周期 */
    uint64_t                 generation;        /**< 代次计数器，用于校验 */
    uint32_t                 flags;             /**< 标志位 */

    /* 资源析构 */
    struct MemoryResource   *resources;         /**< 资源析构回调链表 */
    Size                     resource_count;    /**< 资源析构回调数量 */

    /* OOM 策略 */
    void (*on_oom)(MemoryContext context, Size requested, void *arg); /**< OOM 回调 */
    void                    *on_oom_arg;        /**< OOM 回调参数 */

    /* 状态 */
    bool                     is_reset;          /**< 是否已重置 */
    bool                     is_deleted;        /**< 是否已删除 */

    /* 线程归属 */
    bool                     is_thread_owner;   /**< 是否为线程拥有者 */
    uint64_t                 owner_thread_id;   /**< 拥有者线程 ID */
} MemoryContextData;

/* ========================================================================
 * AllocSet 块结构
 * ======================================================================== */

/**
 * @brief AllocSet 块
 *
 * 每块一次性向系统申请大块内存（initBlockSize~maxBlockSize），
 * 之后的小分配在块内线性推进。
 */
typedef struct AllocSetBlock {
    Size                size;       /**< 块总大小（含本结构） */
    Size                free;       /**< 剩余空间（字节） */
    char               *start;      /**< 数据起始地址（紧接本结构之后） */
    char               *end;        /**< 块结束地址 */
    struct AllocSetBlock *next;     /**< 下一块 */
} AllocSetBlock;

/**
 * @brief AllocSet 私有数据
 */
typedef struct AllocSetContext {
    MemoryContextData   header;     /**< 公共头 */
    AllocSetBlock      *blocks;     /**< 块链表（首块） */
    AllocSetBlock      *large_blocks; /**< 大对象块链表（独立分配） */
    Size                initBlockSize; /**< 初始块大小 */
    Size                maxBlockSize;  /**< 最大块大小 */
} AllocSetContext;

/* ========================================================================
 * 公共 API
 * ======================================================================== */

/**
 * @brief 创建 AllocSet 内存上下文
 *
 * @param parent         父上下文（NULL 表示根上下文）
 * @param name           上下文名称（调试用）
 * @param minContextSize 首块最小容量下限（字节，0 表示不设下限）。
 *                       语义：保证首块的数据区（可分配空间）不小于该值；
 *                       若 initBlockSize 推导出的首块数据区偏小，则按此值抬高首块尺寸。
 * @param initBlockSize  初始块大小（0 表示默认 8KB）
 * @param maxBlockSize   最大块大小（0 表示默认 8KB）
 * @param preset         预设配置（0=DEFAULT, 1=SMALL高频, 2=LARGE, 3=BULK）。
 *                       若 preset > 0，则 initBlockSize / maxBlockSize 参数被忽略，
 *                       使用预设值覆盖；若 preset == 0，保持原有参数语义（向后兼容）。
 *
 * @return 新创建的 MemoryContext；失败返回 NULL
 */
MemoryContext AllocSetContextCreate(
    MemoryContext parent,
    const char *name,
    Size minContextSize,
    Size initBlockSize,
    Size maxBlockSize,
    AllocSetPreset preset);

/**
 * @brief 从上下文中分配内存（自动 8 字节对齐）
 *
 * @param ctx  内存上下文
 * @param size 请求字节数
 *
 * @return 分配的内存指针；失败返回 NULL
 */
void *palloc(MemoryContext ctx, Size size);

/**
 * @brief 从上下文中分配零初始化内存
 *
 * @param ctx  内存上下文
 * @param size 请求字节数
 *
 * @return 分配的内存指针（已清零）；失败返回 NULL
 */
void *palloc0(MemoryContext ctx, Size size);

/**
 * @brief 释放内存
 *
 * AllocSet 实现逻辑释放（含校验）；实际释放由 reset/delete 完成。
 * 保留接口以兼容 PostgreSQL 风格 API。
 *
 * @param ctx 内存上下文
 * @param ptr 待释放指针
 */
void pfree(MemoryContext ctx, void *ptr);

/**
 * @brief 重置上下文：释放所有子上下文块，保留当前上下文主块
 *
 * @param ctx 待重置的内存上下文
 */
void reset_memory(MemoryContext ctx);

/**
 * @brief 重置上下文（标准 API 名称）
 *
 * @param context 待重置的内存上下文
 */
void MemoryContextReset(MemoryContext context);

/**
 * @brief 重置所有子上下文，不重置自身
 *
 * @param context 父内存上下文
 */
void MemoryContextResetChildren(MemoryContext context);

/**
 * @brief 删除上下文：释放当前上下文所有块及子上下文
 *
 * @param ctx 待删除的内存上下文
 */
void delete_memory(MemoryContext ctx);

/**
 * @brief 删除上下文（标准 API 名称）
 *
 * @param context 待删除的内存上下文
 */
void MemoryContextDelete(MemoryContext context);

/* ========================================================================
 * 线程归属校验与 Generation 追踪
 * ======================================================================== */

/**
 * @brief 获取当前线程 ID（跨平台）
 *
 * Windows 下使用 GetCurrentThreadId()，其他平台使用 pthread_self()。
 *
 * @return 当前线程唯一标识
 */
uint64_t mmdb_current_thread_id(void);

/**
 * @brief 设置上下文线程归属
 *
 * 将上下文标记为归属于指定线程。后续访问（Debug 模式下）将校验线程身份。
 *
 * @param context   内存上下文（NULL 则为空操作）
 * @param thread_id 线程 ID（通常为 mmdb_current_thread_id()）
 */
void MemoryContextSetThreadOwner(MemoryContext context, uint64_t thread_id);

/**
 * @brief 检查当前线程是否为所有者
 *
 * 若上下文未启用归属检查（is_thread_owner=false），始终返回 true。
 * 若启用且当前线程 ID 与 owner_thread_id 不匹配，返回 false。
 *
 * @param context 内存上下文
 *
 * @return true 表示访问合法或归属检查未启用；false 表示线程不匹配
 */
bool MemoryContextCheckThread(MemoryContext context);

/**
 * @brief 获取上下文 generation 计数器
 *
 * Generation 在 Reset 时自动递增，可用于检测 use-after-reset 错误。
 *
 * @param context 内存上下文（NULL 返回 0）
 *
 * @return 当前 generation 值
 */
uint64_t MemoryContextGetGeneration(MemoryContext context);

/* ========================================================================
 * CurrentMemoryContext 与 SwitchTo API
 * ======================================================================== */

/* 当前线程的当前上下文（线程局部存储） */
extern __thread MemoryContext CurrentMemoryContext;

/* 获取当前上下文 */
MemoryContext MemoryContextCurrent(void);

/* 切换当前上下文，返回旧上下文 */
MemoryContext MemoryContextSwitchTo(MemoryContext context);

/* ========================================================================
 * 资源析构 API
 * ======================================================================== */

/**
 * @brief 注册资源析构回调（LIFO 顺序执行）
 *
 * 注册一个外部资源，当上下文 Reset 或 Delete 时按 LIFO 顺序自动调用析构函数。
 * 资源节点通过 palloc 在当前上下文中分配，由 Reset/Delete 统一回收。
 *
 * @param context    内存上下文
 * @param resource   资源指针
 * @param destructor 析构回调函数
 * @param arg        析构回调参数（可为 NULL）
 * @param name       资源名称（调试用，可为 NULL）
 *
 * @return 0 成功，-1 参数无效
 */
int mmdb_mem_register_resource(
    MemoryContext context,
    void *resource,
    void (*destructor)(void *resource, void *arg),
    void *arg,
    const char *name);

/**
 * @brief 取消注册资源析构回调
 *
 * 从链表中移除指定资源，后续 Reset/Delete 不再调用其析构函数。
 *
 * @param context  内存上下文
 * @param resource 资源指针
 *
 * @return 0 成功，-1 参数无效或未找到
 */
int mmdb_mem_unregister_resource(MemoryContext context, void *resource);

#ifdef __cplusplus
}
#endif

#endif /* SDK_IMPL_MEMCTX_H */
