# -*- coding: utf-8 -*-
"""生成《全文检索模态实现规格.html》"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_tpl import *

B = []

B.append(sec(
    "和关系模态的根本差异 <span>第四种存储布局：不变文件 + 后台合并</span>",
    "",
    tbl(["维度", "关系模态（heap）", "全文检索模态（ft）", "为什么不同"], [
        ['<span class="mono">写入</span>', '就地更新 + 版本链', '<b>追加进 segment，段写完不可变</b>',
         '倒排索引写起来是随机位置插入，代价高'],
        ['<span class="mono">删除</span>', '打 xmax + vacuum', '<b>tombstone 标记</b>，合并时才物理丢弃',
         '删一个词项要改所有 posting list'],
        ['<span class="mono">索引结构</span>', 'B+tree（一维全序，键→行）', '<b>term → posting list</b>（倒排）',
         '查询是「按词项取一串文档」'],
        ['<span class="mono">打分</span>', '无（只有相等/大小）', '<b>BM25 相关性</b>',
         '排序键依赖全局统计（idf / avgdl）'],
        ['<span class="mono">查询语义</span>', 'WHERE 行过滤', '<b>按相关性排序取 top-k</b>',
         '排序键是计算出来的，不是列值'],
        ['<span class="mono">空间回收</span>', '页级 freelist', '<b>整段丢弃 + 页回收</b>',
         '段是连续的一组页，整段可回收'],
    ])
    + callout("这一版学的是「不变文件 + 后台合并」这个思想", [
        "索引 segment 写完就不可变；删除只打标记；后台把小 segment 合并成大的，"
        "<b>合并时才物理丢弃被删文档</b>。",
        "同一个思想的三个实例：<b>blob 的不可变对象 → Lucene 的 segment → 时序的封窗分区</b>。"
        "这是本项目「三条贯穿线索」的第一条（不变文件 + 后台合并 × 4）。",
        "对标 Lucene 的 segment 机制 —— 这是 Lucene 最重要的设计决定，没有之一。",
    ])
))

B.append(sec(
    "模块与行数 <span>按依赖排序，不是按重要性</span>",
    "",
    tbl(["模块", "职责", "行数", "依赖"], [
        ['<span class="mono">ft_token.c</span>', "英文分词器：小写化 + 非字母数字切分", '<span class="n">~180</span>', "—"],
        ['<span class="mono">ft_score.c</span>', "BM25 评分 + TF-IDF 对照实现", '<span class="n">~220</span>', "—"],
        ['<span class="mono">ft_segment.c</span>', "段的三种页：HEAD / DICT / POST；写完封段", '<span class="n">~520</span>', "pager, wal"],
        ['<span class="mono">ft_merge.c</span>', "Tiered 合并：字典多路归并 + posting 拼接 + tombstone 过滤", '<span class="n">~380</span>', "ft_segment"],
        ['<span class="mono">ft_index.c</span>', "多段管理、tombstone 位图、段目录", '<span class="n">~280</span>', "ft_segment, ft_merge"],
        ['<span class="mono">spi/ft_am.c</span>', "SPI 适配：ft_docs 虚拟表 + db_ft_search", '<span class="n">~300</span>', "spi, ft_index"],
        ['<span class="mono">测试</span>', "test_ft_score / test_ft_segment / test_ft_merge / test_ft_it / ft_bench", '<span class="n">~850</span>', "—"],
    ])
    + ok("<b>占便宜的部分</b>：WAL 复用 <code>WAL_PAGEDATA</code>（第 4 个模态复用）；"
         "页格式复用公共页头；SPI 复用 v0.9 定稿的注册表。"
         "<b>新东西全在「段」这个概念上</b>。")
))

B.append(sec(
    "六步实现顺序 <span>每步都有可量测的验收，不通过不进下一步</span>",
    "",
    steps([
        ("分词器 + 评分器 <span class=\"pill\">纯内存</span>",
         "小写化 + 非字母数字切分；BM25（k1=1.2, b=0.75）+ TF-IDF 对照。"
         "验收：边界用例表（<code>state-of-the-art</code> → 4 词）全过；"
         "两种评分对同一语料的排序差异<b>逐例可解释</b>。",
         "~400"),
        ("单 segment 写读",
         "term 字典（有序数组 + 二分）+ posting list（<code>(doc_id, tf)</code> delta 编码）。"
         "验收：<code>db_ft_search</code> 的 top-k 与线性扫描 BM25 全等（<b>含分数</b>）。",
         "~520"),
        ("多 segment + tombstone + Tiered 合并",
         "段数 &gt; 8 时合并；tombstone 查询时过滤、合并时物理丢弃。"
         "验收：合并前后检索结果<b>全等</b>（合并不透明）；合并后<b>段页数下降</b>。",
         "~660"),
        ("SPI 接线",
         "<code>ft_docs</code> 虚拟表（标量列）+ <code>db_ft_search</code> 专用 API。"
         "验收：<code>grep ft src/query/</code> 零命中（执行器零改动）。",
         "~300"),
        ("崩溃恢复 + 基准",
         "段页 + tombstone 重放。验收：合并总 IO ≈ <code>c·N·log_f N</code>（量级论证）；"
         "单 term 检索页读 = 字典二分页数 + posting 页数（<b>与总文档量无关</b>）。",
         "~350"),
        ("反向验证 + 文档",
         "验收：<b>拿掉 tombstone 过滤 → 已删文档重新出现</b>（证明机制真在起作用）；"
         "拿掉合并 → 段数无限增长；旧 152+ 项全绿。",
         "~300"),
    ])
    + callout("关键提醒", [
        "<b>合并是最容易写错的一步</b>：term 对齐 + posting 的 delta 重算 + tombstone 过滤，"
        "三件事必须<b>同时</b>做对。<b>修法沿用 v0.3 B+tree 的办法</b>：写一个有序数组的参考实现，"
        "合并结果与参考实现逐条 diff。<b>不要靠「看起来对」验收</b>。",
        "<b>合并的代价是可解释的，不是「大就慢」</b>：10 万文档分批插入，"
        "合并总 IO 应呈 <code>O(N × log_f N)</code> 量级（f = 合并因子）。"
        "实测出来如果不是这个量级，说明合并策略写错了（比如每次全量合并而不是增量）。",
    ])
))

# 字节级布局
layout = card(
    "段的物理形态 <span class=\"pill p\">一个段 = 三种页的区间，写完不可变</span>",
    "",
    """<pre>
一个 segment（写完封段，之后只读）：

  ┌────────────────────────────────────────────────────────┐
  │ <b>PT_FT_HEAD</b>（段头页，1 页）                         │
  │   seg_id / doc_count / 创建 LSN / avgdl / 段内 max doc_id │
  │   dict_page_start / dict_page_count                      │
  │   post_page_start / post_page_count                      │
  ├────────────────────────────────────────────────────────┤
  │ <b>PT_FT_DICT</b>（字典页，若干）                         │
  │   term 有序数组：[term_len, term_bytes, df, post_ptr]     │
  │   跨页二分查找（页内有序 + 页间有序）                      │
  ├────────────────────────────────────────────────────────┤
  │ <b>PT_FT_POST</b>（posting 页，若干）                      │
  │   (doc_id_delta, tf) 有序对数组，doc_id 用 delta 编码     │
  │   一个 term 的 posting 可能跨页链（页头带 next）           │
  └────────────────────────────────────────────────────────┘
</pre>
"""
    + tbl(["页型", "作用", "关键字段"], [
        ['<span class="mono">PT_FT_HEAD</span>', '段的元信息 + 页区间指针', 'seg_id / doc_count / avgdl / 三个页区间'],
        ['<span class="mono">PT_FT_DICT</span>', 'term → posting 的定位表', 'term_len / term_bytes / df / post_ptr'],
        ['<span class="mono">PT_FT_POST</span>', '真正的倒排数据', 'doc_id_delta / tf'],
    ])
)
layout += card(
    "为什么 term 用有序数组而不是哈希表 <span class=\"pill\">一个决定，两个好处</span>",
    "",
    """<p style="color:var(--tx2);font-size:13.5px">哈希表在两个方向上都不占：</p>
<pre>
① <b>「按字典序批量扫」是检索的天然需求</b>（前缀查询的将来）
   有序数组：扫一遍即可。哈希表：无序，只能全扫再过滤。

② <b>合并 = 多路归并</b>（这是最关键的理由）
   多个段的字典都是有序的 → 归并是「取最小的头」→ 天然线性
   哈希表：无序 → 合并要变成「收集所有 term 再排序」→ O(T log T)
</pre>
<div class="ok"><b>结论</b>：合并是这一版的核心机制，而有序数组让合并变便宜。"
         "选哈希表等于给自己加一个合并的复杂度。</div>
<div class="bug"><b>坑 1（字典项变长 → 页内切分不是简单切数组）</b>：
term 长度不固定（<code>state-of-the-art</code> 16 字节，<code>a</code> 1 字节），
所以字典数组<b>不能用定长槽页</b> —— 必须变长。
<b>修法</b>：字典页用「变长记录 + 变长偏移数组」（类似 heap 的 slotted page，但只读不写）。
或者更简单：<b>把 term 统一哈希成固定长度的 8 字节指纹</b>（代价是极小概率的假阳性）。
<b>后者更省事，但要如实记录这个取舍</b>。</div>""")
layout += card(
    "posting list 的 delta 编码 <span class=\"pill\">posting 是变长的</span>",
    "",
    """<pre>
一个 term 的 posting（按 doc_id 升序）：

  原始：[ (doc_id=1, tf=1), (doc_id=5, tf=3), (doc_id=12, tf=1), ... ]

  delta 编码（只存与前一个的差）：
  [(1,1), (4,3), (7,1), ...]        ← doc_id_delta 首项为绝对值

  <b>为什么</b>：doc_id 在语料里可能稀疏（1, 5, 12, 300），
  绝对值要 4~8 字节，delta 只要 1~2 字节 → posting list 体积减半以上。
</pre>
<div class="ok"><b>验收数字</b>：1 万文档语料，posting 页数比「不编码」减少 <b>&gt; 40%</b>；
合并后 posting 的 delta 重新计算（不能沿用旧段的 delta）—— 这一条最容易漏。</div>""")
B.append(sec("一 · 段的字节布局 <span>三种页型，写完不可变</span>", "", layout
    + bug("<b>坑 2（合并时 delta 没重算 → posting 全错）</b>：合并把多个段的 posting 拼在一起，"
          "但每段的 delta 是<b>相对于该段自己的前一项</b>。直接拼接 → 边界处 doc_id 跳变 → "
          "<b>检索结果静默错误</b>。<br>"
          "<b>修法</b>：合并时<b>先把绝对 doc_id 还原再重新 delta 编码</b>，"
          "并且合并完必须做「与参考实现逐条 diff」。")
))

# BM25
B.append(sec(
    "二 · BM25 评分 <span>抄业界实现，不抄教科书</span>",
    "",
    """<pre>
<b>Lucene 式 BM25</b>（k1 = 1.2, b = 0.75）：

  idf(t)   = ln( 1 + (N − df + 0.5) / (df + 0.5) )

  score    = Σ<sub>t</sub>  idf(t) · tf·(k1+1) / ( tf + k1·(1 − b + b·dl/avgdl) )

  N   = 语料总文档数（<b>全局量</b>，跨段聚合）
  df  = 含该词的文档数
  dl  = 该文档长度（词项数）
  avgdl= 平均文档长度（<b>全局量</b>）
</pre>
"""
    + card("为什么用 Lucene 式 idf 而不是教科书式", "",
           """<pre>
教科书式： idf(t) = ln( (N − df + 0.5) / (df + 0.5) )

  df = 0    → ln(负数)  → <b>无意义</b>
  df = N    → ln(0.5/…) → <b>负值</b> → 高频词的贡献变负
  单文档语料 → 除零

Lucene 式： idf(t) = ln( 1 + (N − df + 0.5) / (df + 0.5) )

  <b>恒正、平滑、处处有定义</b>
</pre>
<div class="ok"><b>本项目的既定方针</b>：<b>抄业界实现，不抄教科书</b>。"
         "教科书公式是教学简化，业界实现是踩过坑的产物。Lucene 二十年的实践比教科书可信。</div>""")
    + card("跨段的全局统计怎么来 <span class=\"pill\">这也是合并的动机之一</span>", "",
           """<p style="color:var(--tx2);font-size:13.5px">
<code>N</code> 和 <code>avgdl</code> 是<b>跨段的全局量</b> —— 每个段只知道自己那部分的
<code>doc_count</code> 和 <code>avgdl</code>。检索时实时聚合各段头：</p>
<pre>
N      = Σ 各段 doc_count
avgdl  = Σ (各段 doc_count × 各段 avgdl) / N
</pre>
<div class="ok"><b>段数 ≤ 8（合并阈值）时，聚合代价可忽略</b> —— 最多加 8 个数。<br>
<b>这正是合并的动机之一</b>：段越少，聚合越便宜，而且单 term 检索要遍历的段也少。"
         "合并同时优化了 IO 和打分成本 —— <b>机制之间互相强化，这是好设计的标志</b>。</div>
<div class="bug"><b>坑 3（聚合写成「最后一段的 avgdl」）</b>：段是分批插入的，"
          "后插入的段可能都是长文档（或者都是短文档）。"
          "用任意一段的 avgdl 算归一化 → <b>长度归一完全失效</b> → "
          "BM25 退化成 TF-IDF。<b>修法</b>：按 doc_count 加权聚合，"
          "并写一个断言：<code>avgdl</code> 必须落在 [最小段 avgdl, 最大段 avgdl] 区间内。</div>""")
    + card("TF-IDF 作为对照实现（不是多余的工作）", "",
           """<pre>
TF-IDF 对照： score = tf · idf(t)     ← <b>没有长度归一项</b>

同一个语料，BM25 和 TF-IDF 的排序差异<b>只来自一个地方</b>：dl/avgdl 归一。
</pre>
<div class="ok"><b>验收 A2 要求「差异可解释」</b>：挑几个排序不同的文档，"
         "逐例说明「这个文档更长，所以 TF-IDF 把它排前面，BM25 把它压下去了」。"
         "<b>如果解释不出来，说明实现有问题</b> —— 这是一个比「结果全等」更严格的正确性检查。</div>""")
))

# 合并
B.append(sec(
    "三 · Tiered 合并 <span>不变文件 + 后台合并的核心机制</span>",
    "",
    """<pre>
<b>触发</b>：段数 &gt; 8 时，挑「文档数之和最小」的相邻若干段合并成一个。

<b>合并五步</b>：
  ① 多个段的字典 → <b>多路归并</b>（每个段的字典是有序的 → 取最小头）
  ② 对齐后的 term：合并各段的 posting
  ③ posting 拼接时 <b>doc_id 还原成绝对值再重新 delta 编码</b>（坑 2）
  ④ <b>tombstone 过滤</b>：被删文档在这一步被<b>物理丢弃</b>
  ⑤ 重建段头（doc_count / avgdl / 页区间）→ 旧段页进 freelist
</pre>
"""
    + card("tombstone：删除的两段式 <span class=\"pill\">标记 → 物理丢弃</span>", "",
           """<pre>
<b>删除文档 d（在段 S 中）</b>：
  ① 内存里把 (S, d) 记进 tombstone 位图
  ② WAL 追加一条 <code>WAL_FT_TOMB</code>（内容仅 seg_id + doc_id）
  ③ 查询时：命中的 posting list 逐项检查 tombstone → 跳过

<b>合并时</b>：④ 被 tombstone 标记的 doc_id <b>直接从 posting 里删掉</b>（物理丢弃）

  查询时过滤 → <b>即时生效</b>（删除后立刻搜不到）
  合并时丢弃 → <b>空间回收</b>（段页数下降）
</pre>
<div class="ok"><b>两段式的意义</b>：删除必须是<b>即时</b>的（用户搜不到就是搜不到），"
         "但物理丢弃必须是<b>批量</b>的（一个文档可能出现在成千上万个 posting list 里，"
         "逐个去改代价太高）。<b>tombstone 就是把「即时」和「批量」解耦的机制。</b></div>""")
    + bug("<b>坑 4（tombstone 丢了 → 已删文档复活）</b>：tombstone 只在内存里 → 重启后丢失 → "
          "<b>已删除的文档重新出现在结果里</b>。<br>"
          "<b>修法</b>：<b>WAL 逐条追加</b>（<code>WAL_FT_TOMB</code>），重启时重放重建。"
          "<b>验收 A7 覆盖这个场景</b>：删除 → 崩溃 → 恢复 → 文档仍搜不到。")
    + bug("<b>坑 5（合并透明性被破坏）</b>：合并是后台动作，用户随时可能在合并过程中发起查询。"
          "如果合并期间查到的是「旧段 + 新段的一部分」，结果会<b>既不是合并前也不是合并后</b>。<br>"
          "<b>修法</b>：<b>合并的原子切换点 = 新段完全写好并落盘的那一刻</b>；"
          "切换前查旧段，切换后查新段，<b>不做中间态</b>。"
          "段目录的替换是一次「指针改写」，而指针改写在 latch 内完成（硬规则 8）。")
    + ok("<b>验收数字</b>：随机时刻触发合并，<b>合并前后检索结果全等</b>（含分数，容差 1e-6）；"
         "合并后<b>段页数下降</b>（被删文档的 posting 真的被丢弃了）；"
         "10 万文档分批插入，合并总 IO ≈ <code>c·N·log_f N</code>（c 实测，量级论证）。")
))

# 事务与恢复
B.append(sec(
    "四 · 事务与恢复 <span>段不可变 = 日志逻辑简单</span>",
    "",
    tbl(["方面", "语义", "对照"], [
        ['<span class="mono">可见性</span>', '<code>visible()</code> 恒真', 'heap 按快照判 xmin/xmax'],
        ['<span class="mono">隔离</span>', '写者互斥（全局 latch）+ 读不加锁', 'heap 用版本链让读写互不阻塞'],
        ['<span class="mono">提交</span>', '语句级', 'heap 支持 BEGIN/COMMIT 多语句'],
        ['<span class="mono">WAL</span>', '段页 <code>WAL_PAGEDATA</code> 整页镜像 + <code>WAL_FT_TOMB</code> 追加', 'heap 行级 undo-redo'],
        ['<span class="mono">删除</span>', 'tombstone（段级），合并时物理丢弃', 'heap 打 xmax + vacuum'],
    ])
    + ok("<b>段不可变带来的简化</b>：段页<b>只写一次</b> → 日志不需要区分「首次修改记全页 / "
         "后续只记增量」→ 直接用整页镜像。这和 blob 是同一个理由（第 2、4 次出现）。")
    + ok("<b>验收数字</b>：段页 + tombstone 重放后检索一致；"
         "合并中途崩溃 → 恢复后要么是合并前的段集合、要么是合并后的（<b>不能是两半</b>）；"
         "删除后崩溃 → 恢复后文档仍搜不到。")
))

# SQL 侧
B.append(sec(
    "五 · SQL 侧与能力协商 <span>match 不进 SQL 谓词（本版最重要的边界）</span>",
    "",
    tbl(["入口", "内容", "支持", "理由"], [
        ['<span class="mono">db_ft_search</span>', '专用 API：返回 (doc_id, score) 按 BM25 排序', '<b>主路径</b>',
         '相关性评分依赖全局统计'],
        ['<span class="mono">ft_docs</span>', '(doc_id, doc_len, nterms, sealed) 标量列', '<b>SELECT</b>',
         'doc_id 等值/范围下推；元数据查询'],
        ['<span class="mono">match(col, term)</span>', '进 SQL 谓词', '<b>否决</b>',
         '需要 PD_MATCH + 相关性排序键 + 执行器拥有全局统计'],
        ['<span class="mono">写操作</span>', 'INSERT / UPDATE / DELETE on ft_docs', '<b>否决</b>',
         '本版只读；文档写入走专用 API → E_NOTSUP'],
    ])
    + callout("为什么 match 不能进 SQL（写清楚，防止将来心血来潮去改）", [
        "<b>理由 1：契约根本不成立。</b>v0.9 的 <code>pushdown.h</code> 里 "
        "<code>Predicate</code> 是 <code>(col_idx, op, DbValue)</code> —— 单值标量比较。"
        "相关性评分依赖<b>全局统计</b>（idf 随语料变、avgdl 随合并变），"
        "<b>执行器根本无法独立重求值</b> —— 「下推给超集、执行器重判」的契约直接崩掉。",
        "<b>理由 2：代价不对等。</b>为单一模态改三个已验收模态共用的公共契约，"
        "违反 v0.9 的教训 —— <b>接口是照着调用方的需要写的</b>，而执行器不需要。",
        "<b>理由 3：先例已经三个。</b>graph 遍历、vector knn、geo 窗口 —— "
        "<b>模态特有语义不进标量谓词系统</b>。这条边界在 v1.4 结束后写进 DESIGN 复盘。",
        "<b>对照 v1.6</b>：JSON 的 <code>json_match</code> <b>可以</b>进 SQL —— "
        "因为路径提取出的值能映射成<b>通用标量</b>，执行器能独立重判。"
        "<b>一进一不进，判据是同一条：执行器能不能独立重判。</b>",
    ])
    + ok("<b>验收</b>：<code>grep ft src/query/</code> 零命中（执行器零改动）；"
         "<b>IO 可证</b>：单 term 检索的页读数 = 字典二分页数 + 该 term 的 posting 页数，"
         "<b>与总文档量无关</b>（这是倒排索引的核心收益）。")
))

# 验收清单
B.append(sec(
    "全部做完后的验收清单 <span>能全过，这个模态就真的能用了</span>",
    "",
    checklist([
        "<b>检索正确性</b>：千文档语料，<code>db_ft_search</code> 的 top-k 与线性扫描 BM25 全等（<b>含分数</b>）",
        "<b>评分可解释</b>：BM25 vs TF-IDF 的排序差异逐例解释（只来自长度归一项）",
        "<b>删除语义</b>：删除后立即不出现在结果（tombstone 生效）；合并后仍不出现且<b>段页数下降</b>",
        "<b>合并透明</b>：随机时刻触发合并，前后检索结果全等",
        "<b>合并代价</b>：10 万文档分批插入，合并总 IO ≈ <code>c·N·log_f N</code>（量级论证）",
        "<b>IO 可证</b>：单 term 检索页读与总文档量无关",
        "<b>崩溃恢复</b>：段页 + tombstone 重放后检索一致；删除后崩溃 → 文档仍搜不到",
        "<b>反向验证</b>：拿掉 tombstone 过滤 → 已删文档重新出现；拿掉合并 → 段数无限增长",
        "<b>执行器零改动</b>：<code>grep ft src/query/</code> 零命中；旧 152+ 项验收全绿",
        "<b>能力协商</b>：<code>ft_docs</code> 的写操作报 E_NOTSUP 系明确错误",
    ])
))

# 后续
B.append(sec(
    "做完之后接什么 <span>三个方向，按你的目标选</span>",
    "",
    """<div class="card"><div class="blk">
<ul>
<li><b>想继续做多模 → 地理空间（v1.4）</b>：R-tree + 窗口查询。
这里的<b>多路归并的手感</b>（字典有序 → 归并）在 R-tree 的分裂拣选里不会直接复用，
但「有序结构上的批量操作」这个思路会。</li>
<li><b>想把全文做深 → 不排期池</b>：位置索引 / 短语查询 / 高亮、
FST 前缀压缩的 term 字典、中文分词、近实时搜索（NRT）。
<b>这些都是在「倒排 + 合并」这个骨架做对之后的正交扩展</b>。</li>
<li><b>想回看贯穿线索 → 时序（v1.5）</b>：封窗压缩也是「不变文件」思想的应用，
但它多了一个<b>封窗原子性</b>问题（压缩是重写，新旧页并存怎么办）——
"这个问题的解法（<code>sealed_lsn</code> 收敛规则）和本版的「合并原子切换点」可以对照着看。</li>
</ul>
</div></div>"""
))

B.append(foot(
    "整体设计见 <code>db/docs/DESIGN.md</code>（§5.3 模态同构性原则、§7 验收标准）；"
    "版本执行计划见 <code>db/docs/PLAN-v1.3.md</code>（方案判决表 + 8 项验收 + 风险清单）；"
    "本规格是字节级施工图，三者冲突时以代码和 PLAN 的执行记录为准。",
    "Lucene 式 BM25 公式与 idf 变体出自 Apache Lucene 的默认相似度实现"
    "（<code>org.apache.lucene.search.similarities.BM25Similarity</code>，"
    "Robertson &amp; Zaragoza 2009 的经典 BM25 变体）；"
    "segment 的不可变 + 合并机制出自 Lucene 的 <i>Indexes</i> 文档；"
    "tombstone 与「查询时过滤、合并时丢弃」的两段式删除出自 Lucene 的 delete queue 设计。"
))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "全文检索模态实现规格.html")
n = write_spec(path,
               "全文检索模态实现规格 · 从零到能跑",
               "Build Spec · Full-Text Modality",
               "全文检索模态：<em>2500 行</em>怎么拆",
               "倒排索引 + 不可变 segment + Tiered 合并。这是第四个独立模态的施工图，"
               "核心路径约 2500 行，测试约 850 行。对标 Lucene 的 segment 机制 —— "
               "两个判据：合并是透明的（前后结果全等），合并的代价是可解释的（O(N·log_f N) 量级）。",
               "\n".join(B))
print("OK", path, n, "字符")
