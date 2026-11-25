# JSON 列长文档往返实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 256–2048 字节的 JSON 文档能通过 SQL 字面量完整写入 JSON 列并原样读回，同时消除长字符串路径上的三处静默截断。

**Architecture:** 词法器改为分层存储（短字面量留在 token 内，长字面量单独分配并超限报错），解析器接管长字面量的所有权。值层新增两个通用入口：分配式 JSON 编码回调（由 doc 模态实现，模态自己裁决 2048B 上限）与分配式 JSON 渲染（`val_to_json_alloc`）。执行层在写 heap 之前完成编码与长度校验，任何超限都在写盘前失败。

**Tech Stack:** C11、MinGW-w64（`mingw32-make`）、Git Bash（`sh test/run.sh`）、JSONB 自有二进制格式。

## Global Constraints

- 工作目录：`C:\code\projects\engine\db`；构建：`mingw32-make all`；测试：`sh test/run.sh`（Git Bash，PATH 需含 `C:\Program Files\Git\usr\bin` 与 `C:\mingw64\bin`）。
- 编译门禁 `-Wall -Wextra -Werror`：**零诊断**是硬要求。
- 单文件不超过 800 行（`value.c` 当前 786 行 / 非空 713，`exec_write.c` 740 / 非空 649，`parser.c` 651 / 非空 585）。新增逻辑放新文件或精简原文件，不许把 `value.c` 推过 800。
- 分层门禁 `tools/check_deps.sh`：L1 `storage/` 不得 include `modalities/`；L3 `query/` 不得 include `modalities/`。
- A7 门禁 `tools/check_a7.sh`：查询层保持通用，不得出现具体 JSON 分派。
- 注释与提交信息用中文；错误码用 `E_*`。
- 临时产物只放 `test/tmp/`，任务结束前清理；不要提交 `test/tmp/` 下的任何文件。
- 每个任务结束提交一次，提交后立即 `git push origin main`。

## 执行状态（2026-11-25 回填）

> 勾选框按**代码与测试的实际落地情况**回填，不是按计划写作顺序。

| 门禁 | 结果 |
|---|---|
| `mingw32-make clean && mingw32-make all` | **零诊断** |
| `sh test/run.sh` | **120 个测试：120 通过** |
| `sh tools/check_deps.sh` / `check_a7.sh` | 均 ✓ |
| 九个 profile + 八个 smoke | 全部 `rc=0` |

Task 1–6 的全部步骤已落地（对应提交 `5a7e2bbc0`、`cb9bde747`、`c4d3310b0`、
`0e01f9968`、`0a15a24bb`，以及 `0d4471143` 的文档同步）。Task 7 的 Step 1–5
（全量门禁、九个 profile、`MODALITIES.md` M7-35/36/37 三条坑、注释措辞、
C5.3b-a 计划段落）也已完成。

**仍未做**：Step 6 的临时产物清理与提交、Step 7 的父仓指针更新 —— 这两步是
"提交动作"，本轮不代用户执行。全量门禁在本轮重新跑过（见上表）。

---

### Task 1: 词法器 —— 字符串字面量不再静默截断

**Files:**
- Modify: `src/query/lexer.h:7`（新增 `TK_STR_MAX`）、`src/query/lexer.h:18-39`（`Token` 增加 `str_value`）
- Modify: `src/query/lexer.c:147-177`（字符串分支改为两趟扫描 + 分层存储）
- Test: `test/unit/test_lexer_string.c`（新增）

**Interfaces:**
- Consumes: 无（起点）
- Produces:
  - `#define TK_STR_MAX 8192`
  - `Token.str_value`（`char *`）：`TK_STR` 且长度 ≥ `TK_TEXT_MAX` 时有效，非 NULL 时由**词法器分配、解析器释放**；其余情况为 NULL。
  - `Token.len`（`u16`）：字符串字面量的**精确**解码后长度（`''` 按一个字节计）。
  - 词法错误：长度 > `TK_STR_MAX` ⇒ 返回 `E_INVAL`，`tok->text` 写人类可读原因（含上限数字），`tok->type = TK_EOF`。

- [x] **Step 1: 写失败的测试**

创建 `test/unit/test_lexer_string.c`：

```c
/*
 * test_lexer_string.c —— SQL 字符串字面量：精确长度，不许静默截断
 *
 * 【这个测试在钉什么】
 * 词法器原来用 Token.text[256]，写满就停止拷贝但继续扫完输入 ——
 * 等于"读完了却只存了一半"，用户拿到的是被改过的值，而且**没有报错**。
 *
 * 边界（254 / 255 / 256 / 257）必须逐个断言精确长度；
 * 超上限（8193）必须报错且错误文本里带上限。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query/lexer.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 造一个 SQL：SELECT '<text>'，返回字面量 token */
static int scan_literal(const char *text, size_t len, Token *out)
{
    char *sql = malloc(len + 16);
    if (!sql)
        return E_IO;
    memcpy(sql, "SELECT '", 7);
    memcpy(sql + 7, text, len);
    memcpy(sql + 7 + len, "'", 2);

    Lexer lx;
    Token t;
    lex_init(&lx, sql);
    lex_next(&lx, &t);            /* SELECT */
    int rc = lex_next(&lx, &t);   /* 字面量 */
    if (rc == OK)
        *out = t;
    else if (t.str_value)
        free(t.str_value);
    free(sql);
    return rc;
}

int main(void)
{
    printf("test_lexer_string:\n");

    /* ---------------- 1. 边界：长度必须精确 ---------------- */
    {
        size_t sizes[] = { 1, 254, 255, 256, 257, 1000, 8192 };
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            size_t n = sizes[i];
            char *buf = malloc(n + 1);
            if (!buf)
                return 1;
            memset(buf, 'x', n);
            buf[n] = '\0';

            Token t;
            memset(&t, 0, sizeof(t));
            int rc = scan_literal(buf, n, &t);
            CHECK(rc == OK);
            CHECK(t.len == n);
            CHECK((t.len == n) && (memcmp(t.str_value ? t.str_value : t.text,
                                         buf, n) == 0));
            if (t.str_value)
                free(t.str_value);
            free(buf);
        }
    }

    /* ---------------- 2. 超上限：明确报错，不截断 ---------------- */
    {
        size_t n = TK_STR_MAX + 1;
        char *buf = malloc(n + 1);
        if (!buf)
            return 1;
        memset(buf, 'x', n);
        buf[n] = '\0';

        Token t;
        memset(&t, 0, sizeof(t));
        int rc = scan_literal(buf, n, &t);
        CHECK(rc != OK);
        CHECK(strstr(t.text, "8192") != NULL);   /* 错误文本要含上限数字 */
        if (t.str_value)
            free(t.str_value);
        free(buf);
    }

    /* ---------------- 3. '' 转义按一个字节解码 ---------------- */
    {
        Token t;
        memset(&t, 0, sizeof(t));
        /* 'a''b' ⇒ 内容 a'b（3 字节） */
        int rc = scan_literal("a''b", 4, &t);
        CHECK(rc == OK);
        CHECK(t.len == 3);
        CHECK(memcmp(t.str_value ? t.str_value : t.text, "a'b", 3) == 0);
        if (t.str_value)
            free(t.str_value);
    }

    if (failures == 0) {
        printf("  词法器字符串字面量验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
```

- [x] **Step 2: 运行测试确认失败**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_lexer_string|个测试：'
```

预期：`test_lexer_string` 编译通过但运行失败（长度断言红），其余 113 个测试仍通过。

- [x] **Step 3: 实现 `Token.str_value` 与分层存储**

修改 `src/query/lexer.h`：

```c
#define TK_TEXT_MAX 256
#define TK_STR_MAX  8192
```

在 `Token` 结构体里 `len` 之后加：

```c
    /*
     * TK_STR 长字面量的独立存储。
     *
     * 【为什么不能一直用 text[]】
     * text 只有 256 字节。原来的实现"写满就停拷、但继续扫完输入" ——
     * 读完了却只存了一半，用户拿到被改过的值且**没有报错**。
     *
     * 契约：非 NULL 时这块内存归**调用方**（解析器）释放；
     * 词法器在下一次 lex_next 之前不会改它。
     */
    char   *str_value;
```

修改 `src/query/lexer.c` 的字符串分支，替换原来"单趟 + 边扫边拷"的实现：

```c
    /* 字符串：'...'，'' 表示一个单引号 */
    if (c == '\'') {
        /*
         * 两趟：先量长度，再拷贝。
         *
         * 【为什么必须两趟】
         * 单趟时无法事先知道"要不要单独分配" —— 而分配与否取决于长度。
         * 先量长度才知道该写进 text[] 还是 malloc 一块。
         */
        const char *p = s + lx->pos + 1;
        size_t need = 0;

        for (;;) {
            char ch = *p;
            if (ch == '\0') {
                tok->type = TK_EOF;
                snprintf(tok->text, sizeof(tok->text), "字符串没有闭合");
                tok->len = (u16)strlen(tok->text);
                return E_INVAL;
            }
            if (ch == '\'') {
                if (p[1] == '\'') {      /* 转义：'' 算一个字节 */
                    need++;
                    p += 2;
                    continue;
                }
                p++;
                break;
            }
            need++;
            p++;
        }

        if (need > (size_t)TK_STR_MAX) {
            tok->type = TK_EOF;
            snprintf(tok->text, sizeof(tok->text),
                     "字符串常量太长（上限 %d 字节）", TK_STR_MAX);
            tok->len = (u16)strlen(tok->text);
            return E_INVAL;
        }

        char *dst;
        if (need < (size_t)TK_TEXT_MAX) {
            dst = tok->text;
        } else {
            dst = malloc(need + 1u);
            if (!dst)
                return E_IO;
            tok->str_value = dst;
        }

        /* 第二趟：真正拷贝（'' → '） */
        size_t n = 0;
        lx->pos++;
        for (;;) {
            char ch = s[lx->pos];
            if (ch == '\'') {
                if (s[lx->pos + 1] == '\'') {
                    dst[n++] = '\'';
                    lx->pos += 2;
                    continue;
                }
                lx->pos++;
                break;
            }
            dst[n++] = ch;
            lx->pos++;
        }
        dst[n] = '\0';

        tok->len = (u16)n;
        tok->type = TK_STR;
        return OK;
    }
```

- [x] **Step 4: 运行测试确认通过**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_lexer_string|个测试：'
```

预期：`test_lexer_string` PASS，总数 115 个测试全通过。

- [x] **Step 5: 提交**

```bash
cd C:/code/projects
git add engine/db/src/query/lexer.c engine/db/src/query/lexer.h engine/db/test/unit/test_lexer_string.c
git commit -m "fix(query): 字符串字面量不再静默截断，长字面量独立存储

词法器原来用 Token.text[256]，写满即停止拷贝却继续扫完输入 ——
读完了却只存了一半，用户拿到被改过的值且没有报错。

- Token 增加 str_value：长度 <256 仍用 text[]，否则单独分配
- 上限 TK_STR_MAX=8192，超限返回 E_INVAL 并在错误文本里写明上限
- 改成两趟扫描：先量精确长度（含 '' 转义），再拷贝
- 新增 test_lexer_string：边界 254/255/256/257/8192 与超限报错"
git push origin main
```

---

### Task 2: 解析器 —— 长字面量的所有权移交与释放

**Files:**
- Modify: `src/query/parser.h:73-108`（`Expr` 增加 `text_owned` / `text_len`）
- Modify: `src/query/parser.c:34-47`（`p_advance` 释放旧 token 缓冲）、`src/query/parser.c:419-437`（字面量接管）、`src/query/parser.c` 的 `free_expr` 与 `parse_sql` 收尾
- Test: `test/unit/test_lexer_string.c`（追加第 4 组）

**Interfaces:**
- Consumes: Task 1 的 `Token.str_value`、`Token.len`
- Produces:
  - `Expr.text_owned`（`char *`）：长字面量的堆缓冲，所有权归这个 `Expr`，`free_expr()` 释放；短字面量为 NULL。
  - `Expr.text_len`（`u16`）：长字面量长度。
  - `Expr.lit`：长字面量时是**外部 `T_TEXT`**（`val_external_text` + `val_mem_detoast`），`len` 字段不设（协议是长度靠 detoast 问）。

- [x] **Step 1: 追加失败的测试**

在 `test/unit/test_lexer_string.c` 的 `main()` 末尾（`if (failures == 0)` 之前）插入：

```c
    /* ---------------- 4. 解析器接管长字面量 ---------------- */
    {
        char *sql = malloc(TK_STR_MAX + 16);
        if (!sql)
            return 1;
        size_t n = 300;
        memcpy(sql, "SELECT '", 7);
        memset(sql + 7, 'y', n);
        memcpy(sql + 7 + n, "'", 2);

        char err[128];
        Stmt st;
        memset(&st, 0, sizeof(st));
        int rc = parse_sql(sql, &st, err, (int)sizeof(err));
        CHECK(rc == OK);
        CHECK(st.ntargets == 1);
        CHECK(st.targets[0] != NULL);
        CHECK(st.targets[0]->text_owned != NULL);
        CHECK(st.targets[0]->text_len == n);
        CHECK(st.targets[0]->lit.type == T_TEXT);
        CHECK(st.targets[0]->lit.external);

        /* 物化后长度必须是精确的 300 */
        u8 *bytes = NULL;
        u32 blen = 0;
        CHECK(val_detoast(&st.targets[0]->lit, &bytes, &blen) == OK);
        CHECK(blen == n);
        free(bytes);

        free_stmt(&st);          /* 释放后不得再访问 text_owned */
        free(sql);
    }
```

并在文件顶部的 include 区加：

```c
#include "query/parser.h"
#include "storage/value.h"
```

- [x] **Step 2: 运行测试确认失败**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_lexer_string|个测试：'
```

预期：编译失败（`Expr` 没有 `text_owned` 字段）。

- [x] **Step 3: 实现所有权移交**

`src/query/parser.h` 的 `Expr` 里，在 `Expr *r;` 之后加：

```c
    /*
     * EX_LIT 里**超长字符串字面量**的独立存储。
     *
     * 【为什么需要它】
     * DbValue.s[] 只有 256 字节。JSON 文档、长路径这类字面量常常超过它，
     * 而截断是**静默改值** —— 用户会拿到一个语法不完整的文档。
     *
     * 处理方式：非 NULL 时 lit 是一个外部值（ext_ctx 指向这块堆内存），
     * 长度是精确值。所有权归这个 Expr，free_expr() 负责释放。
     */
    char    *text_owned;
    u16      text_len;
```

`src/query/parser.c` 的 `p_advance` 开头加释放：

```c
void p_advance(Parser *p)
{
    if (p->failed)
        return;
    /*
     * 释放上一个 token 的长字面量缓冲。
     * 词法器不会自己回收 —— 它不知道上一个 token 还在不在用。
     */
    free(p->cur.str_value);
    p->cur.str_value = NULL;
    if (lex_next(&p->lx, &p->cur) != OK) {
```

`src/query/parser.c` 的字面量分支：

```c
    if (p->cur.type == TK_STR) {
        e = p_expr_new(EX_LIT);
        if (!e) { p_fail(p, "内存不足"); return NULL; }
        if (p->cur.str_value) {
            /*
             * 长字面量：直接接管词法器的分配，不复制第二份。
             * 用外部 T_TEXT 表示，len 精确保留。
             */
            e->text_owned = p->cur.str_value;
            e->text_len = p->cur.len;
            val_external_text(&e->lit, (u64)(uintptr_t)e->text_owned,
                              e->text_len, val_mem_detoast, e->text_owned);
            p->cur.str_value = NULL;    /* 转移所有权，避免双重释放 */
        } else {
            val_text(&e->lit, p->cur.text, p->cur.len);
        }
        p_advance(p);
        return e;
    }
```

`src/query/parser.c` 的 `free_expr` 里加：

```c
    free(e->args);
    free(e->text_owned);
```

`src/query/parser.c` 的 `parse_sql` 收尾（错误路径与成功路径各一处）：

```c
    if (rc == E_PARSE_DUMMY || p.failed) {
        free(p.cur.str_value);
        p.cur.str_value = NULL;
        free_stmt(out);
        return E_INVAL;
    }
```

```c
    if (p.cur.type != TK_EOF) {
        p_fail(&p, "语句结尾有多余内容");
        free(p.cur.str_value);
        p.cur.str_value = NULL;
        free_stmt(out);
        return E_INVAL;
    }
    free(p.cur.str_value);
    p.cur.str_value = NULL;
    return OK;
```

- [x] **Step 4: 运行测试确认通过**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_lexer_string|个测试：'
```

预期：`test_lexer_string` PASS，115 个测试全通过。

- [x] **Step 5: 提交**

```bash
cd C:/code/projects
git add engine/db/src/query/parser.c engine/db/src/query/parser.h engine/db/test/unit/test_lexer_string.c
git commit -m "fix(query): 解析器接管长字面量所有权，free_expr 负责释放

词法器分配的 str_value 必须有人回收，否则每次解析长字面量都泄漏一块堆。

- Expr 增加 text_owned / text_len，长字面量以外部 T_TEXT 表示
- p_advance 推进前释放上一个 token 的缓冲
- parse_sql 在失败与成功两条路径都回收当前 token 的缓冲
- 接管时把 p->cur.str_value 置 NULL，避免双重释放"
git push origin main
```

---

### Task 3: 值层 —— 外部 T_TEXT 的记录编码修复 + 分配式 JSON 编码/渲染

**Files:**
- Modify: `src/storage/value.h`（新增 `JsonEncodeFn`、`val_set_json_encoder`、`val_json_from_text`、`val_json_free`、`val_to_json_alloc`）
- Modify: `src/storage/value_json.c`（编码器注册表）
- Modify: `src/storage/value.c:648-720`（`val_to_record` 的 `T_TEXT` 分支物化外部值）
- Test: `test/unit/test_value_json.c`（追加第 7 组）、`test/unit/test_text_len.c`（新增）

**Interfaces:**
- Consumes: Task 2 的外部 `T_TEXT` 字面量
- Produces:
  - `typedef int (*JsonEncodeFn)(const char *text, u32 text_len, u8 **bytes_out, u32 *len_out);`
  - `int val_set_json_encoder(JsonEncodeFn fn);` —— 注册一次，重复注册返回 `E_INVAL`。
  - `int val_json_from_text(const DbValue *text, DbValue *value);` —— 把 `T_TEXT` 值编码成 `T_JSON` 值；内部先物化外部值。长度 ≤ `VAL_INLINE_MAX` ⇒ 内联；否则 `external = true`、`ext_ctx` 指向编码器分配的缓冲、`ext_fn = val_mem_detoast`。未注册编码器 ⇒ `E_NOTSUP`。
  - `void val_json_free(DbValue *value);` —— 释放 `val_json_from_text` 产出的外部缓冲（非外部值时是空操作）。
  - `int val_to_json_alloc(const DbValue *v, char **out, u32 *len);` —— 渲染成新分配的 C 字符串（长度由渲染器先测后写），调用方 `free`；未注册渲染器 ⇒ `E_NOTSUP`。
  - `val_to_record()` 现在会物化**外部 `T_TEXT`**（此前写进记录的是空串）。

- [x] **Step 1: 写失败的测试（记录编码修复）**

创建 `test/unit/test_text_len.c`：

```c
/*
 * test_text_len.c —— 外部 T_TEXT 进记录：不能写成空串
 *
 * 【这个测试在钉什么】
 * val_to_record() 的 T_TEXT 分支原来直接取 vals[i].s / vals[i].len，
 * 而外部 T_TEXT 的 len 恒为 0、s[] 里没有内容 ——
 * 于是 300 字节的外部文本写进记录后只剩 23 字节（只有头），**静默丢数据**。
 *
 * 长 SQL 字面量放宽之后，这条路径一定会被踩到，必须先修。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/record.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

int main(void)
{
    printf("test_text_len:\n");

    char payload[512];
    memset(payload, 'z', 300);
    payload[300] = '\0';

    ColDesc col;
    memset(&col, 0, sizeof(col));
    snprintf(col.name, sizeof(col.name), "txt");
    col.type = T_TEXT;
    col.nullable = true;

    /* 外部 T_TEXT：长度靠 detoast 问 */
    DbValue v;
    val_external_text(&v, (u64)(uintptr_t)payload, (u64)strlen(payload),
                      val_mem_detoast, payload);

    u8 rec[4096];
    u16 rlen = 0;
    int rc = val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen);
    CHECK(rc == OK);
    /* 记录 = 头 + 300 字节；此前是 23（内容全丢） */
    CHECK(rlen > 300);

    DbValue back;
    int drc = val_from_record(&col, 1, rec, rlen, &back);
    CHECK(drc == OK);
    CHECK(back.len == 300);

    if (failures == 0) {
        printf("  外部 T_TEXT 记录编码验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
```

- [x] **Step 2: 运行测试确认失败**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_text_len|个测试：'
```

预期：`test_text_len` 运行失败（`rlen > 300` 与 `back.len == 300` 断言红）。

- [x] **Step 3: 实现 `val_to_record` 的 `T_TEXT` 物化**

`src/storage/value.c` 的 `val_to_record()` 里，把 `case T_TEXT:` 改成：

```c
        case T_TEXT:
            /*
             * 外部 T_TEXT 必须先物化 —— 否则写进记录的是**空串**。
             *
             * 【为什么现在才暴露】
             * 外部 T_TEXT 的 len 恒为 0（协议是"长度靠 val_detoast 问"），
             * s[] 里也没有内容。以前 SQL 字面量最长 255B，这条分支碰不到；
             * 词法器放宽到 8192B 之后，长字面量一律是外部值 ⇒ 必须物化。
             *
             * 与 T_JSON 分支同一套记账（ext_bufs / ext_owned）。
             */
            if (vals[i].external) {
                u8 *bytes = NULL;
                u32 blen = 0;
                rc = val_detoast(&vals[i], &bytes, &blen);
                if (rc != OK) {
                    for (int k = 0; k < i; k++) {
                        if (ext_owned[k])
                            free(ext_bufs[k]);
                    }
                    return rc;
                }
                ext_bufs[i] = bytes;
                ext_owned[i] = true;
                tmp[i].u.text.p = bytes;
                tmp[i].u.text.n = (u16)blen;
            } else {
                tmp[i].u.text.p = (const u8 *)vals[i].s;
                tmp[i].u.text.n = vals[i].len;
            }
            break;
```

- [x] **Step 4: 运行测试确认通过**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_text_len|个测试：'
```

预期：`test_text_len` PASS，116 个测试全通过。

- [x] **Step 5: 实现分配式 JSON 编码与渲染接口**

`src/storage/value.h`，在 `JsonRenderFn` 相关声明之后加：

```c
/*
 * JSON 文本编码器（SQL T_TEXT → T_JSON 写入）。
 *
 * 【为什么是分配式】
 * "编码后多大算合规"是**文档模态**的知识（JSON_INLINE_MAX = 2048，
 * 见 modalities/doc/json_ovf.h）。若让调用方给固定缓冲，
 * 2048 就被写死在 query 层，违反 A7 审计。
 *
 * 契约：
 *   - 实现自己分配 *bytes_out（调用方用 val_json_free 释放）
 *   - *len_out 是编码后的字节数
 *   - 超过模态上限 ⇒ 返回 E_NOSPACE 并释放自己分配的任何缓冲
 */
typedef int (*JsonEncodeFn)(const char *text, u32 text_len,
                            u8 **bytes_out, u32 *len_out);

/* 注册编码器（只接受第一次；重复注册返回 E_INVAL） */
int  val_set_json_encoder(JsonEncodeFn fn);

/* 把 T_TEXT 值（内联或外部）编码成 T_JSON 值 */
int  val_json_from_text(const DbValue *text, DbValue *value);

/* 释放 val_json_from_text 产出的外部缓冲（非外部值时是空操作） */
void val_json_free(DbValue *value);

/* 渲染成新分配的 C 字符串（调用方 free）；未注册渲染器 ⇒ E_NOTSUP */
int  val_to_json_alloc(const DbValue *v, char **out, u32 *len);
```

`src/storage/value_json.c` 追加：

```c
/* JSON 文本编码器：doc 模态注册的实现（值层不依赖 doc 模态） */
static JsonEncodeFn g_json_encoder = NULL;

int val_set_json_encoder(JsonEncodeFn fn)
{
    if (!fn)
        return E_INVAL;
    if (g_json_encoder)
        return E_INVAL;     /* 重复注册：明确报错，不静默覆盖 */
    g_json_encoder = fn;
    return OK;
}

int val_json_from_text(const DbValue *text, DbValue *value)
{
    u8 *text_bytes = NULL;
    u32 text_len = 0;
    u8 *out_bytes = NULL;
    u32 out_len = 0;
    int rc;

    if (!text || !value)
        return E_INVAL;
    if (text->is_null || text->type != T_TEXT)
        return E_INVAL;
    if (!g_json_encoder)
        return E_NOTSUP;

    /*
     * 长 SQL 字面量是外部值（len 不设），必须先物化成连续字节。
     *
     * 【为什么要拆成两个缓冲】
     * 不能把 text_bytes 直接当作编码器的输出缓冲 ——
     * 输入与输出别名时，编码器写输出的那一刻输入已经被覆盖了。
     */
    if (text->external) {
        rc = val_detoast(text, &text_bytes, &text_len);
        if (rc != OK)
            return rc;
    } else {
        text_bytes = (u8 *)text->s;
        text_len = text->len;
    }

    rc = g_json_encoder((const char *)text_bytes, text_len,
                        &out_bytes, &out_len);
    if (text->external)
        free(text_bytes);
    if (rc != OK)
        return rc;

    val_null(value);
    value->is_null = false;
    if (out_len <= VAL_INLINE_MAX) {
        value->type = T_JSON;
        value->len = (u16)out_len;
        if (out_len)
            memcpy(value->s, out_bytes, out_len);
        free(out_bytes);
    } else {
        val_external_json(value, (u64)(uintptr_t)out_bytes, out_len,
                          val_mem_detoast, out_bytes);
    }
    return OK;
}

void val_json_free(DbValue *value)
{
    if (!value)
        return;
    if (value->type == T_JSON && value->external && value->ext_ctx) {
        free(value->ext_ctx);
        value->ext_ctx = NULL;
        value->ext_fn = NULL;
        value->external = false;
    }
}
```

在 `src/storage/value.c` 追加 `val_to_json_alloc`：

```c
/*
 * 渲染成新分配的 C 字符串。
 *
 * 【为什么要单独一个函数】
 * val_to_str() 有 cap 上限（REPL / 日志等"短输出"场景用它）。
 * 结果集投影必须给完整文本 —— 512 字节的栈缓冲会截断长文档。
 *
 * 两次调用渲染器：先问长度，再写内容（jsonb_render 的契约）。
 */
int val_to_json_alloc(const DbValue *v, char **out, u32 *len)
{
    u8 *bytes = NULL;
    u32 blen = 0;
    u32 need = 0;
    u32 got = 0;
    int rc;
    JsonRenderFn render = val_json_render();

    if (!v || !out)
        return E_INVAL;
    *out = NULL;
    if (len)
        *len = 0;
    if (!render)
        return E_NOTSUP;

    if (v->external) {
        rc = val_detoast(v, &bytes, &blen);
        if (rc != OK)
            return rc;
    } else {
        bytes = (u8 *)v->s;
        blen = v->len;
    }

    rc = render(bytes, blen, NULL, 0, &need);
    if (rc != E_NOSPACE && rc != OK) {
        if (v->external)
            free(bytes);
        return rc;
    }
    if (need == 0) {
        if (v->external)
            free(bytes);
        return E_CORRUPT;
    }

    char *buf = malloc((size_t)need + 1u);
    if (!buf) {
        if (v->external)
            free(bytes);
        return E_IO;
    }

    rc = render(bytes, blen, (u8 *)buf, need, &got);
    if (v->external)
        free(bytes);
    if (rc != OK || got != need) {
        free(buf);
        return rc == OK ? E_CORRUPT : rc;
    }
    buf[got] = '\0';
    *out = buf;
    if (len)
        *len = got;
    return OK;
}
```

- [x] **Step 6: 追加值层测试并运行**

在 `test/unit/test_value_json.c` 追加第 7 组（编码器未注册时的行为）：

```c
    /* ---------------- 7. 编码器未注册 ⇒ E_NOTSUP ---------------- */
    {
        DbValue text;
        DbValue out;

        val_text(&text, "{\"a\":1}", 7);
        /* 本测试不注册编码器（全局只注册一次）⇒ 必须 E_NOTSUP */
        CHECK(val_json_from_text(&text, &out) == E_NOTSUP);

        /* 非 T_TEXT 输入 ⇒ E_INVAL */
        DbValue num;
        val_int(&num, 5);
        CHECK(val_json_from_text(&num, &out) == E_INVAL);

        /* NULL 输入 ⇒ E_INVAL */
        DbValue nul;
        val_null(&nul);
        CHECK(val_json_from_text(&nul, &out) == E_INVAL);
    }
```

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_value_json|test_text_len|个测试：'
```

预期：两个都 PASS，116 个测试全通过。

- [x] **Step 7: 提交**

```bash
cd C:/code/projects
git add engine/db/src/storage/value.c engine/db/src/storage/value.h engine/db/src/storage/value_json.c engine/db/test/unit/test_value_json.c engine/db/test/unit/test_text_len.c
git commit -m "fix(storage): 外部 T_TEXT 进记录不再写成空串，并加分配式 JSON 编解码入口

- val_to_record 的 T_TEXT 分支按 T_JSON 同一套逻辑物化外部值
  （此前 len 恒为 0 ⇒ 300 字节文本写进记录只剩 23 字节，静默丢数据）
- 新增 JsonEncodeFn / val_set_json_encoder / val_json_from_text / val_json_free
  编码上限由 doc 模态自己裁决（JSON_INLINE_MAX），query 层不写死 2048
- 新增 val_to_json_alloc：先测长度再写，结果集不再受 512 字节栈缓冲限制
- 新增 test_text_len 钉住外部 T_TEXT 的记录编码"
git push origin main
```

---

### Task 4: doc 模态 —— 注册分配式编码器

**Files:**
- Modify: `src/modalities/doc/json_func.c`（新增 `fn_json_encode_alloc` 并注册）
- Modify: `src/modalities/doc/json_ovf.h`（`JSON_INLINE_MAX` 已存在，无需改；仅在注释里补一句"编码器按此裁决"）
- Test: `test/unit/test_json_func.c`（追加第 7 组）

**Interfaces:**
- Consumes: Task 3 的 `JsonEncodeFn`
- Produces: `db_json_init()` 注册 `fn_json_encode_alloc`，使 `val_json_from_text` 可用。

- [x] **Step 1: 追加失败的测试**

在 `test/unit/test_json_func.c` 追加：

```c
    /* ---------------- 7. 分配式编码器 ---------------- */
    {
        DbValue text;
        DbValue out;

        val_text(&text, "{\"a\":5}", 7);
        CHECK(val_json_from_text(&text, &out) == OK);
        CHECK(out.type == T_JSON);
        CHECK(out.len == 7);

        /* 渲染回文本应当还原 */
        char *rendered = NULL;
        u32 rlen = 0;
        CHECK(val_to_json_alloc(&out, &rendered, &rlen) == OK);
        CHECK(rendered != NULL);
        CHECK(strcmp(rendered, "{\"a\":5}") == 0);
        free(rendered);

        /* 非法 JSON ⇒ 报错 */
        DbValue bad;
        val_text(&bad, "{\"a\":}", 6);
        DbValue out2;
        CHECK(val_json_from_text(&bad, &out2) != OK);
    }
```

（`test_json_func.c` 已有 `db_json_init()` 的调用；确认文件顶部 include 了 `storage/value.h`。）

- [x] **Step 2: 运行测试确认失败**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_json_func|个测试：'
```

预期：运行失败（`val_json_from_text` 返回 `E_NOTSUP`，因为还没注册编码器）。

- [x] **Step 3: 实现 `fn_json_encode_alloc`**

在 `src/modalities/doc/json_func.c` 里，`fn_json_extract` 之前加：

```c
/*
 * 分配式 JSON 编码器（值层注册表里的一项）。
 *
 * 【为什么自己分配】
 * "编码后多大算合规"是文档模态的知识：JSON_INLINE_MAX（2048，见 json_ovf.h）。
 * 值层只提供机制，不认识这个数字 ⇒ 编码器自己 malloc、自己判上限。
 *
 * 【为什么超过上限就报错】
 * >2048 字节的文档需要 PT_JSON_OVF 页链，而页链的 tuple 所有权、
 * abort/WAL 恢复、VACUUM 回收尚未设计（C5.3b-b）。
 * 现在放行只会造出没有主的页 ⇒ 明确拒绝。
 */
static int fn_json_encode_alloc(const char *text, u32 text_len,
                                u8 **bytes_out, u32 *len_out)
{
    u32 need = 0;
    u8 *buf = NULL;
    int rc;

    if (!text || !bytes_out || !len_out)
        return E_INVAL;
    *bytes_out = NULL;
    *len_out = 0;

    /* 第一趟：量长度（json_parse_to_jsonb 在 out == NULL 时只算长度） */
    rc = json_parse_to_jsonb(text, text_len, NULL, 0, &need);
    if (rc != OK)
        return rc;

    if (need > JSON_INLINE_MAX)
        return E_NOSPACE;

    buf = malloc(need ? need : 1u);
    if (!buf)
        return E_IO;

    rc = json_parse_to_jsonb(text, text_len, buf, need, len_out);
    if (rc != OK) {
        free(buf);
        *len_out = 0;
        return rc;
    }
    *bytes_out = buf;
    return OK;
}
```

在 `db_json_init()` 里注册（与 `val_set_json_renderer` 并列）：

```c
    rc = val_set_json_encoder(fn_json_encode_alloc);
    if (rc != OK && rc != E_INVAL)
        worst = rc;
```

文件顶部 include 补 `modalities/doc/json_ovf.h`（取 `JSON_INLINE_MAX`）。

- [x] **Step 4: 运行测试确认通过**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_json_func|个测试：'
```

预期：`test_json_func` PASS，116 个测试全通过。

- [x] **Step 5: 提交**

```bash
cd C:/code/projects
git add engine/db/src/modalities/doc/json_func.c engine/db/test/unit/test_json_func.c
git commit -m "feat(doc): 注册分配式 JSON 编码器，上限由文档模态裁决

- 新增 fn_json_encode_alloc：自己 malloc、自己按 JSON_INLINE_MAX 判上限
- 超过 2048 字节返回 E_NOSPACE（页链生命周期属于 C5.3b-b，尚未设计）
- db_json_init 注册 val_set_json_encoder
- 补 test_json_func 的分配式编解码往返断言"
git push origin main
```

---

### Task 5: 执行层 —— JSON/TEXT 列的长度校验与编码中转

**Files:**
- Modify: `src/query/exec_write.c`（`coerce_to_col` 的 `T_TEXT` 分支、INSERT 的 JSON 分支、UPDATE 的 JSON 分支）
- Test: `test/unit/test_json_column.c`（扩充）、`test/unit/test_text_len.c`（追加 SQL 层用例）

**Interfaces:**
- Consumes: Task 3 的 `val_json_from_text` / `val_json_free`、Task 4 的注册
- Produces: 无新公开接口（行为变化）。

- [x] **Step 1: 追加失败的测试**

在 `test/unit/test_text_len.c` 的 `main()` 末尾追加 SQL 层用例：

```c
    /* ---------------- SQL 层：TEXT 列超 255B 必须报错 ---------------- */
    {
        DB *db = NULL;
        const char *path = "test/tmp/text_len.db";
        remove(path);
        remove("test/tmp/text_len.db-wal");
        remove("test/tmp/text_len.db-clog");

        CHECK(db_open(path, &db) == DB_OK);
        if (!db)
            return 1;

        Result *r = NULL;
        CHECK(db_exec(db, "CREATE TABLE s (id INT, txt TEXT)", &r) == DB_OK);
        if (r) { db_result_free(r); r = NULL; }

        char sql[1024];
        char doc[512];
        memset(doc, 'y', 300);
        doc[300] = '\0';
        snprintf(sql, sizeof(sql),
                 "INSERT INTO s (id, txt) VALUES (1, '%s')", doc);

        /* 300 字节写 TEXT 列 ⇒ 必须报错（不能静默截断或存空串） */
        CHECK(db_exec(db, sql, &r) != DB_OK);
        if (r) { db_result_free(r); r = NULL; }

        /* 表里不能留下这一行 */
        CHECK(db_exec(db, "SELECT COUNT(*) FROM s", &r) == DB_OK);
        if (r) {
            CHECK(db_result_rows(r) == 1);
            if (db_result_rows(r) == 1)
                CHECK(strcmp(db_result_value(r, 0, 0), "0") == 0);
            db_result_free(r);
            r = NULL;
        }

        db_close(db);
        remove(path);
        remove("test/tmp/text_len.db-wal");
        remove("test/tmp/text_len.db-clog");
    }
```

在 `test/unit/test_json_column.c` 里，把第 6 组（256–2048B 往返）的 `pad=1200` 用例改为
**期望成功**（当前实现会失败），并补一条 >2048B 期望失败的断言：

```c
        /* pad=1200 ⇒ 约 1216B：仍在阈值内，必须完整 */
        make_big_doc(1200, doc, sizeof(doc));
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t (id, doc) VALUES (61, '%s')", doc);
        exec_ok(sql);
        CHECK(one("SELECT doc FROM t WHERE id = 61", buf, sizeof(buf)) != NULL);
        CHECK(strlen(buf) > 1200);
        CHECK(strstr(buf, "\"a\":7") != NULL);
```

（已有的第 7 组覆盖 >2048B 拒绝，保持不变。）

- [x] **Step 2: 运行测试确认失败**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_text_len|test_json_column|个测试：'
```

预期：`test_text_len` 的 SQL 用例失败（当前 300B 写入静默成功）；`test_json_column` 的 1200B 用例失败。

- [x] **Step 3: 实现 `coerce_to_col` 的 TEXT 长度校验**

`src/query/exec_write.c` 的 `case T_TEXT:` 改成：

```c
    case T_TEXT:
        /*
         * TEXT 列只内联 255 字节（值层的既有契约）。
         *
         * 【为什么必须显式校验】
         * 原来的实现用 64 字节栈缓冲中转：val_to_str(v, buf, 64)
         * 会把任意长度的值截成 63 字节，然后 val_text() 存这 63 字节。
         * 词法器放宽到 8192B 之后，长字面量一律是外部值 ⇒
         * 这条路径必然被踩到，且会静默丢数据。
         *
         * 现在：先取真实长度，超过上限直接报错，不写 heap。
         */
        if (v->type != T_TEXT) {
            /* 非文本值按 TEXT 亲和性转文本（既有语义） */
            char buf[64];
            val_to_str(v, buf, (int)sizeof(buf));
            val_text(v, buf, (u16)strlen(buf));
            break;
        }
        if (v->external) {
            u8 *bytes = NULL;
            u32 blen = 0;
            int rc = val_detoast(v, &bytes, &blen);
            if (rc != OK)
                return rc;
            if (blen > VAL_INLINE_MAX) {
                free(bytes);
                snprintf(err, (size_t)errcap,
                         "列 %s：文本超过 %d 字节上限", col->name, VAL_INLINE_MAX);
                return E_NOSPACE;
            }
            val_text(v, (const char *)bytes, (u16)blen);
            free(bytes);
        } else if (v->len > VAL_INLINE_MAX) {
            snprintf(err, (size_t)errcap,
                     "列 %s：文本超过 %d 字节上限", col->name, VAL_INLINE_MAX);
            return E_NOSPACE;
        }
        break;
```

- [x] **Step 4: 实现 INSERT 的 JSON 编码中转**

`src/query/exec_write.c` 的 `exec_insert()` 里，把 JSON 分支改成用 `val_json_from_text`：

```c
            if (tc.cols[c].type == T_JSON && !row[c].is_null) {
                if (row[c].type != T_TEXT) {
                    snprintf(err, (size_t)errcap,
                             "JSON 列 %s 只接受 JSON 文本或 NULL",
                             tc.cols[c].name);
                    rc = E_INVAL;
                    goto fail;
                }
                /*
                 * 编码交给 doc 模态：它自己判上限、自己分配。
                 * 失败（语法错 / 超过模态允许的长度）在写 heap 之前发生。
                 *
                 * ⚠️ 错误文本里**不写死上限数字** ——
                 *    `JSON_INLINE_MAX` 是 doc 模态的常量，query 层不能 include
                 *    它（A7 审计）。具体数字由模态自己在返回值里体现。
                 */
                rc = val_json_from_text(&row[c], &row[c]);
                if (rc != OK) {
                    snprintf(err, (size_t)errcap,
                             "JSON 列 %s 文本无效或超过允许的长度",
                             tc.cols[c].name);
                    goto fail;
                }
            }
```

在 `exec_insert()` 的行循环末尾（`tc.am->insert` 之后）加释放：

```c
        /* xid 显式传（硬规则 5） */
        Rid rid;
        rc = tc.am->insert(tc.am, db->cur_xid, row, &rid);
        for (int c = 0; c < st->ncols; c++)
            val_json_free(&row[c]);      /* 外部 JSONB 缓冲：写完即还 */
        if (rc != OK)
            goto fail;
```

在 `fail:` 分支之前也要释放（`goto fail` 可能来自编码失败前的路径）：

```c
fail:
    for (int c = 0; c < st->ncols; c++)
        val_json_free(&row[c]);
    qr_free(r);
    return rc;
```

- [x] **Step 5: 实现 UPDATE 的 JSON 编码中转**

`src/query/exec_write.c` 的 `exec_update()` 里，把 JSON 分支改成同一形态：

```c
        if (tc.cols[set_idx].type == T_JSON && !nv.is_null) {
            if (nv.type != T_TEXT) {
                snprintf(err, (size_t)errcap,
                         "JSON 列 %s 只接受 JSON 文本或 NULL",
                         tc.cols[set_idx].name);
                free(rids);
                qr_free(r);
                return E_INVAL;
            }
            rc = val_json_from_text(&nv, &nv);
            if (rc != OK) {
                snprintf(err, (size_t)errcap,
                         "JSON 列 %s 文本无效或超过允许的长度",
                         tc.cols[set_idx].name);
                free(rids);
                qr_free(r);
                return rc;
            }
        }
```

⚠️ **所有权只有一个**：上面 `val_json_from_text(&nv, &nv)` 产出的外部缓冲
由 `nv.ext_ctx` 持有；随后的 `val_copy(&row[set_idx], &nv)` 只是把指针拷过去。
所以释放时**只释放 `row[]` 里的那一份**，绝不能顺手再释放 `nv` ——
那会二次释放同一块内存。

释放点（`tc.am->update` 之后）：

```c
        Rid new_rid;
        rc = tc.am->update(tc.am, db->cur_xid, rids[i], row, &new_rid);
        for (int c = 0; c < tc.ncols; c++)
            val_json_free(&row[c]);      /* 外部 JSONB 缓冲：写完即还 */
        if (rc != OK)
            continue;
```

⚠️ 但 `continue` / `return` 分支（nullable 跳过、coerce 失败）发生在
`tc.am->update` **之前**，此时缓冲还在 `nv` 上。做法：在 `val_copy` 之后
立刻把 `nv` 的外部标记清掉（`nv.ext_ctx = NULL; nv.ext_fn = NULL;
nv.external = false;`），让所有权唯一地落在 `row[set_idx]` 上，
再由统一的释放点回收。

- [x] **Step 6: 运行测试确认通过**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_text_len|test_json_column|个测试：'
```

预期：两个都 PASS，116 个测试全通过。

- [x] **Step 7: 提交**

```bash
cd C:/code/projects
git add engine/db/src/query/exec_write.c engine/db/test/unit/test_text_len.c engine/db/test/unit/test_json_column.c
git commit -m "fix(query): TEXT 超 255B 显式报错，JSON 列按模态上限编码

- coerce_to_col 的 T_TEXT 分支不再用 64 字节栈缓冲中转
  （此前把 300 字节文本截成 63 字节静默存库），超上限直接 E_NOSPACE
- INSERT / UPDATE 的 JSON 列改用 val_json_from_text：
  编码与上限判断交给 doc 模态，失败发生在写 heap 之前
- 所有退出路径（fail / continue / return）都释放外部 JSONB 缓冲
- 扩充 test_text_len 的 SQL 层用例与 test_json_column 的 1200B 往返"
git push origin main
```

---

### Task 6: 结果投影 —— JSON 文本完整输出

**Files:**
- Modify: `src/query/exec.c:196-268`（`b_push()` 的 `T_JSON` 分支）
- Test: `test/unit/test_json_column.c`（第 6 组已含 1200B 断言，本任务只验证它变绿）

**Interfaces:**
- Consumes: Task 3 的 `val_to_json_alloc`
- Produces: 无新公开接口。

- [x] **Step 1: 确认测试已存在且当前为红**

Task 5 已把 `test_json_column.c` 的 1200B 断言写好；本任务先确认它此刻仍红
（因为 `b_push()` 还在用 512 字节栈缓冲）：

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_json_column|个测试：'
```

预期：`test_json_column` 仍 FAIL（`strlen(buf) > 1200` 红）。

- [x] **Step 2: 实现 `b_push()` 的 JSON 分支**

`src/query/exec.c` 的 `b_push()` 里，把 `T_JSON` 分支改成：

```c
        if (vals[i].type == T_JSON) {
            /*
             * JSON 走 val_to_json_alloc：先测长度再写，**不截断**。
             *
             * 【为什么不能沿用 val_to_str(buf[512])】
             * 512 字节的栈缓冲会截断长文档 —— 用户看到的是一个语法
             * 不完整的 JSON，比报错更难查。
             */
            char *json_text = NULL;
            u32 json_len = 0;
            int rc = val_to_json_alloc(&vals[i], &json_text, &json_len);
            if (rc != OK)
                return rc;
            cell = json_text;
        } else if (vals[i].external) {
            /*
             * 普通外部文本值：物化成精确大小的缓冲。
             *
             * T_JSON 的外部表示仍是 JSONB 二进制，不可走此路径直接拷出；
             * 它先走上面的 val_to_json_alloc 分支，由注册渲染器转成 JSON 文本。
             */
            u8 *tmp = NULL;
            u32 n = 0;
            int rc = val_detoast(&vals[i], &tmp, &n);
            if (rc != OK)
                return rc;

            cell = malloc((size_t)n + 1);
            if (!cell) {
                free(tmp);
                return E_IO;
            }
            if (n)
                memcpy(cell, tmp, n);
            cell[n] = '\0';
            free(tmp);
        } else {
            char buf[512];
            val_to_str(&vals[i], buf, (int)sizeof(buf));
            cell = xstrdup(buf);
            if (!cell)
                return E_IO;
        }
```

- [x] **Step 3: 运行测试确认通过**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
sh test/run.sh 2>&1 | grep -E 'test_json_column|个测试：'
```

预期：`test_json_column` PASS，116 个测试全通过。

- [x] **Step 4: 提交**

```bash
cd C:/code/projects
git add engine/db/src/query/exec.c
git commit -m "fix(query): JSON 结果投影不再被 512 字节栈缓冲截断

b_push() 的 T_JSON 分支改用 val_to_json_alloc（先测长度再写），
长文档在 SELECT 时完整输出，不产生半截 JSON。"
git push origin main
```

---

### Task 7: 全量门禁 + 计划与文档同步

**Files:**
- Modify: `docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md`（C5.3b-a 边界补写）
- Modify: `engine/db/docs/MODALITIES.md`（§9.9 补坑编号）
- Modify: `engine/db/test/unit/test_blob_am.c`、`engine/db/test/integration/test_spi_it.c`（注释措辞同步）

**Interfaces:**
- Consumes: Task 1–6 的全部产出
- Produces: 无代码接口。

- [x] **Step 1: 跑完整门禁**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
mingw32-make clean && mingw32-make all 2>&1 | grep -E '^\S+:\d+:\d+: (error|warning|fatal)'
sh test/run.sh 2>&1 | tail -5
sh tools/check_deps.sh
sh tools/check_a7.sh
```

预期：零诊断；测试全通过；两个门禁都打印 ✓。

- [x] **Step 2: 跑九个模态 profile**

```bash
cd C:/code/projects/engine/db
export PATH="/c/Program Files/Git/usr/bin:/c/mingw64/bin:$PATH"
for p in full relational blob graph vector ft geo ts doc; do
  mingw32-make clean >/dev/null 2>&1
  mingw32-make all MM=$p >/dev/null 2>&1
  echo "$p rc=$?"
done
```

预期：九个都是 `rc=0`。

- [x] **Step 3: 补 MODALITIES.md 的坑记录**

在 `engine/db/docs/MODALITIES.md` 的 §9.9 末尾追加三条（编号接在现有 M7-34 之后）：

```markdown
**坑 M7-35 · SQL 字符串字面量被静默截断 → 用户拿到被改过的值**
词法器原来用 `Token.text[256]`，写满即停止拷贝却继续扫完输入 ——
"读完了却只存了一半"，而且**没有任何报错**。
防线：词法器改成两趟（先量精确长度、再拷贝），短字面量放 `text[]`、
长字面量单独分配，超过 `TK_STR_MAX`（8192）明确报错；
解析器接管长字面量的所有权（`Expr.text_owned`），`free_expr` 负责释放。

**坑 M7-36 · 外部 T_TEXT 进记录写成空串 → 静默丢数据**
`val_to_record()` 的 `T_TEXT` 分支原来直接取 `vals[i].s` / `vals[i].len`，
而外部 T_TEXT 的 `len` 恒为 0（协议是"长度靠 `val_detoast` 问"）——
300 字节的文本写进记录只剩 23 字节（只有头）。
防线：外部 `T_TEXT` 与外部 `T_JSON` 走同一套物化逻辑（`val_detoast` +
`ext_bufs[]` 记账）；`coerce_to_col` 的 TEXT 分支不再用定长栈缓冲中转，
超过 `VAL_INLINE_MAX` 直接报错。

**坑 M7-37 · JSONB 编码后塞进 `DbValue.s[256]` → 踩坏后面的字段**
`DbValue.s` 就是 256 字节，多拷一个字节就踩到 `external` / `ext_fn` ——
内联值变成"外部值 + 垃圾句柄"，读的时候报 `E_CORRUPT`。
防线：JSONB ≤ `VAL_INLINE_MAX` 才内联；256–2048B 用有明确所有者的临时外部值
中转（`val_json_from_text` / `val_json_free`），由 doc 模态自己裁决上限
（`JSON_INLINE_MAX`）；>2048B 在写 heap 之前明确拒绝。
```

- [x] **Step 4: 同步既有注释的措辞**

`engine/db/test/unit/test_blob_am.c` 与 `engine/db/test/integration/test_spi_it.c` 里
"塞不进 SQL 的标量值（上限 255）"改成：

```
 * blob 的一帧是 4076 字节，塞不进 SQL 的标量值（TEXT 列上限 255 字节）。
```

- [x] **Step 5: 同步计划文档**

在 `docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md` 的 C5.3b-a 段落里，
把"编码缓冲长度受 2048B 限制"改成：

```
   编码上限由 doc 模态自己裁决（`JSON_INLINE_MAX`，见 `json_ovf.h`）——
   query 层不写死这个数字（A7）。SQL 字符串字面量上限 8192B；
   `TEXT` 列超过 255B 显式报错（不静默截断）。
```

- [ ] **Step 6: 清理临时产物并提交**

```bash
cd C:/code/projects/engine/db
rm -f test/tmp/probe_*.c test/tmp/probe_*.db test/tmp/probe_*.db-wal test/tmp/probe_*.db-clog
rm -f build/probe_*.exe build/diag_*.exe build/test_json_col_debug.exe
cd C:/code/projects
git status --short
git add engine/db/docs/MODALITIES.md engine/db/test/unit/test_blob_am.c engine/db/test/integration/test_spi_it.c docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md
git commit -m "docs(db): 记录 M7-35/36/37 三个静默截断坑并同步计划与注释

- MODALITIES.md §9.9 补三条坑：字面量截断、外部 T_TEXT 写成空串、JSONB 越界
- 计划文档 C5.3b-a 补写：编码上限由模态裁决、TEXT 超 255B 显式报错
- blob 测试注释措辞同步为“TEXT 列上限 255 字节”"
git push origin main
```

- [ ] **Step 7: 更新父仓子模块指针**

```bash
cd C:/code
git add projects
git commit -m "chore(projects): 更新子仓指针到 JSON 长文档往返修复"
git push origin main
```

---

## 自审记录

**规格覆盖**：`2026-11-23-json-column-long-literal-design.md` 的每一节都有对应任务 ——
3.1/3.4（词法器与所有权）→ Task 1/2；3.2（列类型分流）→ Task 5；
3.3（两处静默丢数据）→ Task 3/5；3.5/3.6（编码与渲染）→ Task 3/4/6；
3.7（DML 中转与释放）→ Task 5；§5 各组测试 → Task 1/2/3/4/5/6。

**占位符扫描**：无 TBD / TODO / "类似 Task N"；每个步骤都带具体代码与命令。

**类型一致性**：`JsonEncodeFn` 的签名在 Task 3 定义、Task 4 实现，参数顺序一致；
`val_json_from_text(const DbValue *, DbValue *)` 在 Task 3 定义、Task 4/5 调用一致；
`val_json_free(DbValue *)` 在 Task 3 定义、Task 5 调用一致；
`val_to_json_alloc(const DbValue *, char **, u32 *)` 在 Task 3 定义、Task 6 调用一致。
