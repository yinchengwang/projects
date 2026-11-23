# -*- coding: utf-8 -*-
"""MySQL 机制清单（P4 首批三库之三）。

盘点方法：先用 GitHub 的 tree API 拿到 `mysql-8.0.40` 的完整文件清单做导航，
再把机制盘点实际会引用的实现文件按文件级稀疏检出到本地
（`raw.githubusercontent.com` 单文件并行下载），然后在这些文件里逐个确认
函数定义位置（`find_defs.py` 逐行切分，区分定义与调用），最后按机制簇归并。

**这不是整仓克隆**：mysql-server 的整仓 `--depth 1` 克隆在本机实测 25 分钟
pack 仍为 0 字节，整仓 tarball 约 370 MB，而单连接实测带宽约 80 KB/s。
因此本地检出是**文件级稀疏检出**：只包含本清单引用到的文件，每个文件
与上游 tag `mysql-8.0.40` 逐字节一致。**未下载的文件不在本地**，因此不能对
未列出的路径做 refs 校验——这一点必须与整仓检出的 postgres / redis / sqlite
区分开。

**篇数不是按文件数折算**：像 `sql/sql_parse.cc` 这种巨型文件承载大量机制，
也有只贡献一个机制的小文件；本清单是逐条盘点后合并/拆分/移组得到的 67 个
机制单元。

refs 里的每个 `(path, symbol)` 都已用
`python gen_manifest.py --project mysql-server --spec-module mysql_mechanism_specs --verify`
校验通过（失败 0 条）。注意：**refs 命中只证明符号文本出现在该文件里**，
不证明它就是该机制的语义主体——后者靠人工核对函数体得出。

源码锚点：`mysql-server@mysql-8.0.40`
"""

GROUP_ORDER = [
    "A-连接与命令分派",
    "B-解析与词法",
    "C-优化器与执行计划",
    "D-执行器与行迭代",
    "E-临时表与排序",
    "F-存储引擎接口",
    "G-InnoDB 索引与页",
    "H-InnoDB 缓冲池",
    "I-InnoDB 日志与恢复",
    "J-InnoDB 事务与锁",
    "K-InnoDB 行操作与可见性",
    "L-二进制日志",
    "M-复制与 GTID",
    "N-权限与认证",
]

MECHANISM_SPECS = [
    # ============ A 连接与命令分派 ============
    {
        "feature_group": "A-连接与命令分派",
        "feature": "connection-dispatch",
        "title": "连接与命令分派",
        "page_file": "a01-connection-dispatch.md",
        "boundary": "负责一个客户端连接上的命令读取、分派到对应处理函数、"
                    "以及命令结束后的收尾；不负责 SQL 解析（B 组）、"
                    "不负责优化器（C 组）、不负责线程模型（连接处理器如何分配线程）。",
        "refs": [
            {"path": "sql/sql_parse.cc", "symbols": ["do_command", "dispatch_command"]},
        ],
    },
    {
        "feature_group": "A-连接与命令分派",
        "feature": "thd-lifecycle",
        "title": "THD 线程对象生命周期",
        "page_file": "a02-thd-lifecycle.md",
        "boundary": "负责每个连接对应的 THD（线程对象）的建立、上下文切换与销毁，"
                    "以及事务状态的挂载位置；不负责命令语义（A01）、"
                    "不负责存储引擎层的事务对象（J 组）。",
        "refs": [
            {"path": "sql/sql_class.cc", "symbols": ["THD::sql_parser"]},
            {"path": "sql/mysqld_thd_manager.cc", "symbols": ["THD"]},
        ],
    },
    {
        "feature_group": "A-连接与命令分派",
        "feature": "prepare-execute-protocol",
        "title": "预处理语句的两阶段执行",
        "page_file": "a03-prepare-execute-protocol.md",
        "boundary": "负责 COM_STMT_PREPARE/EXECUTE 的两阶段协议与语句句柄的"
                    "缓存复用；不负责 SQL 解析本身（B 组）、"
                    "不负责参数绑定到执行计划（属于执行器 D 组）。",
        "refs": [
            {"path": "sql/sql_prepare.cc", "symbols": ["Prepared_statement"]},
        ],
    },
    {
        "feature_group": "A-连接与命令分派",
        "feature": "change-db",
        "title": "USE 与当前库切换",
        "page_file": "a04-change-db.md",
        "boundary": "负责切换当前默认数据库以及随之而来的表缓存与元数据失效；"
                    "不负责表打开本身（F 组）、不负责权限校验（N 组）。",
        "refs": [
            {"path": "sql/sql_db.cc", "symbols": ["mysql_change_db"]},
        ],
    },
    {
        "feature_group": "A-连接与命令分派",
        "feature": "error-diagnostics",
        "title": "错误码与诊断信息",
        "page_file": "a05-error-diagnostics.md",
        "boundary": "负责把内部错误状态映射成服务端错误码与面向客户端的诊断信息；"
                    "不负责错误码的定义表（share/errmsg-utf8.txt）、"
                    "不负责 SQLSTATE 与消息的具体文案生成（C 组）。",
        "refs": [
            {"path": "sql/derror.cc", "symbols": ["init_errmessage",
                                                 "mysql_errno_to_builtin"]},
        ],
    },

    # ============ B 解析与词法 ============
    {
        "feature_group": "B-解析与词法",
        "feature": "lexer",
        "title": "词法分析器",
        "page_file": "b01-lexer.md",
        "boundary": "负责把 SQL 文本切成记号流（标识符、字面量、运算符、注释）"
                    "并维护字符集相关状态；不负责语法树构造（B02）、"
                    "不负责 SQL 语义解析（B03）。",
        "refs": [
            {"path": "sql/sql_lex.cc", "symbols": ["lex_start"]},
        ],
    },
    {
        "feature_group": "B-解析与词法",
        "feature": "grammar-parser",
        "title": "语法分析器",
        "page_file": "b02-grammar-parser.md",
        "boundary": "负责按文法把记号流组装成解析树（Query_expression / "
                    "Query_block 等节点）；不负责词法（B01）、"
                    "不负责把名字绑到表列（B03）、不负责优化（C 组）。",
        "refs": [
            {"path": "sql/sql_yacc.yy", "symbols": ["MYSQLparse", "yyparse"]},
        ],
    },
    {
        "feature_group": "B-解析与词法",
        "feature": "parse-tree-items",
        "title": "解析树到 Item 的转换",
        "page_file": "b03-parse-tree-items.md",
        "boundary": "负责把解析树节点转换成可求值的 Item 表达式对象"
                    "（常量、列引用、函数调用、子查询）；不负责语法树构造（B02）、"
                    "不负责表达式的运行期求值（C/D 组）。",
        "refs": [
            {"path": "sql/parse_tree_items.cc", "symbols": ["PTI_comp_op::itemize",
                                                          "PTI_user_variable::itemize"]},
        ],
    },
    {
        "feature_group": "B-解析与词法",
        "feature": "name-resolution",
        "title": "名称解析与列绑定",
        "page_file": "b04-name-resolution.md",
        "boundary": "负责把查询里的表名/列名绑定到具体的表与字段、"
                    "判定歧义并报错；不负责解析树构造（B02）、"
                    "不负责索引选择（C 组）。",
        "refs": [
            {"path": "sql/sql_resolver.cc", "symbols": ["Query_block::resolve_placeholder_tables"]},
            {"path": "sql/item.cc", "symbols": ["Item_field::fix_outer_field"]},
        ],
    },
    {
        "feature_group": "B-解析与词法",
        "feature": "parse-tree-hints",
        "title": "优化器提示的解析",
        "page_file": "b05-parse-tree-hints.md",
        "boundary": "负责把 SQL 里的 hint 注释解析成结构化提示对象；"
                    "不负责提示对计划的影响（C 组）、不负责词法与语法（B01/B02）。",
        "refs": [
            {"path": "sql/parse_tree_hints.cc", "symbols": ["PT_hint_list::contextualize",
                                                           "find_qb_hints"]},
            {"path": "sql/opt_hints.cc", "symbols": ["Opt_hints::check_unresolved",
                                                     "Opt_hints_qb::subquery_strategy"]},
        ],
    },

    # ============ C 优化器与执行计划 ============
    {
        "feature_group": "C-优化器与执行计划",
        "feature": "join-optimize",
        "title": "JOIN 的整体优化",
        "page_file": "c01-join-optimize.md",
        "boundary": "负责一次 JOIN 优化的整体编排：条件收集、子查询转换、"
                    "连接顺序确定、代价估算与最终计划落地；不负责单个索引的"
                    "代价模型细节（C03）、不负责执行期的迭代（D 组）。",
        "refs": [
            {"path": "sql/sql_optimizer.cc", "symbols": ["JOIN::optimize"]},
        ],
    },
    {
        "feature_group": "C-优化器与执行计划",
        "feature": "join-plan",
        "title": "连接计划与表访问顺序",
        "page_file": "c02-join-plan.md",
        "boundary": "负责把参与连接的表排成访问顺序并为每张表选出访问路径；"
                    "不负责单个索引的内部选择（C03）、不负责行迭代实现（D 组）。",
        "refs": [
            {"path": "sql/sql_optimizer.cc", "symbols": ["JOIN::make_join_plan"]},
        ],
    },
    {
        "feature_group": "C-优化器与执行计划",
        "feature": "index-selection",
        "title": "索引与范围访问选择",
        "page_file": "c03-index-selection.md",
        "boundary": "负责在候选索引里按代价挑出访问路径并估算扫描行数；"
                    "不负责连接顺序（C02）、不负责索引的物理结构（G 组）、"
                    "不负责范围条件的构造（C 组 range optimizer）。",
        "refs": [
            {"path": "sql/sql_optimizer.cc", "symbols": ["JOIN::estimate_rowcount",
                                                       "get_best_group_min_max"]},
        ],
    },
    {
        "feature_group": "C-优化器与执行计划",
        "feature": "subquery-transform",
        "title": "子查询改写",
        "page_file": "c04-subquery-transform.md",
        "boundary": "负责把 IN/EXISTS/标量子查询改写成半连接或物化形式，"
                    "以及决定不相关子查询能否提前求值；不负责子查询的运行期执行（D 组）、"
                    "不负责物化临时表的实现（E 组）。",
        "refs": [
            {"path": "sql/item_subselect.cc", "symbols": ["Item_subselect::exec",
                                                         "Item_in_subselect::transform"]},
        ],
    },
    {
        "feature_group": "C-优化器与执行计划",
        "feature": "derived-table-materialization",
        "title": "派生表与 CTE 的物化",
        "page_file": "c05-derived-table-materialization.md",
        "boundary": "负责派生表与公用表表达式在计划期的落地方式"
                    "（物化成临时表还是合并进外层查询）；不负责临时表的"
                    "存储实现（E 组）、不负责 CTE 的递归执行（属于执行器）。",
        "refs": [
            {"path": "sql/sql_derived.cc", "symbols": ["setup_materialized_derived",
                                                       "materialize_derived"]},
        ],
    },
    {
        "feature_group": "C-优化器与执行计划",
        "feature": "partition-pruning",
        "title": "分区裁剪",
        "page_file": "c06-partition-pruning.md",
        "boundary": "负责在查询前把条件与分区定义比对，裁掉不可能命中的分区；"
                    "不负责分区表的存储实现（F 组）、"
                    "不负责分区维护操作（K 组）。",
        "refs": [
            {"path": "sql/partition_info.cc", "symbols": ["partition_info::can_prune_insert"]},
            {"path": "sql/sql_optimizer.cc", "symbols": ["partition_pruning"]},
        ],
    },

    # ============ D 执行器与行迭代 ============
    {
        "feature_group": "D-执行器与行迭代",
        "feature": "query-execution",
        "title": "查询执行入口",
        "page_file": "d01-query-execution.md",
        "boundary": "负责把优化好的计划变成实际执行：建立执行上下文、"
                    "驱动各阶段（初始化 / 迭代 / 收尾）；不负责计划生成（C 组）、"
                    "不负责具体迭代器的扫描逻辑（D02）。",
        "refs": [
            {"path": "sql/sql_select.cc", "symbols": ["Sql_cmd_dml::execute",
                                                      "Sql_cmd_dml::execute_inner"]},
            {"path": "sql/sql_union.cc", "symbols": ["Query_expression::execute"]},
        ],
    },
    {
        "feature_group": "D-执行器与行迭代",
        "feature": "row-iterator",
        "title": "行迭代器框架",
        "page_file": "d02-row-iterator.md",
        "boundary": "负责用统一的迭代器接口抽象各种取行方式（全表扫描、索引扫描、"
                    "物化回读等）并把它们组合起来；不负责单个迭代器的内部算法（D03）、"
                    "不负责计划生成（C 组）。",
        "refs": [
            {"path": "sql/iterators/row_iterator.h", "symbols": ["RowIterator"]},
            {"path": "sql/iterators/composite_iterators.h", "symbols": ["AppendIterator"]},
        ],
    },
    {
        "feature_group": "D-执行器与行迭代",
        "feature": "table-and-index-scan",
        "title": "全表扫描与索引扫描",
        "page_file": "d03-table-and-index-scan.md",
        "boundary": "负责把存储引擎返回的原始行按扫描方向送出、"
                    "以及索引扫描下的边界判定与过滤下推；不负责优化器选哪条路径（C03）、"
                    "不负责存储引擎内部的扫描实现（G/K 组）。",
        "refs": [
            {"path": "sql/iterators/basic_row_iterators.cc", "symbols": ["TableScanIterator::Init",
                                                                        "TableScanIterator::Read"]},
            {"path": "sql/range_optimizer/index_range_scan.cc",
             "symbols": ["IndexRangeScanIterator::Init", "IndexRangeScanIterator::Read"]},
        ],
    },
    {
        "feature_group": "D-执行器与行迭代",
        "feature": "filter-and-aggregate",
        "title": "过滤与聚合迭代",
        "page_file": "d04-filter-and-aggregate.md",
        "boundary": "负责在行流上施加过滤条件、聚合分组与窗口函数的计算；"
                    "不负责条件的表达式求值（B 组）、"
                    "不负责最终结果的排序与截断（E 组）。",
        "refs": [
            {"path": "sql/iterators/composite_iterators.cc", "symbols": ["FilterIterator",
                                                                        "AggregateIterator"]},
            {"path": "sql/iterators/window_iterators.cc", "symbols": ["WindowIterator"]},
        ],
    },
    {
        "feature_group": "D-执行器与行迭代",
        "feature": "join-execution",
        "title": "连接执行",
        "page_file": "d05-join-execution.md",
        "boundary": "负责连接在执行期的推进：嵌套循环连接的内外表配合、"
                    "半连接与反连接的语义差异；不负责连接顺序的决策（C02）、"
                    "不负责行迭代的通用框架（D02）。",
        "refs": [
            {"path": "sql/sql_select.cc", "symbols": ["JOIN::exec"]},
            {"path": "sql/iterators/composite_iterators.h", "symbols": ["NestedLoopIterator"]},
            {"path": "sql/sql_executor.cc", "symbols": ["JOIN::create_access_paths",
                                                        "attach_access_paths_for_having_and_limit"]},
        ],
    },

    # ============ E 临时表与排序 ============
    {
        "feature_group": "E-临时表与排序",
        "feature": "temp-table",
        "title": "内部临时表",
        "page_file": "e01-temp-table.md",
        "boundary": "负责为中间结果建内部临时表：字段推导、引擎选择"
                    "（内存 / 磁盘 / TempTable）、以及从存储引擎回填数据；"
                    "不负责排序过程（E02）、不负责临时表引擎自身的实现"
                    "（storage/temptable）。",
        "refs": [
            {"path": "sql/sql_tmp_table.cc", "symbols": ["create_tmp_table",
                                                        "create_tmp_table_from_fields"]},
        ],
    },
    {
        "feature_group": "E-临时表与排序",
        "feature": "filesort",
        "title": "文件排序",
        "page_file": "e02-filesort.md",
        "boundary": "负责 ORDER BY 无法直接由索引提供时的排序落地："
                    "选择内存排序还是磁盘归并排序、如何选择排序键与附加列；"
                    "不负责临时表的字段定义（E01）、不负责最终结果的截断（E03）。",
        "refs": [
            {"path": "sql/filesort.cc", "symbols": ["Filesort::make_sortorder",
                                                    "Filesort::clear_addon_fields"]},
            {"path": "sql/filesort_utils.cc", "symbols": ["Filesort_buffer::allocate_block"]},
        ],
    },
    {
        "feature_group": "E-临时表与排序",
        "feature": "limit-clause",
        "title": "LIMIT 与行数截断",
        "page_file": "e03-limit-clause.md",
        "boundary": "负责 LIMIT/OFFSET 的语义实现与提前终止；"
                    "不负责排序（E02）、不负责分页游标的稳定性（不属服务端职责）。",
        "refs": [
            {"path": "sql/sql_resolver.cc", "symbols": ["Query_block::resolve_limits"]},
            {"path": "sql/iterators/composite_iterators.h", "symbols": ["LimitOffsetIterator"]},
            {"path": "sql/sql_lex.cc", "symbols": ["Query_block::get_limit",
                                                  "Query_block::print_limit"]},
        ],
    },
    {
        "feature_group": "E-临时表与排序",
        "feature": "join-buffer",
        "title": "连接缓冲",
        "page_file": "e04-join-buffer.md",
        "boundary": "负责无可用索引时用全表扫描配合连接缓冲来避免重复扫描；"
                    "不负责索引扫描路径（D03）、不负责连接顺序决策（C02）。",
        "refs": [
            {"path": "sql/sql_optimizer.cc", "symbols": ["setup_join_buffering",
                                                        "set_join_buffer_properties"]},
        ],
    },

    # ============ F 存储引擎接口 ============
    {
        "feature_group": "F-存储引擎接口",
        "feature": "handler-api",
        "title": "handler 存储引擎接口",
        "page_file": "f01-handler-api.md",
        "boundary": "负责 server 层与存储引擎之间的接口约定：打开表、"
                    "读写行、索引操作、统计信息上报；不负责某个引擎的内部实现"
                    "（G/K 组）、不负责优化器怎么用这些接口（C 组）。",
        "refs": [
            {"path": "sql/handler.cc", "symbols": ["ha_open", "handler::ha_open",
                                                   "handler::ha_write_row"]},
        ],
    },
    {
        "feature_group": "F-存储引擎接口",
        "feature": "table-open-cache",
        "title": "表打开与表缓存",
        "page_file": "f02-table-open-cache.md",
        "boundary": "负责把表定义（TABLE_SHARE）打开成可用的 TABLE 实例、"
                    "以及表缓存的复用与失效；不负责存储引擎打开文件"
                    "（F01 转调引擎）、不负责 MDL 锁（A 组）。",
        "refs": [
            {"path": "sql/table_cache.cc", "symbols": ["Table_cache::init",
                                                       "Table_cache_manager::cached_tables"]},
            {"path": "sql/sql_base.cc", "symbols": ["open_table_from_share"]},
        ],
    },
    {
        "feature_group": "F-存储引擎接口",
        "feature": "metadata-lock",
        "title": "元数据锁",
        "page_file": "f03-metadata-lock.md",
        "boundary": "负责在 DDL 与 DML 之间建立互斥：元数据锁的申请、"
                    "等待队列与锁的升级；不负责数据行锁（J 组）、"
                    "不负责 InnoDB 的内部意向锁（J 组）。",
        "refs": [
            {"path": "sql/mdl.cc", "symbols": ["MDL_context::acquire_lock",
                                               "MDL_context::release_lock"]},
        ],
    },
    {
        "feature_group": "F-存储引擎接口",
        "feature": "dd-data-dictionary",
        "title": "数据字典",
        "page_file": "f04-dd-data-dictionary.md",
        "boundary": "负责 8.0 之后表与列元数据的持久化位置与读取路径"
                    "（数据字典表）；不负责 DDL 的执行（F 组）、"
                    "不负责 InnoDB 自己的字典（sys 表）。",
        "refs": [
            {"path": "sql/dd/dd_table.cc", "symbols": ["fill_dd_columns_from_create_fields"]},
            {"path": "sql/dd/dictionary.h", "symbols": ["is_dd_table_name",
                                                        "acquire_shared_table_mdl"]},
        ],
    },

    # ============ G InnoDB 索引与页 ============
    {
        "feature_group": "G-InnoDB 索引与页",
        "feature": "btr-cursor-search",
        "title": "B-tree 游标定位",
        "page_file": "g01-btr-cursor-search.md",
        "boundary": "负责在 B-tree 上按比较函数定位游标位置（自适应哈希索引除外）、"
                    "以及页级匹配的方向控制；不负责页内记录比较（rem0cmp）、"
                    "不负责缓冲池的页获取（H 组）。",
        "refs": [
            {"path": "storage/innobase/btr/btr0cur.cc",
             "symbols": ["btr_cur_search_to_nth_level",
                         "btr_cur_search_to_nth_level_with_no_latch"]},
        ],
    },
    {
        "feature_group": "G-InnoDB 索引与页",
        "feature": "btr-insert-split",
        "title": "B-tree 插入与页分裂",
        "page_file": "g02-btr-insert-split.md",
        "boundary": "负责把记录插入 B-tree 时的页内插入、页分裂与根页扩张；"
                    "不负责记录本身的编码（K 组）、不负责页的写回（H 组）。",
        "refs": [
            {"path": "storage/innobase/btr/btr0btr.cc", "symbols": ["btr_page_split_and_insert",
                                                                   "btr_page_reorganize"]},
        ],
    },
    {
        "feature_group": "G-InnoDB 索引与页",
        "feature": "page-directory",
        "title": "页目录与记录定位",
        "page_file": "g03-page-directory.md",
        "boundary": "负责页内记录的定位：页目录槽位到记录的二分查找、"
                    "以及记录链的遍历；不负责记录头的变长字段解析"
                    "（rem0rec）、不负责页的缓存（H 组）。",
        "refs": [
            {"path": "storage/innobase/page/page0cur.cc",
             "symbols": ["page_cur_search_with_match",
                         "page_cur_search_with_match_bytes"]},
        ],
    },
    {
        "feature_group": "G-InnoDB 索引与页",
        "feature": "compact-record",
        "title": "紧凑记录格式",
        "page_file": "g04-compact-record.md",
        "boundary": "负责变长记录的字段偏移数组、NULL 位图与字段值的编码；"
                    "不负责记录的比较语义（rem0cmp）、"
                    "不负责页的目录组织（G03）。",
        "refs": [
            {"path": "storage/innobase/rem/rem0rec.cc", "symbols": ["rec_get_offsets",
                                                                   "rec_convert_dtuple_to_rec_comp"]},
        ],
    },
    {
        "feature_group": "G-InnoDB 索引与页",
        "feature": "record-compare",
        "title": "记录比较",
        "page_file": "g05-record-compare.md",
        "boundary": "负责两条记录按键值的比较（定长与变长字段各自规则）"
                    "以及比较函数的构造；不负责页目录的定位（G03）、"
                    "不负责排序键的业务语义（C 组）。",
        "refs": [
            {"path": "storage/innobase/rem/rem0cmp.cc", "symbols": ["cmp_dtuple_rec_with_match_low",
                                                                     "cmp_dtuple_rec"]},
        ],
    },

    # ============ H InnoDB 缓冲池 ============
    {
        "feature_group": "H-InnoDB 缓冲池",
        "feature": "buf-page-get",
        "title": "缓冲池取页",
        "page_file": "h01-buf-page-get.md",
        "boundary": "负责按页号在缓冲池里找到或读入页面、加引用计数与"
                    "钉住页面；不负责页的写回（H02）、"
                    "不负责 LRU 淘汰与脏页刷新（H03）。",
        "refs": [
            {"path": "storage/innobase/buf/buf0buf.cc", "symbols": ["buf_page_get_gen",
                                                                    "buf_pool_get"]},
        ],
    },
    {
        "feature_group": "H-InnoDB 缓冲池",
        "feature": "buf-flush",
        "title": "脏页刷新",
        "page_file": "h02-buf-flush.md",
        "boundary": "负责挑选脏页写回磁盘、批量刷新与刷脏进度控制；"
                    "不负责页的取用（H01）、不负责 redo 日志的写入（I 组）。",
        "refs": [
            {"path": "storage/innobase/buf/buf0flu.cc",
             "symbols": ["buf_flush_page_coordinator_thread",
                         "buf_flush_page_cleaner_thread"]},
        ],
    },
    {
        "feature_group": "H-InnoDB 缓冲池",
        "feature": "buf-lru-eviction",
        "title": "LRU 淘汰",
        "page_file": "h03-buf-lru-eviction.md",
        "boundary": "负责缓冲池空闲页的 LRU 链表维护与淘汰页的选择；"
                    "不负责写回（H02）、不负责页的取用（H01）。",
        "refs": [
            {"path": "storage/innobase/buf/buf0lru.cc", "symbols": ["buf_LRU_free_page",
                                                                   "buf_LRU_stat_update"]},
            {"path": "storage/innobase/buf/buf0buf.cc", "symbols": ["buf_pool_free"]},
        ],
    },
    {
        "feature_group": "H-InnoDB 缓冲池",
        "feature": "buf-pool-instance",
        "title": "缓冲池多实例",
        "page_file": "h04-buf-pool-instance.md",
        "boundary": "负责把缓冲池拆成多个实例以减少互斥竞争、"
                    "以及页到实例的归属计算；不负责实例内部的 LRU 链表（H03）、"
                    "不负责页的读入（H01）。",
        "refs": [
            {"path": "storage/innobase/buf/buf0buf.cc", "symbols": ["buf_pool_init",
                                                                    "buf_pool_free_instance",
                                                                    "buf_pool_invalidate_instance"]},
        ],
    },

    # ============ I InnoDB 日志与恢复 ============
    {
        "feature_group": "I-InnoDB 日志与恢复",
        "feature": "redo-log-write",
        "title": "redo 日志写入",
        "page_file": "i01-redo-log-write.md",
        "boundary": "负责把修改记录进 redo 日志缓冲并按 LSN 推进落盘；"
                    "不负责崩溃恢复（I03）、不负责脏页写回（H02）、"
                    "不负责 undo 日志（K 组）。",
        "refs": [
            {"path": "storage/innobase/log/log0write.cc", "symbols": ["log_write_up_to",
                                                                     "log_write_buffer"]},
        ],
    },
    {
        "feature_group": "I-InnoDB 日志与恢复",
        "feature": "log-checkpoint",
        "title": "检查点",
        "page_file": "i02-log-checkpoint.md",
        "boundary": "负责推进检查点 LSN、决定哪些脏页可以不再要求 redo 重做；"
                    "不负责 redo 的写入（I01）、不负责脏页刷新（H02）。",
        "refs": [
            {"path": "storage/innobase/log/log0log.cc", "symbols": ["log_checkpoint"]},
            {"path": "storage/innobase/log/log0write.cc", "symbols": ["log_write_up_to"]},
        ],
    },
    {
        "feature_group": "I-InnoDB 日志与恢复",
        "feature": "crash-recovery",
        "title": "崩溃恢复",
        "page_file": "i03-crash-recovery.md",
        "boundary": "负责启动时扫描 redo 日志、重做已提交但未落盘的修改；"
                    "不负责 undo 回滚（K 组）、不负责日志的写入（I01）。",
        "refs": [
            {"path": "storage/innobase/log/log0recv.cc",
             "symbols": ["recv_recovery_from_checkpoint_start",
                         "recv_recovery_from_checkpoint_finish",
                         "recv_init_crash_recovery"]},
        ],
    },
    {
        "feature_group": "I-InnoDB 日志与恢复",
        "feature": "file-io",
        "title": "文件 I/O 封装",
        "page_file": "i04-file-io.md",
        "boundary": "负责 InnoDB 层的文件读写、预读与同步（fsync）的封装；"
                    "不负责 OS 层接口（J 组 mysys）、不负责 redo 的语义（I01）。",
        "refs": [
            {"path": "storage/innobase/fil/fil0fil.cc", "symbols": ["fil_io",
                                                                    "fil_flush"]},
        ],
    },

    # ============ J InnoDB 事务与锁 ============
    {
        "feature_group": "J-InnoDB 事务与锁",
        "feature": "trx-lifecycle",
        "title": "事务对象生命周期",
        "page_file": "j01-trx-lifecycle.md",
        "boundary": "负责 InnoDB 事务对象的建立、提交与回滚"
                    "（含 undo 的生成与丢弃时机）；不负责行级锁的判定（J02）、"
                    "不负责 MVCC 的可见性判定（K 组）。",
        "refs": [
            {"path": "storage/innobase/trx/trx0trx.cc", "symbols": ["trx_commit_for_mysql",
                                                                   "trx_create_low"]},
            {"path": "storage/innobase/trx/trx0sys.cc", "symbols": ["trx_sys_write_max_trx_id"]},
        ],
    },
    {
        "feature_group": "J-InnoDB 事务与锁",
        "feature": "row-lock",
        "title": "行锁与锁等待",
        "page_file": "j02-row-lock.md",
        "boundary": "负责记录锁与间隙锁的加锁判定、锁等待队列与死锁检测；"
                    "不负责元数据锁（F03）、不负责意向锁的实现细节（J 组）。",
        "refs": [
            {"path": "storage/innobase/lock/lock0lock.cc",
             "symbols": ["lock_table", "lock_clust_rec_read_check_and_lock",
                         "lock_rec_lock"]},
        ],
    },
    {
        "feature_group": "J-InnoDB 事务与锁",
        "feature": "lock-latch",
        "title": "闩锁",
        "page_file": "j03-lock-latch.md",
        "boundary": "负责保护内存结构的互斥闩（latch）与它们在页、缓冲池、"
                    "字典结构上的使用；不负责事务级的行锁（J02）。",
        "refs": [
            {"path": "storage/innobase/lock/lock0latches.cc",
             "symbols": ["Latches::Page_shards::get_shard",
                         "Latches::Table_shards::get_shard"]},
        ],
    },
    {
        "feature_group": "J-InnoDB 事务与锁",
        "feature": "read-view",
        "title": "读视图",
        "page_file": "j04-read-view.md",
        "boundary": "负责维护一致性读视图（活跃事务列表）并据此判定可见性；"
                    "不负责行锁（J02）、不负责 undo 的结构（K 组）。",
        "refs": [
            {"path": "storage/innobase/trx/trx0sys.cc", "symbols": ["ReadView::check_trx_id_sanity",
                                                                     "trx_sys_write_max_trx_id"]},
        ],
    },
    {
        "feature_group": "J-InnoDB 事务与锁",
        "feature": "undo-log",
        "title": "undo 日志",
        "page_file": "j05-undo-log.md",
        "boundary": "负责记录回滚所需的旧版本信息、undo 链的遍历与"
                    "purge 时的物理删除；不负责行锁（J02）、"
                    "不负责 redo 日志（I 组）。",
        "refs": [
            {"path": "storage/innobase/trx/trx0undo.cc", "symbols": ["trx_undo_page_init",
                                                                     "trx_undo_seg_create",
                                                                     "trx_undo_write_xid"]},
        ],
    },
    {
        "feature_group": "J-InnoDB 事务与锁",
        "feature": "group-commit",
        "title": "组提交",
        "page_file": "j06-group-commit.md",
        "boundary": "负责把多个事务的提交合并成一次日志落盘与 fsync，"
                    "以摊薄提交延迟；不负责单事务的提交路径（J01）、"
                    "不负责 binlog 的组提交（L 组）。",
        "refs": [
            {"path": "storage/innobase/log/log0write.cc", "symbols": ["log_flush_low",
                                                                     "log_flusher"]},
        ],
    },

    # ============ K InnoDB 行操作与可见性 ============
    {
        "feature_group": "K-InnoDB 行操作与可见性",
        "feature": "row-insert",
        "title": "插入行",
        "page_file": "k01-row-insert.md",
        "boundary": "负责把 server 层的一行数据转换成聚簇索引与各二级索引的"
                    "插入操作；不负责 B-tree 的分裂实现（G02）、"
                    "不负责 undo 生成（J05）。",
        "refs": [
            {"path": "storage/innobase/row/row0mysql.cc", "symbols": ["row_insert_for_mysql",
                                                                     "row_ins_step"]},
        ],
    },
    {
        "feature_group": "K-InnoDB 行操作与可见性",
        "feature": "row-update",
        "title": "更新行",
        "page_file": "k02-row-update.md",
        "boundary": "负责按主键定位旧版本、生成新版本并维护二级索引的变更；"
                    "不负责索引的物理维护（G02）、不负责 undo（K 组 J05）。",
        "refs": [
            {"path": "storage/innobase/row/row0mysql.cc", "symbols": ["row_update_for_mysql",
                                                                     "row_upd_step"]},
        ],
    },
    {
        "feature_group": "K-InnoDB 行操作与可见性",
        "feature": "row-delete",
        "title": "删除行",
        "page_file": "k03-row-delete.md",
        "boundary": "负责删除聚簇索引记录并把二级索引标记为删除；"
                    "不负责物理清理（purge 属 J05）、"
                    "不负责 undo 生成（J05）。",
        "refs": [
            {"path": "storage/innobase/row/row0mysql.cc",
             "symbols": ["row_delete_for_mysql_using_cursor",
                         "btr_cur_del_mark_set_clust_rec"]},
        ],
    },
    {
        "feature_group": "K-InnoDB 行操作与可见性",
        "feature": "mvcc-visibility",
        "title": "MVCC 可见性判定",
        "page_file": "k04-mvcc-visibility.md",
        "boundary": "负责在一致性读里按读视图判定某版本对当前事务是否可见、"
                    "以及在写锁下判定是否需要等待；不负责锁的排队（J02）、"
                    "不负责 undo 的存储（J05）。",
        "refs": [
            {"path": "storage/innobase/row/row0sel.cc", "symbols": ["row_search_mvcc",
                                                                   "row_sel_get_clust_rec"]},
        ],
    },

    # ============ L 二进制日志 ============
    {
        "feature_group": "L-二进制日志",
        "feature": "binlog-event-write",
        "title": "binlog 事件写入",
        "page_file": "l01-binlog-event-write.md",
        "boundary": "负责把事件序列化成 binlog 文件格式并写入日志文件、"
                    "维护事件缓存与事务边界事件；不负责 binlog 的复制传输（M 组）、"
                    "不负责 redo 日志（I 组）。",
        "refs": [
            {"path": "sql/log_event.cc", "symbols": ["Log_event::write",
                                                    "Rows_log_event"]},
            {"path": "sql/binlog.cc", "symbols": ["MYSQL_BIN_LOG::write",
                                                  "MYSQL_BIN_LOG::do_write_cache"]},
        ],
    },
    {
        "feature_group": "L-二进制日志",
        "feature": "binlog-statement-vs-row",
        "title": "语句格式与行格式",
        "page_file": "l02-binlog-statement-vs-row.md",
        "boundary": "负责决定一条写操作以语句格式还是行格式记入 binlog、"
                    "以及行镜像的构造规则；不负责事件的文件格式（L01）、"
                    "不负责复制端的重放（M 组）。",
        "refs": [
            {"path": "sql/log_event.cc", "symbols": ["Rows_log_event::Rows_log_event",
                                                    "Query_log_event::Query_log_event"]},
        ],
    },
    {
        "feature_group": "L-二进制日志",
        "feature": "binlog-cache-flush",
        "title": "binlog 缓存与刷盘",
        "page_file": "l03-binlog-cache-flush.md",
        "boundary": "负责事务内事件的暂存缓存、事务提交时一次性落盘、"
                    "以及 binlog 与 InnoDB 提交的两阶段配合；不负责事件序列化（L01）、"
                    "不负责从库接收（M 组）。",
        "refs": [
            {"path": "sql/binlog.cc", "symbols": ["MYSQL_BIN_LOG::flush_cache_to_file",
                                                  "MYSQL_BIN_LOG::commit",
                                                  "MYSQL_BIN_LOG::finish_commit"]},
        ],
    },
    {
        "feature_group": "L-二进制日志",
        "feature": "binlog-dump-source",
        "title": "binlog 发送端",
        "page_file": "l04-binlog-dump-source.md",
        "boundary": "负责把 binlog 内容按复制协议推送给从库："
                    "游标定位、事件读取与网络发送；不负责复制关系的管理（M02）、"
                    "不负责 GTID 的挑选（M03）。",
        "refs": [
            {"path": "sql/rpl_source.cc", "symbols": ["mysql_binlog_send",
                                                     "com_binlog_dump"]},
        ],
    },
    {
        "feature_group": "L-二进制日志",
        "feature": "binlog-rotation",
        "title": "binlog 轮转与清理",
        "page_file": "l05-binlog-rotation.md",
        "boundary": "负责 binlog 文件的轮转、过期文件清理与索引文件的维护；"
                    "不负责事件内容（L01）、不负责从库的重放（M 组）。",
        "refs": [
            {"path": "sql/binlog.cc", "symbols": ["MYSQL_BIN_LOG::rotate",
                                                  "MYSQL_BIN_LOG::purge_logs",
                                                  "MYSQL_BIN_LOG::add_log_to_index"]},
        ],
    },

    # ============ M 复制与 GTID ============
    {
        "feature_group": "M-复制与 GTID",
        "feature": "replica-io-thread",
        "title": "从库 IO 线程",
        "page_file": "m01-replica-io-thread.md",
        "boundary": "负责从库连接源库拉取 binlog 并写入中继日志；"
                    "不负责事件的应用（M02）、不负责 GTID 的集合运算（M03）。",
        "refs": [
            {"path": "sql/rpl_replica.cc", "symbols": ["exec_relay_log_event",
                                                      "apply_event_and_update_pos"]},
        ],
    },
    {
        "feature_group": "M-复制与 GTID",
        "feature": "replica-applier",
        "title": "从库应用线程",
        "page_file": "m02-replica-applier.md",
        "boundary": "负责从中继日志读取事件并按并行度协调应用、"
                    "以及回放位点的持久化；不负责事件拉取（M01）、"
                    "不负责 GTID 集合（M03）。",
        "refs": [
            {"path": "sql/rpl_replica.cc", "symbols": ["start_slave", "stop_slave"]},
            {"path": "sql/rpl_channel_service_interface.cc", "symbols": ["start_slave"]},
        ],
    },
    {
        "feature_group": "M-复制与 GTID",
        "feature": "gtid-set",
        "title": "GTID 集合",
        "page_file": "m03-gtid-set.md",
        "boundary": "负责 GTID 集合的表示、区间合并与包含关系判定；"
                    "不负责 GTID 的分配与持久化（M04）、"
                    "不负责从库已执行集合的维护（M05）。",
        "refs": [
            {"path": "sql/rpl_gtid_set.cc", "symbols": ["Gtid_set::add_gtid_set",
                                                        "Gtid_set::is_subset"]},
        ],
    },
    {
        "feature_group": "M-复制与 GTID",
        "feature": "gtid-allocation",
        "title": "GTID 分配与持久化",
        "page_file": "m04-gtid-allocation.md",
        "boundary": "负责事务 GTID 的分配、以及全局已执行 GTID 集合的落盘；"
                    "不负责集合的运算（M03）、不负责 binlog 里的 GTID 事件"
                    "（L 组）。",
        "refs": [
            {"path": "sql/rpl_gtid_persist.cc", "symbols": ["Gtid_table_persistor::save",
                                                           "Gtid_table_persistor::write_row"]},
            {"path": "sql/rpl_gtid_mode.cc", "symbols": ["Gtid_mode::set",
                                                         "Gtid_mode::get"]},
        ],
    },
    {
        "feature_group": "M-复制与 GTID",
        "feature": "replication-filter",
        "title": "复制过滤规则",
        "page_file": "m05-replication-filter.md",
        "boundary": "负责按库表白名单/黑名单过滤复制事件；"
                    "不负责事件内容（L 组）、不负责从库的位点管理（M02）。",
        "refs": [
            {"path": "sql/rpl_filter.cc", "symbols": ["Rpl_filter::tables_ok",
                                                      "Rpl_filter::reset"]},
        ],
    },
    {
        "feature_group": "M-复制与 GTID",
        "feature": "write-set-tracking",
        "title": "写集跟踪",
        "page_file": "m06-write-set-tracking.md",
        "boundary": "负责记录事务写过的行/表以便并行复制判定依赖；"
                    "不负责并行复制的调度（M02）、不负责 binlog 事件内容（L 组）。",
        "refs": [
            {"path": "sql/rpl_trx_tracking.cc",
             "symbols": ["Writeset_trx_dependency_tracker::get_dependency",
                         "Transaction_dependency_tracker::step"]},
        ],
    },

    # ============ N 权限与认证 ============
    {
        "feature_group": "N-权限与认证",
        "feature": "authentication",
        "title": "认证插件",
        "page_file": "n01-authentication.md",
        "boundary": "负责用户口令的校验流程与认证插件的调用约定；"
                    "不负责权限判定（N02）、不负责连接建立（A 组）。",
        "refs": [
            {"path": "sql/auth/sql_authentication.cc", "symbols": ["acl_authenticate"]},
            {"path": "sql/sql_connect.cc", "symbols": ["acl_authenticate"]},
        ],
    },
    {
        "feature_group": "N-权限与认证",
        "feature": "privilege-check",
        "title": "权限判定",
        "page_file": "n02-privilege-check.md",
        "boundary": "负责按用户与角色表判定一次操作是否被授权、"
                    "以及权限缓存的失效；不负责认证（N01）、"
                    "不负责行级与列级权限的施加（F 组）。",
        "refs": [
            {"path": "sql/auth/sql_authorization.cc", "symbols": ["check_access"]},
            {"path": "sql/auth/sql_security_ctx.cc", "symbols": ["Security_context::check_access"]},
        ],
    },
    {
        "feature_group": "N-权限与认证",
        "feature": "role-model",
        "title": "角色模型",
        "page_file": "n03-role-model.md",
        "boundary": "负责角色的创建、授予与角色到权限的展开；"
                    "不负责权限判定的实现（N02）、不负责认证（N01）。",
        "refs": [
            {"path": "sql/auth/roles.cc", "symbols": ["Role_activation::activate",
                                                     "Role_activation::activate_role_all"]},
            {"path": "sql/auth/sql_authorization.cc", "symbols": ["mysql_grant_role",
                                                                 "mysql_revoke_role"]},
        ],
    },
    {
        "feature_group": "N-权限与认证",
        "feature": "perfschema",
        "title": "性能监控",
        "page_file": "n04-perfschema.md",
        "boundary": "负责采集与暴露运行期指标（等待事件、语句摘要、表与索引统计）；"
                    "不负责慢查询日志（属于日志体系）、"
                    "不负责语句执行本身（D 组）。",
        "refs": [
            {"path": "storage/perfschema/pfs.cc", "symbols": ["pfs"]},
            {"path": "storage/perfschema/pfs_instr_class.cc", "symbols": ["PFS_instr_class"]},
        ],
    },
]
