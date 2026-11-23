---
project: postgres
feature: expression-evaluation
page_type: feature
schema_version: 1
refs:
  - path: src/backend/executor/execExpr.c
    symbols: [ExecInitExpr, ExecInitExprRec]
  - path: src/backend/executor/execExprInterp.c
    symbols: [ExecInterpExpr, ExecReadyInterpretedExpr]
---

# 表达式求值

## 一句话职责

表达式求值把表达式语法树编译成线性求值步骤序列，并在运行期按 step 逐条执行得到 Datum/NULL 结果。

## 机制是什么

`ExprState` 是表达式的执行形态：它保存步骤数组、结果 Datum 与 isnull 指针、可选的 resultslot、求值函数指针 `evalfunc` 及编译期残留信息。`ExprEvalStep` 是单条指令：`opcode` 指明操作，`resvalue`/`resnull` 指明结果写到哪，联合体 `d` 携带该操作的内联参数（slot 类型、attnum、函数 FmgrInfo、跳转目标等）。

编译阶段完全只读表达式树，一个 `ExprState` 可被多次执行，但不能被并发复用，因为执行会改写步骤内的运行期缓存。本页讲“树 → 步骤序列 → 解释执行”这一路径；JIT 生成代码的独立机制不在此展开，`ExecReadyExpr` 只在其编译失败/未启用时才落到解释器。表达式求值本身不决定 SQL 语义（如短路规则由编译器选择的操作码与跳转结构表达），也不负责扫描或连接。

## 怎么实现的

1. `ExecInitExpr` 对 NULL 表达式直接返回 NULL `ExprState`；否则新建 `ExprState`，挂上原表达式与所属 `PlanState`，先调用 `ExecCreateExprSetupSteps` 预扫描整棵树，为所有 Var 引用生成所需的 `EEOP_*_FETCHSOME` 步骤。这一步把 slot 的按需解构提到求值之前。
2. 随后 `ExecInitExprRec` 递归遍历表达式，把 `resv`/`resnull` 作为本节点的结果落点，按节点类型选择 opcode 并填充参数后 `ExprEvalPushStep`。`ExprEvalPushStep` 以倍增方式扩容步骤数组，因此构建期间不能持有指向该数组的指针。
3. 递归处理典型节点：Var 依 `varno` 区分 inner/outer/scan/old/new，依 attnum 区分普通列、系统列、整行；Const 直接存值；Param 区分钟 PARAM_EXEC 与 PARAM_EXTERN；函数调用统一走 `ExecInitFunc` 生成参数子步骤加 `EEOP_FUNCEXPR*`。布尔 AND/OR 拆成一到多条分步 opcode，用 `jumpdone` 表达短路目标。
4. 编译末尾追加 `EEOP_DONE_RETURN`（或 qual 场景的 `EEOP_DONE_NO_RETURN`），然后 `ExecReadyExpr` 选择执行后端：若 JIT 编译成功即返回，否则 `ExecReadyInterpretedExpr`。
5. `ExecReadyInterpretedExpr` 做一次性初始化：断言最后一步是 DONE，把 `evalfunc` 先设为 `ExecInterpExprStillValid`，对极短且形状固定的常见表达式直接选专用快路径函数；启用 computed goto 时把每个 step 的 opcode 就地改写成跳转地址并置 `EEOP_DIRECT_THREADED` 标志；否则 `evalfunc` 设为通用 `ExecInterpExpr`。
6. 首次求值经 `ExecInterpExprStillValid`：它调用 `CheckExprStillValid` 校验步骤中缓存的 attnum/slot 类型与当前 slot 描述符仍匹配（防止计划生成后 schema 变更），然后把 `evalfunc` 换成真正的执行函数，后续调用不再重复校验。
7. `ExecInterpExpr` 逐个执行步骤：依据 opcode 从 `econtext` 的对应 slot 取 Var/系统列，调用函数体，写回 `resvalue`/`resnull`，按 `EEOP_JUMP*`/`EEOP_QUAL` 跳转，最终由 DONE 步骤返回结果。`EEOP_QUAL` 是给 `ExecQual` 用的简化短路：false 或 NULL 都视为不通过。

## 关键取舍

- **编译成线性步骤而非在树上递归解释。** 源码事实：`ExprState.steps` 是扁平的 `ExprEvalStep` 数组，跳转用下标表示。放弃保留树形结构带来的直观性，换取求值热路径去掉指针追逐与 switch-per-node；代价是编译期要显式管理跳转回填（如 `ExecInitQual` 里先记录待修正下标再统一改写）。
- **`ExprEvalStep` 把参数内联进联合体，而非指向独立小结构。** 头文件注释写明联合体应控制在约 40 字节，使整个 step 不超过一条 cacheline。放弃通用指针方案的灵活性，换取单步访问的缓存局部性；代价是每种 opcode 的参数布局都必须谨慎复用同一空间，且新增 opcode 受大小预算约束。
- **首次求值做一次 schema 有效性检查后自我替换。** `ExecReadyInterpretedExpr` 安装 `ExecInterpExprStillValid` 而不是直接装执行函数；它校验成功后把 `evalfunc` 换成 `evalfunc_private`。放弃每次求值都检查的安全性，换取热路径零检查；若计划被缓存而表结构在期间被改，检查能把不匹配提前暴露为错误而非静默错值。
- **极短固定形状表达式绕过通用解释器。** 源码对 4 步/5 步的特定 opcode 组合直接选 `ExecJustConst`、`ExecJustScanVarVirt` 等专用函数。放弃“所有表达式走同一路径”的可维护性，换取 hash join 探测键、简单投影这类高频场景免去 dispatch 开销。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/executor/execExpr.c` | `ExecInitExpr` | 建立 ExprState、插入 slot 解构前置步骤、编译主体并追加 DONE 步骤 |
| `src/backend/executor/execExpr.c` | `ExecInitExprRec` | 递归把每种表达式节点翻译成 opcode 与内联参数，并决定结果落点 |
| `src/backend/executor/execExpr.c` | `ExprEvalPushStep` | 按倍增策略追加步骤数组元素，构建期禁止持有数组内指针 |
| `src/backend/executor/execExprInterp.c` | `ExecReadyInterpretedExpr` | 选择解释执行后端、安装首调校验函数并做 opcode 直接线程化 |
| `src/backend/executor/execExprInterp.c` | `ExecInterpExpr` | 逐 step 执行并处理跳转/短路，最终返回 Datum 与 isnull |
| `src/backend/executor/execExprInterp.c` | `ExecInterpExprStillValid` | 首次求值校验 attnum/slot 兼容性后替换为正式执行函数 |
| `src/include/executor/execExpr.h` | `ExprEvalStep`、`ExprEvalOp` | 定义单条指令布局与全部操作码，约束 dispatch 表顺序 |

## 与主核 / AI 的关系

暂无直接对应。若在主核实现表达式执行，可借鉴“编译成扁平指令流 + 首调校验后自我替换”的分层；但指令参数布局与槽位协议需要与自身 slot 抽象一起设计，不能直接照搬 PostgreSQL 的 `varno` 约定。

## 参考

- [PostgreSQL 官方源码：表达式编译](https://github.com/postgres/postgres/blob/master/src/backend/executor/execExpr.c) —— 支撑步骤生成、slot 解构前置与布尔短路结构
- [PostgreSQL 官方源码：解释执行器](https://github.com/postgres/postgres/blob/master/src/backend/executor/execExprInterp.c) —— 支撑快路径选择、首调校验与 dispatch 循环
- [PostgreSQL 官方源码：ExprEvalStep 定义](https://github.com/postgres/postgres/blob/master/src/include/executor/execExpr.h) —— 支撑操作码集合与单步参数布局
