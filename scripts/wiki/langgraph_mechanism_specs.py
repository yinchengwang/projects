# -*- coding: utf-8 -*-
"""langgraph 机制清单（阶段 1：首个 AI / Agent 项目）。

盘点范围：`libs/langgraph` 包本体（Pregel 执行引擎、通道、检查点、流式、中断、
运行时）。同仓的 `libs/checkpoint*`、`libs/prebuilt`、`libs/cli`、`libs/sdk-*`
属于后续增量，不在本清单内 —— 它们各自有独立包边界，需单独盘点。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析，能区分类方法、
嵌套函数、`async def`），再按包内目录骨架归并成机制簇。**篇数是归并结果，
不是按文件数折算**。

源码锚点：`langgraph@3e3db42c5329dd404669ccd923c005c814bee3a3`。
"""

GROUP_ORDER = [
    "A-图构建与状态模式",
    "B-通道与状态合并",
    "C-Pregel 执行循环",
    "D-任务调度与并发",
    "E-检查点与恢复",
    "F-通道读写",
    "G-流式输出",
    "H-中断超时与校验",
    "I-运行时上下文",
]

MECHANISM_SPECS = [
    # ============ A 图构建与状态模式 ============
    {
        "feature_group": "A-图构建与状态模式",
        "feature": "stategraph-builder",
        "title": "StateGraph 构建器与状态模式注册",
        "page_file": "a01-stategraph-builder.md",
        "boundary": "负责把「状态 schema 列表 + 节点 + 边」编译成可执行的 Pregel 图，"
                    "包括 schema 合并与通道推导；不负责运行时调度（C 组）、"
                    "不负责条件路由的求值（a04）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/graph/state.py",
             "symbols": ["StateGraph", "_add_schema", "_all_edges"]},
        ],
    },
    {
        "feature_group": "A-图构建与状态模式",
        "feature": "add-node-overloads",
        "title": "add_node 的重载与节点包装",
        "page_file": "a02-add-node-overloads.md",
        "boundary": "负责 add_node 如何按调用签名选择包装方式（Runnable / 可调用 / 带 "
                    "config、writer、store 参数），并把节点名与通道写入声明记进图；"
                    "不负责节点内部业务逻辑，也不负责运行期参数注入的实现（a03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/graph/state.py", "symbols": ["add_node"]},
            {"path": "libs/langgraph/langgraph/graph/_node.py",
             "symbols": ["_Node", "_NodeWithConfig", "_NodeWithWriterStore"]},
        ],
    },
    {
        "feature_group": "A-图构建与状态模式",
        "feature": "edge-and-conditional",
        "title": "静态边与条件边登记",
        "page_file": "a03-edge-and-conditional.md",
        "boundary": "负责 add_edge / add_conditional_edges 如何在图上登记路由关系、"
                    "START/END 虚拟节点如何转成入口出口；条件分支的求值过程归 a04，"
                    "不涉及通道合并（B 组）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/graph/state.py",
             "symbols": ["add_edge", "add_conditional_edges"]},
        ],
    },
    {
        "feature_group": "A-图构建与状态模式",
        "feature": "branch-spec-route",
        "title": "分支规格与路由求值",
        "page_file": "a04-branch-spec-route.md",
        "boundary": "负责把用户路由函数包装成 BranchSpec，在运行期读出输入、求值路径、"
                    "把目标节点名转成发送与写入；不负责目标节点的执行（C 组）、"
                    "不负责写入合并算法（c05）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/graph/_branch.py",
             "symbols": ["BranchSpec", "_route", "_aroute"]},
        ],
    },

    # ============ B 通道与状态合并 ============
    {
        "feature_group": "B-通道与状态合并",
        "feature": "channel-base",
        "title": "BaseChannel 契约与检查点协议",
        "page_file": "b01-channel-base.md",
        "boundary": "负责通道基类定义的状态语义边界：ValueType / UpdateType 区分、"
                    "checkpoint / from_checkpoint 序列化协议、consume 与 finish 的时机；"
                    "不负责各具体通道的合并语义（b02–b07）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/channels/base.py",
             "symbols": ["BaseChannel", "ValueType", "UpdateType"]},
        ],
    },
    {
        "feature_group": "B-通道与状态合并",
        "feature": "last-value-channel",
        "title": "LastValue 与结束可见语义",
        "page_file": "b02-last-value-channel.md",
        "boundary": "负责覆盖写语义的通道实现，以及 LastValueAfterFinish 在节点全部结束时"
                    "才可见的差异；不负责聚合语义（b03）、不负责增量通道（b06）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/channels/last_value.py",
             "symbols": ["LastValue", "LastValueAfterFinish", "update"]},
        ],
    },
    {
        "feature_group": "B-通道与状态合并",
        "feature": "binop-aggregate",
        "title": "二元聚合通道与覆盖判定",
        "page_file": "b03-binop-aggregate.md",
        "boundary": "负责以二元算子累积更新的通道：如何识别「整值覆盖」写入、算子相等时"
                    "如何短路；不负责 Topic 的多路累积（b04）、不负责屏障同步（b06）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/channels/binop.py",
             "symbols": ["BinaryOperatorAggregate", "_get_overwrite", "_operators_equal"]},
        ],
    },
    {
        "feature_group": "B-通道与状态合并",
        "feature": "topic-channel",
        "title": "Topic 累积通道与元组展平",
        "page_file": "b04-topic-channel.md",
        "boundary": "负责把多次写入累积成列表语义、以及元组写入的展平规则和可选累加上限；"
                    "不负责二元算子聚合（b03）、不负责增量通道的溢出（b06）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/channels/topic.py",
             "symbols": ["Topic", "_flatten"]},
        ],
    },
    {
        "feature_group": "B-通道与状态合并",
        "feature": "ephemeral-any-untracked",
        "title": "临时值、任意值与不追踪通道",
        "page_file": "b05-ephemeral-any-untracked.md",
        "boundary": "负责三个生命周期受限的通道：临时值（读完即清）、任意值（不做类型"
                    "约束）、不追踪值（不进检查点）；不负责可持久化的 LastValue（b02）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/channels/ephemeral_value.py",
             "symbols": ["EphemeralValue"]},
            {"path": "libs/langgraph/langgraph/channels/any_value.py",
             "symbols": ["AnyValue"]},
            {"path": "libs/langgraph/langgraph/channels/untracked_value.py",
             "symbols": ["UntrackedValue"]},
        ],
    },
    {
        "feature_group": "B-通道与状态合并",
        "feature": "barrier-and-delta-channels",
        "title": "命名屏障与增量通道",
        "page_file": "b06-barrier-and-delta-channels.md",
        "boundary": "负责「等齐若干上游写才放行」的屏障语义，以及只为增量回放保存差异的"
                    "DeltaChannel；不负责普通覆盖写（b02）、不负责快照溢出判定（e03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/channels/named_barrier_value.py",
             "symbols": ["NamedBarrierValue", "NamedBarrierValueAfterFinish"]},
            {"path": "libs/langgraph/langgraph/channels/delta.py",
             "symbols": ["DeltaChannel", "replay_writes"]},
        ],
    },

    # ============ C Pregel 执行循环 ============
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "pregel-object",
        "title": "Pregel 图对象与检查点解析",
        "page_file": "c01-pregel-object.md",
        "boundary": "负责把编译后的图装配成 Pregel 实例：节点表、通道表、检查点器解析、"
                    "流式模式归一；不负责每轮步进（c02）、不负责节点并发执行（d01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/main.py",
             "symbols": ["Pregel", "_resolve_checkpointer"]},
        ],
    },
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "node-builder",
        "title": "NodeBuilder 的订阅与写入声明",
        "page_file": "c02-node-builder.md",
        "boundary": "负责在编译期把每个节点声明成「订阅哪些通道、写到哪些通道」，"
                    "包括缓存策略、重试策略、超时设置的挂载；不负责运行期读写执行（F 组）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/main.py",
             "symbols": ["NodeBuilder", "subscribe_to", "write_to", "build"]},
        ],
    },
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "pregel-loop-tick",
        "title": "PregelLoop 单轮步进",
        "page_file": "c03-pregel-loop-tick.md",
        "boundary": "负责一轮 superstep 的生命周期：tick 判定是否继续、after_tick 收尾"
                    "并推进版本；不负责任务并发（d01）、不负责快照落盘细节（e02）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_loop.py",
             "symbols": ["PregelLoop", "tick", "after_tick"]},
        ],
    },
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "pending-writes",
        "title": "挂起写入与恢复接入",
        "page_file": "c04-pending-writes.md",
        "boundary": "负责错误处理器产生的挂起写入如何暂存、如何在下一轮被重新应用；"
                    "不负责写入合并算法本身（c05）、不负责检查点结构（e01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_loop.py",
             "symbols": ["put_writes", "_put_pending_writes", "accept_push"]},
        ],
    },
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "apply-writes-algo",
        "title": "写入合并算法",
        "page_file": "c05-apply-writes-algo.md",
        "boundary": "负责把一批任务的写入按通道聚合成新状态，包括写入去重、"
                    "版本号递增与本地读取视图；不负责推导下一批任务（c06）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_algo.py",
             "symbols": ["apply_writes", "increment", "local_read"]},
        ],
    },
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "prepare-next-tasks",
        "title": "下一批任务的推导",
        "page_file": "c06-prepare-next-tasks.md",
        "boundary": "负责根据本轮写入与触发条件推导下一批要跑的任务，包括任务 ID 生成、"
                    "触发通道判定与任务去重；不负责执行这些任务（d01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_algo.py",
             "symbols": ["prepare_next_tasks", "_TaskIDFn"]},
        ],
    },
    {
        "feature_group": "C-Pregel 执行循环",
        "feature": "task-writes-protocol",
        "title": "任务写入协议对象",
        "page_file": "c07-task-writes-protocol.md",
        "boundary": "负责任务结果写入的协议抽象：路径、名称、写入列表与触发通道；"
                    "不负责写入的存储与合并（c05）、不负责流式事件（G 组）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_algo.py",
             "symbols": ["WritesProtocol", "PregelTaskWrites", "Call"]},
        ],
    },

    # ============ D 任务调度与并发 ============
    {
        "feature_group": "D-任务调度与并发",
        "feature": "pregel-runner",
        "title": "PregelRunner 并发执行与提交",
        "page_file": "d01-pregel-runner.md",
        "boundary": "负责一批任务如何并发提交、如何等待、如何在异常时把已成功的写入提交"
                    "并把失败包装成挂起写；不负责任务推导（c06）、不负责线程池实现（d03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_runner.py",
             "symbols": ["PregelRunner", "commit", "_panic_or_proceed"]},
        ],
    },
    {
        "feature_group": "D-任务调度与并发",
        "feature": "futures-dict",
        "title": "完成回调与结果表",
        "page_file": "d02-futures-dict.md",
        "boundary": "负责在任务完成时登记结果、回调消费与顺序无关性；不负责任务提交策略"
                    "（d01）、不负责异步执行器（d03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_runner.py",
             "symbols": ["FuturesDict", "on_done"]},
        ],
    },
    {
        "feature_group": "D-任务调度与并发",
        "feature": "background-executor",
        "title": "后台执行器与异步门控",
        "page_file": "d03-background-executor.md",
        "boundary": "负责同步/异步两条后台执行通道：提交、完成检查、上下文管理，"
                    "以及把并发槽位做成可门控的包装；不负责任务语义（c06）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_executor.py",
             "symbols": ["BackgroundExecutor", "AsyncBackgroundExecutor", "gated"]},
        ],
    },

    # ============ E 检查点与恢复 ============
    {
        "feature_group": "E-检查点与恢复",
        "feature": "checkpoint-shape",
        "title": "检查点结构与快照创建",
        "page_file": "e01-checkpoint-shape.md",
        "boundary": "负责检查点的字段构成（通道值、版本、任务、挂起写）与从循环内创建"
                    "快照的路径；不负责版本递增细节（e02）、不负责增量通道（e03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_checkpoint.py",
             "symbols": ["empty_checkpoint", "create_checkpoint"]},
        ],
    },
    {
        "feature_group": "E-检查点与恢复",
        "feature": "checkpoint-versions",
        "title": "版本推进与已见标记",
        "page_file": "e02-checkpoint-versions.md",
        "boundary": "负责每个节点看到的通道版本如何记录与推进，以及「版本已见」判定如何"
                    "影响是否重跑；不负责快照结构（e01）、不负责写入重放（e04）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_checkpoint.py",
             "symbols": ["_mark_bumps_seen", "_bump_keys", "versions_seen_without_bumps"]},
        ],
    },
    {
        "feature_group": "E-检查点与恢复",
        "feature": "delta-snapshot",
        "title": "增量通道快照与溢出判定",
        "page_file": "e03-delta-snapshot.md",
        "boundary": "负责只为增量通道保存差异、判定何时必须退回全量快照；"
                    "不负责增量通道自身的合并语义（b06）、不负责重放（e04）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_checkpoint.py",
             "symbols": ["delta_channels_to_snapshot", "delta_channels_overwritten",
                         "delta_channels_with_pending_writes"]},
        ],
    },
    {
        "feature_group": "E-检查点与恢复",
        "feature": "checkpoint-replay",
        "title": "写入重放与超时判定",
        "page_file": "e04-checkpoint-replay.md",
        "boundary": "负责从历史检查点重放写入、以及「检查点已被取代」的判定；"
                    "不负责快照结构（e01）、不负责任务重新推导（c06）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_checkpoint.py",
             "symbols": ["_replay", "_delta_channels_to_replay", "checkpoint_superseded"]},
        ],
    },

    # ============ F 通道读写 ============
    {
        "feature_group": "F-通道读写",
        "feature": "channel-read",
        "title": "ChannelRead 读通道节点",
        "page_file": "f01-channel-read.md",
        "boundary": "负责读通道在 Runnable 链中的位置：从状态里取订阅值、缺值时的错误"
                    "语义；不负责写入（f03）、不负责节点组装（f02）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_read.py",
             "symbols": ["ChannelRead", "do_read"]},
        ],
    },
    {
        "feature_group": "F-通道读写",
        "feature": "pregel-node",
        "title": "PregelNode 的读写编排",
        "page_file": "f02-pregel-node.md",
        "boundary": "负责把「读通道 → 用户函数 → 写通道」串成一个可 invoke / stream 的节点，"
                    "以及触发器与缓存键的接线；不负责单个读写的语义（f01、f03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_read.py",
             "symbols": ["PregelNode", "flat_writers", "invoke"]},
        ],
    },
    {
        "feature_group": "F-通道读写",
        "feature": "channel-write",
        "title": "ChannelWrite 写通道节点",
        "page_file": "f03-channel-write.md",
        "boundary": "负责写通道如何把函数返回值按静态写入声明装配成通道写入包，"
                    "以及写入器动态注册；不负责读（f01）、不负责合并（c05）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_write.py",
             "symbols": ["ChannelWrite", "do_write", "_assemble_writes"]},
        ],
    },
    {
        "feature_group": "F-通道读写",
        "feature": "io-mapping",
        "title": "输入输出与命令映射",
        "page_file": "f04-io-mapping.md",
        "boundary": "负责外部输入如何映射成通道写入、状态如何投影成输出、以及 Command "
                    "对象的跳转语义；不负责读写节点的实现（f01、f03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_io.py",
             "symbols": ["map_input", "map_output_values", "map_command"]},
        ],
    },

    # ============ G 流式输出 ============
    {
        "feature_group": "G-流式输出",
        "feature": "stream-mux",
        "title": "StreamMux 多路复用与推流",
        "page_file": "g01-stream-mux.md",
        "boundary": "负责多个流通道到单一事件流的合并：注册、推入、关闭与失败传播；"
                    "不负责事件类型定义（g04）、不负责用户侧迭代器（g03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/stream/_mux.py",
             "symbols": ["StreamMux", "push", "close"]},
        ],
    },
    {
        "feature_group": "G-流式输出",
        "feature": "stream-transformers",
        "title": "内置流式转换器",
        "page_file": "g02-stream-transformers.md",
        "boundary": "负责 values / updates / messages / custom 等内置转换器如何把内部事件"
                    "转成用户可见的流片段；不负责多路合并（g01）、不负责驱动（g03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/stream/transformers.py",
             "symbols": ["ValuesTransformer", "UpdatesTransformer", "MessagesTransformer"]},
        ],
    },
    {
        "feature_group": "G-流式输出",
        "feature": "run-stream-driver",
        "title": "流式驱动与投影",
        "page_file": "g03-run-stream-driver.md",
        "boundary": "负责把一次运行包成可迭代的流对象：按需拉取、事件投影、中断与出口；"
                    "不负责转换器语义（g02）、不负责事件类型（g04）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/stream/run_stream.py",
             "symbols": ["GraphRunStream", "_pump_next", "_drive_until_done"]},
        ],
    },
    {
        "feature_group": "G-流式输出",
        "feature": "stream-part-types",
        "title": "流事件类型与负载",
        "page_file": "g04-stream-part-types.md",
        "boundary": "负责公开事件类型的字段约定（值、更新、消息、调试、工具）；"
                    "不负责产生事件（g02）、不负责非流式的任务负载（c07）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/types.py",
             "symbols": ["ValuesStreamPart", "MessagesStreamPart", "DebugStreamPart"]},
        ],
    },

    # ============ H 中断超时与校验 ============
    {
        "feature_group": "H-中断超时与校验",
        "feature": "interrupts-and-errors",
        "title": "中断、父命令与图错误",
        "page_file": "h01-interrupts-and-errors.md",
        "boundary": "负责中断的抛出/捕获路径、父命令越级语义与图级错误分类；"
                    "不负责超时（h02）、不负责错误处理器路由（d01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/errors.py",
             "symbols": ["GraphInterrupt", "ParentCommand", "NodeInterrupt"]},
        ],
    },
    {
        "feature_group": "H-中断超时与校验",
        "feature": "timeout-scope",
        "title": "超时解析与尝试范围",
        "page_file": "h02-timeout-scope.md",
        "boundary": "负责把多种超时写法解析成实际秒数、以及一次尝试的计时范围与空闲超时"
                    "等待；不负责重试策略本身（a02 的 retry 挂载）、不负责中断（h01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_retry.py",
             "symbols": ["_resolve_timeout", "_TimedAttemptScope", "wait_for_idle_timeout"]},
        ],
    },
    {
        "feature_group": "H-中断超时与校验",
        "feature": "tool-call-streaming",
        "title": "工具调用流处理",
        "page_file": "h03-tool-call-streaming.md",
        "boundary": "负责把 LLM 的 tool call 生命周期（开始 / 结束 / 出错）转成流事件；"
                    "不负责通用消息转换器（g02）、不负责工具执行本身。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_tools.py",
             "symbols": ["StreamToolCallHandler", "on_tool_start", "tap_output_iter"]},
        ],
    },
    {
        "feature_group": "H-中断超时与校验",
        "feature": "graph-validation",
        "title": "图结构与键校验",
        "page_file": "h04-graph-validation.md",
        "boundary": "负责在编译期检查图是否合法（可达性、重复节点、保留键冲突）；"
                    "不负责运行期状态校验，也不负责 schema 合并（a01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/pregel/_validate.py",
             "symbols": ["validate_graph", "validate_keys"]},
        ],
    },

    # ============ I 运行时上下文 ============
    {
        "feature_group": "I-运行时上下文",
        "feature": "runtime-object",
        "title": "Runtime 与执行信息",
        "page_file": "i01-runtime-object.md",
        "boundary": "负责节点可访问的运行时对象：执行信息、服务器信息、排空控制与覆盖；"
                    "不负责配置读取（i02）、不负责序列化（i03）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/runtime.py",
             "symbols": ["Runtime", "ExecutionInfo", "RunControl"]},
        ],
    },
    {
        "feature_group": "I-运行时上下文",
        "feature": "config-accessors",
        "title": "配置、存储与流写入器取用",
        "page_file": "i02-config-accessors.md",
        "boundary": "负责节点内部如何从上下文取配置、取 store、取流写入器；"
                    "不负责运行时对象结构（i01）、不负责配置值语义（c01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/config.py",
             "symbols": ["get_config", "get_store", "get_stream_writer"]},
        ],
    },
    {
        "feature_group": "I-运行时上下文",
        "feature": "serde-allowlist",
        "title": "序列化允许清单构建",
        "page_file": "i03-serde-allowlist.md",
        "boundary": "负责从 schema 收集需要序列化的类型、构建允许清单并在检查点器上生效；"
                    "不负责检查点结构（e01）、不负责通道序列化协议（b01）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/_internal/_serde.py",
             "symbols": ["build_serde_allowlist", "collect_allowlist_from_schemas",
                         "apply_checkpointer_allowlist"]},
        ],
    },
    {
        "feature_group": "I-运行时上下文",
        "feature": "managed-values",
        "title": "托管值与剩余步数",
        "page_file": "i04-managed-values.md",
        "boundary": "负责由框架注入的托管值（剩余步数、是否最后一步）如何求值；"
                    "不负责递归上限的错误语义（h01）、不负责通道（B 组）。",
        "refs": [
            {"path": "libs/langgraph/langgraph/managed/base.py",
             "symbols": ["ManagedValue", "is_managed_value"]},
            {"path": "libs/langgraph/langgraph/managed/is_last_step.py",
             "symbols": ["IsLastStepManager", "RemainingStepsManager"]},
        ],
    },
]
