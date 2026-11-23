# -*- coding: utf-8 -*-
"""SQLite 机制清单（P4 首批三库之一）。

盘点方法：先按 `src/` 的文件骨架导航（SQLite 是单库多文件结构，共 125 个 `.c`），
再对候选机制逐个读源文件的函数定义（`dump_defs.py` 抽顶层定义 + 词边界确认
符号确实在该文件里），最后按机制簇归并成组。

**篇数不是按文件数折算**：btree.c / vdbe.c / select.c 这几个巨型文件各自含
上百个函数，但各自只对应少数几个机制单元；反过来 os.c 这类"薄"文件里的
若干能力各自独立成篇。本清单是逐条盘点后合并/拆分/移组得到的 62 个机制单元。

refs 里的每个 `(path, symbol)` 都已用
`python gen_manifest.py --project sqlite --spec-module sqlite_mechanism_specs --verify`
对固定 checkout 校验通过。

源码锚点：`sqlite@d8371154c887fc190d0d11efc5ec221916fb8911`
"""

GROUP_ORDER = [
    "A-连接与生命周期",
    "B-解析与名称解析",
    "C-表达式与代码生成",
    "D-查询规划与执行",
    "E-虚拟机与语句执行",
    "F-Btree 存储引擎",
    "G-分页器与回滚日志",
    "H-WAL 预写日志",
    "I-页缓存",
    "J-OS 接口与内存管理",
    "K-写入路径与约束",
    "L-内置函数与日期时间",
    "M-事务与并发控制",
    "N-模式演进与附加能力",
]

MECHANISM_SPECS = [
    # ============ A 连接与生命周期 ============
    {
        "feature_group": "A-连接与生命周期",
        "feature": "db-connection-open",
        "title": "数据库连接与打开",
        "page_file": "a01-db-connection-open.md",
        "boundary": "负责一次连接从 URL 解析到连接对象建成的全过程，含 VFS 选择与"
                    "文件打开；不负责语句编译（B 组）、不负责事务状态（M 组）、"
                    "不负责底层字节读写（J 组）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_open_v2", "sqlite3ParseUri"]},
        ],
    },
    {
        "feature_group": "A-连接与生命周期",
        "feature": "db-close",
        "title": "连接关闭与僵尸连接",
        "page_file": "a02-db-close.md",
        "boundary": "负责关闭连接的清理顺序（未 finalize 的语句、事务回滚、"
                    "扩展卸载）以及存在未释放语句时的延迟关闭策略；"
                    "不负责语句本身的释放（E 组）、不负责文件句柄释放的底层实现（J 组）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_close", "sqlite3_close_v2",
                                               "sqlite3LeaveMutexAndCloseZombie"]},
        ],
    },
    {
        "feature_group": "A-连接与生命周期",
        "feature": "global-initialization",
        "title": "全局初始化与关闭",
        "page_file": "a03-global-initialization.md",
        "boundary": "负责库级的一次性初始化（互斥量、内存子系统、页缓存子系统）"
                    "与进程退出时的反初始化；不负责单连接对象的建立（A01）、"
                    "不负责内置函数的注册（L 组）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_initialize", "sqlite3_shutdown"]},
        ],
    },
    {
        "feature_group": "A-连接与生命周期",
        "feature": "schema-cookie",
        "title": "模式版本与缓存失效",
        "page_file": "a04-schema-cookie.md",
        "boundary": "负责模式变更时递增 cookie 值，并让其他连接据此判定自己缓存的"
                    "模式是否过期；不负责模式本身的增删（K/N 组）、"
                    "不负责语句的重新编译（E 组）。",
        "refs": [
            {"path": "src/build.c", "symbols": ["sqlite3ChangeCookie"]},
        ],
    },
    {
        "feature_group": "A-连接与生命周期",
        "feature": "busy-handler",
        "title": "锁冲突忙等待回调",
        "page_file": "a05-busy-handler.md",
        "boundary": "负责拿不到文件锁时调用用户注册的忙等待回调并重试；"
                    "不负责锁本身的获取与释放（M 组）、不负责页缓存的固定页锁（I 组）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_busy_handler", "sqlite3_busy_timeout",
                                               "sqlite3InvokeBusyHandler"]},
        ],
    },
    {
        "feature_group": "A-连接与生命周期",
        "feature": "connection-status-metrics",
        "title": "连接与全局状态统计",
        "page_file": "a06-connection-status-metrics.md",
        "boundary": "负责对外暴露库级与连接级的运行指标（页数、内存用量、锁计数）"
                    "及高水位统计；不负责实际计数点的埋设（散落在各层）、"
                    "不负责内存分配器本身（J 组）。",
        "refs": [
            {"path": "src/status.c", "symbols": ["sqlite3_status", "sqlite3_db_status",
                                                 "sqlite3StatusUp", "sqlite3StatusDown"]},
        ],
    },

    # ============ B 解析与名称解析 ============
    {
        "feature_group": "B-解析与名称解析",
        "feature": "tokenizer",
        "title": "词法分析器",
        "page_file": "b01-tokenizer.md",
        "boundary": "负责把 SQL 文本切分成记号流（标识符、字面量、运算符、注释），"
                    "以及关键字与标识符的区分；不负责语法树的构造（B02）、"
                    "不负责名称到对象的解析（B03）。",
        "refs": [
            {"path": "src/tokenize.c", "symbols": ["sqlite3GetToken", "sqlite3RunParser",
                                                   "sqlite3IsIdChar"]},
        ],
    },
    {
        "feature_group": "B-解析与名称解析",
        "feature": "sql-parser",
        "title": "语法分析与语法树",
        "page_file": "b02-sql-parser.md",
        "boundary": "负责按文法把记号流组装成语法树（含列存表、选择、表达式等节点）；"
                    "不负责词法切分（B01）、不负责把名字绑到具体对象（B03）、"
                    "不负责把语法树变成可执行程序（C 组）。",
        "refs": [
            {"path": "src/parse.y", "symbols": ["sqlite3Parser"]},
        ],
    },
    {
        "feature_group": "B-解析与名称解析",
        "feature": "name-resolution",
        "title": "名称解析",
        "page_file": "b03-name-resolution.md",
        "boundary": "负责把语法树里的表名/列名绑定到模式里的具体对象，处理"
                    "别名与歧义报错；不负责语法树的构造（B02）、"
                    "不负责生成执行代码（C 组）。",
        "refs": [
            {"path": "src/resolve.c", "symbols": ["sqlite3ResolveExprNames",
                                                   "sqlite3ResolveSelectNames",
                                                   "sqlite3ResolveOrderGroupBy"]},
        ],
    },
    {
        "feature_group": "B-解析与名称解析",
        "feature": "prepare-statement",
        "title": "语句编译入口",
        "page_file": "b04-prepare-statement.md",
        "boundary": "负责 prepare 接口的对外语义：把 SQL 文本编译成一条可执行的"
                    "预编译语句，并返回语句句柄；不负责语法分析本身（B02）、"
                    "不负责执行时的虚拟机推进（E 组）。",
        "refs": [
            {"path": "src/prepare.c", "symbols": ["sqlite3_prepare_v2", "sqlite3Prepare"]},
        ],
    },
    {
        "feature_group": "B-解析与名称解析",
        "feature": "schema-load",
        "title": "模式表加载",
        "page_file": "b05-schema-load.md",
        "boundary": "负责启动时从 sqlite_master 读出模式并建好内存中的表/索引对象；"
                    "不负责模式的修改（K/N 组）、不负责语句编译（B04）。",
        "refs": [
            {"path": "src/prepare.c", "symbols": ["sqlite3InitOne", "sqlite3ReadSchema"]},
        ],
    },
    {
        "feature_group": "B-解析与名称解析",
        "feature": "reprepare",
        "title": "模式变更后的语句重编译",
        "page_file": "b06-reprepare.md",
        "boundary": "负责在模式 cookie 变化后把失效的预编译语句自动重新编译，"
                    "以及主动触发重编译的接口；不负责模式加载（B05）、"
                    "不负责语句执行（E 组）。",
        "refs": [
            {"path": "src/prepare.c", "symbols": ["sqlite3Reprepare"]},
        ],
    },

    # ============ C 表达式与代码生成 ============
    {
        "feature_group": "C-表达式与代码生成",
        "feature": "expression-tree",
        "title": "表达式树",
        "page_file": "c01-expression-tree.md",
        "boundary": "负责表达式的树形表示、节点构造与遍历 walker 的注册；"
                    "不负责把表达式变成字节码（C02）、不负责列存表与类型的判定（C03）。",
        "refs": [
            {"path": "src/expr.c", "symbols": ["sqlite3ExprCodeTarget", "sqlite3ExprAffinity",
                                               "sqlite3ExprAnalyzeAggregates"]},
        ],
    },
    {
        "feature_group": "C-表达式与代码生成",
        "feature": "expression-codegen",
        "title": "表达式代码生成",
        "page_file": "c02-expression-codegen.md",
        "boundary": "负责把表达式树翻译成虚拟机的操作码序列（含常量、列值、"
                    "函数调用的取值动作）；不负责表达式树的语义分析（C01）、"
                    "不负责查询的整体规划（D 组）。",
        "refs": [
            {"path": "src/expr.c", "symbols": ["sqlite3ExprCode", "sqlite3ExprCodeGetColumn",
                                               "sqlite3ExprCodeTarget"]},
        ],
    },
    {
        "feature_group": "C-表达式与代码生成",
        "feature": "type-affinity",
        "title": "类型亲和性",
        "page_file": "c03-type-affinity.md",
        "boundary": "负责列与表达式的类型亲和性判定及其在比较、插入时的转换规则；"
                    "不负责具体值的存储表示（E 组）、不负责函数的类型约定（L 组）。",
        "refs": [
            {"path": "src/expr.c", "symbols": ["sqlite3AffinityType", "sqlite3CompareAffinity"]},
            {"path": "src/build.c", "symbols": ["sqlite3AffinityType"]},
        ],
    },
    {
        "feature_group": "C-表达式与代码生成",
        "feature": "subquery-and-in",
        "title": "子查询与 IN 条件",
        "page_file": "c04-subquery-and-in.md",
        "boundary": "负责把子查询与 IN/EXISTS 条件展开成可执行的代码路径"
                    "（相关子查询的逐行求值、IN 列表的物化与短路）；"
                    "不负责外层查询的规划（D 组）、不负责表达式的通用代码生成（C02）。",
        "refs": [
            {"path": "src/expr.c", "symbols": ["sqlite3CodeSubselect", "sqlite3ExprCodeIN",
                                               "sqlite3CodeRhsOfIN"]},
        ],
    },
    {
        "feature_group": "C-表达式与代码生成",
        "feature": "common-table-expression",
        "title": "公用表表达式",
        "page_file": "c05-common-table-expression.md",
        "boundary": "负责 WITH 子句定义的临时命名空间、递归 CTE 的展开方式"
                    "以及 CTE 表的生命周期；不负责子查询的一般表达式路径（C04）、"
                    "不负责物化执行（D 组）。",
        "refs": [
            {"path": "src/build.c", "symbols": ["sqlite3CteDelete"]},
            {"path": "src/select.c", "symbols": ["sqlite3ExpandSubquery", "sqlite3SelectExpand"]},
        ],
    },
    {
        "feature_group": "C-表达式与代码生成",
        "feature": "virtual-machine-program-build",
        "title": "虚拟机构建",
        "page_file": "c06-virtual-machine-program-build.md",
        "boundary": "负责生成虚拟机的操作码程序（寄存器分配、操作码追加与改写）；"
                    "不负责操作码的执行语义（E 组）、不负责查询规划（D 组）。",
        "refs": [
            {"path": "src/vdbeaux.c", "symbols": ["sqlite3VdbeAddOp0", "sqlite3VdbeAddOp1",
                                                 "sqlite3VdbeAddOp2", "sqlite3VdbeAddOp3",
                                                 "sqlite3VdbeAddOp4"]},
            {"path": "src/build.c", "symbols": ["sqlite3VdbeMakeReady"]},
        ],
    },

    # ============ D 查询规划与执行 ============
    {
        "feature_group": "D-查询规划与执行",
        "feature": "select-compile",
        "title": "SELECT 编译",
        "page_file": "d01-select-compile.md",
        "boundary": "负责把一条 SELECT 语句组织成可执行计划（含 FROM 子句处理、"
                    "结果列定义、排序与聚合的落点）；不负责 where 条件的索引选择（D02）、"
                    "不负责执行期的行推进（E 组）。",
        "refs": [
            {"path": "src/select.c", "symbols": ["sqlite3SelectPrep", "sqlite3Select",
                                                 "sqlite3GenerateColumnNames"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "where-loop-optimization",
        "title": "WHERE 条件的索引选择",
        "page_file": "d02-where-loop-optimization.md",
        "boundary": "负责为 WHERE 条件挑选可用的索引并组织成循环计划（含多表连接时"
                    "的顺序选择与代价估算）；不负责表达式求值（C 组）、"
                    "不负责实际索引扫描的实现（F 组）。",
        "refs": [
            {"path": "src/where.c", "symbols": ["sqlite3WhereBegin", "sqlite3WhereEnd",
                                                "sqlite3WhereCodeOneLoopStart",
                                                "sqlite3ShowWhereLoop"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "join-planning",
        "title": "连接顺序与外连接",
        "page_file": "d03-join-planning.md",
        "boundary": "负责多表连接时的连接顺序确定、外连接的语义展开与"
                    "连接结果列的生成；不负责单表索引选择（D02）、"
                    "不负责连接结果的物化（D04）。",
        "refs": [
            {"path": "src/select.c", "symbols": ["sqlite3ProcessJoin", "sqlite3JoinType",
                                                 "sqlite3SelectExpand"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "materialization-and-co-routines",
        "title": "物化与协程式子查询",
        "page_file": "d04-materialization-and-co-routines.md",
        "boundary": "负责子查询结果的物化成临时表，以及用协程把子查询展开成"
                    "逐行推进的执行单元；不负责临时表的存储实现（F 组）、"
                    "不负责聚合函数的实现（L 组）。",
        "refs": [
            {"path": "src/delete.c", "symbols": ["sqlite3MaterializeView"]},
            {"path": "src/select.c", "symbols": ["sqlite3SelectDestInit"]},
            {"path": "src/delete.c", "symbols": ["sqlite3MaterializeView"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "group-by-and-aggregate",
        "title": "分组与聚合",
        "page_file": "d05-group-by-and-aggregate.md",
        "boundary": "负责 GROUP BY/HAVING 的分组键提取与聚合累加器的组织；"
                    "不负责聚合函数本身的实现（L 组）、不负责排序的执行（D06）。",
        "refs": [
            {"path": "src/expr.c", "symbols": ["sqlite3ExprAnalyzeAggList",
                                                "sqlite3ExprAnalyzeAggregates"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "order-by-limit",
        "title": "排序与 LIMIT",
        "page_file": "d06-order-by-limit.md",
        "boundary": "负责 ORDER BY 的排序键表达与 LIMIT/OFFSET 的截断语义"
                    "（含是否可用索引直接满足顺序）；不负责排序器的具体实现"
                    "（F 组 sorter）、不负责索引扫描（F 组）。",
        "refs": [
            {"path": "src/select.c", "symbols": ["sqlite3CopySortOrder",
                                                 "sqlite3ConvertedOrderByColl"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "delete-compile",
        "title": "DELETE 编译",
        "page_file": "d07-delete-compile.md",
        "boundary": "负责 DELETE 语句的编译：定位要删的行、生成删除动作、"
                    "维护变更计数；不负责 B-tree 的实际删除（F 组）、"
                    "不负责外键级联（K 组）。",
        "refs": [
            {"path": "src/delete.c", "symbols": ["sqlite3DeleteFrom", "sqlite3GenerateRowDelete",
                                                 "sqlite3CodeChangeCount"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "update-compile",
        "title": "UPDATE 编译",
        "page_file": "d08-update-compile.md",
        "boundary": "负责 UPDATE 语句的编译：定位待更新行、生成新行记录、"
                    "处理列默认值与生成列；不负责 B-tree 的写入（F 组）、"
                    "不负责触发器（K 组）。",
        "refs": [
            {"path": "src/update.c", "symbols": ["sqlite3Update", "sqlite3ColumnDefault"]},
        ],
    },
    {
        "feature_group": "D-查询规划与执行",
        "feature": "insert-compile",
        "title": "INSERT 编译",
        "page_file": "d09-insert-compile.md",
        "boundary": "负责 INSERT 语句的编译：拼装新行、分配 rowid、"
                    "维护自增序列；不负责 B-tree 的写入（F 组）、"
                    "不负责唯一/外键约束的检查（K 组）。",
        "refs": [
            {"path": "src/insert.c", "symbols": ["sqlite3Insert", "sqlite3CompleteInsertion",
                                                 "sqlite3AutoincrementBegin"]},
        ],
    },

    # ============ E 虚拟机与语句执行 ============
    {
        "feature_group": "E-虚拟机与语句执行",
        "feature": "vdbe-execution",
        "title": "虚拟机主循环",
        "page_file": "e01-vdbe-execution.md",
        "boundary": "负责按操作码逐条推进程序的执行循环（取指、跳转、寄存器读写、"
                    "异常中断）；不负责操作码的生成（C06）、"
                    "不负责语句的对外生命周期（E02）。",
        "refs": [
            {"path": "src/vdbe.c", "symbols": ["sqlite3VdbeExec"]},
        ],
    },
    {
        "feature_group": "E-虚拟机与语句执行",
        "feature": "step-and-finalize",
        "title": "step 与 finalize",
        "page_file": "e02-step-and-finalize.md",
        "boundary": "负责 sqlite3_step 每次推进一行的对外语义，以及 finalize 时"
                    "释放语句资源；不负责虚拟机内部的操作码语义（E01）、"
                    "不负责参数绑定（E04）。",
        "refs": [
            {"path": "src/vdbeapi.c", "symbols": ["sqlite3_step", "sqlite3_finalize",
                                                  "sqlite3_reset"]},
        ],
    },
    {
        "feature_group": "E-虚拟机与语句执行",
        "feature": "column-access",
        "title": "结果列读取",
        "page_file": "e03-column-access.md",
        "boundary": "负责把结果行里的寄存器内容按列号取出并转换为调用方要的"
                    "类型（文本/整数/浮点/Blob）；不负责列的类型定义（C03）、"
                    "不负责参数绑定（E04）。",
        "refs": [
            {"path": "src/vdbeapi.c", "symbols": ["sqlite3_column_int", "sqlite3_column_text",
                                                  "sqlite3_column_type", "sqlite3_column_blob"]},
        ],
    },
    {
        "feature_group": "E-虚拟机与语句执行",
        "feature": "parameter-binding",
        "title": "参数绑定",
        "page_file": "e04-parameter-binding.md",
        "boundary": "负责把调用方绑定的参数写入语句的变量槽位，并处理"
                    "绑定名到序号的解析；不负责参数在表达式中的求值（C 组）、"
                    "不负责结果列读取（E03）。",
        "refs": [
            {"path": "src/vdbeapi.c", "symbols": ["sqlite3_bind_int64", "sqlite3_bind_text",
                                                  "sqlite3_bind_blob", "sqlite3_bind_parameter_index",
                                                  "sqlite3_bind_parameter_count"]},
        ],
    },
    {
        "feature_group": "E-虚拟机与语句执行",
        "feature": "transaction-begin-in-vdbe",
        "title": "虚拟机内的事务与保存点",
        "page_file": "e05-transaction-begin-in-vdbe.md",
        "boundary": "负责语句级事务边界与保存点的建立/释放在虚拟机里的表达；"
                    "不负责文件锁的获取（M 组）、不负责回滚日志的写入（G 组）。",
        "refs": [
            {"path": "src/build.c", "symbols": ["sqlite3BeginTransaction",
                                                "sqlite3BeginWriteOperation"]},
        ],
    },

    # ============ F Btree 存储引擎 ============
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-open-and-cursor",
        "title": "B-tree 打开与游标",
        "page_file": "f01-btree-open-and-cursor.md",
        "boundary": "负责 B-tree 的打开（文件头解析）与游标的建立、移动、失效；"
                    "不负责页的读写落地（G 组/I 组）、不负责索引扫描的规划（D02）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeOpen", "sqlite3BtreeCursor",
                                                 "sqlite3BtreeCloseCursor", "sqlite3BtreeCursorZero"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-cell-access",
        "title": "B-tree 单元与记录访问",
        "page_file": "f02-btree-cell-access.md",
        "boundary": "负责在页内定位单元、解析变长记录（记录头与字段类型串）与"
                    "溢出页的跟随；不负责页的缓存（I 组）、"
                    "不负责索引键的比较语义（D 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreePayload", "sqlite3BtreePayloadChecked",
                                                 "sqlite3BtreePayloadSize", "accessPayload"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-insert",
        "title": "B-tree 插入与单元搬移",
        "page_file": "f03-btree-insert.md",
        "boundary": "负责把新单元插入 B-tree 时的页内搬移、页分裂与平衡；"
                    "不负责页写回磁盘（G 组）、不负责事务可见性（M 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeInsert", "balance_nonroot",
                                                 "sqlite3BtreeClearTable"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-delete-and-rebalance",
        "title": "B-tree 删除与页合并",
        "page_file": "f04-btree-delete-and-rebalance.md",
        "boundary": "负责单元删除后的页内填充、页合并与借位；"
                    "不负责删除动作的编译（D07）、不负责页写回（G 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeDelete", "sqlite3BtreeClearCursor",
                                                 "sqlite3BtreeDropTable"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-cursor-movement",
        "title": "B-tree 游标移动",
        "page_file": "f05-btree-cursor-movement.md",
        "boundary": "负责游标的前后移动、首尾定位与在重定位时的页缓存保持；"
                    "不负责定位到指定键（f06）、不负责页缓存的引用计数（I 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeNext", "sqlite3BtreePrevious",
                                                 "sqlite3BtreeFirst", "sqlite3BtreeLast",
                                                 "sqlite3BtreeEof"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-search",
        "title": "B-tree 键定位",
        "page_file": "f06-btree-search.md",
        "boundary": "负责在 B-tree 中按比较函数定位到目标键所在的单元位置；"
                    "不负责游标的移动（f05）、不负责比较函数本身的语义（C03）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeIndexMoveto", "sqlite3BtreeTableMoveto",
                                                 "btreeMoveto"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-transaction",
        "title": "B-tree 事务与提交",
        "page_file": "f07-btree-transaction.md",
        "boundary": "负责 B-tree 层的事务开启、提交两阶段与回滚；"
                    "不负责分页器层面的日志与提交（G 组）、"
                    "不负责 WAL 的帧写入（H 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeBeginTrans", "sqlite3BtreeCommit",
                                                 "sqlite3BtreeCommitPhaseOne",
                                                 "sqlite3BtreeCommitPhaseTwo",
                                                 "sqlite3BtreeRollback"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "btree-integrity-check",
        "title": "B-tree 完整性检查",
        "page_file": "f08-btree-integrity-check.md",
        "boundary": "负责遍历整棵树核对页结构、单元顺序与父子指针的一致性；"
                    "不负责损坏后的修复、不负责页缓存的强制淘汰（I 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeIntegrityCheck",
                                                 "sqlite3BtreeCount", "sqlite3BtreeRowCountEst"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "autovacuum",
        "title": "自动整理空闲页",
        "page_file": "f09-autovacuum.md",
        "boundary": "负责增量 vacuum 模式下把尾部空闲页搬回文件末尾，"
                    "以及自动触发条件的判定；不负责 VACUUM 命令本身（K/N 组）、"
                    "不负责页的写回（G 组）。",
        "refs": [
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeIncrVacuum", "sqlite3BtreeGetAutoVacuum",
                                                 "sqlite3BtreeSetAutoVacuum"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "sqlite-record-format",
        "title": "记录格式与 varint",
        "page_file": "f10-sqlite-record-format.md",
        "boundary": "负责行记录在页内的字节布局（记录头、序列类型串、字段值）"
                    "与变长整数编码；不负责 B-tree 的单元管理（F03）、"
                    "不负责值的类型亲和性（C03）。",
        "refs": [
            {"path": "src/insert.c", "symbols": ["sqlite3SetMakeRecordP5"]},
        ],
    },
    {
        "feature_group": "F-Btree 存储引擎",
        "feature": "sorter",
        "title": "外排排序器",
        "page_file": "f11-sorter.md",
        "boundary": "负责 ORDER BY 无法走索引时的排序：把待排行写入临时文件"
                    "或内存块、归并读回；不负责排序键的表达（D06）、"
                    "不负责临时文件的 OS 层实现（J 组）。",
        "refs": [
            {"path": "src/vdbesort.c", "symbols": ["sqlite3VdbeSorterInit", "sqlite3VdbeSorterWrite",
                                                  "sqlite3VdbeSorterNext", "sqlite3VdbeSorterClose"]},
        ],
    },

    # ============ G 分页器与回滚日志 ============
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "pager-open-and-roles",
        "title": "分页器与回滚模式切换",
        "page_file": "g01-pager-open-and-roles.md",
        "boundary": "负责分页器的打开与角色分配（主库/临时库/日志/主日志的归属），"
                    "以及 journal 模式的查询与设置入口；不负责实际写日志（G03）、"
                    "不负责 WAL（H 组）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerOpen", "sqlite3PagerGetJournalMode",
                                                 "sqlite3PagerSetJournalMode",
                                                 "sqlite3PagerOkToChangeJournalMode"]},
        ],
    },
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "pager-page-cache-ref",
        "title": "分页器的页获取与引用计数",
        "page_file": "g02-pager-page-cache-ref.md",
        "boundary": "负责按页号从缓存取页、页的引用计数与释放、以及把页标记为脏；"
                    "不负责缓存的淘汰策略（I 组）、不负责页的写回（G04）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerGet", "sqlite3PagerRef",
                                                "sqlite3PagerUnref", "sqlite3PagerPageRefcount",
                                                "sqlite3PagerPagecount"]},
        ],
    },
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "rollback-journal",
        "title": "回滚日志写入",
        "page_file": "g03-rollback-journal.md",
        "boundary": "负责把修改前的原始页写入回滚日志并同步落盘，"
                    "以及从日志读回原像做回滚；不负责主日志（G05）、"
                    "不负责 WAL（H 组）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerWrite", "sqlite3PagerRollback",
                                                "sqlite3PagerCommitPhaseOne",
                                                "sqlite3PagerCommitPhaseTwo"]},
        ],
    },
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "page-writeback",
        "title": "脏页写回与同步",
        "page_file": "g04-page-writeback.md",
        "boundary": "负责把缓存里的脏页写回数据库文件、文件同步与截断；"
                    "不负责日志的写入（G03）、不负责 OS 层写调用（J 组）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerFlush", "sqlite3PagerSync",
                                                "sqlite3PagerTruncateImage",
                                                "sqlite3PagerUnrefNotNull"]},
        ],
    },
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "master-journal",
        "title": "主日志与多文件原子提交",
        "page_file": "g05-master-journal.md",
        "boundary": "负责附加库场景下的跨文件提交：把各库的名字记进主日志，"
                    "提交时同步主日志作为原子性边界；不负责单库事务（M 组）、"
                    "不负责 WAL（H 组）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerSetFlags", "sqlite3PagerSavepoint",
                                                "sqlite3PagerOpenSavepoint"]},
        ],
    },
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "pager-locking",
        "title": "分页器文件锁",
        "page_file": "g06-pager-locking.md",
        "boundary": "负责在数据库文件上获取共享/排他/保留锁以及锁状态的升级降级，"
                    "是并发写入的互斥点；不负责忙等待回调（A05）、"
                    "不负责页缓存（I 组）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerSharedLock", "sqlite3PagerExclusiveLock",
                                                "sqlite3PagerLockingMode"]},
        ],
    },
    {
        "feature_group": "G-分页器与回滚日志",
        "feature": "sqlite-file-header",
        "title": "数据库文件头",
        "page_file": "g07-sqlite-file-header.md",
        "boundary": "负责数据库文件头的字段含义、版本号与页大小的解析校验，"
                    "以及写回时的头信息维护；不负责页的读写（G02/G04）、"
                    "不负责 B-tree 的结构（F 组）。",
        "refs": [
            {"path": "src/pager.c", "symbols": ["sqlite3PagerReadFileheader", "sqlite3PagerSetPagesize"]},
        ],
    },

    # ============ H WAL 预写日志 ============
    {
        "feature_group": "H-WAL 预写日志",
        "feature": "wal-mode-and-index",
        "title": "WAL 模式与索引文件",
        "page_file": "h01-wal-mode-and-index.md",
        "boundary": "负责 WAL 模式的开启关闭、WAL 文件与其共享内存索引的建立与映射；"
                    "不负责帧的写入（h02）、不负责 checkpoint（h03）。",
        "refs": [
            {"path": "src/wal.c", "symbols": ["sqlite3WalOpen", "sqlite3WalClose",
                                              "sqlite3WalBeginReadTransaction"]},
            {"path": "src/pager.c", "symbols": ["sqlite3PagerOpenWal", "sqlite3PagerCloseWal"]},
        ],
    },
    {
        "feature_group": "H-WAL 预写日志",
        "feature": "wal-frames",
        "title": "WAL 帧写入与读取",
        "page_file": "h02-wal-frames.md",
        "boundary": "负责把修改后的页写成 WAL 帧（含校验和与提交帧）、"
                    "以及按帧号读回；不负责 checkpoint（h03）、"
                    "不负责回滚日志（G 组）。",
        "refs": [
            {"path": "src/wal.c", "symbols": ["sqlite3WalFrames", "sqlite3WalReadFrame",
                                             "sqlite3WalFindFrame", "sqlite3WalFramesize"]},
        ],
    },
    {
        "feature_group": "H-WAL 预写日志",
        "feature": "wal-checkpoint",
        "title": "WAL 检查点",
        "page_file": "h03-wal-checkpoint.md",
        "boundary": "负责把 WAL 中的帧回写进数据库文件并截断 WAL，"
                    "以及三种检查点模式的取舍；不负责帧的写入（h02）、"
                    "不负责页缓存的固定页锁（I 组）。",
        "refs": [
            {"path": "src/wal.c", "symbols": ["sqlite3WalCheckpoint", "sqlite3WalExclusiveMode",
                                              "sqlite3WalWriteLock"]},
            {"path": "src/pager.c", "symbols": ["sqlite3PagerCheckpoint"]},
        ],
    },
    {
        "feature_group": "H-WAL 预写日志",
        "feature": "wal-readers",
        "title": "WAL 读事务与快照",
        "page_file": "h04-wal-readers.md",
        "boundary": "负责读者如何在 WAL 中定位可见帧、读事务的建立与结束、"
                    "以及快照的取得；不负责 checkpoint（h03）、"
                    "不负责保存点的撤销（H05）。",
        "refs": [
            {"path": "src/wal.c", "symbols": ["sqlite3WalBeginReadTransaction",
                                              "sqlite3WalEndReadTransaction",
                                              "sqlite3WalSnapshotGet", "sqlite3WalSnapshotOpen"]},
        ],
    },
    {
        "feature_group": "H-WAL 预写日志",
        "feature": "wal-savepoint-undo",
        "title": "WAL 保存点与撤销",
        "page_file": "h05-wal-savepoint-undo.md",
        "boundary": "负责 WAL 模式下保存点的记录与回滚到保存点时的帧截断；"
                    "不负责回滚日志的撤销（G03）、不负责读事务快照（H04）。",
        "refs": [
            {"path": "src/wal.c", "symbols": ["sqlite3WalSavepoint", "sqlite3WalSavepointUndo",
                                              "sqlite3WalUndo"]},
        ],
    },

    # ============ I 页缓存 ============
    {
        "feature_group": "I-页缓存",
        "feature": "pcache-lookup",
        "title": "页缓存查找与固定",
        "page_file": "i01-pcache-lookup.md",
        "boundary": "负责按页号在页缓存中查找或新建页、页的固定（pin）与"
                    "引用计数管理；不负责脏页的写回（G04）、"
                    "不负责淘汰策略（i03）。",
        "refs": [
            {"path": "src/pcache.c", "symbols": ["sqlite3PcacheFetch", "sqlite3PcacheRef",
                                                 "sqlite3PcacheRelease", "sqlite3PcachePageRefcount"]},
        ],
    },
    {
        "feature_group": "I-页缓存",
        "feature": "pcache-dirty-tracking",
        "title": "脏页跟踪",
        "page_file": "i02-pcache-dirty-tracking.md",
        "boundary": "负责标记与清除页的脏状态，以及统计缓存中脏页占比；"
                    "不负责脏页写回的具体流程（G04）、"
                    "不负责 WAL 模式下的页引用（H 组）。",
        "refs": [
            {"path": "src/pcache.c", "symbols": ["sqlite3PcacheMakeDirty", "sqlite3PcacheMakeClean",
                                                 "sqlite3PCacheIsDirty", "sqlite3PCachePercentDirty"]},
        ],
    },
    {
        "feature_group": "I-页缓存",
        "feature": "pcache-eviction",
        "title": "页缓存淘汰与截断",
        "page_file": "i03-pcache-eviction.md",
        "boundary": "负责缓存满时的可回收页选择、脏页回收（spill）与"
                    "缓存截断；不负责脏页写回的顺序（G04）、"
                    "不负责分页器的引用计数（G02）。",
        "refs": [
            {"path": "src/pcache.c", "symbols": ["sqlite3PcacheShrink", "sqlite3PcacheTruncate",
                                                 "sqlite3PcacheIterateDirty", "sqlite3PcacheDrop"]},
        ],
    },
    {
        "feature_group": "I-页缓存",
        "feature": "pcache-size-policy",
        "title": "页缓存容量策略",
        "page_file": "i04-pcache-size-policy.md",
        "boundary": "负责页缓存容量上限的设置与换算（含按页数与按字节两种口径）、"
                    "以及 spill 阈值；不负责实际淘汰（i03）、"
                    "不负责全局堆内存上限（J 组）。",
        "refs": [
            {"path": "src/pcache.c", "symbols": ["sqlite3PcacheSetCachesize", "sqlite3PcacheGetCachesize",
                                                 "sqlite3PcacheSetSpillsize", "sqlite3PcacheSize"]},
        ],
    },

    # ============ J OS 接口与内存管理 ============
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "vfs-abstraction",
        "title": "VFS 抽象层",
        "page_file": "j01-vfs-abstraction.md",
        "boundary": "负责把文件操作抽象成 VFS 接口并路由到具体实现，"
                    "含自定义 VFS 的注册；不负责具体平台的实现、"
                    "不负责数据库文件格式（G 组/F 组）。",
        "refs": [
            {"path": "src/os.c", "symbols": ["sqlite3OsOpen", "sqlite3OsRead", "sqlite3OsWrite",
                                             "sqlite3OsLock", "sqlite3OsUnlock", "sqlite3_vfs_register"]},
        ],
    },
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "os-file-control",
        "title": "文件控制与共享内存",
        "page_file": "j02-os-file-control.md",
        "boundary": "负责按操作码对文件做特殊控制（取大小、截断、同步、"
                    "以及 WAL 用的共享内存映射）；不负责普通读写（j01）、"
                    "不负责 WAL 的帧语义（H 组）。",
        "refs": [
            {"path": "src/os.c", "symbols": ["sqlite3OsFileControl", "sqlite3OsFileSize",
                                             "sqlite3OsShmMap", "sqlite3OsShmLock",
                                             "sqlite3OsFetch", "sqlite3OsUnfetch"]},
        ],
    },
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "memory-allocator",
        "title": "内存分配器",
        "page_file": "j03-memory-allocator.md",
        "boundary": "负责内存申请的封装与尺寸统计、软硬堆上限的设置与"
                    "超限时的回调；不负责单块内存的页布局（j04）、"
                    "不负责页缓存（I 组）。",
        "refs": [
            {"path": "src/malloc.c", "symbols": ["sqlite3MallocInit", "sqlite3MallocSize",
                                                 "sqlite3DbMallocSize", "sqlite3HeapNearlyFull",
                                                 "sqlite3_soft_heap_limit64", "sqlite3_hard_heap_limit64"]},
        ],
    },
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "mem0-page-allocator",
        "title": "小内存块分配器",
        "page_file": "j04-mem0-page-allocator.md",
        "boundary": "负责小尺寸申请的按页分配与复用，以及整块释放的归还；"
                    "不负责通用分配器（j03）、不负责字符串格式化（j06）。",
        "refs": [
            {"path": "src/mem0.c", "symbols": ["sqlite3MemInit", "sqlite3MemFree", "sqlite3MemSize",
                                               "sqlite3MemRoundup", "sqlite3MemShutdown"]},
        ],
    },
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "string-formatter",
        "title": "字符串格式化",
        "page_file": "j05-string-formatter.md",
        "boundary": "负责 SQLite 自带的 printf 实现与动态字符串拼接；"
                    "不负责通用分配器（j03）、不负责 SQL 层的字符串函数（L 组）。",
        "refs": [
            {"path": "src/printf.c", "symbols": ["sqlite3_str_vappendf", "sqlite3_str_appendf",
                                                 "sqlite3_str_finish"]},
        ],
    },
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "hash-table",
        "title": "哈希表",
        "page_file": "j06-hash-table.md",
        "boundary": "负责内部哈希表的插入查找删除与桶数调整；"
                    "不负责模式对象的管理（B05）、不负责 B-tree（F 组）。",
        "refs": [
            {"path": "src/hash.c", "symbols": ["sqlite3HashInit", "sqlite3HashClear",
                                               "sqlite3HashInsert", "sqlite3HashFind"]},
        ],
    },
    {
        "feature_group": "J-OS 接口与内存管理",
        "feature": "mutex-and-threading",
        "title": "互斥与多线程",
        "page_file": "j07-mutex-and-threading.md",
        "boundary": "负责互斥模式的配置、进入与离开，以及多线程下的"
                    "串行化/并发模式选择；不负责 WAL 的读写锁（H 组）、"
                    "不负责文件锁（G06）。",
        "refs": [
            {"path": "src/mutex.c", "symbols": ["sqlite3_mutex_enter", "sqlite3_mutex_leave",
                                                "sqlite3_mutex_alloc", "sqlite3MutexInit"]},
        ],
    },

    # ============ K 写入路径与约束 ============
    {
        "feature_group": "K-写入路径与约束",
        "feature": "constraint-checks",
        "title": "约束检查",
        "page_file": "k01-constraint-checks.md",
        "boundary": "负责在写入时生成并执行约束检查（主键、唯一、非空、"
                    "CHECK）的代码路径；不负责索引的实际存储（F 组）、"
                    "不负责外键（k02）。",
        "refs": [
            {"path": "src/insert.c", "symbols": ["sqlite3GenerateConstraintChecks"]},
            {"path": "src/build.c", "symbols": ["sqlite3AddNotNull", "sqlite3AddCheckConstraint",
                                                "sqlite3AddPrimaryKey"]},
        ],
    },
    {
        "feature_group": "K-写入路径与约束",
        "feature": "foreign-keys",
        "title": "外键约束",
        "page_file": "k02-foreign-keys.md",
        "boundary": "负责外键的登记、级联动作的判定与触发时机；"
                    "不负责索引查找的实现（F 组）、不负责触发器（k03）。",
        "refs": [
            {"path": "src/fkey.c", "symbols": ["sqlite3FkCheck", "sqlite3FkActions",
                                               "sqlite3FkRequired", "sqlite3FkLocateIndex"]},
        ],
    },
    {
        "feature_group": "K-写入路径与约束",
        "feature": "triggers",
        "title": "触发器",
        "page_file": "k03-triggers.md",
        "boundary": "负责在表操作前后编译并执行触发器程序，以及触发器的"
                    "登记与删除；不负责 DML 的编译（D07/D08/D09）、"
                    "不负责外键（k02）。",
        "refs": [
            {"path": "src/trigger.c", "symbols": ["sqlite3CodeRowTrigger", "sqlite3BeginTrigger",
                                                 "sqlite3FinishTrigger", "sqlite3DeleteTrigger"]},
        ],
    },
    {
        "feature_group": "K-写入路径与约束",
        "feature": "generated-columns",
        "title": "生成列",
        "page_file": "k04-generated-columns.md",
        "boundary": "负责生成列的表达计算与在写入路径中的求值时机；"
                    "不负责表达式代码生成（C02）、不负责视图（N 组）。",
        "refs": [
            {"path": "src/insert.c", "symbols": ["sqlite3ComputeGeneratedColumns"]},
            {"path": "src/expr.c", "symbols": ["sqlite3ExprCodeGeneratedColumn"]},
        ],
    },
    {
        "feature_group": "K-写入路径与约束",
        "feature": "index-maintenance",
        "title": "索引维护",
        "page_file": "k05-index-maintenance.md",
        "boundary": "负责在表数据变化后同步维护相关索引的键值；"
                    "不负责索引的选择（D02）、不负责 B-tree 写入（F03）。",
        "refs": [
            {"path": "src/delete.c", "symbols": ["sqlite3GenerateIndexKey",
                                                 "sqlite3GenerateRowIndexDelete"]},
            {"path": "src/build.c", "symbols": ["sqlite3CreateIndex"]},
        ],
    },

    # ============ L 内置函数与日期时间 ============
    {
        "feature_group": "L-内置函数与日期时间",
        "feature": "builtin-functions",
        "title": "内置标量与聚合函数",
        "page_file": "l01-builtin-functions.md",
        "boundary": "负责内置函数的注册与分派，以及标量/聚合两类函数的"
                    "执行约定；不负责用户自定义函数（l02）、"
                    "不负责日期时间函数（l03）。",
        "refs": [
            {"path": "src/func.c", "symbols": ["sqlite3RegisterBuiltinFunctions",
                                               "sqlite3RegisterPerConnectionBuiltinFunctions",
                                               "sqlite3QuoteValue"]},
        ],
    },
    {
        "feature_group": "L-内置函数与日期时间",
        "feature": "user-defined-functions",
        "title": "用户自定义函数",
        "page_file": "l02-user-defined-functions.md",
        "boundary": "负责把应用注册的 C 函数登记到连接并在调用时回调，"
                    "含聚合函数的累加器约定；不负责内置函数（l01）、"
                    "不负责参数绑定（E04）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_create_function", "sqlite3CreateFunc"]},
            {"path": "src/vdbeapi.c", "symbols": ["sqlite3_result_text", "sqlite3_result_int64",
                                                  "sqlite3_value_blob", "sqlite3_user_data"]},
        ],
    },
    {
        "feature_group": "L-内置函数与日期时间",
        "feature": "date-time-functions",
        "title": "日期时间函数",
        "page_file": "l03-date-time-functions.md",
        "boundary": "负责日期时间值的解析、格式化成字符串以及时间函数的注册；"
                    "不负责通用内置函数（l01）、不负责随机数（J 组）。",
        "refs": [
            {"path": "src/date.c", "symbols": ["sqlite3RegisterDateTimeFunctions"]},
        ],
    },
    {
        "feature_group": "L-内置函数与日期时间",
        "feature": "json-functions",
        "title": "JSON 函数",
        "page_file": "l04-json-functions.md",
        "boundary": "负责 JSON 文本的解析树表示与内置 JSON 函数的注册执行；"
                    "不负责通用内置函数（l01）、不负责 SQL 层的字符串函数（l01）。",
        "refs": [
            {"path": "src/json.c", "symbols": ["sqlite3RegisterJsonFunctions"]},
        ],
    },
    {
        "feature_group": "L-内置函数与日期时间",
        "feature": "like-and-glob-matching",
        "title": "LIKE 与 GLOB 匹配",
        "page_file": "l05-like-and-glob-matching.md",
        "boundary": "负责 LIKE/GLOB 的模式匹配实现与大小写折叠的处理；"
                    "不负责通用内置函数（l01）、不负责正则（FTS 不在清单内）。",
        "refs": [
            {"path": "src/func.c", "symbols": ["sqlite3RegisterLikeFunctions", "sqlite3_strlike",
                                               "sqlite3_strglob", "sqlite3IsLikeFunction"]},
        ],
    },

    # ============ M 事务与并发控制 ============
    {
        "feature_group": "M-事务与并发控制",
        "feature": "transaction-state",
        "title": "事务状态机",
        "page_file": "m01-transaction-state.md",
        "boundary": "负责连接级的事务状态（空闲/写/回滚）与自动提交的判定，"
                    "以及回滚全部；不负责文件锁（G06）、"
                    "不负责日志的写入（G03）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_get_autocommit", "sqlite3_txn_state",
                                               "sqlite3RollbackAll"]},
        ],
    },
    {
        "feature_group": "M-事务与并发控制",
        "feature": "savepoints",
        "title": "保存点",
        "page_file": "m02-savepoints.md",
        "boundary": "负责保存点的建立、释放与回滚到保存点时的语句级撤销；"
                    "不负责 WAL 模式的帧截断（H05）、"
                    "不负责分页器层的保存点（G05）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3CloseSavepoints"]},
            {"path": "src/btree.c", "symbols": ["sqlite3BtreeSavepoint"]},
        ],
    },
    {
        "feature_group": "M-事务与并发控制",
        "feature": "snapshot-isolation",
        "title": "快照与并发读",
        "page_file": "m03-snapshot-isolation.md",
        "boundary": "负责显式快照的取得与释放、以及快照期间对模式变更的拒绝；"
                    "不负责 WAL 帧的读取（H02）、不负责文件锁（G06）。",
        "refs": [
            {"path": "src/main.c", "symbols": ["sqlite3_snapshot_get"]},
            {"path": "src/pager.c", "symbols": ["sqlite3PagerSnapshotGet", "sqlite3PagerSnapshotCheck",
                                                "sqlite3PagerSnapshotRecover"]},
        ],
    },
    {
        "feature_group": "M-事务与并发控制",
        "feature": "unlock-notify",
        "title": "解锁通知",
        "page_file": "m04-unlock-notify.md",
        "boundary": "负责在锁释放时通知等待方、以及等待方被唤醒后的处理；"
                    "不负责文件锁本身（G06）、不负责忙等待回调（A05）。",
        "refs": [
            {"path": "src/notify.c", "symbols": ["sqlite3_unlock_notify", "sqlite3ConnectionBlocked",
                                                 "sqlite3ConnectionUnlocked",
                                                 "sqlite3ConnectionClosed"]},
        ],
    },

    # ============ N 模式演进与附加能力 ============
    {
        "feature_group": "N-模式演进与附加能力",
        "feature": "alter-table",
        "title": "ALTER TABLE",
        "page_file": "n01-alter-table.md",
        "boundary": "负责增删改列、重命名表与列等模式变更的实现路径"
                    "（含改写底层数据文件的步骤）；不负责模式缓存的失效（A04）、"
                    "不负责 DDL 的编译入口（B 组）。",
        "refs": [
            {"path": "src/alter.c", "symbols": ["sqlite3AlterRenameTable", "sqlite3AlterRenameColumn",
                                                "sqlite3AlterBeginAddColumn", "sqlite3AlterFinishAddColumn",
                                                "sqlite3AlterDropColumn"]},
        ],
    },
    {
        "feature_group": "N-模式演进与附加能力",
        "feature": "views",
        "title": "视图",
        "page_file": "n02-views.md",
        "boundary": "负责视图的登记与其定义在查询中的展开；"
                    "不负责子查询的一般机制（C04）、"
                    "不负责触发器（k03）。",
        "refs": [
            {"path": "src/build.c", "symbols": ["sqlite3CreateView"]},
        ],
    },
    {
        "feature_group": "N-模式演进与附加能力",
        "feature": "attach-database",
        "title": "附加数据库",
        "page_file": "n03-attach-database.md",
        "boundary": "负责 ATTACH/DETACH 的语义：把另一个数据库文件挂到当前连接上"
                    "并参与跨库事务；不负责主日志的原子提交（G05）、"
                    "不负责模式加载（B05）。",
        "refs": [
            {"path": "src/attach.c", "symbols": ["sqlite3Attach", "sqlite3Detach",
                                                 "sqlite3FixSrcList", "sqlite3FixExpr"]},
        ],
    },
    {
        "feature_group": "N-模式演进与附加能力",
        "feature": "load-extension",
        "title": "可加载扩展",
        "page_file": "n04-load-extension.md",
        "boundary": "负责动态库的加载与入口点调用，以及自动扩展的注册链；"
                    "不负责用户自定义函数（l02）、不负责 VFS 注册（j01）。",
        "refs": [
            {"path": "src/loadext.c", "symbols": ["sqlite3LoadExtension", "sqlite3_auto_extension",
                                                  "sqlite3_enable_load_extension",
                                                  "sqlite3CloseExtensions"]},
        ],
    },
    {
        "feature_group": "N-模式演进与附加能力",
        "feature": "pragma-processing",
        "title": "PRAGMA 处理",
        "page_file": "n05-pragma-processing.md",
        "boundary": "负责 PRAGMA 语句的分派与各类参数的读写（页大小、缓存大小、"
                    "日志模式等）；不负责这些参数的实际消费方（各层）、"
                    "不负责模式变更本身（N01）。",
        "refs": [
            {"path": "src/pragma.c", "symbols": ["sqlite3Pragma", "sqlite3GetBoolean"]},
        ],
    },
    {
        "feature_group": "N-模式演进与附加能力",
        "feature": "returning-clause",
        "title": "RETURNING 子句",
        "page_file": "n06-returning-clause.md",
        "boundary": "负责 DML 的 RETURNING 子句在编译期被识别并生成返回结果列；"
                    "不负责 DML 本身的编译（D07/D08/D09）、"
                    "不负责结果列的读取（E03）。",
        "refs": [
            {"path": "src/build.c", "symbols": ["sqlite3AddReturning"]},
            {"path": "src/trigger.c", "symbols": ["sqlite3ProcessReturningSubqueries",
                                                  "sqlite3ReturningSubqueryCorrelated"]},
        ],
    },
]
