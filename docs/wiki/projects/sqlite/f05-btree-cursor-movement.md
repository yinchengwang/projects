---
project: sqlite
feature: btree-cursor-movement
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeNext, sqlite3BtreePrevious, sqlite3BtreeFirst, sqlite3BtreeLast, sqlite3BtreeEof]
---

# B-tree 游标移动

## 一句话职责

`sqlite3BtreeNext` / `sqlite3BtreePrevious` 负责把游标沿 B-tree 的叶子链前后移动一格（跨页时逐级上溯父页再下沉子页），`sqlite3BtreeFirst` / `sqlite3BtreeLast` 负责把游标定位到整棵树的最左 / 最右叶子单元，`sqlite3BtreeEof` 负责回答「游标当前是否已不在有效条目上」。

## 机制是什么

B-tree 的游标（`BtCursor`）是一个「页栈 + 单元下标」的结构，而不是一个指针。游标当前所在位置由三样东西共同确定：

- `BtCursor.pPage`：当前所在页（`MemPage`）。
- `BtCursor.ix`：当前页内的单元下标（`u16`）。
- `BtCursor.apPage[]` / `BtCursor.aiIdx[]`：从根页到当前页的祖先页栈与每层下标。

由此产生两类移动：

**页内移动（快路径）。** 目标单元就在当前页上，只需改 `ix`，不碰页栈。这是绝大多数扫描步走的路径。

**跨页移动（慢路径）。** 目标单元在别的页上，需要沿页栈上行或下行：向前移动越过当前叶页末尾时，先 `moveToParent` 上溯到父页，若父页也走到末尾就继续上溯，直到某一层还有下一个单元，再从那一层下行到最左叶子；向后移动越过当前叶页开头时同理，上溯到父页后进入对应子树的最右叶子。

源码把这套机制拆成「一个外层薄封装 + 一个 `SQLITE_NOINLINE` 慢路径」：

| 外层（薄、快） | 慢路径（`SQLITE_NOINLINE`） |
|---|---|
| `sqlite3BtreeNext` | `btreeNext` |
| `sqlite3BtreePrevious` | `btreePrevious` |

`sqlite3BtreeNext` 的注释写明：「主入口是 `sqlite3BtreeNext()`。该例程针对『仅仅把单元计数器 `BtCursor.aiIdx` 递增到当前页下一个单元』这一常见情形做了优化。当需要移动到不同页或恢复游标时，调用（较慢的）`btreeNext()` 辅助例程。」`sqlite3BtreePrevious` 的注释是镜像的：「针对仅仅把单元计数器递减到当前页上一个单元这一常见情形做了优化。」

**方向不对称。** 这不是实现上的随意选择，而是 B-tree 结构本身决定的：内部页有 K 个分隔单元和 K+1 个孩子指针，因此「当前分隔单元的左孩子」与「当前分隔单元的右孩子」在页布局上不对称——向左走要用 `findCell(pPage, idx)` 取出单元头 4 字节里的左孩子页号，向右走则要用 `get4byte(&pPage->aData[pPage->hdrOffset+8])` 取页头固定位置上的最右孩子页号。`btreeNext` 与 `btreePrevious` 里的这两行代码就是这个不对称的直接体现。

**首尾定位。** `sqlite3BtreeFirst` / `sqlite3BtreeLast` 不走增量路径，而是先回到根页再一路下沉到最左 / 最右叶子。`moveToLeftmost` 与 `moveToRightmost` 的一处差别值得注意：`moveToLeftmost` 从「当前指着的那个单元的左孩子」开始下沉（`get4byte(findCell(pPage, pCur->ix))`），而 `moveToRightmost` 直接取「当前页的最右孩子」（页头偏移 `hdrOffset+8` 处的 4 字节），并把 `pCur->ix` 设为 `pPage->nCell`。源码注释把这个差别写明：「注意 `moveToLeftmost()` 与 `moveToRightmost()` 的不同。」

**空树与 EOF。** 根页没有单元且是叶子页时，`moveToRoot` 把游标状态置为 `CURSOR_INVALID` 并返回 `SQLITE_EMPTY`；`sqlite3BtreeFirst` / `sqlite3BtreeLast` 把 `SQLITE_EMPTY` 翻译成「`*pRes = 1`（表为空）」并把返回值改回 `SQLITE_OK`。`sqlite3BtreeEof` 的判据极简：`return (CURSOR_VALID != pCur->eState);`——只要游标状态不是 `CURSOR_VALID` 就认为到了末尾。

## 怎么实现的

### 游标状态机（`src/btreeInt.h`）

`BtCursor.eState` 取值定义在 `src/btreeInt.h`：

| 常量 | 值 | 语义 |
|---|---|---|
| `CURSOR_VALID` | 0 | 游标正指向一条有效条目 |
| `CURSOR_INVALID` | 1 | 游标不指向任何条目（走到头、或表为空） |
| `CURSOR_SKIPNEXT` | 2 | 恢复位置后需要「跳过一条」 |
| `CURSOR_REQUIRESEEK` | 3 | 位置已失效，需要按保存的键重新定位 |

`BtCursor.curFlags` 里与本机制直接相关的位：

| 常量 | 值 | 语义 |
|---|---|---|
| `BTCF_ValidNKey` | 0x02 | `info.nKey` 有效 |
| `BTCF_ValidOvfl` | 0x04 | `aOverflow` 有效 |
| `BTCF_AtLast` | 0x08 | 游标正指向最后一条 |

`src/btreeInt.h` 对 `skipNext` 的语义有明确注释：`eState==SKIPNEXT` 时「`sqlite3BtreeNext()` 在 `skipNext>0` 时是空操作，`sqlite3BtreePrevious()` 在 `skipNext<0` 时是空操作」；`eState==REQUIRESEEK` 时「`restoreCursorPosition()` 把游标恢复成 `eState=SKIPNEXT`（当 `skipNext!=0`）」。

### `sqlite3BtreeNext`：向前一格

函数体（`src/btree.c`）：

```
sqlite3BtreeNext(BtCursor *pCur, int flags)
  pCur->info.nSize = 0;
  pCur->curFlags &= ~(BTCF_ValidNKey|BTCF_ValidOvfl);
  if( pCur->eState!=CURSOR_VALID ) return btreeNext(pCur);
  pPage = pCur->pPage;
  if( (++pCur->ix)>=pPage->nCell ){
    pCur->ix--;
    return btreeNext(pCur);
  }
  if( pPage->leaf ) return SQLITE_OK;
  else return moveToLeftmost(pCur);
```

要点：

1. **先清缓存标记。** 无论走哪条路径，都先把 `info.nSize` 置 0 并清掉 `BTCF_ValidNKey` / `BTCF_ValidOvfl`——原来那条单元的解析结果与溢出页缓存已经对不上新位置了。
2. **状态非 VALID 一律走慢路径。** 慢路径 `btreeNext` 第一件事就是 `restoreCursorPosition(pCur)`。
3. **页内快路径。** `++pCur->ix` 后若没越界，看当前页是不是叶子：叶子页直接返回 `SQLITE_OK`（单元就在本页，位置已对）；非叶子页则要 `moveToLeftmost` 往下钻到最左叶子。
4. **越界则回退一格再走慢路径。** `pCur->ix--` 把下标退回合法范围，慢路径从头再做一遍 `++pCur->ix`。

### `btreeNext`：慢路径

`btreeNext`（`src/btree.c`）分三段：

**第一段：恢复位置。**

```
if( pCur->eState!=CURSOR_VALID ){
  rc = restoreCursorPosition(pCur);
  if( rc!=SQLITE_OK ) return rc;
  if( CURSOR_INVALID==pCur->eState ) return SQLITE_DONE;
  if( pCur->eState==CURSOR_SKIPNEXT ){
    pCur->eState = CURSOR_VALID;
    if( pCur->skipNext>0 ) return SQLITE_OK;
  }
}
```

`restoreCursorPosition` 是宏（`src/btree.c`）：`(p->eState>=CURSOR_REQUIRESEEK ? btreeRestoreCursorPosition(p) : SQLITE_OK)`，只在状态确实失效时才做事。`btreeRestoreCursorPosition` 内部调 `btreeMoveto` 按保存的键重新定位；若定位结果带 `skipNext`，则把状态置为 `CURSOR_SKIPNEXT`。回到 `btreeNext` 里，`skipNext>0` 时本次 `Next` 就是空操作（`return SQLITE_OK`），即「跳过一条」。

**第二段：页内推进与损坏检查。**

```
pPage = pCur->pPage;
idx = ++pCur->ix;
if( sqlite3FaultSim(412) ) pPage->isInit = 0;
if( !pPage->isInit ) return SQLITE_CORRUPT_BKPT;
```

`sqlite3FaultSim(412)` 是故障注入点，用于测试；命中就把页标记成未初始化，随后的 `!pPage->isInit` 判定返回 `SQLITE_CORRUPT_BKPT`。

**第三段：跨页下行与上溯。**

```
if( idx>=pPage->nCell ){
  if( !pPage->leaf ){
    rc = moveToChild(pCur, get4byte(&pPage->aData[pPage->hdrOffset+8]));
    if( rc ) return rc;
    return moveToLeftmost(pCur);
  }
  do{
    if( pCur->iPage==0 ){
      pCur->eState = CURSOR_INVALID;
      return SQLITE_DONE;
    }
    moveToParent(pCur);
    pPage = pCur->pPage;
  }while( pCur->ix>=pPage->nCell );
  if( pPage->intKey ){
    return sqlite3BtreeNext(pCur, 0);
  }else{
    return SQLITE_OK;
  }
}
```

- 当前页是**非叶子**且已越过末尾：走「最右孩子」（页头 `hdrOffset+8` 处的 4 字节）再 `moveToLeftmost` 钻到最左叶子。**这是从内部页向下的分支**——内部页的最后一个分隔单元之后就是最右孩子。
- 当前页是**叶子**且已越过末尾：进入 `do-while` 上溯循环。`pCur->iPage==0` 说明已经到根页还是没找到下一条，于是置 `CURSOR_INVALID` 并返回 `SQLITE_DONE`。
- 上溯到某一层后 `pCur->ix < pPage->nCell`（该层还有下一个单元）时退出循环。此时若该页 `intKey`（是 rowid 表的内部页），还要再调一次 `sqlite3BtreeNext(pCur, 0)` 继续往下走；否则（索引 B-tree 的内部页）直接返回 `SQLITE_OK`。

### `sqlite3BtreePrevious`：向后一格

```
sqlite3BtreePrevious(BtCursor *pCur, int flags)
  pCur->curFlags &= ~(BTCF_AtLast|BTCF_ValidOvfl|BTCF_ValidNKey);
  pCur->info.nSize = 0;
  if( pCur->eState!=CURSOR_VALID
   || pCur->ix==0
   || pCur->pPage->leaf==0
  ){
    return btreePrevious(pCur);
  }
  pCur->ix--;
  return SQLITE_OK;
```

三个走慢路径的条件：**状态非 VALID**、**当前是页内第一个单元（`ix==0`）**、**当前页不是叶子页**。只有三者都不满足时，才是纯粹的页内后退（`pCur->ix--`）。

`btreePrevious`（`src/btree.c`）：

```
if( pCur->eState!=CURSOR_VALID ){
  rc = restoreCursorPosition(pCur);
  if( rc!=SQLITE_OK ) return rc;
  if( CURSOR_INVALID==pCur->eState ) return SQLITE_DONE;
  if( CURSOR_SKIPNEXT==pCur->eState ){
    pCur->eState = CURSOR_VALID;
    if( pCur->skipNext<0 ) return SQLITE_OK;
  }
}
pPage = pCur->pPage;
if( sqlite3FaultSim(412) ) pPage->isInit = 0;
if( !pPage->isInit ) return SQLITE_CORRUPT_BKPT;
if( !pPage->leaf ){
  int idx = pCur->ix;
  rc = moveToChild(pCur, get4byte(findCell(pPage, idx)));
  if( rc ) return rc;
  rc = moveToRightmost(pCur);
}else{
  while( pCur->ix==0 ){
    if( pCur->iPage==0 ){
      pCur->eState = CURSOR_INVALID;
      return SQLITE_DONE;
    }
    moveToParent(pCur);
  }
  pCur->ix--;
  pPage = pCur->pPage;
  if( pPage->intKey && !pPage->leaf ){
    rc = sqlite3BtreePrevious(pCur, 0);
  }else{
    rc = SQLITE_OK;
  }
}
```

与 `btreeNext` 的三处镜像差异：

1. **下行取的是左孩子而非最右孩子。** 非叶子页时 `get4byte(findCell(pPage, idx))` 取当前单元头 4 字节里的左孩子页号，然后 `moveToRightmost` 钻到最右叶子。
2. **`skipNext` 的判据方向相反。** `btreeNext` 判 `pCur->skipNext>0`，`btreePrevious` 判 `pCur->skipNext<0`。
3. **上溯循环条件相反。** `btreeNext` 的循环条件是 `pCur->ix>=pPage->nCell`（越过末尾就上溯），`btreePrevious` 是 `while( pCur->ix==0 )`（在开头就上溯）。

### `sqlite3BtreeFirst`：定位最左

```
sqlite3BtreeFirst(BtCursor *pCur, int *pRes)
  rc = moveToRoot(pCur);
  if( rc==SQLITE_OK ){
    assert( pCur->pPage->nCell>0 );
    *pRes = 0;
    rc = moveToLeftmost(pCur);
  }else if( rc==SQLITE_EMPTY ){
    *pRes = 1;
    rc = SQLITE_OK;
  }
  return rc;
```

- 成功定位到条目：`*pRes = 0`。
- 表为空（`moveToRoot` 返回 `SQLITE_EMPTY`）：`*pRes = 1`，且返回值被改写成 `SQLITE_OK`——**空表不算错误**。
- `moveToLeftmost` 沿左孩子链下沉到最左叶子（`src/btree.c`）：

```
while( rc==SQLITE_OK && !(pPage = pCur->pPage)->leaf ){
  assert( pCur->ix<pPage->nCell );
  pgno = get4byte(findCell(pPage, pCur->ix));
  rc = moveToChild(pCur, pgno);
}
```

注意它的下沉源是「当前单元的左孩子」，而 `moveToRoot` 已经把 `pCur->ix` 置为 0，所以首次下沉取的是根页第 0 个单元的左孩子。

### `sqlite3BtreeLast`：定位最右

`sqlite3BtreeLast`（`src/btree.c`）比 `sqlite3BtreeFirst` 多一层「已经在末尾就不动」的短路：

```
if( CURSOR_VALID==pCur->eState && (pCur->curFlags & BTCF_AtLast)!=0 ){
  *pRes = 0;
  return SQLITE_OK;
}
return btreeLast(pCur, pRes);
```

`btreeLast` 是 `SQLITE_NOINLINE` 的内部函数，做 `moveToRoot` → `moveToRightmost`，并根据结果设置或清除 `BTCF_AtLast`：

```
rc = moveToRoot(pCur);
if( rc==SQLITE_OK ){
  *pRes = 0;
  rc = moveToRightmost(pCur);
  if( rc==SQLITE_OK ) pCur->curFlags |= BTCF_AtLast;
  else               pCur->curFlags &= ~BTCF_AtLast;
}else if( rc==SQLITE_EMPTY ){
  *pRes = 1;
  rc = SQLITE_OK;
}
```

`BTCF_AtLast` 是「一次性定位末尾」这一优化的载体：`sqlite3BtreeNext` 会清掉它（`pCur->curFlags &= ~(BTCF_ValidNKey|BTCF_ValidOvfl)` 之外的 `moveToRoot` 也清），`sqlite3BtreePrevious` 同样清掉它（`pCur->curFlags &= ~(BTCF_AtLast|BTCF_ValidOvfl|BTCF_ValidNKey)`），因此只有「定位到末尾后没有再动过」时才短路。

`moveToRightmost`（`src/btree.c`）的取孩子方式与 `moveToLeftmost` 不同：

```
while( !(pPage = pCur->pPage)->leaf ){
  pgno = get4byte(&pPage->aData[pPage->hdrOffset+8]);
  pCur->ix = pPage->nCell;
  rc = moveToChild(pCur, pgno);
  if( rc ) return rc;
}
pCur->ix = pPage->nCell-1;
```

取的是页头 `hdrOffset+8` 处的最右孩子（不是某个单元的左孩子），因此**不受当前 `pCur->ix` 影响**。

### `sqlite3BtreeEof`：末尾判定

```
int sqlite3BtreeEof(BtCursor *pCur){
  return (CURSOR_VALID!=pCur->eState);
}
```

判定条件只有一个：游标状态不是 `CURSOR_VALID` 就算 EOF。源码注释说明这涵盖三种情形：`sqlite3BtreeNext()` 走过最后一条之后、`sqlite3BtreePrev()` 越过第一条之后、以及表本身为空。函数内还留了一条 TODO：「如果游标处于 `CURSOR_REQUIRESEEK` 但所有表项都已被删除怎么办？这个 API 需要改成同时返回错误码和布尔结果。」

### 跨页移动的底层：三个页操作原语

| 原语 | 行为 |
|---|---|
| `moveToChild(BtCursor *pCur, u32 newPgno)` | 压栈一层：把当前 `ix` 存进 `aiIdx[iPage]`、当前页存进 `apPage[iPage]`，`iPage++`，用 `getAndInitPage` 载入新页。若新页 `nCell<1` 或 `intKey` 与 `curIntKey` 不符，释放该页并返回 `SQLITE_CORRUPT_PGNO(newPgno)`。若 `pCur->iPage>=(BTCURSOR_MAX_DEPTH-1)` 返回 `SQLITE_CORRUPT_BKPT` |
| `moveToParent(BtCursor *pCur)` | 弹栈一层：`pCur->ix = pCur->aiIdx[pCur->iPage-1]`，`pCur->pPage = pCur->apPage[--pCur->iPage]`，并 `releasePageNotNull(pLeaf)` 释放刚才那一层的页 |
| `moveToRoot(BtCursor *pCur)` | 回到根页。若 `pCur->iPage>=0` 且非零，释放中间各层页，只保留 `apPage[0]`；若 `pCur->pgnoRoot==0` 则置 `CURSOR_INVALID` 并返回 `SQLITE_EMPTY`。根页无单元且非叶子时（只可能是页 1 的虚拟根页），下取其唯一子页 |

**页栈深度上限**来自 `src/btreeInt.h` 的 `#define BTCURSOR_MAX_DEPTH 20`，`BtCursor.apPage[]` / `aiIdx[]` 的数组长度因此是 `BTCURSOR_MAX_DEPTH-1`，即 **19**。`moveToChild` 在 `pCur->iPage>=(BTCURSOR_MAX_DEPTH-1)` 时返回 `SQLITE_CORRUPT_BKPT`。

**页缓存保持**体现在 `moveToParent` 的 `releasePageNotNull(pLeaf)` 与 `moveToChild` 的 `getAndInitPage` 上：向下走时新页从页缓存取出（引用计数加一），向上走时离开的页被释放引用（引用计数减一）。**引用的具体计数由 pager 层维护，不在本机制内**（页缓存引用计数属于 I 组）。

### 真实调用方

| 调用方 | 文件 | 调用方式 |
|---|---|---|
| `OP_Next` | `src/vdbe.c` | `rc = sqlite3BtreeNext(pC->uc.pCursor, pOp->p3);` 随后 `goto next_tail` |
| `OP_Prev` | `src/vdbe.c` | `rc = sqlite3BtreePrevious(pC->uc.pCursor, pOp->p3);` 随后 `goto next_tail` |
| `OP_Rewind` | `src/vdbe.c` | `rc = sqlite3BtreeFirst(pCrsr, &res);`，结果写入 `pC->nullRow` |
| `OP_Last` / `OP_SeekEnd` | `src/vdbe.c` | `rc = sqlite3BtreeLast(pCrsr, &res);` |
| `OP_IfSizeBetween` | `src/vdbe.c` | `rc = sqlite3BtreeFirst(pCrsr, &res);` 后接 `sqlite3BtreeRowCountEst` |
| `OP_IfNoHope` | `src/vdbe.c` | `sqlite3BtreeCursorIsValidNN(pCrsr)` 为真时直接 break，否则 `sqlite3BtreeLast` |
| `sqlite3BtreeTableMoveto` | `src/btree.c` | `if( pCur->info.nKey+1==intKey ){ *pRes = 0; rc = sqlite3BtreeNext(pCur, 0); }`，注释说明「如果请求的键正好是上一个键加一，就尝试用 `sqlite3BtreeNext()` 到达那里而不是做一次完整的二分查找」 |
| `sqlite3VdbeSorterRewind` 辅助路径 | `src/vdbeaux.c` | `sqlite3BtreeFirst(pCur, &res)` 与 `sqlite3BtreeNext(pCur, 0)` 配对使用，循环条件 `sqlite3BtreeEof(pCur)==0 && ii<BTREE_FDK_RANGE` |
| `sqlite3BtreeNext` / `sqlite3BtreePrevious` 的测试辅助 | `src/test3.c` | 直接调用这三个 API 做 B-tree 层单测 |

`OP_Next` / `OP_Prev` 传给 `sqlite3BtreeNext` / `sqlite3BtreePrevious` 的 `pOp->p3` 是**给 B-tree 实现的提示**：注释写明「如果 `sqlite3BtreeNext(C,F)` 的 F 参数的 0x01 位是 1，说明游标对应一个 SQL 索引，且如果该 SQL 索引是唯一索引，本指令本来可以跳过。F 参数是给实现者的提示。SQLite 的 btree 实现不使用这个提示，但 COMDB2 使用。」源码里 `UNUSED_PARAMETER( flags );` 与 `assert( flags==0 || flags==1 );` 印证了这一点。

### 关键魔数与常量出处

| 数值 | 含义 | 出处 |
|---|---|---|
| 0 / 1 / 2 / 3 | `CURSOR_VALID` / `CURSOR_INVALID` / `CURSOR_SKIPNEXT` / `CURSOR_REQUIRESEEK` | `src/btreeInt.h` |
| 0x02 | `BTCF_ValidNKey` | `src/btreeInt.h` |
| 0x04 | `BTCF_ValidOvfl` | `src/btreeInt.h` |
| 0x08 | `BTCF_AtLast` | `src/btreeInt.h` |
| 20 | `BTCURSOR_MAX_DEPTH`，页栈深度上限 | `src/btreeInt.h` 的 `#define BTCURSOR_MAX_DEPTH 20` |
| 19 | `BtCursor.apPage[]` / `aiIdx[]` 数组长度（`BTCURSOR_MAX_DEPTH-1`） | `src/btreeInt.h` 的 `MemPage *apPage[BTCURSOR_MAX_DEPTH-1];` |
| 8 | 页头内「最右孩子页号」的偏移（`hdrOffset+8`） | `btreeNext` / `moveToRightmost` 里的 `get4byte(&pPage->aData[pPage->hdrOffset+8])` |
| 4 | 内部页单元头 4 字节里存左孩子页号 | `btreePrevious` 里的 `get4byte(findCell(pPage, idx))` |
| 410 / 412 | 故障注入点编号 | `btreeRestoreCursorPosition` 里的 `sqlite3FaultSim(410)`、`btreeNext` / `btreePrevious` 里的 `sqlite3FaultSim(412)` |

## 关键取舍

- **快慢路径分离，外层薄封装 + `SQLITE_NOINLINE` 慢路径。** `sqlite3BtreeNext` / `sqlite3BtreePrevious` 本身只有十几行，把跨页、恢复、损坏检查等复杂逻辑全部推到 `SQLITE_NOINLINE` 的 `btreeNext` / `btreePrevious`。`SQLITE_NOINLINE` 的作用是让慢路径不占用外层的内联空间——顺序扫描时快路径占绝大多数执行次数，内联它收益最大。
- **页栈而非指针链。** 游标位置用「页栈 + 下标」表达，使上溯（`moveToParent`）与下沉（`moveToChild`）成为 O(1) 的数组操作，不需要回溯查找。
- **向前与向后的实现天然不对称，且不对称被保留在代码里。** 向左走取单元头里的左孩子页号，向右走取页头固定位置的最右孩子页号。源码没有试图把两者统一成同一个「取孩子」抽象。
- **首尾定位不走增量路径。** `sqlite3BtreeFirst` / `sqlite3BtreeLast` 直接 `moveToRoot` 再下沉到最左 / 最右叶子，而不是从当前位置一格一格挪过去——代价是一次完整下行，收益是不依赖当前游标位置。
- **末尾状态用「状态位」而不是「位置比较」表达。** `sqlite3BtreeEof` 只看 `eState != CURSOR_VALID`，不需要知道末单元的键值或页号，因此对索引 B-tree（键是任意字节串）同样成立。代价是源码注释里那条 TODO：无法区分「游标本来就没定位」和「表被清空了」。
- **`BTCF_AtLast` 让「重复定位末尾」变成 O(1)。** `sqlite3BtreeLast` 在游标已标记 `BTCF_AtLast` 时直接返回，不重走一遍下行。但任何一次移动都会清掉这个标记——`sqlite3BtreePrevious` 显式清 `BTCF_AtLast`，`sqlite3BtreeNext` 则通过 `moveToRoot` 里的 `pCur->curFlags &= ~(BTCF_AtLast|BTCF_ValidNKey|BTCF_ValidOvfl)` 清除。
- **失效游标不静默报错，而是先尝试恢复位置。** `btreeNext` / `btreePrevious` 在 `eState != CURSOR_VALID` 时先调 `restoreCursorPosition`，只有恢复到 `CURSOR_INVALID` 才返回 `SQLITE_DONE`。这让「B-tree 被重平衡或行被删除后游标失效」的场景对上层透明。
- **损坏检测放在慢路径上，且带故障注入开关。** `sqlite3FaultSim(412)` 命中时把 `pPage->isInit` 清零，随后的 `!pPage->isInit` 判定返回 `SQLITE_CORRUPT_BKPT`。这使损坏分支在测试中可达。
- **`flags` 参数保留但不使用。** `sqlite3BtreeNext` / `sqlite3BtreePrevious` 都保留 `flags` 参数并断言其取值只能是 0 或 1，函数体内用 `UNUSED_PARAMETER( flags )` 显式标注未使用。注释说明这是给 COMDB2 留的接口位置。
- **页缓存引用计数不在本机制内。** 移动过程中页的取与放由 `getAndInitPage` / `releasePageNotNull` 完成，引用计数的实际维护属于 pager 层（I 组）。

## 与主核 / AI 的关系

本机制是主核 B-tree 存储层「顺序扫描」语义的最底层实现：一个 `SELECT * FROM t` 的每一行输出，对应 VDBE 的一次 `OP_Next`，对应一次 `sqlite3BtreeNext`，对应一次 `pCur->ix` 的自增或一次页栈的上溯下沉。

**架构推论：** SQLite 把「页内快路径」与「跨页慢路径」显式分成两个函数、并给慢路径加 `SQLITE_NOINLINE`，这是一条对顺序扫描 workload 极敏感的优化决策——顺序扫描的快路径命中率极高，把它做成可内联的薄壳收益显著，而跨页逻辑只占很小比例，把它踢出内联区可以避免拖慢主路径。主核 `engine/db/storage/` 若实现同类 B-tree，值得照搬「薄壳 + NOINLINE 慢路径」的分层，而不是写成一个大 `switch`。

**架构推论：** 游标位置用「页栈 + 下标」而不是「单元指针」表达，使得「树被重平衡后旧位置失效」这一情形可以统一处理：位置失效时退化为 `CURSOR_REQUIRESEEK`，靠保存的键重新定位（`btreeRestoreCursorPosition` → `btreeMoveto`），而不是试图修补指针。主核的多模态存储层若有类似的「位置失效」场景，采用「失效后按键重定位」比「尝试修复指针」更简单。

**架构推论：** `sqlite3BtreeEof` 只用一个状态位判断末尾、并且明确地把「无法区分末尾与表被清空」记成一条 TODO，说明这个 API 的表达能力是有已知边界的。主核若要把「扫描结束」与「扫描过程中表被清空」区分开，需要在自己的接口里显式带上结束原因，而不是沿用一个布尔值。

**对 AI 辅助读码的意义：** 本机制的错误分支高度依赖 `eState` 的取值——同一个 `restoreCursorPosition` 调用，在 `CURSOR_REQUIRESEEK` 下会真的重新定位，在 `CURSOR_VALID` 下（经宏判定）直接返回 `SQLITE_OK`。读这一段代码时必须先确认 `eState` 的取值来源，否则会把「宏短路」误读成「总是重新定位」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/btree.c | `sqlite3BtreeNext` | 向前移动的外层入口：清 `BTCF_ValidNKey`/`BTCF_ValidOvfl` 与 `info.nSize`；状态非 VALID 走 `btreeNext`；页内 `++pCur->ix` 未越界则叶子页直接返回、非叶子页 `moveToLeftmost`；越界则 `pCur->ix--` 后转 `btreeNext` |
| src/btree.c | `btreeNext` | 向前移动的慢路径（`SQLITE_NOINLINE`）：`restoreCursorPosition` 恢复位置并处理 `CURSOR_SKIPNEXT`/`skipNext>0`；`sqlite3FaultSim(412)` 损坏注入与 `SQLITE_CORRUPT_BKPT`；非叶子页取最右孩子（`hdrOffset+8`）后 `moveToLeftmost`；叶子页用 `do-while` 上溯到根仍无下一条则置 `CURSOR_INVALID` 并返回 `SQLITE_DONE`；上溯后若 `pPage->intKey` 再递归调 `sqlite3BtreeNext(pCur, 0)` |
| src/btree.c | `sqlite3BtreePrevious` | 向后移动的外层入口：清 `BTCF_AtLast`/`BTCF_ValidOvfl`/`BTCF_ValidNKey` 与 `info.nSize`；状态非 VALID、`ix==0`、或当前页非叶子三者任一成立即走 `btreePrevious`；否则 `pCur->ix--` 并返回 `SQLITE_OK` |
| src/btree.c | `btreePrevious` | 向后移动的慢路径（`SQLITE_NOINLINE`）：`restoreCursorPosition` 后处理 `CURSOR_SKIPNEXT` 与 `skipNext<0`；`sqlite3FaultSim(412)` 损坏检查；非叶子页取 `findCell(pPage, idx)` 头 4 字节的左孩子后 `moveToRightmost`；叶子页用 `while( pCur->ix==0 )` 上溯，到根仍无上一条则置 `CURSOR_INVALID` 并返回 `SQLITE_DONE` |
| src/btree.c | `sqlite3BtreeFirst` | 定位最左：`moveToRoot` 成功后 `*pRes = 0` 并 `moveToLeftmost`；`SQLITE_EMPTY` 时 `*pRes = 1` 且返回值改写为 `SQLITE_OK` |
| src/btree.c | `sqlite3BtreeLast` | 定位最右的外层入口：`CURSOR_VALID` 且 `BTCF_AtLast` 已置时直接返回 `*pRes = 0`；否则转 `btreeLast` |
| src/btree.c | `btreeLast` | 定位最右的内部实现（`SQLITE_NOINLINE`）：`moveToRoot` → `moveToRightmost`，成功则置 `BTCF_AtLast`、失败则清之；`SQLITE_EMPTY` 时 `*pRes = 1` 并把返回值改写为 `SQLITE_OK` |
| src/btree.c | `sqlite3BtreeEof` | 末尾判定：`return (CURSOR_VALID!=pCur->eState);`；注释说明涵盖越过最后一条、越过第一条、表为空三种情形，并留有「需改为返回错误码」的 TODO |
| src/btree.c | `moveToLeftmost` | 沿左孩子链下沉到最左叶子：每层取 `get4byte(findCell(pPage, pCur->ix))` 的左孩子页号，`moveToChild` 压栈 |
| src/btree.c | `moveToRightmost` | 沿最右孩子链下沉到最右叶子：每层取 `get4byte(&pPage->aData[pPage->hdrOffset+8])`，`pCur->ix = pPage->nCell`；到底后 `pCur->ix = pPage->nCell-1` |
| src/btree.c | `moveToChild` | 压栈一层：`aiIdx[iPage]=ix`、`apPage[iPage]=pPage`、`iPage++`，`getAndInitPage` 载入子页；子页 `nCell<1` 或 `intKey` 不匹配则 `releasePageNotNull` 并返回 `SQLITE_CORRUPT_PGNO`；`iPage>=(BTCURSOR_MAX_DEPTH-1)` 返回 `SQLITE_CORRUPT_BKPT` |
| src/btree.c | `moveToParent` | 弹栈一层：`ix = aiIdx[iPage-1]`，`pPage = apPage[--iPage]`，`releasePageNotNull(pLeaf)` 释放离开的页 |
| src/btree.c | `moveToRoot` | 回到根页：释放中间层页；`pgnoRoot==0` 置 `CURSOR_INVALID` 并返回 `SQLITE_EMPTY`；根页无单元且非叶子时下取其唯一子页（只可能是页 1）；`skip_init` 之后清 `BTCF_AtLast`/`BTCF_ValidNKey`/`BTCF_ValidOvfl` 并置 `pCur->ix = 0` |
| src/btree.c | `restoreCursorPosition` | 宏：`p->eState>=CURSOR_REQUIRESEEK` 才调 `btreeRestoreCursorPosition`，否则返回 `SQLITE_OK` |
| src/btree.c | `btreeRestoreCursorPosition` | 按 `pCur->pKey`/`pCur->nKey` 调 `btreeMoveto` 重新定位；成功后释放 `pKey`、按 `skipNext` 决定是否置 `CURSOR_SKIPNEXT`；`sqlite3FaultSim(410)` 命中时返回 `SQLITE_IOERR` |
| src/btree.c | `saveCursorPosition` | 保存位置：`saveCursorKey` 存键后 `btreeReleaseAllCursorPages` 释放所有页并置 `CURSOR_REQUIRESEEK`；`BTCF_Pinned` 置位时返回 `SQLITE_CONSTRAINT_PINNED` |
| src/btree.c | `btreeReleaseAllCursorPages` | 释放 `apPage[]` 各层与当前页的引用，`pCur->iPage = -1` |
| src/btree.c | `sqlite3BtreeClearCursor` | 清游标位置：释放 `pKey` 并置 `eState = CURSOR_INVALID` |
| src/btree.c | `sqlite3BtreeCursorIsValidNN` | 返回 `pCur->eState==CURSOR_VALID`；`OP_SeekEnd` / `OP_IfNoHope` 用它判断能否跳过重新定位 |
| src/btreeInt.h | `CURSOR_VALID` / `CURSOR_INVALID` / `CURSOR_SKIPNEXT` / `CURSOR_REQUIRESEEK` | 游标状态常量，取值 0 / 1 / 2 / 3 |
| src/btreeInt.h | `BTCF_ValidNKey` / `BTCF_ValidOvfl` / `BTCF_AtLast` | 游标标志位，取值 0x02 / 0x04 / 0x08；移动时被清除 |
| src/btreeInt.h | `BTCURSOR_MAX_DEPTH` | 宏，值 20，页栈深度上限；`apPage[]`/`aiIdx[]` 长度为 `BTCURSOR_MAX_DEPTH-1` 即 19 |
| src/vdbe.c | `OP_Next` | `rc = sqlite3BtreeNext(pC->uc.pCursor, pOp->p3);` 后 `goto next_tail`；`p3` 是「唯一索引可跳过本指令」的提示位 |
| src/vdbe.c | `OP_Prev` | `rc = sqlite3BtreePrevious(pC->uc.pCursor, pOp->p3);` 后 `goto next_tail` |
| src/vdbe.c | `OP_Rewind` | `rc = sqlite3BtreeFirst(pCrsr, &res);`，`res` 写入 `pC->nullRow` |
| src/vdbe.c | `OP_Last` / `OP_SeekEnd` | `rc = sqlite3BtreeLast(pCrsr, &res);`；`OP_SeekEnd` 先用 `sqlite3BtreeCursorIsValidNN` 判断是否可跳过 |
| src/vdbeaux.c | `sqlite3VdbeSorterRewind` 相关路径 | `sqlite3BtreeFirst(pCur, &res)` 与 `sqlite3BtreeNext(pCur, 0)` 配对，循环条件 `sqlite3BtreeEof(pCur)==0 && ii<BTREE_FDK_RANGE` |

## 参考

- [Database File Format：B-tree 页（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html)
- [The SQLite Bytecode Engine：OP_Next / OP_Prev / B-Tree Cursors（sqlite.org 官方文档）](https://sqlite.org/opcode.html)
- [Architecture of SQLite：B-Tree 一节（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/btree.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/btree.c)
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
