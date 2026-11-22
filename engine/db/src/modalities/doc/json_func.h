#ifndef DB_MODALITIES_DOC_JSON_FUNC_H
#define DB_MODALITIES_DOC_JSON_FUNC_H

/*
 * json_func.h —— 文档模态向通用值层注册的两个函数（v1.6 C5.7）
 *
 * ==================== 这个文件很小，是为了说清一件事 ====================
 *
 * 文档模态**只通过值层的通用函数表**和执行器相连。
 * 执行器不认识 `json_match` / `json_extract` 这两个名字 ——
 * 它只知道"EX_FUNC 要按名字查表、调用某个函数指针"。
 *
 * 这是 A7 审计清单里落点 ① 与 ② 的分界：
 *   ① 值层：提供**通用**函数表（机制）
 *   ② 求值入口：一次查表 + 调用（不认识具体名字）
 *   而"这两个名字对应什么实现"由本模态在 `db_json_init` 里注册。
 *
 * ==================== 顺带把 JSONB 渲染器也注册了 ====================
 *
 * `val_to_str` 遇到 T_JSON 时需要一个"JSONB → JSON 文本"的实现。
 * 值层（L1）不能 include 文档模态（L4），所以它只持一个函数指针，
 * 由这里把 `jsonb_render` 注册进去（见 storage/value.h 的 JsonRenderFn）。
 */

#include "common/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 注册文档模态提供给 SQL 层的函数。
 *
 * 注册两个标量函数 + 一个 JSONB 渲染器：
 *   json_extract(doc, '$.a.b')   → 取路径上的标量
 *   json_match(doc, '$.a = 5')   → 规格求值，返回 1/0
 *   jsonb_render                 → 值层输出 T_JSON 时用
 *
 * ⚠️ **必须在任何查询开始之前调用**（值层函数表是进程级只读全局）。
 *    调用点在 `db_open()` 里。
 *
 * ⚠️ **重复调用是安全的**：同名注册返回 E_INVAL，本函数把
 *    E_INVAL 视为"已注册"并继续 —— 多连接场景每个连接都会走一次
 *    db_open，不能因此失败。
 *
 * ⚠️ **调用点必须被 `#if DB_HAS_DOC` 守卫**（`db.c` 的 `db_open()`）。
 *    计划原文（C5.14）要求"无条件调用"，前提是"`DB_HAS_DOC` 还不存在"。
 *    实测执行到 C5 时 W-B P1 已落地 —— `tools/mm_profiles.mk` 为每个模态
 *    都定义了宏（开=1、关=0），而 `MM_FILES_doc` 会把
 *    `src/modalities/doc/` 目录下所有 .c 整个裁掉 ⇒ 无条件调用就是链接失败：
 *
 *        build_mm_relational/db.o: undefined reference to `db_json_init'
 *
 *    所以守卫是必须的（九个 profile 全编过才算完）。
 */
int db_json_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_DOC_JSON_FUNC_H */
