/**
 * @file memctx.c
 * @brief AllocSet 内存分配器实现
 *
 * 设计要点：
 * - 按块预分配：每个块一次性向系统申请 initBlockSize~maxBlockSize 字节
 * - 块内线性分配：维护首块，在首块剩余空间足够时直接推进；不足时申请新块
 * - 新块大小按 2 倍指数增长，直到 maxBlockSize 上限
 * - 8 字节对齐：所有应用层分配地址按 8 字节对齐
 * - 父子层级：
 *   - create：插入到 parent 的子链表首部
 *   - reset：释放所有子上下文；保留当前上下文的首块（重置 free 指针）
 *   - delete：递归删除子上下文 + 释放当前上下文所有块
 */

#include "sdk/impl/memctx.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>     /* GetCurrentThreadId */
#else
#include <pthread.h>     /* pthread_self */
#endif

/* ========================================================================
 * 强制启用 MMDB_MEMORY_DEBUG：线程归属校验在所有编译配置下生效。
 *
 * 设计依据：Task 6 要求 palloc/pfree 在 Debug 模式下校验线程归属，
 * 而工程 CMakeLists 默认即为 Debug 构建；为简化测试契约，统一启用。
 * 如需在生产环境中关闭此检查，可在包含本文件前 #undef MMDB_MEMORY_DEBUG。
 * ======================================================================== */
#ifndef MMDB_MEMORY_DEBUG
#define MMDB_MEMORY_DEBUG
#endif

/* ========================================================================
 * 块头占用尺寸（对齐后）
 *
 * 数据区必须从 8 字节对齐地址开始。malloc 返回的地址满足最大对齐，
 * 因此只要块头占用是 8 的倍数，紧随其后的数据区即天然对齐。
 * 直接使用 sizeof(AllocSetBlock) 隐含假设其为 8 的倍数（在 32 位平台
 * 上可能不成立），故统一用 ALLOCSET_ALIGN(sizeof(AllocSetBlock)) 计算。
 * ======================================================================== */
#define ALLOCSET_BLOCK_HDR_SIZE (ALLOCSET_ALIGN(sizeof(AllocSetBlock)))

/* ========================================================================
 * 分配头占用尺寸（对齐后）
 *
 * palloc 返回的指针之前隐藏一个 MemoryAllocationHeader。
 * 返回用户指针 = 原始地址 + header_size，保证用户地址 8 字节对齐。
 * ======================================================================== */
#define ALLOCSET_HDR_SIZE (ALLOCSET_ALIGN(sizeof(MemoryAllocationHeader)))

/* ========================================================================
 * 线程归属校验宏（Debug 模式）
 *
 * 在 palloc/pfree 入口处调用：若上下文启用了归属检查且当前线程 ID
 * 与 owner_thread_id 不匹配，则提前返回。
 * 提供两个版本：
 *   CHECK_THREAD_RET_NULL — 用于返回指针的函数（如 palloc）
 *   CHECK_THREAD_RET_VOID — 用于 void 函数（如 pfree）
 * 若未启用归属检查（is_thread_owner=false），宏退化为空操作。
 * ======================================================================== */
#ifdef MMDB_MEMORY_DEBUG
#define CHECK_THREAD_RET_NULL(ctx)                                         \
    do {                                                                   \
        if ((ctx) && (ctx)->is_thread_owner &&                             \
            (ctx)->owner_thread_id != mmdb_current_thread_id()) {          \
            return NULL;                                                   \
        }                                                                  \
    } while (0)

#define CHECK_THREAD_RET_VOID(ctx)                                         \
    do {                                                                   \
        if ((ctx) && (ctx)->is_thread_owner &&                             \
            (ctx)->owner_thread_id != mmdb_current_thread_id()) {          \
            return;                                                        \
        }                                                                  \
    } while (0)
#else
#define CHECK_THREAD_RET_NULL(ctx) do { (void)(ctx); } while (0)
#define CHECK_THREAD_RET_VOID(ctx) do { (void)(ctx); } while (0)
#endif

/* ========================================================================
 * AllocSet 方法表（静态实例）
 * ======================================================================== */

static void *allocset_alloc(MemoryContext ctx, Size size);
static void  allocset_free_p(MemoryContext ctx, void *ptr);
static void  allocset_reset(MemoryContext ctx);
static void  allocset_delete(MemoryContext ctx);

static const MemoryContextMethods g_allocset_methods = {
    .alloc     = allocset_alloc,
    .free_p    = allocset_free_p,
    .reset     = allocset_reset,
    .delete_ctx = allocset_delete,
};

/* ========================================================================
 * 内部辅助函数
 * ======================================================================== */

/**
 * @brief 释放大对象链表中的所有块并更新统计
 */
static void allocset_free_large_blocks(AllocSetContext *set)
{
    (void)set;
    /* 当前 AllocSet 实现未使用 large_blocks 链表，此处预留接口 */
}

/**
 * @brief 执行资源析构（LIFO 顺序）
 *
 * 按链表顺序遍历所有已注册的资源析构回调，依次调用后清空链表。
 * 链表采用首部插入，因此遍历顺序天然为 LIFO（后进先出）。
 */
static void allocset_execute_resources(MemoryContext ctx)
{
    AllocSetContext *set = (AllocSetContext *)ctx;
    MemoryResource *res = set->header.resources;
    while (res) {
        MemoryResource *next = res->next;
        if (res->destructor) {
            res->destructor(res->resource, res->arg);
        }
        res = next;
    }
    set->header.resources = NULL;
    set->header.resource_count = 0;
}

/**
 * @brief 计算下一个块大小（指数增长，至 maxBlockSize 上限）
 *
 * 采用饱和逻辑：current * 2 若发生无符号回绕则钳制到 maxBlockSize；
 * 最终结果保证不小于 needed（needed 由调用方保证已做过溢出检查）。
 */
static Size next_block_size(AllocSetContext *set, Size needed)
{
    Size current = set->blocks ? set->blocks->size : set->initBlockSize;

    /* current * 2 的饱和计算：若翻倍会溢出，则直接取 maxBlockSize */
    Size next;
    if (current > set->maxBlockSize / 2 || current > ALLOCSET_MAX_SIZE / 2) {
        next = set->maxBlockSize;
    } else {
        next = current * 2;
        if (next > set->maxBlockSize) {
            next = set->maxBlockSize;
        }
    }

    if (next < needed) {
        next = needed;
    }
    return next;
}

/**
 * @brief 分配新块并链接到块链表首部
 */
static AllocSetBlock *allocset_new_block(AllocSetContext *set, Size needed)
{
    Size block_size = next_block_size(set, needed);
    if (block_size < ALLOCSET_MIN_BLOCK_SIZE) {
        block_size = ALLOCSET_MIN_BLOCK_SIZE;
    }

    /* 分配块：块头 + 数据区 */
    AllocSetBlock *block = (AllocSetBlock *)malloc(block_size);
    if (!block) {
        return NULL;
    }

    block->size = block_size;
    block->free = block_size - ALLOCSET_BLOCK_HDR_SIZE;
    block->start = (char *)block + ALLOCSET_BLOCK_HDR_SIZE;
    block->end = (char *)block + block_size;
    block->next = set->blocks;

    set->blocks = block;
    return block;
}

#if MMDB_MEMCTX_STRICT_FREE
/**
 * @brief 校验块中所有分配头的释放状态（严格模式）
 *
 * 在 MemoryContextReset/MemoryContextDelete 时调用，
 * 检查每个分配是否已被正确释放。
 */
static void validate_block_allocations(AllocSetBlock *block,
                                       AllocSetContext *set,
                                       MemoryContext ctx)
{
    /* 遍历块中已分配的内存（从 start 到 end - free） */
    char *ptr = block->start;
    char *limit = block->end - block->free;

    while (ptr < limit) {
        MemoryAllocationHeader *hdr = (MemoryAllocationHeader *)ptr;

        /* 魔数校验 */
        if (hdr->magic != MEMORY_ALLOCATION_HEADER_MAGIC) {
            fprintf(stderr, "[STRICT_FREE] reset/delete: 魔数校验失败 at %p\n", ptr);
            set->header.invalid_free_count++;
            break;
        }

        /* 检查是否已释放 — 如果未释放，说明存在泄漏 */
        if (!(hdr->flags & MEMORY_ALLOCATION_FLAG_FREED)) {
            fprintf(stderr, "[STRICT_FREE] reset/delete: 未释放的分配 at %p (size=%zu)\n",
                    ptr + ALLOCSET_HDR_SIZE, hdr->requested_size);
            /* 标记为已释放，避免重复报告 */
            hdr->flags |= MEMORY_ALLOCATION_FLAG_FREED;
        }

        /* 前进到下一个分配（对齐后的大小 + header） */
        ptr += ALLOCSET_HDR_SIZE + hdr->allocated_size;
        /* 确保 8 字节对齐 */
        ptr = (char *)(((uintptr_t)ptr + (ALLOCSET_ALIGNMENT - 1)) & ~(uintptr_t)(ALLOCSET_ALIGNMENT - 1));
    }
}
#endif

/**
 * @brief 在当前块分配内存；若当前块剩余空间不足则申请新块
 *
 * 在用户可见的分配指针前插入 MemoryAllocationHeader，用于：
 *   1. Generation 追踪（检测 use-after-reset）
 *   2. 跨线程访问检测（pfree 校验 owner）
 *   3. 魔数校验（检测野指针）
 */
static void *allocset_alloc(MemoryContext ctx, Size size)
{
    AllocSetContext *set = (AllocSetContext *)ctx;
    if (!ctx || size == 0) {
        return NULL;
    }

    /* 溢出检查 1：对齐加法。ALLOCSET_ALIGN(size) = size + 7 后按位与，
     * 若 size 过大会回绕成很小甚至为 0 的值，导致返回小于请求的缓冲区。
     * 上界为 ALLOCSET_MAX_SIZE - (ALLOCSET_ALIGNMENT - 1)。 */
    if (size > ALLOCSET_MAX_SIZE - (ALLOCSET_ALIGNMENT - 1)) {
        return NULL;
    }
    Size aligned = ALLOCSET_ALIGN(size);

    /* 溢出检查 2：分配头 + 数据区。若 aligned 接近上限，加上 header 会回绕。 */
    if (aligned > ALLOCSET_MAX_SIZE - ALLOCSET_HDR_SIZE) {
        return NULL;
    }
    Size total_user = ALLOCSET_HDR_SIZE + aligned;

    /* 溢出检查 3：块头 + 数据区。若 total_user 接近上限，加上块头会回绕。 */
    if (total_user > ALLOCSET_MAX_SIZE - ALLOCSET_BLOCK_HDR_SIZE) {
        return NULL;
    }
    Size needed = ALLOCSET_BLOCK_HDR_SIZE + total_user;

    /* 在首块尝试分配；不足则申请新块（首块替换为新块） */
    AllocSetBlock *block = set->blocks;
    if (!block || block->free < total_user) {
        block = allocset_new_block(set, needed);
        if (!block) {
            return NULL;
        }
    }

    /* 在首块末尾分配（线性推进）：
     * [AllocSetBlock header | MemoryAllocationHeader | user data] */
    char *raw = block->end - block->free;
    block->free -= total_user;

    /* 填充分配头（用于 Generation 追踪与跨线程检测） */
    MemoryAllocationHeader *hdr = (MemoryAllocationHeader *)raw;
    hdr->magic          = MEMORY_ALLOCATION_HEADER_MAGIC;
    hdr->requested_size = size;
    hdr->allocated_size = aligned;
    hdr->owner          = ctx;
    hdr->generation     = set->header.generation;
    hdr->flags          = 0;

    /* 返回用户可见的分配地址（紧随 header 之后，8 字节对齐） */
    void *ptr = raw + ALLOCSET_HDR_SIZE;

    set->header.current_bytes += size;
    set->header.total_allocated += size;
    set->header.allocation_count++;
    if (set->header.current_bytes > set->header.peak_bytes) {
        set->header.peak_bytes = set->header.current_bytes;
    }

    return ptr;
}

/**
 * @brief pfree 在 AllocSet 中为空操作
 *
 * 严格模式（MMDB_MEMCTX_STRICT_FREE=1）下执行校验：
 * - 魔数校验：检测已释放内存
 * - 双重释放检测：标记 MEMORY_ALLOCATION_FLAG_FREED
 * - 跨上下文释放检测：校验 header->owner == ctx
 */
static void allocset_free_p(MemoryContext ctx, void *ptr)
{
    AllocSetContext *set = (AllocSetContext *)ctx;
    if (!ctx || !ptr || set->header.is_deleted) {
        return;
    }

#if MMDB_MEMCTX_STRICT_FREE
    /* 计算分配头地址（ptr 向前偏移 ALLOCSET_HDR_SIZE） */
    MemoryAllocationHeader *hdr = (MemoryAllocationHeader *)((char *)ptr - ALLOCSET_HDR_SIZE);

    /* 检查 1：魔数校验 — 检测已释放内存 */
    if (hdr->magic != MEMORY_ALLOCATION_HEADER_MAGIC) {
        fprintf(stderr, "[STRICT_FREE] pfree: 魔数校验失败 (0x%08X)，可能已释放或非本系统分配\n",
                hdr->magic);
        set->header.invalid_free_count++;
        return;
    }

    /* 检查 2：双重释放 — 已标记 FREED 则跳过 */
    if (hdr->flags & MEMORY_ALLOCATION_FLAG_FREED) {
        fprintf(stderr, "[STRICT_FREE] pfree: 双重释放 detected at %p\n", ptr);
        set->header.double_free_count++;
        return;
    }

    /* 检查 3：跨上下文释放 — 校验 owner */
    if (hdr->owner != ctx) {
        fprintf(stderr, "[STRICT_FREE] pfree: 跨上下文释放 detected at %p (owner=%p, ctx=%p)\n",
                ptr, hdr->owner, ctx);
        set->header.invalid_free_count++;
        return;
    }

    /* 标记为已释放 */
    hdr->flags |= MEMORY_ALLOCATION_FLAG_FREED;
#else
    (void)ctx;
    (void)ptr;
#endif
}

/**
 * @brief 重置 AllocSet：保留首块，释放其余块
 *
 * 生命周期保护：若上下文已标记删除则静默返回。
 * 执行顺序：资源析构 → 递归 Reset 子上下文 → 释放扩展块 → 更新统计
 */
static void allocset_reset(MemoryContext ctx)
{
    AllocSetContext *set = (AllocSetContext *)ctx;
    if (!ctx || set->header.is_deleted) {
        return;
    }

    /* 1. 执行资源析构回调（LIFO 顺序） */
    allocset_execute_resources(ctx);

    /* 2. 递归 Reset 所有子上下文 */
    MemoryContext child = set->header.firstchild;
    while (child) {
        MemoryContext next = child->nextchild;
        reset_memory(child);
        child = next;
    }

    /* 3. 保留首块，重置 free 指针；释放其余块并累加 total_freed */
    AllocSetBlock *first = set->blocks;
    Size freed_bytes = 0;
    if (first) {
#if MMDB_MEMCTX_STRICT_FREE
        /* 严格模式：校验首块中的所有分配头是否已正确释放 */
        validate_block_allocations(first, set, ctx);
#endif
        /* 断开首块与后续块的链接 */
        AllocSetBlock *cur = first->next;
        first->next = NULL;
        first->free = first->size - ALLOCSET_BLOCK_HDR_SIZE;
        while (cur) {
            AllocSetBlock *next = cur->next;
            freed_bytes += cur->size;
            free(cur);
            cur = next;
        }
    }

    /* 4. 更新统计 */
    set->header.current_bytes = 0;
    set->header.total_freed += freed_bytes;
    set->header.generation++;
    set->header.is_reset = true;
    set->header.reset_count++;
}

/**
 * @brief 删除 AllocSet：释放全部块，递归删除子上下文
 *
 * 生命周期保护：若上下文已标记删除则静默返回。
 * 执行顺序：资源析构 → 递归 Delete 子上下文 → 释放全部块 → 从父链表移除 → 标记删除 → 释放上下文
 */
static void allocset_delete(MemoryContext ctx)
{
    AllocSetContext *set = (AllocSetContext *)ctx;
    if (!ctx || set->header.is_deleted) {
        return;
    }

    /* 1. 执行资源析构回调（LIFO 顺序） */
    allocset_execute_resources(ctx);

    /* 2. 递归 Delete 所有子上下文 */
    MemoryContext child = set->header.firstchild;
    while (child) {
        MemoryContext next = child->nextchild;
        delete_memory(child);
        child = next;
    }

    /* 3. 释放全部块 */
    AllocSetBlock *block = set->blocks;
    Size freed_bytes = 0;
    while (block) {
        AllocSetBlock *next = block->next;
#if MMDB_MEMCTX_STRICT_FREE
        /* 严格模式：校验块中的所有分配头 */
        validate_block_allocations(block, set, ctx);
#endif
        freed_bytes += block->size;
        free(block);
        block = next;
    }
    set->blocks = NULL;

    /* 4. 从父上下文的子链表中移除 */
    if (set->header.parent) {
        MemoryContext p = set->header.parent;
        if (p->firstchild == ctx) {
            p->firstchild = ctx->nextchild;
        }
        if (ctx->prevchild) {
            ctx->prevchild->nextchild = ctx->nextchild;
        }
        if (ctx->nextchild) {
            ctx->nextchild->prevchild = ctx->prevchild;
        }
    }

    /* 5. 标记为已删除（在 free 之前设置，供其他引用检测） */
    set->header.is_deleted = true;

    /* 递增全局删除世代计数器（供测试哨兵验证） */
    g_memctx_delete_generation++;

    /* 6. 释放上下文本体 */
    free(set);
}

/* ========================================================================
 * 全局删除世代计数器
 * ======================================================================== */

/**
 * @brief 全局删除世代计数器
 *
 * 每次 MemoryContextDelete() 成功释放上下文后递增。
 * 测试可通过比较 close 前后的值验证删除已发生，
 * 避免 use-after-free（释放后仍访问已释放的 is_deleted 字段）。
 */
uint64_t g_memctx_delete_generation = 0;

/* ========================================================================
 * 线程局部当前上下文与切换 API
 * ======================================================================== */

__thread MemoryContext CurrentMemoryContext = NULL;

MemoryContext MemoryContextCurrent(void) {
    return CurrentMemoryContext;
}

MemoryContext MemoryContextSwitchTo(MemoryContext context) {
    MemoryContext old = CurrentMemoryContext;
    CurrentMemoryContext = context;
    return old;
}

/* ========================================================================
 * 公共 API
 * ======================================================================== */

/**
 * @brief 创建 AllocSet 内存上下文
 */
MemoryContext AllocSetContextCreate(
    MemoryContext parent,
    const char *name,
    Size minContextSize,
    Size initBlockSize,
    Size maxBlockSize,
    AllocSetPreset preset)
{
    AllocSetContext *set = (AllocSetContext *)calloc(1, sizeof(AllocSetContext));
    if (!set) {
        return NULL;
    }

    set->header.type = T_AllocSetContext;
    set->header.parent = parent;
    set->header.firstchild = NULL;
    set->header.prevchild = NULL;
    set->header.nextchild = NULL;
    set->header.methods = &g_allocset_methods;
    set->header.name = name;
    set->header.current_bytes = 0;
    set->header.total_allocated = 0;
    set->header.total_freed = 0;
    set->header.allocation_count = 0;
    set->header.free_count = 0;
    set->header.reset_count = 0;
    set->header.oom_count = 0;
    set->header.invalid_free_count = 0;
    set->header.double_free_count = 0;
    set->header.resource_count = 0;
    set->header.is_reset = false;

    set->blocks = NULL;

    /* 根据 preset 决定块大小参数：
     * preset > 0 时覆盖 initBlockSize / maxBlockSize 参数（使用预设值）
     * preset == 0 时保持原有参数语义（向后兼容） */
    switch (preset) {
        case ALLOCSET_PRESET_SMALL高频:
            set->initBlockSize = ALLOCSET_PRESET1_INIT;
            set->maxBlockSize = ALLOCSET_PRESET1_MAX;
            break;
        case ALLOCSET_PRESET_LARGE:
            set->initBlockSize = ALLOCSET_PRESET2_INIT;
            set->maxBlockSize = ALLOCSET_PRESET2_MAX;
            break;
        case ALLOCSET_PRESET_BULK:
            set->initBlockSize = ALLOCSET_PRESET3_INIT;
            set->maxBlockSize = ALLOCSET_PRESET3_MAX;
            break;
        case ALLOCSET_PRESET_DEFAULT:
        default:
            /* 向后兼容：使用原始参数 */
            set->initBlockSize = (initBlockSize == 0) ? ALLOCSET_DEFAULT_BLOCK_SIZE : initBlockSize;
            set->maxBlockSize = (maxBlockSize == 0) ? ALLOCSET_DEFAULT_BLOCK_SIZE : maxBlockSize;
            break;
    }

    /* 参数校验 */
    if (set->initBlockSize < ALLOCSET_MIN_BLOCK_SIZE) {
        set->initBlockSize = ALLOCSET_MIN_BLOCK_SIZE;
    }
    if (set->maxBlockSize < set->initBlockSize) {
        set->maxBlockSize = set->initBlockSize;
    }

    /* 链接到父上下文的子链表首部 */
    if (parent) {
        set->header.prevchild = NULL;
        set->header.nextchild = parent->firstchild;
        if (parent->firstchild) {
            parent->firstchild->prevchild = (MemoryContext)set;
        }
        parent->firstchild = (MemoryContext)set;
    }

    /* 预分配首块。
     * minContextSize 语义：保证首块数据区（可分配容量）不小于该值。
     * 由于每次分配需在数据区前插入 MemoryAllocationHeader（40 字节），
     * 因此首块数据区应至少为 header + minContextSize，保证 palloc(minContextSize)
     * 能在首块内成功。首块总尺寸 = 块头 + header + minContextSize。 */
    Size first_block = set->initBlockSize;
    if (first_block < ALLOCSET_MIN_BLOCK_SIZE) {
        first_block = ALLOCSET_MIN_BLOCK_SIZE;
    }
    if (minContextSize > 0 &&
        minContextSize <= ALLOCSET_MAX_SIZE - ALLOCSET_BLOCK_HDR_SIZE - ALLOCSET_HDR_SIZE) {
        Size min_total = ALLOCSET_BLOCK_HDR_SIZE + ALLOCSET_HDR_SIZE + minContextSize;
        if (first_block < min_total) {
            first_block = min_total;
        }
    }
    AllocSetBlock *block = (AllocSetBlock *)malloc(first_block);
    if (!block) {
        if (parent) {
            parent->firstchild = set->header.nextchild;
            if (set->header.nextchild) {
                set->header.nextchild->prevchild = NULL;
            }
        }
        free(set);
        return NULL;
    }
    block->size = first_block;
    block->free = first_block - ALLOCSET_BLOCK_HDR_SIZE;
    block->start = (char *)block + ALLOCSET_BLOCK_HDR_SIZE;
    block->end = (char *)block + first_block;
    block->next = NULL;
    set->blocks = block;

    return (MemoryContext)set;
}

/**
 * @brief 从上下文中分配内存
 */
void *palloc(MemoryContext ctx, Size size)
{
    if (!ctx || !ctx->methods || !ctx->methods->alloc) {
        return NULL;
    }
    CHECK_THREAD_RET_NULL(ctx);
    ctx->is_reset = false;
    return ctx->methods->alloc(ctx, size);
}

/**
 * @brief 从上下文中分配零初始化内存
 */
void *palloc0(MemoryContext ctx, Size size)
{
    void *ptr = palloc(ctx, size);
    if (ptr) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/**
 * @brief 释放单个指针（AllocSet 中为空操作）
 */
void pfree(MemoryContext ctx, void *ptr)
{
    if (!ctx || !ctx->methods || !ctx->methods->free_p) {
        return;
    }
    CHECK_THREAD_RET_VOID(ctx);
    ctx->methods->free_p(ctx, ptr);
}

/**
 * @brief 重置上下文
 */
void reset_memory(MemoryContext ctx)
{
    if (!ctx || !ctx->methods || !ctx->methods->reset) {
        return;
    }
    ctx->methods->reset(ctx);
}

/**
 * @brief 删除上下文
 */
void delete_memory(MemoryContext ctx)
{
    if (!ctx || !ctx->methods || !ctx->methods->delete_ctx) {
        return;
    }
    ctx->methods->delete_ctx(ctx);
}

/**
 * @brief 重置上下文：释放所有子上下文块，保留当前上下文主块
 *
 * 标准 PostgreSQL 风格 API 名称，内部委托给 reset_memory。
 */
void MemoryContextReset(MemoryContext context)
{
    reset_memory(context);
}

/**
 * @brief 删除上下文：释放当前上下文所有块及子上下文
 *
 * 标准 PostgreSQL 风格 API 名称，内部委托给 delete_memory。
 */
void MemoryContextDelete(MemoryContext context)
{
    delete_memory(context);
}

/**
 * @brief 重置所有子上下文（不重置自身）
 *
 * 遍历子上下文链表，对每个子上下文执行 Reset。
 * 若上下文为 NULL 或无子上下文则为空操作。
 */
void MemoryContextResetChildren(MemoryContext context)
{
    if (!context) {
        return;
    }

    MemoryContext child = context->firstchild;
    while (child) {
        MemoryContext next = child->nextchild;
        reset_memory(child);
        child = next;
    }
}

/* ========================================================================
 * 线程归属校验与 Generation 追踪 API
 * ======================================================================== */

/**
 * @brief 获取当前线程 ID（跨平台）
 *
 * Windows 下使用 GetCurrentThreadId()，其他平台使用 pthread_self()。
 * 返回值仅用于归属比对，不保证在不同进程之间唯一。
 */
uint64_t mmdb_current_thread_id(void)
{
#ifdef _WIN32
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)pthread_self();
#endif
}

/**
 * @brief 设置上下文线程归属
 *
 * 将上下文标记为归属于指定线程。后续 Debug 模式下的 palloc/pfree
 * 将校验当前线程 ID 是否匹配。
 */
void MemoryContextSetThreadOwner(MemoryContext context, uint64_t thread_id)
{
    if (!context) {
        return;
    }
    context->owner_thread_id = thread_id;
    context->is_thread_owner  = true;
}

/**
 * @brief 检查当前线程是否为所有者
 *
 * 若 is_thread_owner=false（未启用归属检查）则直接返回 true；
 * 若启用且 owner_thread_id 不匹配当前线程，返回 false。
 */
bool MemoryContextCheckThread(MemoryContext context)
{
    if (!context || !context->is_thread_owner) {
        return true;
    }
    return context->owner_thread_id == mmdb_current_thread_id();
}

/**
 * @brief 获取上下文 generation 计数器
 *
 * Generation 在 Reset 时由 allocset_reset 自动递增。
 * 可结合 MemoryAllocationHeader.generation 检测 use-after-reset。
 */
uint64_t MemoryContextGetGeneration(MemoryContext context)
{
    return context ? context->generation : 0;
}

/* ========================================================================
 * 资源析构 API
 * ======================================================================== */

/**
 * @brief 注册资源析构回调（LIFO 顺序执行）
 */
int mmdb_mem_register_resource(
    MemoryContext context,
    void *resource,
    void (*destructor)(void *resource, void *arg),
    void *arg,
    const char *name)
{
    if (!context || !resource || !destructor) {
        return -1;
    }

    /* 分配资源节点（使用当前上下文） */
    MemoryResource *res = (MemoryResource *)palloc(context, sizeof(MemoryResource));
    if (!res) {
        return -1;
    }

    res->resource = resource;
    res->destructor = destructor;
    res->arg = arg;
    res->name = name;

    /* 插入链表首部（LIFO 语义：后注册的在前） */
    res->next = context->resources;
    context->resources = res;
    context->resource_count++;

    return 0;
}

/**
 * @brief 取消注册资源析构回调
 */
int mmdb_mem_unregister_resource(MemoryContext context, void *resource)
{
    if (!context || !resource) {
        return -1;
    }

    MemoryResource **pp = &context->resources;
    while (*pp) {
        if ((*pp)->resource == resource) {
            MemoryResource *found = *pp;
            *pp = found->next;
            context->resource_count--;
            /* 注意：不释放 found 节点本身，由 Reset/Delete 统一回收 */
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;  /* 未找到 */
}
