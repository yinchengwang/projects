---
project: sqlite
feature: sql-parser
page_type: feature
schema_version: 1
refs:
  - path: src/parse.y
    symbols: [sqlite3Parser]
---

# 语法分析与语法树

## 一句话职责

`src/parse.y` 是 SQLite 的 Lemon 文法文件，它把词法分析器（B01）切出的记号流按文法归约成语法树——具体产出的节点类型是 `Select`（查询骨架）、`SrcList`（FROM 子句）、`Expr` / `ExprList`（表达式树）、`IdList`（列名清单）、`With` / `Cte`（公用表表达式）、`Window`（窗口定义）、`Upsert`（冲突处理）；它不负责把名字绑到具体对象（B03），也不负责把语法树变成可执行代码（C 组）。

## 机制是什么

SQLite 的语法分析由两部分组成：`src/parse.y` 是**文法与动作**的源文件，`src/tokenize.c` 的 `sqlite3RunParser()` 是**驱动循环**。前者描述「什么记号序列构成什么语句、归约时构造什么节点」，后者负责「按顺序取记号、逐个喂给状态机、遇错收尾」。

`src/parse.y` 用 Lemon（SQLite 自带的 LALR(1) 解析器生成器）语法书写，关键声明如下：

- `%name sqlite3Parser` 声明生成的过程名。`src/tokenize.c` 的 `sqlite3RunParser()` 在其中反复调用 `sqlite3Parser(pEngine, tokenType, pParse->sLastToken)`，每喂一个记号推进一次状态机。
- `%stack_size 50` 给出解析栈的初始槽位数；`%stack_size_limit parserStackSizeLimit` 指定栈上限由函数决定。
- `%token_prefix TK_` 让所有记号常量以 `TK_` 前缀命名；`%token_type {Token}` 与 `%default_type {Token}` 决定记号与非终结符都携带 `Token` 类型的数据。
- `%extra_context {Parse *pParse}` 让所有归约动作都能拿到解析上下文 `pParse`。

文法的主体是一组**产生式**（production rule），每条形如 `cmd ::= ...` 或 `expr(A) ::= ...`，右侧是终结符与非终结符，末尾的大括号里是**归约动作**——真正构造语法树节点的代码就在这些大括号里。因此 `src/parse.y` 同时是「文法表」和「建树代码」两份内容。

非终结符用 `%type` 声明其语义类型（`Expr*`、`Select*`、`SrcList*`、`Token`、`int`、`u32` 等），用 `%destructor` 声明当某个非终结符在解析中途被丢弃（语法错误回退）时如何释放其已构造的节点。例如 `%destructor expr {sqlite3ExprDelete(pParse->db, $$);}` 负责回收表达式节点，`%destructor select {sqlite3SelectDelete(pParse->db, $$);}` 负责回收查询节点。

## 怎么实现的

### 驱动循环：sqlite3RunParser 逐记号喂给状态机

`src/tokenize.c` 的 `sqlite3RunParser(Parse *pParse, const char *zSql)` 是解析的真正入口，其循环体做三件事：

1. 调 `sqlite3GetToken((u8*)zSql, &tokenType)` 取下一个记号，累加进 `mxSqlLen`；`mxSqlLen` 初值取自 `db->aLimit[SQLITE_LIMIT_SQL_LENGTH]`。若 `mxSqlLen<0`，则置 `pParse->rc = SQLITE_TOOBIG` 并 `pParse->nErr++` 后 `break`。
2. 处理三类**不进入状态机**的记号：
   - `TK_SPACE` 直接跳过（`zSql += n; continue;`）。
   - `TK_COMMENT` 在「正在重解析模式」（`db->init.busy`）或 `SQLITE_DBCONFIG_ENABLE_COMMENTS` 打开时跳过。
   - 其余 `TK_SPACE` 以上的记号（`TK_ILLEGAL` 等）若不是 `TK_QNUMBER`，则报 `sqlite3ErrorMsg(pParse, "unrecognized token: \"%T\"", &x)` 后 `break`。
   - `TK_QNUMBER` 是参数占位符，允许进入状态机。
3. 把记号写进 `pParse->sLastToken`，调 `sqlite3Parser(pEngine, tokenType, pParse->sLastToken)` 推进状态机；随后检查 `pParse->rc!=SQLITE_OK` 就 `break`。

输入结束时（`zSql[0]==0`）循环不会立刻停：它先补一个 `TK_SEMI`，再补一个 `0`（EOF），靠文法里的 `ecmd ::= SEMI.` 与 `cmdlist` 规则完成收尾。

解析器栈在**非 amalgamation 构建**下由 `sqlite3ParserAlloc(sqlite3Malloc, pParse)` 分配，分配失败则调 `sqlite3OomFault(db)` 并返回 `SQLITE_NOMEM_BKPT`；在 **amalgamation 构建**下（`src/parse.y` 里 `#ifdef SQLITE_AMALGAMATION` 定义 `sqlite3Parser_ENGINEALWAYSONSTACK`），状态机对象 `yyParser sEngine` 直接放在栈上，只调 `sqlite3ParserInit()`。

### 栈上限：parserStackSizeLimit 直接读运行期上限

`src/parse.y` 里 `parserStackSizeLimit(Parse *pParse)` 的函数体只有一行：`return pParse->db->aLimit[SQLITE_LIMIT_PARSER_DEPTH];`。也就是说栈深上限**不是编译期常量，而是运行期可调**的连接级限制。

对应的编译期硬上限是 `src/sqliteLimit.h` 里的 `SQLITE_MAX_PARSER_DEPTH`，未定义时取 2500；`src/main.c` 的 `aHardLimit[]` 数组里 `SQLITE_LIMIT_PARSER_DEPTH` 对应的位置就是 `SQLITE_MAX_PARSER_DEPTH`。同理 `SQLITE_LIMIT_COMPOUND_SELECT` 对应 `SQLITE_MAX_COMPOUND_SELECT`（未定义时取 500），`SQLITE_LIMIT_COLUMN` 对应 `SQLITE_MAX_COLUMN`（未定义时取 2000）。

栈真正溢出时，`src/parse.y` 的 `%stack_overflow` 块执行 `if( pParse->nErr==0 ) sqlite3ErrorMsg(pParse, "Recursion limit");`——**只在还没有别的错误时才补这条信息**，避免覆盖更精确的诊断。

### 语法错误的两条分支

`src/parse.y` 的 `%syntax_error` 块按当前记号是否为空分成两条路径：

- 若 `TOKEN.z[0]` 非零，调 `parserSyntaxError(pParse, &TOKEN)`；该函数体是 `sqlite3ErrorMsg(pParse, "near \"%T\": syntax error", p)`。
- 若 `TOKEN.z[0]` 为空（即输入在记号中途结束），调 `sqlite3ErrorMsg(pParse, "incomplete input")`。

注意 `src/parse.y` 里还显式定义了 `#define YYNOERRORRECOVERY 1`，注释写明用途是「Disable all error recovery processing in the parser push-down automaton」。也就是说**语法错误后不做错误恢复**，一次报错即终止整条语句的解析。

### 表达式树：tokenExpr 造叶子，sqlite3PExpr 造内部节点

`expr` / `term` 是表达式树的两层非终结符：`expr(A) ::= term(A).` 是入口，而 `term` 承载原子。

**叶子节点**由 `src/parse.y` 里的 `tokenExpr(Parse *pParse, int op, Token t)` 构造。函数体做的事：用 `sqlite3DbMallocRawNN(pParse->db, sizeof(Expr)+t.n+1)` 分配一块**同时容纳 Expr 头与 token 文本**的内存（token 字符串直接落在 `p[1]` 之后，省一次分配）；把 `p->op` 设为传入的 `op`、`p->flags` 设为 `EP_Leaf`；把 token 文本拷进 `p->u.zToken` 并补 `\0`；记下 `p->w.iOfst = (int)(t.z - pParse->zTail)`，即该记号相对 SQL 文本起点的偏移，用于错误定位；最后若首字符是引号（`sqlite3Isquote(p->u.zToken[0])`）则调 `sqlite3DequoteExpr(p)` 去掉引号。在 `SQLITE_MAX_EXPR_DEPTH>0` 的编译下还把 `p->nHeight` 初始化为 1。

`tokenExpr` 还有一条**重命名模式**分支：若 `IN_RENAME_OBJECT` 为真，`return (Expr*)sqlite3RenameTokenMap(pParse, (void*)p, &t);`——即把新节点直接交给重命名映射表接管。

`IN_RENAME_OBJECT` 定义在 `src/sqliteInt.h`：`#define IN_RENAME_OBJECT (pParse->eParseMode>=PARSE_MODE_RENAME)`，即处于 ALTER TABLE RENAME 的重解析模式时为真（`SQLITE_OMIT_ALTERTABLE` 下恒为 0）。

**内部节点**由 `sqlite3PExpr(pParse, op, pLeft, pRight)` 构造，二元运算符都走它：`expr(A) ::= expr(A) OR(OP) expr(Y).` 归约成 `A=sqlite3PExpr(pParse,@OP,A,Y);`，`AND` 走 `sqlite3ExprAnd(pParse,A,Y)`，`EQ`/`NE`/`LT`/`GT`/`GE`/`LE`/`PLUS`/`MINUS`/`STAR`/`SLASH`/`REM`/`CONCAT`/位运算各走同一条 `sqlite3PExpr` 路径。

**运算符优先级**由文法顶部的 `%left` / `%right` / `%nonassoc` 声明决定，顺序即优先级从低到高：

```
%left OR.
%left AND.
%right NOT.
%left IS MATCH LIKE_KW BETWEEN IN ISNULL NOTNULL NE EQ.
%left GT LE LT GE.
%right ESCAPE.
%left BITAND BITOR LSHIFT RSHIFT.
%left PLUS MINUS.
%left STAR SLASH REM.
%left CONCAT PTR.
%left COLLATE.
%right BITNOT.
%nonassoc ON.
```

其中 `IS NOT DISTINCT FROM` 复用 `TK_IS`、`IS DISTINCT FROM` 复用 `TK_ISNOT`——`src/parse.y` 的 `expr(A) ::= expr(A) IS NOT DISTINCT FROM expr(Y).` 归约成 `A = sqlite3PExprIs(pParse, TK_IS, A, Y);`，而 `expr(A) ::= expr(A) IS DISTINCT FROM expr(Y).` 归约成 `A = sqlite3PExprIs(pParse, TK_ISNOT, A, Y);`。两个 DISTINCT FROM 变体都**不走 `NOT` 包裹**，与 `IS NOT` 的常规路径不同。

一元正负号有专门的折叠规则：`expr(A) ::= PLUS|MINUS(B) expr(X).` 里的 `u8 op = @B + (TK_UPLUS-TK_PLUS);`，并断言 `TK_UMINUS == TK_MINUS + (TK_UPLUS - TK_PLUS)`。若被包裹的表达式本身就是 `TK_UPLUS`，则**直接改写其 op**（`p->op = op; A = p;`），不新建节点；否则才调 `sqlite3PExpr(pParse, op, p, 0)`。

### 函数调用与聚合

`expr(A) ::= idj(X) LP distinct(D) exprlist(Y) RP.` 归约成 `A = sqlite3ExprFunction(pParse, Y, &X, D);`。`distinct` 非终结符把 `DISTINCT` 映射成 `SF_Distinct`、`ALL` 映射成 `SF_All`、缺省映射成 0——这三个常量定义在 `src/sqliteInt.h`：`SF_Distinct` 为 `0x0000001`，`SF_All` 为 `0x0000002`。

`count(*)` 形式的 `expr(A) ::= idj(X) LP STAR RP.` 走 `A = sqlite3ExprFunction(pParse, 0, &X, 0);`——参数表传 0。

`WITHIN GROUP`（有序集聚合）在 `SQLITE_ENABLE_ORDERED_SET_AGGREGATES` 下由 `sqlite3ExprAddOrderedsetFunction()` 处理：它把 ORDER BY 表达式**追加到参数列表最前面**，再调 `sqlite3ExprFunction()` 生成一个普通函数节点，随后做两项校验——用 `sqlite3FindFunction(pParse->db, pExpr->u.zToken, -2, enc, 0)` 查函数定义，若 `pDef==0` 或该函数没有 `SQLITE_SELFORDER1` 标志则报 `"%#T() is not an ordered-set aggregate"`；若 `isDistinct==SF_Distinct` 则报 `"DISTINCT not allowed on ordered-set aggregate %T()"`。

### IN 运算符：三种形态，三种建树方式

`expr(A) ::= expr(A) in_op(N) LP exprlist(Y) RP.` 的归约体按 `Y` 的形态分三条路：

1. **空列表**（`Y==0`）：`expr1 IN ()` 简化为常量 0、`expr1 NOT IN ()` 简化为常量 1。源码注释写明了例外条件——若 `A` 带 `EP_HasFunc` 属性则不做此优化，改为 `A = sqlite3PExpr(pParse, N ? TK_OR : TK_AND, pB, A);`，注释解释原因是该函数**可能是聚合函数**（此时还没确定），而这会改变整个查询的语义。
2. **单元素常量列表**：当 `Y->nExpr==1` 且 `sqlite3ExprIsConstant(pParse,pRHS)` 为真、`ExprHasProperty(pRHS, EP_Collate)==0` 且 `A->op!=TK_VECTOR` 时，把右值包成 `TK_UPLUS` 再构造 `TK_EQ`——即**常量 IN 列表降级为等值比较**。
3. **单元素子查询**（`pRHS->op==TK_SELECT`）：构造 `TK_IN` 节点后用 `sqlite3PExprAddSelect()` 挂上子查询。
4. **其他**：构造 `TK_IN` 节点；若左值是 `TK_VECTOR`（即 `expr(A) ::= LP nexprlist(X) COMMA expr(Y) RP.` 构造的行值），则用 `sqlite3ExprListToValues(pParse, nExpr, Y)` 把列表转成 VALUES 查询再挂上去；否则把参数表直接赋给 `A->x.pList`。

`NOT IN` 统一在最外层包裹：`if( N ) A = sqlite3PExpr(pParse, TK_NOT, A, 0);`。

### CASE 表达式

`expr(A) ::= CASE case_operand(X) case_exprlist(Y) case_else(Z) END.` 归约成 `A = sqlite3PExpr(pParse, TK_CASE, X, 0);`，随后若 `Z` 非空则 `A->x.pList = sqlite3ExprListAppend(pParse,Y,Z);`，否则 `A->x.pList = Y;`，最后调 `sqlite3ExprSetHeightAndFlags(pParse, A);`。

`case_exprlist` 是**扁平的 ExprList**，每条 `WHEN ... THEN ...` 往里追加两个元素（先 WHEN 表达式，再 THEN 表达式）——源码的追加顺序是 `A = sqlite3ExprListAppend(pParse,A, Y);`（WHEN）紧跟 `A = sqlite3ExprListAppend(pParse,A, Z);`（THEN）。`case_else` 为空时 `A = 0;`，此时不追加。

### SELECT：oneselect 汇总成 Select 节点

`oneselect(A) ::= SELECT distinct(D) selcollist(W) from(X) where_opt(Y) groupby_opt(P) having_opt(Q) orderby_opt(Z) limit_opt(L).` 的归约体只有一句：`A = sqlite3SelectNew(pParse,W,X,Y,P,Q,Z,D,L);`。`sqlite3SelectNew` 的原型在 `src/sqliteInt.h`：`Select *sqlite3SelectNew(Parse*,ExprList*,SrcList*,Expr*,ExprList*,Expr*,ExprList*,u32,Expr*);`——九个参数依次对应结果列表、FROM 源表、WHERE、GROUP BY、HAVING、ORDER BY、DISTINCT 标志、LIMIT。

窗口函数版本 `oneselect(A) ::= SELECT ... window_clause(R) orderby_opt(Z) limit_opt(L).` 在 `sqlite3SelectNew()` 之外额外把 `A->pWinDefn = R;`，若 `sqlite3SelectNew()` 返回 0 则 `sqlite3WindowListDelete(pParse->db, R);` 回收。

`SELECT *` 与 `tbl.*` 走 `selcollist` 的两条专门规则：`selcollist(A) ::= sclp(A) scanpt STAR(X).` 构造一个 `TK_ASTERISK` 节点（`sqlite3Expr(pParse->db, TK_ASTERISK, 0)`）；`selcollist(A) ::= sclp(A) scanpt nm(X) DOT STAR(Y).` 则构造 `TK_ASTERISK` 右节点 + `TK_ID` 左节点 + `TK_DOT` 组合。

结果列的别名与源码区间由 `selcollist(A) ::= sclp(A) scanpt(B) expr(X) scanpt(Z) as(Y).` 处理：追加表达式后，若 `Y.n>0` 则 `sqlite3ExprListSetName(pParse, A, &Y, 1);`，再 `sqlite3ExprListSetSpan(pParse,A,B,Z);`。`scanpt` 非终结符捕获的是「刚移入的那个记号的起始指针」，因此 `B` 与 `Z` 合起来正好框住该结果列的源码文本。

### FROM 子句：seltablist 构造 SrcList

`seltablist(A) ::= stl_prefix(A) nm(Y) dbnm(D) as(Z) on_using(N).` 归约成 `A = sqlite3SrcListAppendFromTerm(pParse,A,&Y,&D,&Z,0,&N);`。带 `INDEXED BY` 的变体额外调 `sqlite3SrcListIndexedBy(pParse, A, &I);`；带表函数参数的变体额外调 `sqlite3SrcListFuncArgs(pParse, A, E);`。

连接类型由 `joinop` 非终结符给出：`joinop(X) ::= COMMA|JOIN.` 给出 `JT_INNER`；`JOIN_KW(A) JOIN.`、`JOIN_KW(A) nm(B) JOIN.`、`JOIN_KW(A) nm(B) nm(C) JOIN.` 分别调 `sqlite3JoinType(pParse,&A,0,0)`、`sqlite3JoinType(pParse,&A,&B,0)`、`sqlite3JoinType(pParse,&A,&B,&C)`。

FROM 子句末尾还会调 `sqlite3SrcListShiftJoinType(pParse,A);`（在 `from(A) ::= FROM seltablist(X).` 的归约体里）。

### 复合 SELECT：parserDoubleLinkSelect 串链并查项数

`selectnowith(A) ::= selectnowith(A) multiselect_op(Y) oneselect(Z).` 的归约体把右操作数挂到左操作数的 `pPrior` 上：先 `pRhs->op = (u8)Y;`，再 `pRhs->pPrior = pLhs;`，并清掉两侧的 `SF_MultiValue` 标志；若 `Y!=TK_ALL` 则置 `pParse->hasCompound = 1;`。

`parserDoubleLinkSelect(Parse *pParse, Select *p)` 负责把链首尾接起来并做两项检查：沿 `pPrior` 链遍历，给每个节点打上 `SF_Compound` 标志，同时**检查 ORDER BY / LIMIT 的位置**——若某个节点自带 `pOrderBy` 或 `pLimit`，报 `"%s clause should come after %s not before"`；然后统计链长 `cnt`，若复合项数超过 `pParse->db->aLimit[SQLITE_LIMIT_COMPOUND_SELECT]` 则报 `"too many terms in compound SELECT"`。注意 `cnt>mxSelect` 的判断还额外要求 `(p->selFlags & (SF_MultiValue|SF_Values))==0`，即多行 VALUES 不按复合项计数。

### CTE：with 非终结符与 attachWithToSelect

`select(A) ::= WITH wqlist(W) selectnowith(X).` 与 `WITH RECURSIVE` 变体都归约成 `A = attachWithToSelect(pParse,X,W);`。该辅助函数的函数体：若 `pSelect` 非空则挂上 `pSelect->pWith = pWith;` 并调 `parserDoubleLinkSelect(pParse, pSelect);`；若 `pSelect` 为空则调 `sqlite3WithDelete(pParse->db, pWith);` 回收。

`with ::= WITH wqlist(W).` 归约成 `sqlite3WithPush(pParse, W, 1);`。CTE 体由 `wqitem(A) ::= withnm(X) eidlist_opt(Y) wqas(M) LP select(Z) RP.` 归约成 `A = sqlite3CteNew(pParse, &X, Y, Z, M);`，其中 `wqas` 把 `AS` 映射为 `M10d_Any`、`AS MATERIALIZED` 映射为 `M10d_Yes`、`AS NOT MATERIALIZED` 映射为 `M10d_No`。

### DML：DELETE / UPDATE / INSERT 的入口

- **DELETE**：`cmd ::= with DELETE FROM xfullname(X) indexed_opt(I) where_opt_ret(W).` 归约成 `sqlite3DeleteFrom(pParse,X,W,0,0);`（不带 ORDER BY/LIMIT 的分支）。
- **UPDATE**：`cmd ::= with UPDATE orconf(R) xfullname(X) indexed_opt(I) SET setlist(Y) from(F) where_opt_ret(W).` 归约成 `sqlite3Update(pParse,X,Y,W,R,0,0,0);`，且在归约前调 `sqlite3ExprListCheckLength(pParse,Y,"set list");` 校验 SET 列表长度——该校验读 `pParse->db->aLimit[SQLITE_LIMIT_COLUMN]`，超限报 `"too many columns in %s"`。
- **INSERT**：`cmd ::= with insert_cmd(R) INTO xfullname(X) idlist_opt(F) select(S) upsert(U).` 归约成 `sqlite3Insert(pParse, X, S, F, R, U);`；`DEFAULT VALUES` 变体调 `sqlite3Insert(pParse, X, 0, F, R, 0);`。

**UPDATE 的 ORDER BY / LIMIT 门禁**由 `updateDeleteLimitError()` 实现：它只在 `#if !defined(SQLITE_ENABLE_UPDATE_DELETE_LIMIT) && defined(SQLITE_UDL_CAPABLE_PARSER)` 下编译，函数体按 `pOrderBy` 是否为空分别报 `"syntax error near \"ORDER BY\""` 或 `"syntax error near \"LIMIT\""`，随后 `sqlite3ExprListDelete(pParse->db, pOrderBy);` 与 `sqlite3ExprDelete(pParse->db, pLimit);` 回收。调用方是 `src/parse.y` 里 UPDATE 与 DELETE 的 `cmd` 归约体（`if( O || L ){ updateDeleteLimitError(pParse,O,L); O = 0; L = 0; }`）。

### 关键字回退与通配符

`src/parse.y` 用 `%fallback ID` 声明一大串关键字在**不能按原义解析时退回成标识符**。这条列表以 `ABORT ACTION AFTER ANALYZE ASC ATTACH BEFORE BEGIN BY CASCADE CAST COLUMNKW CONFLICT DATABASE DEFERRED DESC DETACH DO EACH END EXCLUSIVE EXPLAIN FAIL FOR IGNORE IMMEDIATE INITIALLY INSTEAD LIKE_KW MATCH NO PLAN QUERY KEY OF OFFSET PRAGMA RAISE RECURSIVE RELEASE REPLACE RESTRICT ROW ROWS ROLLBACK SAVEPOINT TEMP TRIGGER VACUUM VIEW VIRTUAL WITH WITHOUT NULLS FIRST LAST` 开头，并按编译开关追加 `EXCEPT INTERSECT UNION`（`SQLITE_OMIT_COMPOUND_SELECT` 下）、`CURRENT FOLLOWING PARTITION PRECEDING RANGE UNBOUNDED EXCLUDE GROUPS OTHERS TIES`（`SQLITE_OMIT_WINDOWFUNC` 下）、`WITHIN`（`SQLITE_ENABLE_ORDERED_SET_AGGREGATES` 下）、`GENERATED ALWAYS`（`SQLITE_OMIT_GENERATED_COLUMNS` 未定义时）。源码注释说明了这条规则的目的：`This obviates the need for the "id" non-terminal.`

`%wildcard ANY.` 声明 `ANY` 是通配符记号。

`EXPLAIN` 的处理：两条规则 `explain ::= EXPLAIN.` 与 `explain ::= EXPLAIN QUERY PLAN.` 分别把 `pParse->explain` 设为 1 与 2，且都带 `if( pParse->pReprepare==0)` 前置条件——即重编译时不改写 explain 模式。`ecmd ::= explain cmdx SEMI.` 还标了 `{NEVER-REDUCE}`。

### 记号编号的两条硬约束

`src/parse.y` 末尾用编译期断言守住两件事：

- **记号总数上限**：`%include { #if TK_SPAN>255 # error too many tokens in the grammar #endif }`。源码注释解释：`There must be no more than 255 tokens defined above.` 新增记号必须排在这条线之后。
- **特殊记号必须在最后**：`%token SPACE COMMENT ILLEGAL.` 放在文件最末，注释写明 `The TK_SPACE, TK_COMMENT, and TK_ILLEGAL tokens must be the last three tokens. The parser depends on this.`

另外 `src/parse.y` 用 `%token` 块声明了一批**只给代码生成器用、不出现在文法里的合成记号**：`COLUMN`、`AGG_FUNCTION`、`AGG_COLUMN`、`TRUEFALSE`、`ISNOT`、`FUNCTION`、`UPLUS`、`UMINUS`、`TRUTH`、`REGISTER`、`VECTOR`、`SELECT_COLUMN`、`IF_NULL_ROW`、`ASTERISK`、`SPAN`、`ERROR`。

`REGISTER` 记号与**嵌套解析**有关：`expr(A) ::= VARIABLE(X).` 的归约体检查 `X.z[0]=='#' && sqlite3Isdigit(X.z[1])`，若命中则走嵌套解析分支——`pParse->nested==0` 时报语法错误，否则构造 `TK_REGISTER` 节点并用 `sqlite3GetInt32(&t.z[1], &A->iTable)` 取出寄存器号。源码注释写明：`#N is the N-th register.`

### 类型名扫描：typetoken 用 scanpt 技巧切原文

列类型名不是靠逐记号拼接语义，而是**直接切原文**：规则 `typetoken(A) ::= typename(A) LP signed RP(Y).` 的归约体是 `A.n = (int)(&Y.z[Y.n] - A.z);`——用 `&Y.z[Y.n]`（右括号之后的地址）减去类型名起始地址，得到类型名文本的精确长度。`typename(A) ::= typename(A) ids(Y).` 则用 `A.n=Y.n+(int)(Y.z-A.z);` 累加长度。这套 `scanpt` 技巧的原理在 `src/parse.y` 的注释里：把某个短语用两个 `scanpt` 非终结符夹住，就能捕获该短语的输入文本。

`scanpt(A) ::= .` 的归约体是 `A = yyLookaheadToken.z;`（取当前前瞻记号的起始指针），`scantok(A) ::= .` 则取整个记号（`A = yyLookaheadToken;`）。两者都带 `assert( yyLookahead!=YYNOCODE );` 断言。

### CREATE TABLE：列约束与表选项

`create_table ::= createkw temp(T) TABLE ifnotexists(E) nm(Y) dbnm(Z).` 归约成 `sqlite3StartTable(pParse,&Y,&Z,T,0,0,E);`。`createkw(A) ::= CREATE(A).` 的归约体调 `disableLookaside(pParse);`——该函数把 `pParse->disableLookaside` 加一，并 `memset(&pParse->u1.cr, 0, sizeof(pParse->u1.cr));`，注释写明用途是「Disable lookaside memory allocation for objects that might be shared across database connections」。

`create_table_args ::= LP columnlist conslist_opt(X) RP(E) table_option_set(F).` 归约成 `sqlite3EndTable(pParse,&X,&E,F,0);`；`AS select(S)` 变体归约成 `sqlite3EndTable(pParse,0,0,0,S);` 后紧跟 `sqlite3SelectDelete(pParse->db, S);`。

**表选项**只有两种合法取值，都在校验失败时报 `"unknown table option: %.*s"`：
- `table_option(A) ::= WITHOUT nm(X).` 中，`X.n==5 && sqlite3_strnicmp(X.z,"rowid",5)==0` 时取 `TF_WithoutRowid | TF_NoVisibleRowid`，否则报错。长度判断用的是常量 **5**（`"rowid"` 的长度），比较用 `sqlite3_strnicmp`（大小写不敏感）。
- `table_option(A) ::= nm(X).` 中，`X.n==6 && sqlite3_strnicmp(X.z,"strict",6)==0` 时取 `TF_Strict`，否则报错。长度判断用的是常量 **6**（`"strict"` 的长度）。

**列约束**由 `ccons` 系列规则映射到不同构造器：`DEFAULT` 走 `sqlite3AddDefaultValue(pParse,X,A.z,&A.z[A.n]);`（`scantok` 变体）或 `sqlite3AddDefaultValue(pParse,X,A.z+1,Z.z);`（括号变体，跳过 `(`）；`NOT NULL` 走 `sqlite3AddNotNull(pParse, R);`；`PRIMARY KEY` 走 `sqlite3AddPrimaryKey(pParse,0,R,I,Z);`；`UNIQUE` 走 `sqlite3CreateIndex(pParse,0,0,0,0,R,0,0,0,0,SQLITE_IDXTYPE_UNIQUE);`；`CHECK` 走 `sqlite3AddCheckConstraint(pParse,X,A.z,B.z);`；`REFERENCES` 走 `sqlite3CreateForeignKey(pParse,0,&T,TA,R);`；`COLLATE` 走 `sqlite3AddCollateType(pParse, &C);`；`GENERATED ALWAYS AS` / `AS` 走 `sqlite3AddGenerated(pParse,E,0);` 或带类型的 `sqlite3AddGenerated(pParse,E,&TYPE);`。

**外键动作**用位掩码编码：`refarg(A) ::= ON DELETE refact(X).` 给出 `A.value = X; A.mask = 0x0000ff;`，`ON UPDATE` 给出 `A.value = X<<8; A.mask = 0x00ff00;`。`refact` 的取值来自 `OE_SetNull`、`OE_SetDflt`、`OE_Cascade`、`OE_Restrict`、`OE_None` 五个常量。`refargs(A) ::= .` 的默认值是 `OE_None*0x0101`（源码注释标为 `EV: R-19803-45884`）。

### PRAGMA / ATTACH / ALTER TABLE

- `PRAGMA` 的三种形态分别调 `sqlite3Pragma(pParse,&X,&Z,0,0);`、`sqlite3Pragma(pParse,&X,&Z,&Y,0);`、`sqlite3Pragma(pParse,&X,&Z,&Y,0);`。
- `cmd ::= ATTACH database_kw_opt expr(F) AS expr(D) key_opt(K).` 与 `DETACH` 各有对应归约。
- `ALTER TABLE` 的 9 条子规则分别覆盖 `RENAME TO`、`ADD`、`DROP COLUMN`、`RENAME COLUMN`、`DROP CONSTRAINT`、`ALTER COLUMN DROP NOT NULL`、`ALTER COLUMN SET NOT NULL`、`ADD CONSTRAINT`、`ADD CHECK`。

## 关键取舍

**文法与建树代码写在同一个文件里。** `src/parse.y` 的每条产生式末尾的大括号就是构造语法树节点的代码，不是「查表 → 调函数」的间接层。代价是 `src/parse.y` 体积大且不能手改生成物；收益是加一个语法特性只需改一处。

**语法错误不做恢复。** `src/parse.y` 显式定义 `#define YYNOERRORRECOVERY 1`，注释写明用途是禁用解析器下推自动机里的错误恢复处理。一条语句里出现语法错误就整条终止，不尝试跳到下一个分号继续。

**用「捕获原文偏移」而不是逐记号重建文本。** `typetoken` 规则用 `A.n = (int)(&Y.z[Y.n] - A.z);` 直接从输入缓冲区切出类型名文本，`scanpt` / `scantok` 非终结符则负责捕获前瞻记号的起始指针。这套做法让列类型名、结果列别名等文本能原样保留，供 `sqlite3AddDefaultValue()`、`sqlite3AddCheckConstraint()` 这类需要原文的接口使用。

**复合 SELECT 的项数上限查运行期配置而非编译期常量。** `parserDoubleLinkSelect()` 读 `pParse->db->aLimit[SQLITE_LIMIT_COMPOUND_SELECT]`，编译期硬上限 `SQLITE_MAX_COMPOUND_SELECT`（默认 500）只是这个运行期值的上界。

**「IN 列表降级为等值比较」是有条件的优化，且主动放弃含函数的场景。** `expr(A) ::= expr(A) in_op(N) LP exprlist(Y) RP.` 只在右值列表恰有 1 个元素、该元素 `sqlite3ExprIsConstant()` 为真、不带 `EP_Collate`、且左值不是 `TK_VECTOR` 时才降级为 `TK_EQ`。空列表则降级为常量 0/1，但源码注释明确排除含 `EP_HasFunc` 的左值——因为那个函数可能是聚合函数，改写会改变语义。

**`IS NOT DISTINCT FROM` 与 `IS DISTINCT FROM` 复用 `TK_IS` / `TK_ISNOT`，而非常规的 `NOT` 包裹路径。** 两者的归约体分别调 `sqlite3PExprIs(pParse, TK_IS, A, Y);` 与 `sqlite3PExprIs(pParse, TK_ISNOT, A, Y);`，没有额外的 `TK_NOT` 节点——语义差异交给 `TK_IS` / `TK_ISNOT` 自身承担。

**`CREATE` 关键字触发 lookaside 禁用。** `createkw(A) ::= CREATE(A).` 的归约体调 `disableLookaside(pParse);`，源码注释解释原因是「对象可能在多个数据库连接之间共享」，禁用 lookaside 可避免这些对象被缓存分配器跨连接复用。

**表选项的长度判断用硬编码常量而非查表。** `WITHOUT ROWID` 用常量 5 比对 `"rowid"`，`STRICT` 用常量 6 比对 `"strict"`，且都用 `sqlite3_strnicmp` 做大小写不敏感比较；不认识的选项报 `"unknown table option: %.*s"`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/parse.y | `sqlite3Parser` | Lemon 生成的状态机过程名，由 `src/tokenize.c` 的 `sqlite3RunParser()` 逐记号调用推进归约 |
| src/parse.y | `parserSyntaxError` | 语法错误分支：报 `near "%T": syntax error`；输入在记号中途结束时由 `%syntax_error` 块改报 `incomplete input` |
| src/parse.y | `parserStackSizeLimit` | 解析栈上限函数，函数体返回 `pParse->db->aLimit[SQLITE_LIMIT_PARSER_DEPTH]` |
| src/parse.y | `parserStackRealloc` / `parserStackFree` | 解析栈的 realloc/free 回调，`parserStackRealloc` 内含 `sqlite3FaultSim(700)` 故障注入点 |
| src/parse.y | `parserDoubleLinkSelect` | 复合 SELECT 链接与校验：串 `pPrior` 链、置 `SF_Compound`、检查 ORDER BY/LIMIT 位置、查 `SQLITE_LIMIT_COMPOUND_SELECT` 项数上限 |
| src/parse.y | `attachWithToSelect` | 把 CTE 的 `With` 对象挂到 `Select` 上并调 `parserDoubleLinkSelect()`；`pSelect` 为空时调 `sqlite3WithDelete()` 回收 |
| src/parse.y | `tokenExpr` | 从单个记号造 `Expr` 叶子：一次分配同时容纳 `Expr` 头与 token 文本，填 `EP_Leaf`、记 `w.iOfst` 偏移、首字符为引号时调 `sqlite3DequoteExpr()` |
| src/parse.y | `disableLookaside` | 遇 `CREATE` 关键字时禁用 lookaside 分配并清零 `pParse->u1.cr`，避免共享对象被跨连接复用 |
| src/parse.y | `updateDeleteLimitError` | UPDATE/DELETE 上出现 ORDER BY 或 LIMIT 时的报错：报 `syntax error near "ORDER BY"` 或 `syntax error near "LIMIT"`，并回收表达式 |
| src/parse.y | `sqlite3ExprAddOrderedsetFunction` | `WITHIN GROUP` 有序集聚合：把 ORDER BY 表达式插到参数表最前，查 `SQLITE_SELFORDER1` 标志，不满足则报错 |
| src/parse.y | `scanpt` / `scantok` | 捕获前瞻记号的起始指针 / 整个记号，用于从输入缓冲区精确切出源码片段 |
| src/parse.y | `refarg` / `refact` / `refargs` | 外键动作的位掩码编码：`ON DELETE` 用掩码 `0x0000ff`、`ON UPDATE` 用 `X<<8` 与掩码 `0x00ff00`，缺省值 `OE_None*0x0101` |
| src/tokenize.c | `sqlite3RunParser` | 解析驱动循环：取记号、跳过 `TK_SPACE`/注释、`SQLITE_LIMIT_SQL_LENGTH` 超限报 `SQLITE_TOOBIG`、补 `TK_SEMI` 与 EOF 收尾、调 `sqlite3Parser()` 推进 |
| src/sqliteInt.h | `IN_RENAME_OBJECT` | 重命名模式宏：`pParse->eParseMode>=PARSE_MODE_RENAME`；`SQLITE_OMIT_ALTERTABLE` 下恒为 0 |
| src/sqliteInt.h | `SF_Distinct` / `SF_All` / `SF_Compound` / `SF_Values` / `SF_MultiValue` / `SF_NestedFrom` | `Select.selFlags` 的位标志，取值依次为 `0x0000001`、`0x0000002`、`0x0000100`、`0x0000200`、`0x0000400`、`0x0000800` |
| src/sqliteInt.h | `sqlite3SelectNew` | 九参数构造 `Select` 节点，参数依次为结果列、FROM、WHERE、GROUP BY、HAVING、ORDER BY、DISTINCT 标志、LIMIT |
| src/expr.c | `sqlite3ExprListCheckLength` | SET 列表长度校验：读 `pParse->db->aLimit[SQLITE_LIMIT_COLUMN]`，超限报 `too many columns in %s` |
| src/sqliteLimit.h | `SQLITE_MAX_PARSER_DEPTH` / `SQLITE_MAX_COMPOUND_SELECT` / `SQLITE_MAX_COLUMN` | 编译期硬上限，未定义时依次取 2500、500、2000 |
| src/main.c | `aHardLimit` | 各 `SQLITE_LIMIT_*` 的硬上限表，`SQLITE_LIMIT_PARSER_DEPTH` 位置为 `SQLITE_MAX_PARSER_DEPTH`、`SQLITE_LIMIT_COMPOUND_SELECT` 位置为 `SQLITE_MAX_COMPOUND_SELECT` |

## 与主核 / AI 的关系

**架构推论：** 主核（`engine/db/`）的分层铁律把 `query/` 定为 L3、`spi/` 定为 L3，两者都禁依赖 `modalities/`。SQLite 的 `src/parse.y` 与 `src/resolve.c`（B03）的对应关系说明：语法树构造与名称绑定必须是**两个独立层**，前者只产出「带名字的节点」，后者才把名字绑到目录对象。主核若把二者揉进一层，`query/` 就会直接依赖 `catalog/`，违反依赖方向。

**架构推论：** SQLite 的语法错误处理是「一次报错即终止整条语句」，且 `%syntax_error` 块区分「记号处出错」与「输入不完整」两条错误消息。这对主核的 SQL 引擎适配层有直接含义：AI 生成的 SQL 常在末尾缺分号或括号不配对，若主核只提供一种「语法错误」文案，上层就无法区分「SQL 写错了」与「SQL 被截断了」——而这两种情况的修复建议完全不同。

**架构推论：** SQLite 的错误消息通过 `sqlite3ErrorMsg(pParse, ...)` 统一写入 `Parse` 对象，解析器本身不直接返回错误码，而是靠 `pParse->rc` 与 `pParse->nErr` 两个字段传递状态（`src/tokenize.c` 的 `sqlite3RunParser()` 在循环末尾用 `if( pParse->rc!=SQLITE_OK ) break;` 检查）。主核若采用同样的「解析上下文对象 + 错误累积」模式，就能天然支持「一次解析收集多个错误」而不是遇错即返回——这对 AI 场景下的错误反馈质量有直接影响。

**架构推论：** `src/parse.y` 的 `IN_RENAME_OBJECT` 分支说明 SQLite 把「解析模式」做成了 `Parse.eParseMode` 的一个字段，同一套文法在正常解析与重命名解析下走不同分支。主核的嵌入式 SDK 若要支持 DDL 重命名而不重新实现一遍文法，最省事的路径也是给解析上下文加一个模式字段，而不是复制一套语法规则。

## 参考

- [SQLite 架构图 — sqlite.org 官方文档](https://sqlite.org/arch.html)：官方给出的 SQL 文本经词法分析器、解析器、代码生成器、优化器到 VDBE 的完整链路，界定了解析器在整体管线中的位置。
- [Lemon 解析器生成器 — sqlite.org 官方文档](https://sqlite.org/doc/trunk/doc/lemon.html)：Lemon 的语法记号（`%token`、`%type`、`%left`/`%right`/`%nonassoc`、`%fallback`、`%wildcard`、`%destructor`、`%stack_size` 等）语义说明，是读懂 `src/parse.y` 的语法手册。
- [sqlite/sqlite 仓库 src/parse.y — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/parse.y)：`sqlite3Parser` 的文法定义、`parserDoubleLinkSelect()`、`tokenExpr()`、`parserStackSizeLimit()` 等辅助函数的实现原文。
- [sqlite/sqlite 仓库 src/tokenize.c — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/tokenize.c)：`sqlite3RunParser()` 驱动循环、`sqlite3Parser()` 的调用点与 `SQLITE_LIMIT_SQL_LENGTH` 的检查。
- [sqlite/sqlite 仓库 src/sqliteInt.h — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h)：`SF_*` 标志位、`IN_RENAME_OBJECT` 宏、`sqlite3SelectNew()` 原型与 `Parse` 结构定义。
- [sqlite.org Fossil 源码树 src/parse.y](https://sqlite.org/src/doc/trunk/src/parse.y)：SQLite 官方 Fossil 仓库中 `src/parse.y` 的在线浏览版。
