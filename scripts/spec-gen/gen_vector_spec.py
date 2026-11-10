# -*- coding: utf-8 -*-
"""生成《向量模态实现规格.html》"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_tpl import *

B = []

B.append(sec(
    "和已有两个模态的根本差异 <span>第三种存储布局</span>",
    "",
    tbl(["维度", "关系模态（heap）", "blob 模态", "向量模态"], [
        ['<span class="mono">布局</span>', '行存（slotted page，变长记录）', '对象 + 定长帧', '<b>列式</b>（一页连续放 K 个 fp32 向量）'],
        ['<span class="mono">寻址</span>', 'RID = (page_id, slot)', 'blob_id = 页号 + 1', '<b>RID = (数据页号, 行内序号)</b> —— 纯算术'],
        ['<span class="mono">标量与向量</span>', '一行 = 一条记录，所有列在一起', '一行 = 一帧原始字节', '<b>向量与标量分行存</b>，靠<b>序号对齐</b>'],
        ['<span class="mono">索引</span>', 'B+tree（一维全序）', '无（帧号即位置）', '<b>HNSW（图结构，近似）</b>'],
        ['<span class="mono">查询语义</span>', '行过滤 + 排序', '整对象读', '<b>knn：按距离排序取 top-k</b> —— 排序键是计算出来的'],
        ['<span class="mono">写入</span>', '就地更新 + 版本链', '不可变，一次写完', '<b>追加</b> + 增量插入进 HNSW'],
    ])
    + callout("列式为什么是必须的（不是风格问题）", [
        "128 维 fp32 = 512 B。若按行存，一页 4096 B 只能放 7 个向量（还要扣页头和 slot），"
        "而且<b>相邻向量分散在页的各处</b> —— 扫描一个维度、做量化训练、算距离，全都要跳读。",
        "列式下一页装 <code>(4096 − 8) / 512 = 7</code> 个向量，<b>同样是一页，但相邻向量物理相邻</b>。"
        "这不是「更优雅」，是<b>同样的页能装下同样的数据但访问模式对列友好</b>。",
        "对标 Lance / Parquet —— 这两个格式的核心不是「压缩」，是「列式布局 + 批量访问」。",
    ])
))

B.append(sec(
    "模块与行数 <span>按依赖排序，不是按重要性</span>",
    "",
    tbl(["模块", "职责", "行数", "依赖"], [
        ['<span class="mono">storage/fixed_page</span>', "<b>定长槽页下沉</b>：图模态的 gstore 搬到 L1，图和向量共用", '<span class="n">~300</span>', "pager"],
        ['<span class="mono">modalities/vector/vstore</span>', "向量列式页 + 标量槽页的寻址与读写", '<span class="n">~380</span>', "fixed_page, pager"],
        ['<span class="mono">modalities/vector/vhnsw</span>', "HNSW 构建、贪心搜索、启发式修剪、层级分配", '<span class="n">~620</span>', "vstore"],
        ['<span class="mono">modalities/vector/vector_am</span>', "SPI 适配：虚拟表、标量下推、knn 接线", '<span class="n">~340</span>', "spi, vhnsw"],
        ['<span class="mono">db_vector.c</span>', "公开 API：db_vec_knn / db_vec_knn_exact / db_vec_insert（全部持 latch）", '<span class="n">~200</span>', "vector_am, latch"],
        ['<span class="mono">测试</span>', "test_vstore / test_hnsw / test_vector_it / test_vector_crash / vector_bench", '<span class="n">~900</span>', "—"],
    ])
    + ok("<b>步骤 1 是纯搬家的前置重构</b>：图模态在 v0.11 刚把「定长记录 + 空闲链」"
         "这套原语写对并测透，向量模态的 HNSW 邻接表要的正是它 —— "
         "但<b>模态之间不许互相 include</b>（硬规则 3），所以必须下沉到 <code>storage/</code>。"
         "<b>搬完立刻跑图模态全部测试作为回归门</b>。")
))

B.append(sec(
    "七步实现顺序 <span>每步都有可量测的验收，不通过不进下一步</span>",
    "",
    steps([
        ("<b>前置重构</b>：定长槽页下沉",
         "把图模态的 <code>gstore</code> 搬到 <code>storage/fixed_page</code>，图模态改为依赖它。"
         "验收：<b>图模态全部测试原样通过</b>（纯搬家，不改逻辑）；依赖方向检查通过。",
         "~300"),
        ("向量列式存储",
         "向量页 + 标量页，行号 → 向量位置的纯算术寻址。验收：第 i 个向量的寻址算术正确；"
         "扫描 IO ≈ 连续页读（无跳读）。",
         "~380"),
        ("HNSW 构建（纯内存）",
         "层级分配、贪心搜近邻、启发式修剪。验收：<b>recall@10 ≥ 0.95</b>（10 万 × 128 维，"
         "M=16 / efC=200 / ef=64，对暴力扫 GT）；邻接表结构断言全过。",
         "~620"),
        ("落盘 + 增量插入 + 恢复",
         "索引头页 + 邻接页落盘；逐点插入进已有索引。验收：重开后 recall 与内存版一致；"
         "边插边查 recall 不塌（每插 1 万查一次，最终 ≥ 0.93）。",
         "~280"),
        ("SPI 接线",
         "<code>vector_am</code> + <code>index_am</code> 的 <code>knn</code> 首次真实现 + "
         "<code>vec_items</code> 虚拟表。验收：<b>下推行数 == 全扫过滤行数</b>（超集断言）；"
         "写操作报 E_NOTSUP。",
         "~340"),
        ("验收框架",
         "暴力扫做 ground truth + hnswlib 双重对照。验收：recall 与 hnswlib 差 &lt; 2 点；"
         "<b>IO 可证</b>：ANN 查询页读 &lt; 暴力扫的 1/20。",
         "~300"),
        ("反向验证 + 文档",
         "验收：拿掉过滤 → 结果变成超集（证明过滤在执行器侧生效）；"
         "拿掉索引 → 页读暴涨（证明索引真被用上）；旧 152+ 项全绿。",
         "~250"),
    ])
    + callout("关键提醒", [
        "<b>HNSW 写错但不报错</b>是这一版最大的风险：层级分配错、修剪判据错 → "
        "recall 低，但「能返回结果」「不崩溃」「看起来很快」——<b>所有表面信号都是绿的</b>。"
        "所以 A2 的 recall 硬指标不是可选项，是这一版的<b>唯一裁判</b>。",
        "<b>召回异常时的排查顺序</b>：先 diff 邻接表结构（每个节点的邻居数、层级分配是否正确），"
        "再怀疑参数（M / efC / ef），最后才怀疑实现。<b>不要一上来就调 ef</b> —— "
        "ef 是旋钮不是答案，调大 ef 能掩盖任何 bug。",
    ])
))

layout = card(
    "向量页布局 <span class=\"pill p\">PT_VEC_DATA</span>",
    "",
    """<pre>
偏移      内容
────────  ──────────────────────────────────────────────
0x0000    page header                <b>8 B</b>（向量页用自己的精简页头）
0x0008    vector[0]                  dim × 4 B  ┐
0x0008+D  vector[1]                  dim × 4 B  │ 连续排列，无 slot 数组
...       ...                                 │ 一页装 K = (4096−8)/(dim×4) 个
0x0FFF    (页尾)                                ┘

<b>向量 i 的物理位置 = 数据页起始 + i × dim × 4</b>   ← 纯算术，零查找
</pre>
"""
    + tbl(["项", "dim = 128 时的值", "说明"], [
        ['<span class="mono">K（每页向量数）</span>', '<b>7</b>', '(4096 − 8) / 512 = 7.98 → 7'],
        ['<span class="mono">10 万向量</span>', '<b>14,286 页</b>', '≈ 55.8 MB'],
        ['<span class="mono">100 万向量</span>', '<b>142,858 页</b>', '≈ 558 MB（内存也吃紧 → 量化是 v1.2+ 的事）'],
        ['<span class="mono">标量页</span>', '每行 = (row_id, label, …)', '普通定长槽页，<b>复用 fixed_page</b>'],
    ])
    + ok("<b>容量速算</b>：10 万 × 128 维 fp32 = <b>51.2 MB 数据 + 14,286 个数据页</b>；"
         "HNSW 邻接表另需 ~30 MB（每节点 2M = 32 个 u32 = 128 B）。"
         "<b>总内存约 81 MB</b> —— 可接受，所以本版验收规模钉死在 10 万。")
)
layout += card(
    "标量页与列对齐 <span class=\"pill\">向量与标量分行，靠序号对齐</span>",
    "",
    """<pre>
标量页（定长槽页）：每行 = (row_id, label, value...)
  row_id = <b>全局序号</b>，第 i 个向量 == 标量表第 i 行

  ┌──────────────┬──────────────┐
  │ 标量页        │ 向量页        │
  │ row 0        │ vector 0     │  ← 靠 <b>row_id 序号</b> 对齐，不是靠指针
  │ row 1        │ vector 1     │
  │ row 2        │ vector 2     │
  │ ...          │ ...          │
  └──────────────┴──────────────┘
</pre>
"""
    + ok("<b>列对齐是列式的灵魂</b>：过滤标量列命中 row_id 集合 → <b>直接换算出向量位置</b> → "
         "这是「过滤 + ANN」和将来「量化训练」的地基。"
         "如果用 RID 指针关联，每过滤一次就要多一次随机页读 —— 列式的优势全没了。")
)
layout += card(
    "HNSW 索引页 <span class=\"pill p\">PT_VEC_INDEX</span>",
    "",
    """<pre>
索引头页（1 页）：
  dim / count / M / entry_point / max_level / 数据页链头

邻接页（定长槽页，<b>复用 fixed_page</b>）：
  每槽 = 一个节点的邻居数组
  上层（level ≥ 1）：M 个 u32 row_id         ← M = 16
  第 0 层           ：2M 个 u32 row_id        ← 2M = 32
  每槽固定 32 + 16 = 48 B（对齐后）
</pre>
"""
    + tbl(["项", "值", "说明"], [
        ['<span class="mono">M</span>', '<b>16</b>', 'hnswlib 默认；上层邻居数'],
        ['<span class="mono">2M</span>', '<b>32</b>', '第 0 层邻居数（HNSW 的关键设计：底层更密）'],
        ['<span class="mono">ef_construction</span>', '<b>200</b>', 'hnswlib 默认；构建时的候选队列长度'],
        ['<span class="mono">ef_search</span>', '<b>64</b>（可调）', '查询时的候选队列长度 —— <b>召回/延迟的旋钮，不写死</b>'],
        ['<span class="mono">mL</span>', '<b>1/ln(M)</b> ≈ 0.36', '层级分配参数（Malkov &amp; Yashunin 2016 原式）'],
    ])
)
B.append(sec("一 · 列式向量页与索引页 <span>页大小 4096，第三种页格式</span>", "", layout
    + bug("<b>坑 1（页数差一个 7，容量对不上）</b>：<code>(4096 − 8) / 512 = 7.98</code> —— "
          "按 8 算就会越界写坏下一页。<b>修法</b>：<code>K = (PAGE_SIZE − hdr) / (dim * 4)</code> "
          "<b>整除取下界</b>，并且每页末尾留出的字节永远不写。"
          "<b>这类「差一个」的错误在 dim 可配时是必然会遇到的</b> —— dim 不是 128 就更明显。")
    + bug("<b>坑 2（列对齐被破坏，最贵的一类 bug）</b>：插入时向量和标量<b>没有同时追加</b> —— "
          "比如标量写成功但向量页写失败（或反之），<code>row_id</code> 和向量位置就对不上了。"
          "症状：<b>过滤命中了但返回的向量完全不对</b>，而且不会报错。"
          "<b>修法</b>：列对齐靠「序号」而不是「指针」，所以必须在<b>同一个事务里</b>同时追加，"
          "并且用一个「当前行数」计数器做对账（向量页数 × K 应该 == 标量行数，允许最后一页不满）。")
))

B.append(sec(
    "二 · HNSW 的机制推导 <span>为什么它比暴力扫快两个数量级</span>",
    "",
    card("层级分配 <span class=\"pill\">论文原式</span>", "",
         """<pre>
每插入一个元素，用一个随机数决定它属于哪一层：

  level = floor( −ln(uniform(0,1)) × mL )     其中 mL = 1 / ln(M)

M = 16 → mL = 1/ln(16) = 0.3607

  期望每层节点数：N, N·e<sup>−1/mL</sup>, N·e<sup>−2/mL</sup>, …
  M = 16 时衰减因子 e<sup>−1/0.3607</sup> = e<sup>−2.7726</sup> = 0.0625 = 1/16

  10 万节点 → 第 0 层 100,000；第 1 层 6,250；第 2 层 391；第 3 层 24
  <b>顶层只有 24 个节点</b> —— 搜索从顶层开始，每层都是「在几百个候选里找最近」
</pre>
<div class="ok"><b>关键洞察</b>：指数衰减让<b>上层小到可以全扫、下层才是真正的搜索</b>。"
         "搜索代价 ≈ 层的数量 × 每层的贪心步数，而不是 N。这就是「分层可 navigable small world」"
         "（可导航小世界）的全部含义。</div>""")
    + card("贪心搜索 <span class=\"pill\">每层从入口点贪心下降</span>", "",
           """<pre>
search(q, ep, ef):
  L = 顶层；  W = {ep}（候选堆，按距离）
  while L ≥ 0:
     W' = { }；  c = closest(W)
     <b>while c 有更近的邻居 e 且 dist(e) &lt; dist(c):</b>
         W' += e
         c = closest(W' ∪ W)
     W = W' ∪ {c}
     L ← L − 1
  <b>第 0 层</b>：W 扩展到 ef 大小后停止 → 得到 ef 个候选 → 取 top-k
</pre>
<div class="ok"><b>为什么 ef 是召回/延迟的旋钮</b>：ef = 64 意味着第 0 层保留 64 个候选。"
         "ef 越大，探索得越彻底 → recall 越高 → 但要算更多距离 → 更慢。"
         "<b>这是近似算法的本质：用一点精度换一大截速度</b>。</div>""")
    + card("启发式修剪 <span class=\"pill\">论文 Algorithm 4</span>", "",
           """<pre>
候选邻居 c1..cq（按距离排序），要保留 M 个：
  for i in 1..q:
      if |{ j &lt; i : c_i 与 c_j 方向被 c_j 覆盖 }| == 0:      <b>方向多样性判据</b>
          keep c_i
          if 已保留 == M: break
</pre>
<p style="color:var(--tx2);font-size:13.5px">判据的思想：<b>两个邻居如果方向很接近，保留一个就够</b> —— "
"留着两个几乎同方向的邻居不增加可达性，只增加搜索时的距离计算量。</p>
<div class="bug"><b>坑 3（判据写错，recall 静默下降）</b>："
"「方向被覆盖」不是简单的「距离更近」—— 是「c_j 在 c_i 的方向上且 dist(c_j) ≤ dist(c_i)」。"
"写成简化的「保留最近的 M 个」→ 邻接表会挤在数据的一侧 → "
"<b>recall 从 0.95 掉到 0.6，但代码看起来完全正常</b>。"
"这就是为什么必须用暴力扫 GT 做对照，而不是看「能不能返回结果」。</div>""")
    + ok("<b>验收数字</b>：10 万 × 128 维随机向量，M=16 / efC=200 / ef=64 → "
         "<b>recall@10 ≥ 0.95</b>（对照暴力扫 GT 逐条 diff）；"
         "与 hnswlib 同参数构建的 recall@10 差距 &lt; <b>2 个百分点</b>。")
))

B.append(sec(
    "三 · 过滤 + ANN：给超集，不给子集 <span>这一版最贵的判决</span>",
    "",
    tbl(["候选", "做法", "判决"], [
        ['<span class="mono">(a)</span>', "先 ANN 取 top-k，再过滤", '<b>否决</b> —— 若 top-k 都不满足过滤，结果<b>少于 k 且不可补</b> → <b>静默丢数据</b>'],
        ['<span class="mono">(b)</span>', "先过滤标量，再对幸存者暴力扫", '<b>否决</b>（默认路径）—— 正确但 O(N)，索引白建'],
        ['<span class="mono">(c)</span>', "<b>ANN 扩大候选池（ef ≥ 4k）再过滤</b>", '<b>采纳</b> —— ANN 给超集候选，过滤后可能少于 k → <b>如实报告实际数量</b>'],
    ])
    + card("采纳 (c) 的语义必须写死在 API 文档里", "",
           """<pre>
db_vec_knn(conn, filter, query, k, out)

  <b>返回</b>：满足过滤条件的、ANN 能找到的最相似向量
  <b>不保证恰好 k 个</b>（可能少于 k）

  这是 ANN 的<b>固有性质</b>（近似），不是 bug。
  需要精确结果请用 db_vec_knn_exact（暴力扫，供回归对照）。
</pre>
<div class="ok"><b>这延续 v0.9 的纪律</b>：<b>保守的优化是安全的，激进的优化是危险的</b>。"
         "hnswlib 的 filtered 检索也是同样性质（它的 pre-filter 模式同样不保证 k 个）。</div>""")
    + bug("<b>坑 4（超集契约被违反 → 静默丢数据）</b>：如果实现时图省事，"
          "改成「ANN 取 top-k → 过滤 → 返回」—— 这时<b>少给结果不会有任何报错</b>，"
          "调用方只会觉得「召回有点低」。"
          "<b>修法</b>：<code>db_vec_knn</code> 必须返回 <code>actual_count</code>，"
          "调用方能区分「只有 3 个符合条件」和「算法坏了」。"
          "<b>反向验证</b>：拿掉过滤 → 结果必须变成超集（条数变多），而不是变少。")
    + bug("<b>坑 5（过滤和 ANN 的顺序反了）</b>：正确的顺序是"
          "<b>先扩大候选池（ef 拉高），再过滤</b>。"
          "反过来（先过滤后 ANN）在本版不成立 —— 过滤后幸存者集合与 HNSW 的图结构无关，"
          "必须在<b>原图上</b>搜出足够大的候选集，才能保证「过滤后不漏」。"
          "<b>这正是本版不做 filtered-HNSW 的原因</b>：吸收进图需要改图的构建逻辑，是激进优化。")
))

B.append(sec(
    "四 · 事务与恢复 <span>锁 + WAL，与图模态同款</span>",
    "",
    tbl(["方面", "语义", "对照"], [
        ['<span class="mono">可见性</span>', '<code>visible()</code> 恒真', 'heap 按快照判 xmin/xmax'],
        ['<span class="mono">隔离</span>', '写者互斥（全局 latch）+ 读不加锁', 'heap 用版本链让读写互不阻塞'],
        ['<span class="mono">提交</span>', '语句级', 'heap 支持 BEGIN/COMMIT 多语句'],
        ['<span class="mono">WAL</span>', '改页即 <code>WAL_PAGEDATA</code> 整页镜像；xid = 0（系统事务）', 'heap 行级 undo-redo'],
    ])
    + ok("<b>为什么向量不需要 MVCC</b>：HNSW 的邻接表是<b>全局共享</b>的 —— "
         "一个节点的邻居表被所有查询依赖。MVCC 的版本链意味着「读到的可能是旧版本的邻接表」，"
         "而旧邻接表指向的行可能已经不在当前版本里 → 悬空引用。"
         "Neo4j 面对同样的问题，选择了不做 MVCC。<b>向量模态和图模态是同一个选择的两个实例。</b>")
    + bug("<b>坑 6（崩溃后 HNSW 索引不一致）</b>：向量页和索引页是<b>两批页</b>，"
          "崩溃可能发生在「向量写完、索引没更新」之间。"
          "如果恢复后直接用旧索引 → 索引指向不存在的向量 → 查出来是垃圾。"
          "<b>修法</b>：<code>count</code> 字段记录「已入库向量数」，索引头页的 <code>count</code> "
          "与之对账；不一致时走 <b>reindex 路径</b>（重建索引，而不是用坏的索引）。"
          "这和 v1.6 的路径索引用「reindex 恢复」是同一个套路。")
    + ok("<b>验收数字</b>：插入后断电 → 恢复 → 向量数据完整；索引要么一致、要么被标记为需重建；"
         "重建后的 recall 与崩溃前一致。")
))

B.append(sec(
    "SQL 侧与能力协商 <span>knn 是第一个真实现的口子</span>",
    "",
    tbl(["入口", "内容", "支持", "理由"], [
        ['<span class="mono">index_am.knn</span>', "<code>knn(am, query, k, out)</code>",
         '<b>真实现</b>', 'v0.9 留的口子，v1.0 是第一个非 E_NOTSUP 的实现'],
        ['<span class="mono">db_vec_knn</span>', '带过滤的 top-k 查询', '<b>主路径</b>',
         '距离排序键是计算出来的，SQL 表达不了'],
        ['<span class="mono">db_vec_knn_exact</span>', '暴力扫（ground truth）', '<b>提供</b>',
         '回归对照 + 需要精确结果时'],
        ['<span class="mono">vec_items</span>', '虚拟表（标量列）', '<b>SELECT</b>',
         'label 等值下推 → 只扫匹配的 row_id'],
        ['<span class="mono">距离排序进 SQL</span>', 'ORDER BY vec &lt;-&gt; q LIMIT 10', '<b>否决</b>',
         '排序键计算在执行器侧 → 需要新的算子类型 → 违反「下推结果是行源」的同构性'],
        ['<span class="mono">写操作</span>', 'INSERT / UPDATE / DELETE on vec_items', '<b>否决</b>',
         '本版只读，装载走专用 API → E_NOTSUP'],
    ])
    + callout("为什么不把距离排序做成 SQL 算子", [
        "下推契约（v0.9）说：<b>AM 给候选行，执行器重新求值 WHERE</b>。"
        "但 <code>ORDER BY 距离 LIMIT k</code> 不是 WHERE —— 它要求执行器拥有"
        "<b>整个结果集的排序能力</b>，而且排序键来自向量计算，不是某一列的值。",
        "把 <code>&lt;-&gt;</code> 做成 SQL 算子，等于让执行器理解「向量」这个模态概念 —— "
        "v1.6 的 A7 验收明确要求「执行器改动审计里<b>无模态名硬编码</b>」。"
        "<b>所以本版走 API 路线，判据和 v1.3（全文检索）完全一致</b>："
        "执行器不能独立重判的语义，不进标量谓词系统。",
    ])
    + ok("<b>验收</b>：<code>grep vector src/query/</code> 零命中（执行器零改动）；"
         "<code>vec_items</code> 的写操作报明确 E_NOTSUP；"
         "<b>IO 可证</b>：<code>db_vec_page_reads()</code> 前后对比，ANN 页读 &lt; 暴力扫的 1/20。")
))

B.append(sec(
    "全部做完后的验收清单 <span>能全过，这个模态就真的能用了</span>",
    "",
    checklist([
        "<b>列式寻址</b>：第 i 个向量的寻址算术正确；扫描 IO ≈ 连续页读（无跳读）",
        "<b>HNSW 正确性</b>：recall@10 ≥ 0.95（10 万 × 128 维，M=16 / efC=200 / ef=64，对暴力扫 GT）",
        "<b>对照 hnswlib</b>：同参数构建，recall@10 差距 &lt; 2 点",
        "<b>增量插入</b>：边插边查 recall 不塌（每插 1 万查一次，最终 ≥ 0.93）",
        "<b>落盘一致</b>：重开后 recall 与内存版一致（索引页重放后可用）",
        "<b>IO 可证</b>：ANN 查询页读 &lt; 暴力扫的 1/20（10 万向量 = 14,286 页）",
        "<b>过滤超集</b>：<code>db_vec_knn</code> 返回 actual_count；拿掉过滤 → 结果变超集而非变少",
        "<b>崩溃恢复</b>：插入后断电 → 向量完整；索引一致或被标记需重建；重建后 recall 一致",
        "<b>执行器零改动</b>：<code>grep vector src/query/</code> 零命中；旧 152+ 项验收全绿",
        "<b>能力协商</b>：<code>vec_items</code> 的写操作报 E_NOTSUP 系明确错误",
        "<b>回归门</b>：<code>fixed_page</code> 下沉后图模态全部测试原样通过",
    ])
))

B.append(sec(
    "做完之后接什么 <span>三个方向，按你的目标选</span>",
    "",
    """<div class="card"><div class="blk">
<ul>
<li><b>想继续做多模 → 全文检索（v1.3）</b>：倒排 + segment 合并。
HNSW 落盘用的定长槽页（fixed_page）在 Lucene 的 posting list 里还会再用到 ——
不过 posting 是变长的，所以那里用的是另一种页型。</li>
<li><b>想深入向量检索 → v1.1 向量化执行 + CBO</b>：
本版的 <code>knn</code> 已经接进 SPI，v1.1 让它真正参与代价估算
（CBO 决定「走 ANN 还是走全扫」）。<b>这是 <code>index_am.knn</code> 口子的兑现。</b></li>
<li><b>向量检索的进阶（不排期池）</b>：量化（PQ / SQ）、filtered-HNSW、
多向量混合检索、IVF。<b>这些都是在「HNSW 本身做对」之后的正交优化</b> ——
recall 都不达标时做它们没有意义。</li>
</ul>
</div></div>"""
))

B.append(foot(
    "整体设计见 <code>db/docs/DESIGN.md</code>（§5.3 模态同构性原则、§7 验收标准）；"
    "版本执行计划见 <code>db/docs/PLAN-v1.0.md</code>（方案判决表 + 10 项验收 + 风险清单）；"
    "前置重构（图模态 gstore 下沉）见 <code>PLAN-v1.0</code> 执行计划步骤 1。",
    "HNSW 算法与参数（M=16 / efC=200 / mL=1/ln(M) / 启发式修剪 Algorithm 4）出自 "
    "Malkov &amp; Yashunin, <i>Efficient and robust approximate nearest neighbor search using "
    "Hierarchical Navigable Small World graphs</i>, arXiv:1603.09320 (2016)；"
    "hnswlib 默认参数取自其源码 <code>hnswlib.h</code>；列式布局对标 Lance / Parquet 格式规范。"
))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "向量模态实现规格.html")
n = write_spec(path,
               "向量模态实现规格 · 从零到能跑",
               "Build Spec · Vector Modality",
               "向量模态：<em>2000 行</em>怎么拆",
               "列式向量页 + HNSW 图索引 + knn 接线。这是第三个独立模态的施工图，"
               "核心路径约 2000 行，测试约 900 行。对标 Lance/Parquet 的列式布局与 hnswlib 的 HNSW 实现 —— "
               "两个判据：索引真的被用上（IO 可证），recall 与 hnswlib 差 < 2 点。",
               "\n".join(B))
print("OK", path, n, "字符")
