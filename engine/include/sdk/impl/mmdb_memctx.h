/**
 * @file mmdb_memctx.h
 * @brief SDK 兼容层：内存上下文（Memory Context）公共 API
 *
 * 在 SDK 层以 `mmdb_mem_*` 前缀对外暴露 AllocSet 内存上下文能力，
 * 与 `sdk/impl/memctx.h` 中的核心 API 一一对应，并补充溢出检查、
 * 线程归属校验、资源析构注册等面向上层调用的便利封装。
 *
 * 类型设计：
 * - mmdb_memctx_t 直接 typedef 自核心 MemoryContext，本身即指针
 *   （MemoryContextData*），与核心 API 保持零拷贝映射。
 * - API 函数统一以 mmdb_memctx_t 按值传递上下文（与 palloc 风格一致），
 *   避免引入双重指针歧义。
 *
 * 设计原则：
 * 1. 兼容层零破坏 —— 不修改任何既有 `mmdb_*` 函数签名
 * 2. 入口参数校验 —— 空指针 / 0 字节 / 跨线程调用一律返回 NULL/-1
 * 3. 与核心 API 行为对齐 —— 所有行为通过核心 `MemoryContext` 实现
 * 4. 限额由 SDK 层兜底 —— 核心 AllocSet 不内置限额，由本层做预检
 */

#ifndef SDK_IMPL_MMDB_MEMCTX_H
#define SDK_IMPL_MMDB_MEMCTX_H

#include "sdk/impl/memctx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SDK 层内存上下文类型：直接复用核心 MemoryContext，避免 ABI 冗余 */
typedef MemoryContext mmdb_memctx_t;

/**
 * @brief 创建 SDK 层内存上下文
 *
 * @param parent     父上下文（NULL 表示根上下文）
 * @param name       上下文名称（调试用，不可为 NULL）
 * @param max_bytes  内存限额（0 表示无限制）
 *
 * @return 新创建的内存上下文；失败返回 NULL
 */
mmdb_memctx_t mmdb_memctx_create(mmdb_memctx_t parent,
                                 const char *name,
                                 size_t max_bytes);

/**
 * @brief 从上下文中分配内存（自动 8 字节对齐）
 *
 * @param ctx  内存上下文
 * @param size 请求字节数
 *
 * @return 分配的内存指针；失败返回 NULL
 */
void *mmdb_mem_alloc(mmdb_memctx_t ctx, size_t size);

/**
 * @brief 从上下文中分配零初始化内存
 *
 * @param ctx   内存上下文
 * @param count 元素数量
 * @param size  单个元素大小
 *
 * @return 分配的内存指针（已清零）；失败返回 NULL
 */
void *mmdb_mem_calloc(mmdb_memctx_t ctx, size_t count, size_t size);

/**
 * @brief 重新分配内存（alloc+memcpy+free 语义）
 *
 * AllocSet 是 arena 分配器，不支持原地 realloc；SDK 层通过分配-拷贝-
 * 释放模拟 realloc 语义，保证调用方代码可移植。
 *
 * @param ctx  内存上下文
 * @param ptr  原内存指针（NULL 等价于 alloc）
 * @param size 新请求字节数（0 等价于 free，返回 NULL）
 *
 * @return 新分配的内存指针；失败返回 NULL
 */
void *mmdb_mem_realloc(mmdb_memctx_t ctx, void *ptr, size_t size);

/**
 * @brief 在上下文中复制字符串
 *
 * @param ctx   内存上下文
 * @param value 待复制字符串（不可为 NULL）
 *
 * @return 新分配的字符串副本；失败返回 NULL
 */
char *mmdb_mem_strdup(mmdb_memctx_t ctx, const char *value);

/**
 * @brief 释放单个分配（AllocSet 实现下为空操作，由 reset/delete 统一回收）
 *
 * @param ctx 内存上下文
 * @param ptr 待释放指针（NULL 安全）
 */
void mmdb_mem_free(mmdb_memctx_t ctx, void *ptr);

/**
 * @brief 重置上下文（释放所有子上下文块，保留当前上下文主块）
 *
 * @param ctx 待重置的内存上下文（NULL 安全）
 */
void mmdb_memctx_reset(mmdb_memctx_t ctx);

/**
 * @brief 删除上下文（释放当前上下文所有块及子上下文）
 *
 * @param ctx 待删除的内存上下文（NULL 安全）
 */
void mmdb_memctx_delete(mmdb_memctx_t ctx);

/*
 * 资源析构 API（mmdb_mem_register_resource / mmdb_mem_unregister_resource）
 * 由 SDK 自持的内存上下文（sdk/impl/memctx.h）提供，本头文件已通过
 * #include "sdk/impl/memctx.h" 自动暴露其声明，SDK 层不重复包装，避免符号冲突。
 */

#ifdef __cplusplus
}
#endif

#endif /* SDK_IMPL_MMDB_MEMCTX_H */
