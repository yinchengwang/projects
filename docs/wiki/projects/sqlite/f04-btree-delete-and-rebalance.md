---
project: sqlite
feature: btree-delete-and-rebalance
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeDelete, sqlite3BtreeClearCursor, sqlite3BtreeDropTable]
---

# B-tree 删除与页合并

## 一句话职责

在 B-tree 中删掉一个单元后，把因删除而空出来的空间还给页、把相邻页的内容重新分配或合并，使树的填充度重新落在合法区间内。

## 机制是什么

SQLite 的 B-tree 是页式结构：一页有固定大小的可用空间（`pBt->usableSize`），页内用单元指针数组（cell pointer array）索引单元内容。删掉一个单元会同时释放它的内容区和指针槽，页的空闲量 `nFree` 随之变大。当空闲量超过阈值时，单页就装不满新数据，必须把内容在兄弟页之间重新摊开，必要时减少树的层数。

这条机制由三层构成：

- **单元级删除**：`sqlite3BtreeDelete` 负责把目标单元从页内摘掉，并把该单元的溢出页链归还给空闲页链表。
- **页级平衡**：`balance` 根据页面状态在三种平衡原语之间选择——`balance_quick`（新增兄弟页承接溢出单元）、`balance_deeper`（根页超满时向下加一层）、`balance_nonroot`（在父页的多个子页之间重排内容）。
- **表级清空/删除**：`sqlite3BtreeClearTable` 与 `sqlite3BtreeDropTable` 走的是另一条路径——不是逐个单元删，而是整棵子树递归清空后把根页回收。

本条目覆盖 `src/btree.c` 中这三组符号的行为与调用关系，不覆盖删除语句的 SQL 编译（属于 D07 组），也不覆盖页写回磁盘与日志协议（属于 G 组）。

## 怎么实现的

### 1. `sqlite3BtreeDelete`：删除单个单元

`sqlite3BtreeDelete(BtCursor *pCur, u8 flags)` 是删除路径的入口。函数体开头是一组前置断言，限定了它的工作条件：必须处于写事务 `TRANS_WRITE`（`pBt->inTransaction==TRANS_WRITE`）、库非只读（`pBt->btsFlags & BTS_READ_ONLY` 为 0）、游标带写标志（`pCur->curFlags & BTCF_WriteFlag`）、且没有读冲突（`!hasReadConflicts(...)`）。`flags` 只允许 `BTREE_SAVEPOSITION`（`0x02`）与 `BTREE_AUXDELETE`（`0x04`）两个位，断言 `(flags & ~(BTREE_SAVEPOSITION | BTREE_AUXDELETE))==0` 拦住了其它组合。

**游标状态前置检查**：若 `pCur->eState!=CURSOR_VALID`，当 `eState>=CURSOR_REQUIRESEEK` 时先调 `btreeRestoreCursorPosition` 恢复位置；恢复失败或状态仍非 `CURSOR_VALID` 就直接返回错误码。状态更低时返回 `SQLITE_CORRUPT_PGNO(pCur->pgnoRoot)`。恢复成功后才继续。

**页与单元定位的完整性检查**（这一段是防损坏文件的闸门，不是业务逻辑）：

- `pPage->nCell<=iCellIdx` → 返回 `SQLITE_CORRUPT_PAGE(pPage)`；
- `pPage->nFree<0` 且 `btreeComputeFreeSpace(pPage)` 返回非零 → 返回 `SQLITE_CORRUPT_PAGE(pPage)`；
- `pCell<&pPage->aCellIdx[pPage->nCell]` → 返回 `SQLITE_CORRUPT_PAGE(pPage)`。

**游标位置保存的两条路径**由 `bPreserve` 区分，取值直接对应源码注释里的三种情形：`bPreserve==0` 表示无需保存；`bPreserve==1` 表示用 `CURSOR_REQUIRESEEK` 保存；`bPreserve==2` 表示游标不移动、置 `CURSOR_SKIPNEXT`。

进入保存分支后，判定「删除后是否必然触发再平衡」的条件是三个条件的或：

```
!pPage->leaf
|| (pPage->nFree + pPage->xCellSize(pPage,pCell) + 2) > (int)(pBt->usableSize*2/3)
|| pPage->nCell==1
```

其中 `2/3` 这个阈值来自 `usableSize*2/3`，`usableSize` 本身是页大小减去文件头第 20 字节记录的保留字节数（`usableSize = pageSize - page1[20]`）。第三个条件 `pPage->nCell==1` 的源码注释直接指向回归用例 `dbfuzz001.test`。满足任一条件则调 `saveCursorKey(pCur)` 保存键（失败即返回），游标随后进入 `CURSOR_REQUIRESEEK`；否则 `bPreserve` 置为 `2`，走 `CURSOR_SKIPNEXT` 路径。

**内部节点的删除要借位**：若被删单元所在页不是叶页（`!pPage->leaf`），先调 `sqlite3BtreePrevious(pCur, 0)` 把游标移到「树中小于被删键的最大条目」。源码注释解释了为什么用前一个而不是后一个：前一个条目必然属于被删单元所辖子树，这样删除后做平衡更容易。返回码 `SQLITE_DONE` 在这里被断言排除（`assert( rc!=SQLITE_DONE )`），出错则返回。

**其它游标与 incrblob 的处理**：`pCur->curFlags & BTCF_Multiple` 为真时，调 `saveAllCursors(pBt, pCur->pgnoRoot, pCur)` 保存同表其它游标的位置；若是表 B-tree（`pCur->pKeyInfo==0`）且库上有 incrblob 游标（`p->hasIncrblobCur`），调 `invalidateIncrblobCursors(p, pCur->pgnoRoot, pCur->info.nKey, 0)` 使被删行上的增量 blob 游标失效。

**真正的物理删除**按固定顺序执行：

1. `sqlite3PagerWrite(pPage->pDbPage)` 把页置为可写，失败即返回。
2. `BTREE_CLEAR_CELL(rc, pPage, pCell, info)` —— 这是一个宏，先用 `pPage->xParseCell` 解析出 `info`，若 `info.nLocal!=info.nPayload`（说明单元带溢出页），再调 `clearCellOverflow(pPage, pCell, &info)` 释放溢出链。
3. `dropCell(pPage, iCellIdx, info.nSize, &rc)` 把单元从页内摘除。

`clearCellOverflow` 的细节：溢出页数 `nOvfl` 由 `(pInfo->nPayload - pInfo->nLocal + ovflPageSize-1)/ovflPageSize` 算出，其中 `ovflPageSize = pBt->usableSize - 4`。循环里对每个溢出页号检查 `ovflPgno<2 || ovflPgno>btreePagecount(pBt)`，越界即返回 `SQLITE_CORRUPT_BKPT`（注释说明 0 不是合法页号、页 1 不能作溢出页）。同时检查该页的 pager 引用计数 `sqlite3PagerPageRefcount(pOvfl->pDbPage)!=1`，不为 1 则返回 `SQLITE_CORRUPT_BKPT`——注释说这是为了在 `freePage2` 可能清零页内容（secure-delete 模式）之前先发现问题。正常路径调 `freePage2(pBt, pOvfl, ovflPgno)` 把溢出页还回空闲链表。

`dropCell` 的行为：`nFree` 已经是负数时先 `btreeComputeFreeSpace`；检查 `pc+sz > pBt->usableSize` 越界则 `SQLITE_CORRUPT_BKPT`；调 `freeSpace(pPage, pc, sz)` 回收内容区；`pPage->nCell--`；若 `nCell` 归零则把页头复位（`memset(&data[hdr+1], 0, 4)`、`data[hdr+7] = 0`、内容区起始写回 `pBt->usableSize`，`nFree` 重算为 `usableSize - hdrOffset - childPtrSize - 8`）；否则 `memmove` 把后面的单元指针前移 2 字节，页头单元数改写，`nFree += 2`。

**内部节点的补位**：若被删单元在内部节点，函数继续从叶子页取最右侧单元上提到内部节点替换：先 `sqlite3PagerWrite(pLeaf->pDbPage)`，再 `insertCell(pPage, iCellIdx, pCell-4, nCell+4-(pCell[0]==2), pTmp, n)` 把叶页最右单元插入内部页，最后 `dropCell(pLeaf, pLeaf->nCell-1, nCell, &rc)` 从叶页摘掉它。这里 `pTmp` 是 `pBt->pTmpSpace`。

**触发再平衡**：判断条件写在函数体内：

```
if( pCur->pPage->nFree*3<=(int)pCur->pBt->usableSize*2 ){
    rc = SQLITE_OK;   /* 空闲空间不足 2/3，balance() 必然是空操作 */
}else{
    rc = balance(pCur);
}
```

即 `nFree*3 <= usableSize*2` 时跳过 `balance`，否则调 `balance(pCur)`。若删除发生在内部节点且平衡后游标仍停在更深的页（`pCur->iPage>iCellDepth`），则回退游标到 `iCellDepth` 层并再调一次 `balance(pCur)`——源码注释说明：先平衡叶页，如果平衡走得足够高就能保证内部节点的问题已修正；否则回退游标再平衡内部节点。

**收尾的状态机**：`bPreserve>1`（即 `CURSOR_SKIPNEXT` 路径）时置 `pCur->eState = CURSOR_SKIPNEXT`，若被删单元是页内最后一个则 `pCur->skipNext = -1` 且 `pCur->ix = pPage->nCell-1`，否则 `pCur->skipNext = 1`；同时把 `pCur->info.nSize = 0` 清零，注释说明这是防止后续 `saveCursorPosition` 误信 `pCur->info` 的内容（此时 `pCur->ix` 已指向另一个单元）。`bPreserve` 为 0 或 1 时走 `moveToRoot(pCur)`；若 `bPreserve` 为 1（即需要保位置），额外调 `btreeReleaseAllCursorPages(pCur)` 并置 `pCur->eState = CURSOR_REQUIRESEEK`。`moveToRoot` 返回 `SQLITE_EMPTY` 时改写为 `SQLITE_OK`。

### 2. `balance`：页级平衡的分派器

`balance(BtCursor *pCur)` 用一个 `do { } while( rc==SQLITE_OK )` 循环逐层向上处理。每轮先 `btreeComputeFreeSpace` 修正 `nFree`，然后判断：

- `pPage->nOverflow==0 && pPage->nFree*3<=(int)pCur->pBt->usableSize*2` → 无需再平衡，`break`。源码注释把这两个条件写成「(1) 没有溢出单元；(2) 页上空闲空间少于可用空间的三分之二」。
- 当前页是根页（`iPage==0`）且有溢出单元 → 调 `balance_deeper(pPage, &pCur->apPage[1])` 把根页内容整体下移一层，随后 `pCur->iPage = 1`、游标指向新子页。源码注释还提到此处会先调 `anotherValidCursor(pCur)` 检查同一 B-tree 上是否还有别的有效游标，若有则返回 `SQLITE_CORRUPT_PAGE`——注释解释这是为了防止「两个 SQL 表指向同一棵 B-tree + BEFORE TRIGGER 二次插入」的场景破坏第一个游标的不变量。
- 非根页且 `sqlite3PagerPageRefcount(pPage->pDbPage)>1` → 返回 `SQLITE_CORRUPT_PAGE(pPage)`。注释说明这只可能是「该页是自己的祖先页」，属于损坏。
- 其余情况 → 分派到 `balance_quick` 或 `balance_nonroot`。

`balance_quick` 的触发条件是一组合取（`SQLITE_OMIT_QUICKBALANCE` 未定义时）：`pPage->intKeyLeaf && pPage->nOverflow==1 && pPage->aiOvfl[0]==pPage->nCell && pParent->pgno!=1 && pParent->nCell==iIdx`。`balance_quick` 内部先用 `allocateBtreePage` 分配新页作为右兄弟，用 `rebuildPage` 把溢出单元搬过去，然后构造分隔单元（4 字节页号 + 变长整数键）插进父页。源码注释给出的分隔单元最大长度是 13 字节，这正是 `balance` 里 `u8 aBalanceQuickSpace[13]` 缓冲区大小的来源。

`balance_nonroot` 的分派是 `balance` 里的默认分支：分配 `u8 *pSpace = sqlite3PageMalloc(pCur->pBt->pageSize)` 作为父页溢出单元的暂存区，然后调 `balance_nonroot(pParent, iIdx, pSpace, iPage==1, pCur->hints&BTREE_BULKLOAD)`。上一轮用过的 `pFree` 缓冲区在此时释放（注释说明其内容已被写进真实页或新缓冲区）。

循环末尾统一 `pPage->nOverflow = 0`、`releasePage(pPage)`、`pCur->iPage--`，游标上移一层，下一轮平衡父页。

### 3. `balance_nonroot`：兄弟页之间的内容重排

`balance_nonroot` 是页合并/借位的核心。它最多涉及 3 个页——`#define NN 1`（每侧邻居数）、`#define NB 3`（`NN*2+1`，参与平衡的页总数）。源码注释明确写着 `NB` 自 SQLite 诞生起就是 3，从未测试过其它值。

**第一步：收集兄弟页与分隔单元**。循环从 `pRight`（父页中最右孩子的指针，或某个分隔单元）出发，沿左兄弟方向收集最多 `NB` 个旧页 `apOld[]`，同时把父页上的分隔单元 `apDiv[]` 从父页摘下（`dropCell(pParent, i+nxDiv-pParent->nOverflow, szNew[i], &rc)`）。摘分隔单元时有一个 secure-delete 相关的分支：若 `pBt->btsFlags & BTS_FAST_SECURE` 为真，`dropCell` 会把整个单元清零，源码注释说明此时必须先把分隔单元拷进 `aOvflSpace[]` 缓冲。

**第二步：把全部单元装进一条数组**。`b.apCell[]` 收集兄弟页的所有单元和分隔单元。注释说明：叶页的子指针会被剥离（`leafCorrection` 为 4），非叶页保留（`leafCorrection` 为 0），这样数组里所有单元形态一致。溢出单元按位置插入数组，且有断言 `k==0 || pOld->aiOvfl[k-1]+1==pOld->aiOvfl[k]`（注释标注为 "NOTE 1"）保证多个溢出单元相邻且有序。

**第三步：计算每页能装多少**。`usableSpace = pBt->usableSize - 12 + leafCorrection`——这里的 `12` 是页头的固定开销，`leafCorrection` 补回叶页少掉的子指针空间。然后贪心地把单元从后往前塞进各页（`szNew[]` 记录每页占用，`cntNew[]` 记录每页的起始单元索引）。

**第四步：修正左偏**。源码注释说明上一步的打包「偏向左侧兄弟」，左边几乎总是满的，最右的兄弟可能几乎为空；并强调这一步**不只是优化**：最右兄弟可能完全为空，那样的分布是非法的。循环从 `i=k-1` 往回走，把左右两侧的单元在 `cntNew[i-1]` 边界上来回搬移，直到右侧不再能接纳左侧的单元为止。搬移时的字节数是 `szD + 2` 与 `szR + 2`，那个 `2` 是单元指针槽本身的宽度。

**第五步：分配与复用页**。新页数 `k` 个，前 `nOld` 个尽量复用 `apOld[i]`（调 `sqlite3PagerWrite`），不够的调 `allocateBtreePage` 新分配。复用时会检查 `sqlite3PagerPageRefcount(pNew->pDbPage)!=1+(i==(iParentIdx-nxDiv))`，不符即 `SQLITE_CORRUPT_BKPT`。auto-vacuum 库下新页还要写指针映射 `ptrmapPut(pBt, pNew->pgno, PTRMAP_BTREE, pParent->pgno, &rc)`。

**第六步：重排页号**。源码注释说明这一步是为了让新页号在文件中升序排列，利于操作系统按序预读；排序用 O(N*N) 算法，因为「N 从不超过 NB+2 (5)」，性能不是问题。注释还给出量化收益：当 `NB==3` 时，这个优化让大规模插入和删除的数据库快约 25%。交换通过 `sqlite3PagerRekey` 完成。

**第七步：写回父页与新页**。父页最右孩子指针写为 `apNew[nNew-1]->pgno`；新页之间插入新的分隔单元（`insertCell(pParent, nxDiv+i, pCell, sz, pTemp, pNew->pgno)`）；非叶页还要保证最右新页的右孩子指针继承自原最右旧页。auto-vacuum 库下用 `ptrmapPut` / `ptrmapPutOvflPtr` 修正指针映射。

**第八步：树变矮（balance-shallower）**。若 `isRoot && pParent->nCell==0 && pParent->hdrOffset<=apNew[0]->nFree`——即根页现在没有单元了、只剩一个右孩子——就把唯一子页的内容整体搬进根页，树高减一。源码注释说明这被称为「balance-shallower」子算法，并且强调「子页必须先碎片整理再拷进父页，因为若父页是页 1，它比子页小（要给文件头让出 100 字节），所以所有空闲空间必须提前腾好」。代码正是 `defragmentPage(apNew[0], -1)` 后 `copyNodeContent(apNew[0], pParent, &rc)`，最后 `freePage(apNew[0], &rc)` 回收子页。

**第九步：回收多余旧页**。`for(i=nNew; i<nOld; i++) freePage(apOld[i], &rc);`——这就是「页合并」中真正把多余页还给空闲链表的地方。

### 4. `balance_deeper`：根页超满时向下加一层

`balance_deeper(MemPage *pRoot, MemPage **ppChild)` 在根页有溢出单元时被调。流程是：把根页置为可写（`sqlite3PagerWrite`），用 `allocateBtreePage(pBt,&pChild,&pgnoChild,pRoot->pgno,0)` 分配新子页，`copyNodeContent(pRoot, pChild, &rc)` 把根页内容整体复制过去，auto-vacuum 库下写指针映射 `ptrmapPut(pBt, pgnoChild, PTRMAP_BTREE, pRoot->pgno, &rc)`。然后把根页的溢出单元数组拷到子页（`pChild->nOverflow = pRoot->nOverflow`），`zeroPage(pRoot, pChild->aData[0] & ~PTF_LEAF)` 清空根页，`put4byte(&pRoot->aData[pRoot->hdrOffset+8], pgnoChild)` 把新子页设为根页的右孩子。注释说明这一步把树整体加深一层，随后由 `balance` 的下一轮循环去平衡新子页。

### 5. `sqlite3BtreeClearTable` 与 `sqlite3BtreeDropTable`：整表清空与删除

`sqlite3BtreeClearTable(Btree *p, int iTable, i64 *pnChange)` 清空一张表的全部内容但保留根页。它先 `sqlite3BtreeEnter(p)` 拿互斥锁（并断言 `p->inTrans==TRANS_WRITE`），再 `saveAllCursors(pBt, (Pgno)iTable, 0)` 保存该表上所有游标，然后 `invalidateIncrblobCursors(p, (Pgno)iTable, 0, 1)`，最后调 `clearDatabasePage(pBt, (Pgno)iTable, 0, pnChange)`。函数注释明确写着：若有打开的读游标，本操作会以 `SQLITE_LOCKED` 失败；打开的写游标会被移到表的根。

`clearDatabasePage` 是递归清空。`freePageFlag` 参数兼两职：位 0 表示清空后是否释放该页；高位（`freePageFlag>>1`）是递归深度，用来防止损坏文件把 CPU 栈压爆——深度超过 `BTCURSOR_MAX_DEPTH`（`#define BTCURSOR_MAX_DEPTH 20`）就返回 `SQLITE_CORRUPT_PGNO(pgno)`。递归体遍历页内每个单元：非叶页的单元里存着子页号，递归清空子页（`(freePageFlag+2)|1`）；叶页单元则用 `BTREE_CLEAR_CELL` 释放溢出链。非叶页还要额外递归清空最右孩子指针指向的页（`get4byte(&pPage->aData[hdr+8])`）。最后按 `freePageFlag&1` 决定 `freePage(pPage, &rc)` 还是仅 `zeroPage(pPage, pPage->aData[hdr] | PTF_LEAF)`。

`sqlite3BtreeDropTable(Btree *p, int iTable, int *piMoved)` 是对外包装：`sqlite3BtreeEnter(p)` → `btreeDropTable(p, iTable, piMoved)` → `sqlite3BtreeLeave(p)`。内部函数 `btreeDropTable` 先调 `sqlite3BtreeClearTable(p, iTable, 0)` 清空整棵树，再取根页。auto-vacuum 库下有两条分支：

- 若被删的正是最大根页号（`iTable==maxRootPgno`，`maxRootPgno` 来自 `sqlite3BtreeGetMeta(p, BTREE_LARGEST_ROOT_PAGE, &maxRootPgno)`），直接 `freePage(pPage, &rc)` 回收根页。
- 否则把最大根页号的页搬进 `iTable` 腾出的空位：`btreeGetPage` 取出后调 `relocatePage(pBt, pMove, PTRMAP_ROOTPAGE, 0, iTable, 0)`，再把原最大根页号回收；`*piMoved = maxRootPgno` 记下被搬走的页号。

源码注释解释为什么要搬：auto-vacuum 要求所有根页都排在文件开头，所以删掉一个非末尾的根页后必须把末尾根页补上来，`*piMoved` 供上层更新 schema。搬完后 `maxRootPgno--`，并跳过 `PENDING_BYTE_PAGE(pBt)` 和 `PTRMAP_ISPAGE(pBt, maxRootPgno)` 这两类不可用页号，最后 `sqlite3BtreeUpdateMeta(p, 4, maxRootPgno)` 写回文件头的最大根页号字段。

### 6. `sqlite3BtreeClearCursor`：把游标置为失效

`sqlite3BtreeClearCursor(BtCursor *pCur)` 的函数体只有三行有效操作：`sqlite3_free(pCur->pKey)` 释放已保存的键、`pCur->pKey = 0`、`pCur->eState = CURSOR_INVALID`。它断言调用方持有互斥锁。

真实调用方有两个：`sqlite3BtreeTripAllCursors` 在「使所有游标失效」时对每个游标调它，随后置 `p->eState = CURSOR_FAULT` 并把 `p->skipNext = errCode`；`moveToRoot` 在 `pCur->eState>=CURSOR_REQUIRESEEK` 且状态为 `CURSOR_FAULT` 之外的恢复路径上，也会在 `pCur->eState>=CURSOR_REQUIRESEEK` 时调它来丢弃旧的键。

## 关键取舍

- **源码事实：**再平衡的触发阈值是「空闲空间的三分之二」，判断式在 `sqlite3BtreeDelete` 与 `balance` 中各写一遍：`nFree*3<=usableSize*2`。**架构推论：**这个阈值把「需要重排」和「只是不好看」区分开：页装不满到三分之一时，再平衡是必需的结构维护；否则只是空间利用率的波动。取三分之二而非更紧的阈值，是为了让页在两次平衡之间能吸收更多删除，避免小批量删除引发频繁重排。
- **源码事实：**内部节点删除用「向前一个条目」补位，源码注释明确说明前一个条目必然属于被删单元所辖子树。**架构推论：**用同子树的条目补位，补位后内部节点与叶页的一致性更容易维持，平衡算法的输入状态更规整；改用后一个条目会让子树边界错位，重排时需要额外处理跨子树的情形。
- **源码事实：**删除时用「保存键 + `CURSOR_REQUIRESEEK`」而非「保存页内位置」来保住游标位置；`saveCursorKey` 对表 B-tree 只存 rowid，对索引 B-tree 则完整拷贝键内容并额外分配 `9+8` 字节的填充。**架构推论：**保存键比保存页号更耐结构变化：页合并与页号重排会让页内位置失效，而键可以重新定位。索引树必须完整拷贝键，是因为索引键是变长记录且恢复时会被反序列化，多留的字节是给解码越界留的余量。
- **源码事实：**`balance_nonroot` 的左偏修正是源码注释标注为「不只是优化」的必需步骤。**架构推论：**若不修，最右兄弟可能完全为空；一棵允许存在空兄弟页的树在后续插入时无法保证每个页都有合理填充度，等于把不变形的风险推迟到写路径。
- **源码事实：**页号重排用 O(N*N) 排序，注释给出的理由是「N 从不超过 NB+2 (5)」，并量化收益为「当 `NB==3` 时大约快 25%」。**架构推论：**参与平衡的页数被 `NN`/`NB` 硬性限制在 3，这一小常数换来了可以用最朴素算法实现排序的自由；算法复杂度只在常数规模上起作用，不值得引入更复杂的数据结构。
- **源码事实：**`balance_quick` 是为「整数键叶页仅剩一个溢出单元且该单元位于页尾」这一特例开的旁路，它复用 `balance` 栈上的 13 字节缓冲。**架构推论：**这是把常见情形从通用重排算法里剥出来的快速路径；13 字节这个尺寸直接来自源码注释给出的分隔单元上界（4 字节页号 + 至多 9 字节变长整数），不是经验取值。
- **源码事实：**清空整表走 `clearDatabasePage` 的递归而非逐个 `sqlite3BtreeDelete`，且递归深度上限 `BTCURSOR_MAX_DEPTH` 为 20。**架构推论：**整表清空是批量操作，逐条删除的常数开销与日志量都不可接受；深度上限则是针对「损坏文件构造出环」这一类输入的兜底，20 层足够覆盖正常 B-tree 高度，又不至于压爆栈。
- **源码事实：**`sqlite3BtreeDelete` 内部对页和单元位置做了三处 `SQLITE_CORRUPT_PAGE` 检查，对溢出页号做了 `SQLITE_CORRUPT_BKPT` 检查。**架构推论：**数据库文件在 SQLite 的威胁模型里是不可信输入，删除路径直接按字节读取偏移量，任何越界都可能造成越权内存访问，所以每一步位置计算后都要复核边界。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/btree.c` | `sqlite3BtreeDelete` | 删除单个单元的主入口：游标状态检查、位置保存策略、内部节点补位、溢出链释放、触发 `balance` 并收尾游标状态。 |
| `src/btree.c` | `balance` | 页级平衡的分派器：按页面状态在 `balance_quick` / `balance_deeper` / `balance_nonroot` 之间选择，并逐层向上处理父页。 |
| `src/btree.c` | `balance_nonroot` | 兄弟页之间的内容重排：收集单元、计算每页容量、修正左偏、分配/复用页、重排页号、写回父页、变矮与回收旧页。 |
| `src/btree.c` | `balance_quick` | 特例快速路径：分配新右兄弟页承接溢出单元并向父页插入分隔单元。 |
| `src/btree.c` | `balance_deeper` | 根页超满时向下加一层：根页内容整体复制到新子页，根页清空并指向新子页。 |
| `src/btree.c` | `balance_cleanup` | `balance_nonroot` 的统一清理出口：释放栈上缓冲并归还所有新旧页引用。 |
| `src/btree.c` | `dropCell` | 从页内摘除一个单元：回收内容区、前移单元指针、修正 `nFree` 与页头单元数。 |
| `src/btree.c` | `clearCellOverflow` | 释放单元的溢出页链：按 `usableSize-4` 算出页数并逐页归还空闲链表。 |
| `src/btree.c` | `BTREE_CLEAR_CELL` | 宏：解析单元大小，若带溢出则调用 `clearCellOverflow`，否则直接置 `SQLITE_OK`。 |
| `src/btree.c` | `sqlite3BtreeClearTable` | 清空一张表的全部内容但保留根页；保存游标、失效 incrblob 游标后递归清空。 |
| `src/btree.c` | `clearDatabasePage` | 递归清空页及其子页，受 `BTCURSOR_MAX_DEPTH` 限制深度，按标志决定释放或清零。 |
| `src/btree.c` | `sqlite3BtreeDropTable` | 删除整张表的对外包装：加锁后转调 `btreeDropTable`。 |
| `src/btree.c` | `btreeDropTable` | 删表实现：清空后回收根页；auto-vacuum 下把最大根页搬进空位并更新文件头最大根页号。 |
| `src/btree.c` | `sqlite3BtreeClearCursor` | 释放游标已保存的键并把游标状态置为 `CURSOR_INVALID`。 |
| `src/btree.c` | `saveCursorKey` | 保存游标当前键：表 B-tree 存 rowid，索引 B-tree 完整拷贝键内容并留出解码余量。 |
| `src/btree.c` | `saveAllCursors` | 保存同一棵树上其它游标的位置，删除/插入前调用。 |
| `src/btree.c` | `freePage2` | 把页归还空闲链表：递增文件头空闲页计数、secure-delete 下清零页内容、auto-vacuum 下写指针映射。 |
| `src/btree.c` | `defragmentPage` | 碎片整理页，根页变矮前必须先整理子页。 |
| `src/btree.c` | `copyNodeContent` | 把一页的内容整体复制到另一页并修正指针映射。 |

## 与主核 / AI 的关系

**架构推论：**本条目覆盖的是「删除造成的结构修复」，主核 `engine/db/` 的存储层是否需要同一套机制，取决于它自己的页格式与并发模型：SQLite 这套平衡算法建立在「页内无并发写者、B-tree 节点独占」的前提上，主核若存在页内并发写或采用不同的节点分裂策略，删除路径的再平衡不能直接照搬。

**架构推论：**`sqlite3BtreeDelete` 里「先判断是否必然再平衡，再决定用 `CURSOR_REQUIRESEEK` 还是 `CURSOR_SKIPNEXT`」的做法可以迁移为主核的游标失效策略参考——按后续操作是否会破坏位置来分级处理游标，而不是一律失效或一律保留。

**架构推论：**`balance_nonroot` 把「收集 → 容量计算 → 偏斜修正 → 分配 → 回写」拆成可独立验证的阶段，主核若自研页合并逻辑，可以照此切分阶段并对每阶段单独做属性测试。

## 参考

- [SQLite 官方文档：Database File Format](https://sqlite.org/fileformat2.html) —— 说明 B-tree 页布局、页大小与可用空间（usable size）、空闲页链表（freelist）与指针映射页的磁盘格式。
- [SQLite 官方源码：src/btree.c](https://github.com/sqlite/sqlite/blob/master/src/btree.c) —— 对照 `sqlite3BtreeDelete`、`balance`、`balance_nonroot`、`balance_quick`、`balance_deeper`、`sqlite3BtreeClearTable` 与 `sqlite3BtreeDropTable` 的完整实现。
- [SQLite 官方文档：Architecture of SQLite](https://sqlite.org/arch.html) —— 说明 B-tree 层在 VDBE 与分页器之间的定位与职责边界。
