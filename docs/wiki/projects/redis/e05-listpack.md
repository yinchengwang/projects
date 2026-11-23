---
project: redis
feature: listpack
page_type: feature
schema_version: 1
refs:
  - path: src/listpack.c
    symbols: [lpAppend, lpGet, lpGetValue, lpDelete, lpReplace, lpInsert]
---

# 紧凑列表 listpack

## 一句话职责

listpack 是 Redis 的紧凑列表格式：一整段连续内存里依次排着「头 + 一串条目 + 结束标记」，每个条目自带反向长度，因此既能从头顺扫也能从尾倒扫，同时省掉每个元素一个独立分配的指针开销。

## 机制是什么

`src/listpack.c` 的整体布局是：**6 字节头部 + 若干条目 + 一个 `0xFF` 结束标记**。头部由 `LP_HDR_SIZE`（值为 6，注释写「32 bit total len + 16 bit number of elements」）与两个宏定义：前 4 字节是整个 listpack 的总字节数（`lpGetTotalBytes`），后 2 字节是元素个数（`lpGetNumElements`）。最后那个 `0xFF` 是 `LP_EOF`。

**每个条目由三段组成：`<编码头> <数据> <反向长度>`。** 编码头决定数据怎么解释：

- **字符串**有三种：`0xxxxxxx`（6 位长度，1 字节头）、`1110xxxx xxxxxxxx`（12 位长度，2 字节头）、`11110000` 后跟 4 字节长度（32 位长度，5 字节头）。
- **整数**有六种：7 位无符号（1 字节头）、13 位有符号（3 字节头）、16 位（4 字节头）、24 位（5 字节头）、32 位（6 字节头）、64 位（10 字节头）。

**反向长度是双向遍历的关键。** 条目尾部额外存一个「反向长度」字段，值是本条目**编码头 + 数据**的总长度（不含反向长度本身）。`lpEncodeBacklen` 按 1 到 5 字节变长编码：长度不超过 127 用 1 字节，不超过 16383 用 2 字节，不超过 2097151 用 3 字节，不超过 268435455 用 4 字节，更大用 5 字节（`LP_MAX_BACKLEN_SIZE` 就是 5）。因此反向走一步是 O(1)：读出反向长度、减去它就是上一个条目的起点；而正向走一步必须解编码头才能算出本条目多长。

**头里的元素计数可能「不可信」。** 元素个数字段只有 16 位，满了之后 `lpLength` 就无法直接给出答案。源码用 `LP_HDR_NUMELE_UNKNOWN`（`UINT16_MAX`）表示「计数已溢出、不可用」。

本页只讲 listpack 自身的编解码与增删改。把它包成链式节点的列表是 e06；哈希 / 集合 / 有序集合选用紧凑编码的策略是 D 组。

## 怎么实现的

**一切修改都走 `lpInsert`。** `lpAppend`、`lpReplace`、`lpDelete` 都只是它的薄封装，源码注释写明「It is implemented in terms of lpInsert(), so the return value is the same as lpInsert().」

`lpInsert(lp, elestr, eleint, size, p, where, newp)` 的完整流程：

1. **判断这是不是删除。** `delete = (elestr == NULL && eleint == NULL)`；若是删除，`where` 被强制改成 `LP_REPLACE`。源码注释解释：「when deletion, it is conceptually replacing the element with a zero-length element. So whatever we get passed as 'where', set it to LP_REPLACE.」
2. **把 `LP_AFTER` 归一化成 `LP_BEFORE`。** 注释写明：「If we need to insert after the current element, we just jump to the next element (that could be the EOF one) and handle the case of inserting before. So the function will actually deal with just two cases: LP_BEFORE and LP_REPLACE.」——整个实现只有「插到前面」与「替换当前」两种动作。
3. **记下偏移量。** `poff = p - lp`。因为后面可能要 `realloc`，裸指针会失效，偏移量不会。
4. **编码元素。** 给了字符串就调 `lpEncodeGetType` 判断它能不能表示成整数——能就返回 `LP_ENCODING_INT` 并把编码结果写进临时缓冲 `intenc`，此时 `eleint` 指向它；否则按字符串编码。`enclen` 是编码后的长度。
5. **算反向长度。** `backlen_size = lpEncodeBacklen(backlen, enclen)`——删除时为 0。
6. **算新长度。** 若是替换，先算旧条目的占用 `replaced_len = lpCurrentEncodedSizeUnsafe(p) + lpEncodeBacklenBytes(replaced_len)`；新总长 `= 旧总长 + enclen + backlen_size - replaced_len`。**超过 `UINT32_MAX` 直接返回 NULL**——头部只有 32 位长度字段，装不下就放弃。
7. **按需 realloc。** 变长时先 `lp_realloc` 再 `memmove`；变短时先 `memmove` 再 `lp_realloc` 释放空间。源码注释说明了这个顺序：「we do that before memmoving the memory to make room for the new element if the final allocation will get larger, or we do it after if the final allocation will get smaller.」
8. **写入条目与反向长度。** 整数走 `memcpy(dst, eleint, enclen)`，字符串走 `lpEncodeString`，然后把 `backlen` 跟在后面。
9. **更新头部。** 插入时元素数 +1、删除时 -1；总数写回 `lpSetTotalBytes`。
10. **回填 `*newp`。** 若调用方要了新位置，就填上 `dst`；若是删除且 `dst[0] == LP_EOF`（说明删的是最后一个元素），`*newp` 置 NULL。

**`lpAppend` / `lpReplace` / `lpDelete` 三个封装。**

- `lpAppend(lp, ele, size)`：算出末尾的 EOF 指针 `eofptr = lp + lpBytes - 1`，然后 `lpInsert(lp, ele, NULL, size, eofptr, LP_BEFORE, NULL)`——即在结束标记之前插入。
- `lpReplace(lp, p, s, slen)`：`lpInsert(lp, s, NULL, slen, *p, LP_REPLACE, p)`，**同时把调用方的游标 `*p` 更新到新位置**。
- `lpDelete(lp, p, newp)`：`lpInsert(lp, NULL, NULL, 0, p, LP_REPLACE, newp)`，两个元素指针都传 NULL 触发删除语义；`newp` 拿到被删元素的右边那个（删的是最后一个则为 NULL）。

**读：`lpGet` 与 `lpGetValue`。** `lpGet(p, count, intbuf)` 的行为取决于 `intbuf`：

- `intbuf` 为 NULL 时，若条目编码成整数，**返回 NULL** 并把整数值写进 `*count`；若编码成字符串，返回**指向 listpack 内部**的字符串指针，并把长度写进 `*count`。
- `intbuf` 非 NULL 时，函数**总是按字符串返回**：整数条目会用 `ll2string` 渲染进 `intbuf`，返回该缓冲指针。

`lpGetValue(p, slen, lval)` 是更常用的封装：先调 `lpGet(p, &ele_len, NULL)`；拿到字符串指针就 `*slen = ele_len` 返回该指针；否则 `*lval = ele_len` 并返回 NULL。源码注释写明：「When the function returns NULL, it populates the integer value by reference in 'lval'. Otherwise if the element is encoded as a string a pointer to the string (pointing inside the listpack itself) is returned, and 'slen' is set to the length of the string.」——**返回的字符串指针指向 listpack 内部，没有任何拷贝**。

**`lpLength` 会顺手改头。** 元素数没溢出时直接返回头部字段；溢出时从头扫一遍数出个数，若扫出来的数又落回 16 位可表示范围，就把新值**写回头部**。源码注释专门提醒：「As a side effect of calling this function, the listpack header could be modified.」

**真实 caller。**

- **哈希的 listpack 编码。** `src/t_hash.c` 是最典型的使用者：字段与值交替存放，所以条目数是字段数的两倍——`length = lpLength(o->ptr) / 2`；带过期时间的 LISTPACK_EX 编码是三倍（`lpLength(lpt->lp) / 3`）。改值走 `lpReplace(zl, &vptr, (unsigned char*)value, sdslen(value))`，遍历走 `lpGetValue(vptr, &slen, &val)`。
- **集合的 listpack 编码。** `src/t_set.c`：`set->ptr = lpAppend(lp, (unsigned char *)str, len)` 写入，`lpDelete(set->ptr, p, NULL)` 删除，随机取元素用 `lpSeek(lp, r)` 定位后 `lpGetValue`。
- **有序集合的 listpack 编码。** `src/t_zset.c` 同样以 `lpReplace` / `lpGetValue` 为主。
- **列表命令。** `src/t_list.c`：写入用 `lpPrepend` / `lpAppend`，删除用 `subject->ptr = lpDelete(subject->ptr, p, NULL)`，按下标改写用 `lpReplace(subject->ptr, &entry->lpe, ...)`。
- **quicklist 的节点内容。** `src/quicklist.c` 的每个节点持有一整个 listpack：`lpPrepend` / `lpAppend` 往里塞，`lpDelete` 删，`lpReplace` 改，`lpLength` 记节点元素数。
- **持久化。** `src/rdb.c` 用 `lpAppend` 逐条把集合 / 哈希写进 listpack，载入时用 `lpGet` / `lpGetValue` 逐条读出。
- **其它。** `src/db.c` 的 SCAN 紧凑编码分支、`src/geo.c` 的地理编码、`src/module.c` 的哈希字段扫描、`src/debug.c` 的调试输出、`src/aof.c` 的 AOF 命令重写。

**退化与回退路径。**

- **返回的字符串指针随修改失效。** `lpGet` / `lpGetValue` 返回的是 listpack **内部**的地址，任何一次 `lpInsert` 都可能 `realloc` 整个结构。真实调用方 `src/t_list.c` 的迭代器删除就同时更新了两个指针：`iter->subject->ptr = lpDelete(iter->subject->ptr, p, &p);`——结构指针与游标指针都要跟着改。
- **删除会「抹掉」一个条目而不是压缩内存。** 删除语义是「替换成零长度元素」，随后由 `lpSetTotalBytes` 把总长调小、尾部数据前移；条目个数减一。
- **计数溢出的降级。** 元素数超过 `UINT16_MAX` 后头部计数变成 `LP_HDR_NUMELE_UNKNOWN`，此后 `lpLength` 退化为**全量扫描**。源码注释写明：「Too many elements inside the listpack. We need to scan in order to get the total number.」
- **长度装不下就放弃。** `lpInsert` 里 `if (new_listpack_bytes > UINT32_MAX) return NULL;`——调用方必须处理 NULL，否则会用一个野指针。
- **有一段专门用来抓 bug 的死代码。** `lpInsert` 末尾有一段被 `#if 0` 关掉的逻辑，注释写明：「This code path is normally disabled: what it does is to force listpack to return *always* a new pointer after performing some modification to the listpack, even if the previous allocation was enough. This is useful in order to spot bugs in code using listpacks: by doing so we can find if the caller forgets to set the new pointer where the listpack reference is stored, after an update.」——把「忘记接收新指针」这类错误变成可检测的崩溃。

## 关键取舍

**源码事实：** listpack 是单块连续内存，头部 6 字节，尾部 `0xFF` 结束标记。**源码事实：** 条目由编码头、数据、反向长度三段组成，反向长度按 1 到 5 字节变长编码。**源码事实：** 元素个数只有 16 位，溢出后 `lpLength` 退化为全扫描并会顺手回写头部。**源码事实：** 所有修改都收敛到 `lpInsert` 一个原语，`LP_AFTER` 被归一化成 `LP_BEFORE`。**源码事实：** `lpGet` 返回的是内部指针，`intbuf` 非空时整数会被渲染成字符串。**源码事实：** 新总长超过 `UINT32_MAX` 时 `lpInsert` 返回 NULL。

**架构推论：** 用「每条目冗余反向长度」换来双向 O(1) 遍历，代价是每条目多占 1 到 5 字节，并且**插入或删除会让被改动位置之后所有条目的反向长度失效**，需要级联修正——这正是 listpack 复杂度的主要来源。**架构推论：** 「所有修改走一个原语」把 4 种操作（追加 / 插入 / 替换 / 删除）压成 2 种内存动作（插到前面 / 替换当前），大幅减少了需要单独验证的路径数；代价是这个原语的参数组合多（元素指针两个、长度、游标、位置语义、位置回填），调用者容易传错——源码专门留了一段 `#if 0` 的检测代码，说明这条路径历史上确实出过「忘记接收新指针」的 bug。**架构推论：** 「长度装不下就返回 NULL」是把格式本身的 32 位限制暴露成错误，而不是静默截断——这是正确的取舍，但对调用方是硬约束。主核若要复用 listpack 式紧凑编码，需要注意它**完全不适合并发写**：整个结构是一块连续内存，任何插入都要 `memmove` 后半段，这在主核的多写者模型下会与行锁粒度直接冲突——主核更适合「每元素独立分配 + 目录数组」的形态，把紧凑性让给只读快照与不可变数据。AI 侧若把向量或文档片段塞进 listpack，要意识到任何一次写入都要搬动后半段数据，向量集合频繁更新时这会成为主要开销。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/listpack.c` | `lpInsert` | 唯一的修改原语：处理插入 / 替换 / 删除三种语义，负责编码、反向长度、搬移与头部更新 |
| `src/listpack.c` | `lpAppend` | 在结束标记之前追加元素，是 `lpInsert` 的封装 |
| `src/listpack.c` | `lpGet` | 按条目指针取值：整数回填到出参、字符串返回内部指针，或统一渲染进调用方缓冲 |
| `src/listpack.c` | `lpGetValue` | 取值封装：字符串回填长度并返回内部指针，整数回填到 `lval` 并返回 NULL |
| `src/listpack.c` | `lpReplace` | 替换当前条目并同步更新调用方游标 |
| `src/listpack.c` | `lpDelete` | 删除当前条目，并把右边相邻条目的指针回传给调用方 |

## 参考

- [Redis 源码：listpack.c](https://github.com/redis/redis/blob/unstable/src/listpack.c)
- [Redis 源码：listpack.h](https://github.com/redis/redis/blob/unstable/src/listpack.h)
- [listpack 格式规范](https://github.com/antirez/listpack)
- [Redis 文档：数据类型（Hash）](https://redis.io/docs/latest/develop/data-types/hashes/)

## 与主核 / AI 的关系

listpack 对主核的价值主要在**只读快照与不可变数据**上：把一批定长或短变长字段压进一块连续内存，既省指针开销又让顺序扫描极快，这正是「训练样本清单」「模型元数据表」「一次推理的输入打包」这类场景需要的。但它的**插入要搬动后半段**这一条决定了它不能做主核的可变主存储——主核的多模态记录是并发写入的，任何一次更新都会把整块结构挪一遍。

主核真正值得借鉴的是**「把格式本身的容量上限暴露成显式失败」**这条纪律：listpack 的长度字段只有 32 位，装不下时 `lpInsert` 返回 NULL 而不是静默截断；主核的页格式、段格式、向量块格式都应有同等的显式失败路径，否则溢出会在很后面才以难以定位的方式爆掉。

另一条是**「内部指针的失效范围要写进契约」**：listpack 的所有读接口都返回内部地址，任何写操作之后全部失效。这在单线程里靠「重新赋回指针」就能兜住，在主核的并发模型下必须换成不可变快照或引用计数 + 写时复制。AI 侧则要注意一个具体的坑：把向量塞进 listpack 后**任何一次追加都会让之前取出的指针失效**，若调用方缓存了元素指针再回头使用，读到的可能是被覆盖的数据——这类 bug 在小集合下几乎不出现，只有元素变多触发 `memmove` 后才会显形。
