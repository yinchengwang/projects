# JSON 列长文档往返设计（C5.3b-a 补丁）

**日期**：2026-11-23
**状态**：待审阅
**关联计划**：`docs/superpowers/plans/2026-11-18-eight-modality-symmetry.md` 的 C5.3、C5.3b-a、C5.3b-b
**内核位置**：`engine/db`

## 1. 问题陈述

C5.3b-a 声称支持"≤2048B JSON 列端到端往返"，但实测发现三处断点，任何一处都会让 256B 以上的文档走不通：

| 断点 | 位置 | 现象 |
|---|---|---|
| A. 词法器截断 | `src/query/lexer.c` 的字符串分支 | `Token.text[256]` 写满即静默丢弃剩余字节；256B 以上的 SQL 字面量被悄悄改值 |
| B. DML 越界写 | `src/query/exec_write.c` | 把 JSONB `memcpy` 进 `DbValue.s[256]`，超过 255B 就踩坏后面的 `external` / `ext_fn` 字段 |
| C. 结果集截断 | `src/query/exec.c` 的 `b_push()` | JSON 渲染用 512B 栈缓冲，长文档在 `SELECT doc` 时被截断 |

三条断点各自独立可复现：只看 A 会以为"改了 exec_write 就行"，只看 B 会以为"只有渲染问题"。它们必须一起修，否则"256–2048B 完整往返"这个验收永远不可能成立。

### 1.1 实测证据

```
# 词法器（探针 probe_lexer_stage.exe）
字面量 254 字节 → Token.len = 254   （正确）
字面量 258 字节 → Token.len = 255   （被截断，无报错）

# DbValue 写入（探针 probe_encode_values.exe）
JSONB 255 字节 → external = 0（内联，正确）
JSONB 257 字节 → external = 120（越界踩到字段，值已损坏）

# SQL 端到端（探针 probe_medium2.exe）
文本 200B → INSERT OK，SELECT 200B 完整
文本 250B → INSERT 返回 E_CORRUPT（B 断点）
文本 260B+ → INSERT 报"文本无效或超过 2048 字节"（A 断点导致 JSONB 编码失败）
```

## 2. 范围

### 2.1 本设计包含

1. SQL 字符串字面量上限从 255B 放宽到 **8192B**，超限**明确报错**，绝不截断。
2. `T_JSON` 列的三层值表示真正可用：≤255B 内联；256–2048B 记录内联 + 运行时外部值；>2048B 拒绝。
3. JSON 列写入只接受 `TEXT`（JSON 文本）或 SQL `NULL`；`INT / REAL / BOOL` 报类型错误。
4. 结果集投影保证 JSON 文本完整，不受 512B 缓冲限制。
5. `TEXT` 列对 256–8192B 显式报错（不新增能力）；并修复两处既有的静默丢数据（3.3）。
6. 上述路径的所有权、失败清理、边界与测试。

### 2.2 本设计不包含

- `PT_JSON_OVF` 页链写入、tuple 所有权、abort/WAL 恢复、VACUUM 回收 —— 属于 **C5.3b-b**，需独立设计（计划里已列为阻塞项）。
- 参数绑定 / 预处理语句（当前公开 API 只有 `db_exec`，长文档只能经 SQL 字面量）。
- 路径索引（C5.8）。

**为什么 >2048B 仍要拒绝而不是"先凑合存"**：超过 `JSON_INLINE_MAX` 的文档没有合规的落盘位置；塞进 record 会挤爆行，塞进页链则缺少所有权与恢复协议。拒绝是可解释的错误，凑合是静默的风险。

## 3. 设计

### 3.1 词法器：分层存储 + 明确上限

`Token` 增加 `char *str_value`：≤255B 仍放 `Token.text`（绝大多数 token 不付堆代价），256–8192B 由词法器单独分配。两趟扫描：先量长度（`''` 计一字节），再拷贝。

```
上限检查：need > TK_STR_MAX(8192)  ⇒ E_INVAL + 具体错误文本
否则：need < TK_TEXT_MAX(256) ⇒ dst = tok->text
      否则                   ⇒ dst = malloc(need + 1)
```

**关键点：不许静默截断。** 现有实现在 `n < TK_TEXT_MAX - 1` 处停止拷贝但仍继续 `lx->pos++`，等于"读完了却只存了一半"。新实现要么完整存下，要么报错。

### 3.2 列类型决定长值的命运（否则放宽字面量会制造新错误）

**放宽字面量不等于放宽列能力。** `TEXT` 列在值层的读路径上就是 255 字节封顶
（`val_text()` 与 `dbvalue_from_record_var()` 都按 `VAL_INLINE_MAX` 截断），
它没有外部值落点。所以必须按列类型分流，每个分支都"要么完整、要么报错"：

| 列类型 | 字面量长度 | 行为 |
|---|---|---|
| `TEXT` | ≤255B | 正常存（既有行为不变） |
| `TEXT` | 256–8192B | **`E_NOSPACE` + 明确错误文本**，不写 heap |
| `JSON` | JSONB ≤255B | 内联 `DbValue.s[]` |
| `JSON` | JSONB 256–2048B | 外部值中转（见 3.5 / 3.7），**完整往返** |
| `JSON` | JSONB >2048B | `E_NOSPACE`（由模态判），不写 heap |

若不做这一步，放宽字面量会把"截到 255B"变成"截到 63B"或"存成空串" —— 比原来更糟。
实测证据（探针 `probe_text_long.exe` / `probe_text_gaps.exe`）：

```
SQL: INSERT INTO s (id, txt) VALUES (1, '<300B>')   ← TEXT 列
→ insert rc=0（静默成功）
→ select len=63                                      ← coerce_to_col 的 64B 栈缓冲截断
→ 直接喂 val_to_record：记录长度 23（外部 T_TEXT 被丢弃，写成空串）
```

### 3.3 顺带修复两处既有的静默丢数据

这两处不是本设计引入的，但**长字面量一定会踩到它们**，所以必须一起修：

1. **`val_to_record()` 的 `T_TEXT` 分支不物化外部值**（`src/storage/value.c`）。
   现在它直接 `tmp[i].u.text.p = vals[i].s; tmp[i].u.text.n = vals[i].len;`，
   而外部 T_TEXT 的 `len` 恒为 0、`s[]` 里没有内容 ⇒ **写进记录的是空串**。
   修法：与 `T_JSON` 分支同一套物化逻辑（`val_detoast` + `ext_bufs[]` / `ext_owned[]` 记账）。

2. **`coerce_to_col()` 的 `T_TEXT` 分支用 64B 栈缓冲**（`src/query/exec_write.c`）。
   `val_to_str(v, buf, sizeof(buf))` 把任意长度的值截成 63 字节，然后 `val_text()` 存这 63 字节。
   修法：先按真实长度判断（`val_detoast` 或读取 `len`），超过 `VAL_INLINE_MAX` 直接返回
   `E_NOSPACE` 并写错误文本；不再用定长缓冲做中转。

修完之后，"TEXT 列只支持 ≤255B"成为一条**显式校验**而不是隐式截断。

### 3.4 词法器→解析器的所有权

`Token.str_value` 归词法器分配，**归解析器释放**。三个释放点：

1. `p_advance()` 推进前释放上一个 token 的缓冲；
2. `parse_sql()` 在解析失败 / 尾部有多余内容时释放当前 token 的缓冲；
3. 成功把长字面量转交 `Expr.text_owned` 后，把 `p->cur.str_value` 置 NULL —— 转移所有权，避免双重释放。

`Expr` 增加 `char *text_owned` 与 `u16 text_len`，由 `free_expr()` 释放。`u16` 够用：上限 8192。

### 3.5 值层：JSON 文本 → JSONB 的机制（模态提供实现）

沿用渲染器的同一条纪律（值层提供机制、doc 模态提供实现），把编码器注册改成**分配式**：

```c
/* doc 模态注册的实现：自己决定"编码后多大算合规"，自己分配缓冲 */
typedef int (*JsonEncodeFn)(const char *text, u32 text_len,
                            u8 **bytes_out, u32 *len_out);
int val_set_json_encoder(JsonEncodeFn fn);

/* 值层入口：把 T_TEXT 值（内联或外部）编码成 T_JSON 值 */
int val_json_from_text(const DbValue *text, DbValue *value);
/* 释放 val_json_from_text 产出的外部 JSON 缓冲 */
void val_json_free(DbValue *value);
```

`val_json_from_text` 的契约：

- 入参 `text` 必须是 `T_TEXT` 且非 NULL；**内部先物化**（`external` 时调 `val_detoast`），因为长 SQL 字面量本身就是外部值（见 3.7）。
- 出参 `value`：长度 ≤ `VAL_INLINE_MAX` 时内联；否则是外部值，`ext_ctx` 指向一块**调用方需用 `val_json_free` 释放**的堆缓冲。
- 编码器未注册 ⇒ `E_NOTSUP`；文本非法或超过模态上限 ⇒ 编码器返回的错误原样透出。

**为什么把"多大算合规"交给模态**：2048 是 `json_ovf.h` 的 `JSON_INLINE_MAX`，是文档模态的磁盘表示知识。若写进 `exec_write.c`，query 层就认识了模态细节，违反 A7。分配式回调让模态自己判、自己报错。

**为什么编码器必须自己分配**：原型用的"调用方给 2048B 栈缓冲"接口有两个硬伤：
其一，2048 写死在 query 层，违反 A7；其二，它无法表达"模态决定的上限"，
编码后再由调用方强转 `u16` 也不能保证语义正确。

**模态侧的实现形态**：`fn_json_encode_alloc` 先调 `json_parse_to_jsonb(NULL,
0, ...)` 量出所需长度，再按该长度 `malloc`、真正编码；长度 > `JSON_INLINE_MAX`
就直接释放并返回 `E_NOSPACE`，因为 >2048B 属于 C5.3b-b 的页链领地。

### 3.6 值层：完整 JSON 文本的渲染

`val_to_str()` 保持现状（有 `cap` 上限，用于 REPL / 日志等"短输出"场景）。新增：

```c
/* 渲染成新分配的 C 字符串；长度由渲染器先测后写，调用方 free */
int val_to_json_alloc(const DbValue *v, char **out, u32 *len);
```

`b_push()` 的 `T_JSON` 分支改用它。这样"两次调用渲染器（先测再写）"的逻辑只存在于值层一处，而不是结果构造器里再写一遍 —— 也避免 `exec.c` 直接依赖 `val_json_render()`。

**这条修复的必要性有实测支撑**：原型里 `b_push()` 虽然改成了"先测后写"，
但把这段逻辑写在 `exec.c` 里，等于结果构造器要知道渲染器的两次调用协议；
放在值层后，`exec.c` 只调用一个 `val_json_alloc`，分层不变。


### 3.7 DML：三个必须处理的细节

**(1) 长字面量是外部值。** `val_external_text()` 不设置 `len`（协议是"长度靠 `val_detoast` 问"）。因此 `exec_write.c` **不能**直接读 `row[c].s` / `row[c].len` —— 必须先物化。这正是原型代码失败的原因：它对 300B 的字面量拿到 `len == 0`，交给 JSON 解析器的是空串。

**(2) 临时缓冲的所有者必须显式。** `256–2048B` 的 JSONB 不在 `s[]` 里，而是 `val_json_from_text` 分配的堆缓冲。生命周期：从编码完成到 heap AM 的 `insert` / `update` 返回（`val_to_record` 在 AM 内部同步物化）。之后立即 `val_json_free`。

**(3) 每条退出路径都要释放。** INSERT 的 `fail:` 分支、UPDATE 的 `continue` / `return` 分支（nullable 跳过、coerce 失败、AM 失败）都可能持有一个未释放的 JSON 缓冲。设计上要求：**每轮循环开始前把该行所有 JSON 列缓冲先释放一次**（幂等：`val_json_free` 对非外部 JSON 是空操作），并让所有 `goto fail` 走同一个清理点。

### 3.8 数据流

```
SQL 文本
  └─ lexer：≤255B 进 Token.text / >255B 进 Token.str_value（上限 8192，超限报错）
      └─ parser：长字面量移交给 Expr.text_owned，Expr.lit = 外部 T_TEXT
          └─ exec_write INSERT/UPDATE：
               val_json_from_text(文本值)          ← 内部 detoast + 编码
                 ├─ JSONB ≤255B  ⇒ 内联 T_JSON
                 ├─ 256–2048B    ⇒ 外部 T_JSON（拥有缓冲）
                 └─ >2048B / 语法错 ⇒ 错误，不写 heap
                    └─ AM.insert/update → val_to_record 同步物化进记录变长区
                         └─ 返回后 val_json_free
```

读取方向：

```
record 变长区（内联 JSONB）
  └─ val_from_record*：≤255B 内联 DbValue；256–2048B 外部值指向记录缓冲
      ├─ json_extract / json_match：take_json 内部 detoast（已有实现）
      └─ 结果投影 b_push：val_to_json_alloc → 完整 JSON 文本
```

## 4. 错误语义

| 情形 | 返回 | 附加要求 |
|---|---|---|
| 字符串字面量 > 8192B | `E_INVAL` | 错误文本含上限；parser 失败，不产生部分 Stmt |
| `TEXT` 列写入 256–8192B | `E_NOSPACE` | 错误文本说明 TEXT 上限是 255B；不写 heap |
| JSON 文本语法非法 | 编码器错误（如 `E_INVAL`） | 不写 heap，不留下半个 tuple |
| JSONB 编码后 > 2048B | `E_NOSPACE`（由模态判） | 同上 |
| 编码器未注册 | `E_NOTSUP` | 明确报错，不退化成"当 TEXT 存" |
| JSON 列为 `INT/REAL/BOOL` | `E_INVAL` | 不做隐式标量转 JSON |
| SQL `NULL` | 存 SQL NULL | 不写成 JSONB `null` |
| 渲染器未注册 | `E_NOTSUP` | 不吐原始字节 |

## 5. 测试

### 5.1 词法器与解析器（`test/unit/test_lexer_string.c`，新增）

- 边界：254 / 255 / 256 / 257 / 8192 / 8193 字节
- 8193 ⇒ `E_INVAL` 且错误文本提到上限
- `''` 转义在长字面量里仍按一字节解码
- 断言：解析 300B 字面量后 `free_stmt` 不泄漏（可复用现有 memcheck 用例机制）

### 5.2 TEXT 列的长值校验（`test/unit/test_text_len.c`，新增）

- 254 / 255 / 256 字节写入 `TEXT` 列：255 通过且读回 255；256 报 `E_NOSPACE`
- 300 字节写入 `TEXT` 列：报错且表里没有该行（钉死"不能静默存空串/截到 63B"）
- `val_to_record` 直接喂外部 `T_TEXT`：记录长度 ≈ 头 + 真实长度（不是 23）
- `coerce_to_col` 不再经 64B 栈缓冲：255B 文本写入后读回 255B（不是 63B）

### 5.3 JSON 列端到端（扩充 `test/unit/test_json_column.c`）

已有 7 组覆盖小文档 / NULL / `json_extract` / `json_match` / UPDATE / 拒绝。补充：

- **256–2048B 往返**：`SELECT doc` 文本长度与语义完整、`json_extract` 取得到标量
- **UPDATE 到长文档**：同一条编码路径
- **>2048B 拒绝**：`INSERT` 报错且表里没有该行
- **长文档 + UPDATE 失败回滚**：不泄漏外部缓冲（memcheck 用例）

### 5.4 结果投影（`test/unit/test_json_column.c` 内）

- 1200B 文档经 `SELECT doc` 返回完整长度（钉死 512B 截断不能再出现）


### 5.5 回归门禁

`mingw32-make all`（`-Werror` 零诊断）、`sh test/run.sh` 全绿、`tools/check_deps.sh`、`tools/check_a7.sh`、九个 MM profile 全部构建通过。

## 6. 与计划的接口

- C5.3b-a 的实现边界据此改写：其中"编码缓冲长度受 2048B 限制"一句需要补充"上限由模态裁决"，并补上"`TEXT` 列超过 255B 显式报错"。
- C5.3b-b 保持原样（页链所有权仍待设计）。
- 本设计**不改** `DetoastFn` 契约，不改 `DbValue` 布局（新增字段只加在 `Token` / `Expr`）。

## 7. 未决与风险

1. **8192 是拍出来的上限**。取它是为了覆盖"2048B 文档经转义后膨胀"的余量（UTF-8 中文 + `\"` 转义约 1:1 到 1:2）。若将来要更长的普通 TEXT，另开一步，不与 JSON 混。
2. **`val_to_json_alloc` 与 `val_to_str` 的渲染逻辑共享**：实现时两者必须调用同一个渲染器两次调用序列，避免"短的走截断、长的走完整"两套行为分叉。
3. **原型代码已存在但未提交**（`git status` 里的 `exec.c` / `exec_write.c` / `lexer.c` / `lexer.h` / `parser.c` / `parser.h` 本地改动）。本设计获批后按实施计划以 TDD 方式重做，不直接沿用原型；原型先回退到干净基线。
4. **`TEXT` 列 255B 语义是对外契约的一部分**：`test_blob_am.c` / `test_spi_it.c` 的注释都写明"SQL 标量值上限 255"。这些注释在实现时要同步更新措辞（改为"TEXT 列上限 255B"），否则文档与代码不一致。

