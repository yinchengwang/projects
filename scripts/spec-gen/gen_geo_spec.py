# -*- coding: utf-8 -*-
"""生成《地理空间模态实现规格.html》"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_tpl import *

B = []

B.append(sec(
    "和关系模态的根本差异 <span>第五种存储布局：多维数据怎么进树</span>",
    "",
    tbl(["维度", "关系模态（B+tree）", "地理空间模态（R-tree）", "为什么不同"], [
        ['<span class="mono">键</span>', '一维全序的值（int / text）', '<b>MBR 包围盒</b>（minx,miny,maxx,maxy）',
         '空间数据没有全序，只有相交/包含'],
        ['<span class="mono">比较</span>', '标量比较（&lt; &gt; =）', '<b>相交 / 包含 / mindist</b>',
         '「相交」不是全序关系，不能用 B+tree 的键比较'],
        ['<span class="mono">内部节点</span>', '分隔键 + 子指针', '<b>子节点的 MBR 概括</b>',
         '剪枝的依据是 MBR，不是分隔键'],
        ['<span class="mono">分裂</span>', '按中位点一分为二', '<b>二次分裂</b>（选种子 + 分配余量）',
         '不能保证平衡，只能保证「不重叠」'],
        ['<span class="mono">查询</span>', '范围扫描（键区间）', '<b>窗口查询（剪枝）+ KNN（mindist 队列）</b>',
         '两种查询形态'],
        ['<span class="mono">KNN 性质</span>', '不适用', '<b>精确</b>（与 HNSW 的近似形成对照）',
         'mindist 是下界，可以安全剪枝'],
    ])
    + callout("这一版的主题：多维数据怎么进树", [
        "B+tree 的键是<b>一维全序</b>；空间数据没有全序。R-tree 用<b>最小包围矩形（MBR）嵌套</b>"
        "把「二维邻近」变成「树内聚集」—— <b>窗口查询 = 子树剪枝；KNN = mindist 优先队列</b>。",
        "三个判据：① 正确性（窗口/KNN 与暴力全等，含全部边角）；"
        "② <b>剪枝真的发生</b>（1% 选择率的窗口查询页读 &lt; 全扫的 1/10）；"
        "③ <b>两条路线的对照有结论</b>（真 R-tree vs Z-order+B+tree，各自什么分布下谁赢）。",
    ])
))

B.append(sec(
    "模块与行数 <span>按依赖排序，不是按重要性</span>",
    "",
    tbl(["模块", "职责", "行数", "依赖"], [
        ['<span class="mono">geo_math.c</span>', "MBR 相交 / 包含 / mindist 三个纯函数 + 暴力基准", '<span class="n">~260</span>', "—"],
        ['<span class="mono">geo_rtree.c</span>', "插入 / ChooseSubtree / 二次分裂 / 窗口查询 / mindist KNN", '<span class="n">~620</span>', "geo_math, pager"],
        ['<span class="mono">geo_zorder.c</span>', "<b>对照实现</b>：Morton 码 → B+tree，窗口 = 区间分解", '<span class="n">~340</span>', "btree"],
        ['<span class="mono">spi/geo_am.c</span>', "SPI 适配：geo_points 虚拟表 + 窗口/KNN 接线", '<span class="n">~280</span>', "spi, geo_rtree"],
        ['<span class="mono">db_geo.c</span>', "公开 API：db_geo_window / db_geo_knn（全部持 latch）", '<span class="n">~170</span>', "geo_am, latch"],
        ['<span class="mono">测试</span>', "test_geo_math / test_rtree / test_zorder / test_geo_it / geo_bench", '<span class="n">~780</span>', "—"],
    ])
    + ok("<b>占便宜的部分</b>：WAL 复用 <code>WAL_PAGEDATA</code>；页格式复用；"
         "SPI 复用 v0.9 定稿；<code>IndexAM</code> 接口零改动"
         "（<code>knn(am, query, k, out)</code> 的 query 是 <code>void*</code>，传 Mbr/点即可）。")
))

B.append(sec(
    "六步实现顺序 <span>每步都有可量测的验收，不通过不进下一步</span>",
    "",
    steps([
        ("MBR 数学 + 暴力基准 <span class=\"pill\">纯函数</span>",
         "相交 / 包含 / mindist 三个函数 + 暴力扫描的参考实现。"
         "验收：纯函数对照手算全对（含退化：点矩形、共边、负坐标）。",
         "~260"),
        ("R-tree 插入 + 分裂 + 窗口查询",
         "ChooseSubtree（选面积最小的重叠区）+ 二次分裂（平方和最小的种子对）。"
         "验收：<b>小树全量验证</b>（≤ 百点逐树断言 MBR 正确概括子树）+ 窗口结果 == 暴力。",
         "~620"),
        ("mindist KNN",
         "优先队列按 mindist 展开，结果集满 k 且堆顶 mindist ≥ 第 k 近的真实距离时提前终止。"
         "验收：KNN 结果 == 暴力排序（并列按 tie-break 规则）。",
         "~240"),
        ("Z-order 对照实现",
         "Morton 码 → 现有 B+tree；窗口查询 = 分解成多个 Z 区间（BigMin）。"
         "验收：分解出的区间并集 <b>⊇</b> 窗口内所有点（超集断言）。",
         "~340"),
        ("SPI 接线",
         "<code>geo_am</code> + <code>geo_points</code> 虚拟表。"
         "验收：<code>grep geo src/query/</code> 零命中（执行器零改动）；写操作报 E_NOTSUP。",
         "~280"),
        ("崩溃恢复 + 基准 + 反向验证 + 文档",
         "验收：1% 选择率窗口查询页读 &lt; 全扫 1/10；三组分布（均匀/聚簇/高斯基组）"
         "两树 IO 对比 + 每组一句解释。",
         "~450"),
    ])
    + callout("关键提醒", [
        "<b>R-tree 的分裂拣选是最容易写错的一步</b>：种子对选错、分配余量错 → "
        "树退化成链 → <b>查询全对但慢十倍</b>（剪枝完全失效）。"
        "<b>修法</b>：小树全量验证 —— 每插一个点就断言「每个内部节点的 MBR 正确概括其子树」"
        "和「相邻兄弟的 MBR 重叠面积 &lt; 阈值」。",
        "<b>float 距离并列会随机击穿全等验收</b>：网格点、对称分布下，"
        "第 k 名可能不唯一。<b>tie-break 定死为 row_id 升序</b>，"
        "而且暴力对照实现用<b>同一套规则</b> —— 否则「全等」验收会被浮点噪声随机击穿。",
    ])
))

# 判决
B.append(sec(
    "一 · 四个关键判决 <span>先把边界划死，再写代码</span>",
    "",
    card("判决 1：真 R-tree，不做 Z-order 一维编码塞 B+tree <span class=\"pill p\">主路径 vs 对照</span>", "",
         """<table>
<thead><tr><th>候选</th><th>判决</th></tr></thead>
<tbody>
<tr><td>(a) 点 → Morton 码 → 塞进现有 B+tree</td>
    <td><b>否决（作为主路径）</b> —— 复用 B+tree 学习增量是零；窗口查询要拆成多个 Z 区间（BigMin），逻辑绕</td></tr>
<tr><td>(b) <b>真 R-tree</b>（节点存 MBR 数组）</td>
    <td><b>采纳（主路径）</b> —— 要学的就是多维树自己的分裂与剪枝</td></tr>
<tr><td>(a′) Z-order 作为<b>对照实现</b></td>
    <td><b>采纳（bench 路径）</b> —— 与 R-tree 比 IO，结论可解释</td></tr>
</tbody></table>
<div class="callout">
<h3>学习增量论（写进 DESIGN 复盘）</h3>
<p>复用已有结构的工作量小，但本项目的目标是<b>「把机制亲手实现一遍」</b> ——
Z-order 塞 B+tree 学的是 Morton 编码，真 R-tree 学的是多维树。</p>
<p><b>对照实验把两者的真实代价差摆出来，这比单独做任何一个都完整。</b></p>
</div>""")
    + card("判决 2：R-tree 的键不走 <code>index_key</code> <span class=\"pill\">接口够用，语义要换</span>", "",
           """<p style="color:var(--tx2);font-size:13.5px">
<code>index_key</code>（v0.9 从 query/ 下沉）解决的是「列值 → 一维可比字节串」。
R-tree 的「比较」是<b>相交 / 包含 / mindist</b>，不是全序比较：</p>
<pre>
/* R-tree 的键就是 MBR 结构体，直接存进树页条目： */
typedef struct { float minx, miny, maxx, maxy; } Mbr;

/* 树页条目（PT_RTREE 页）：
   内部节点：[child_page, Mbr] 数组   （Mbr 概括整棵子树）
   叶子节点：[row_id,    Mbr] 数组   （点 → 退化 MBR：minx=maxx, miny=maxy）*/
</pre>
<div class="ok"><b>IndexAM 接口够用</b>：<code>knn(am, query, k, out)</code> 的 query 是
<code>void*</code>（传 Mbr/点）；范围扫描的语义换成「窗口」。"
"<b>IndexAM 是模态级契约</b>，R-tree 是它的第三个实现（btree → hnsw → rtree），<b>接口零改动</b>。</div>""")
    + card("判决 3：KNN 是精确的 <span class=\"pill\">与 v1.0 形成对照</span>", "",
           """<pre>
优先队列（按 mindist(节点MBR, 查询点) 最小堆）：
  弹出 → 若是叶子条目：入结果集（真实距离）
        → 若是内部节点：把子条目按 mindist 压栈
  结果集满 k 个且堆顶 mindist ≥ 第 k 近的真实距离 → <b>提前终止</b>
</pre>
<div class="callout">
<h3>两版对照是设计教育点</h3>
<p><b>近似</b>（HNSW）的代价是召回，收益是跳过大部分树；
<b>精确</b>（R-tree mindist）的代价是必须展开所有「可能更近」的子树，收益是结果无懈可击。</p>
<p>验收里两边的对照方法不同（<b>recall vs 全等</b>），这个差异本身要写进文档。</p>
</div>""")
    + card("判决 4：并列距离的稳定顺序 <span class=\"pill\">浮点验收的生死线</span>", "",
           """<p style="color:var(--tx2);font-size:13.5px">
浮点距离并列时（网格点、对称分布），KNN 的第 k 名可能不唯一。
<b>tie-break 定死为 row_id 升序</b>，暴力对照实现用同一规则。</p>
<div class="bug"><b>坑 1（浮点噪声随机击穿全等验收）</b>：
若两边用不同的计算路径（一边 SIMD 优化过、一边没优化），"
"<b>同一个距离会算出两个相差 1e-7 的值</b> → 全等断言随机失败 → "
"开发者会误以为是 R-tree 的 bug。<br>"
"<b>修法</b>：距离计算用<b>同一个函数</b>（同一份代码，两处调用），"
"比较用 <code>==</code>（同源计算，无舍入路径差），不用 <code>fabs(a-b) &lt; eps</code>。</div>""")
))

# 字节布局
layout = card(
    "MBR 的数学 <span class=\"pill p\">三个纯函数，全版的基石</span>",
    "",
    """<pre>
<b>相交</b>（两个 MBR 有公共区域）：
  a.minx ≤ b.maxx  AND  a.maxx ≥ b.minx  AND
  a.miny ≤ b.maxy  AND  a.maxy ≥ b.miny

<b>包含</b>（a 完全在 b 内）：
  b.minx ≤ a.minx  AND  a.maxx ≤ b.maxx  AND
  b.miny ≤ a.miny  AND  a.maxy ≤ b.maxy

<b>mindist</b>（查询点到 MBR 的最短距离平方）：
  dx = max( 0, a.minx − qx, qx − a.maxx )
  dy = max( 0, a.miny − qy, qy − a.maxy )
  <b>mindist² = dx² + dy²</b>
</pre>
"""
    + tbl(["性质", "为什么重要"], [
        ['<span class="mono">mindist 是下界</span>',
         '<b>mindist(MBR, q) ≤ 真实距离</b> —— 这是 KNN 能安全剪枝的<b>唯一</b>依据。'
         'MBR 里的任何点到 q 的距离都不小于 mindist'],
        ['<span class="mono">点退化</span>',
         '点的 MBR 是 minx=maxx=x, miny=maxy=y → mindist² = 到点的距离平方（自然成立，无需特判）'],
        ['<span class="mono">共边</span>',
         'a.maxx == b.minx → <b>算相交</b>（闭区间）。窗口边界上的点必须被包含 —— 这是 A2 的必测边界'],
    ])
    + ok("<b>为什么用距离平方</b>：省掉 <code>sqrt</code>，而且<b>平方是单调的</b> —— "
         "比较平方等价于比较距离，不影响排序结果。")
)
layout += card(
    "R-tree 页布局 <span class=\"pill p\">PT_RTREE</span>",
    "",
    """<pre>
<b>叶子页</b>：
  条目 i: [ row_id: u32 ][ Mbr: 16 B ]     → 20 B / 条
  每页 (4096 − 20) / 20 = <b>203 条</b>

<b>内部页</b>：
  条目 i: [ child_page: u32 ][ Mbr: 16 B ]  → 20 B / 条
  每页 <b>203 条</b>

  <b>内部节点与叶子页布局相同</b>，只靠页类型区分
  （page_type = PT_RTREE_INNER / PT_RTREE_LEAF）
</pre>
"""
    + tbl(["项", "值", "说明"], [
        ['<span class="mono">每页条目数</span>', '<b>203</b>', '(4096 − 20) / 20'],
        ['<span class="mono">10 万点</span>', '<b>493 页</b>', '叶子页 ≈ 55.8 MB 空间（MBR 冗余的代价）'],
        ['<span class="mono">MBR 冗余</span>', '<b>16 B / 点</b>', '每个点都存一份 MBR —— 这是 R-tree 的空间代价'],
    ])
    + bug("<b>坑 2（节点满了才分裂 → 树高失控）</b>：Guttman 原始 R-tree 的分裂时机是"
          "「节点要装不下了才分」，但<b>根节点必须能分</b>（否则树无法长高）。"
          "<b>修法</b>：根节点特殊处理 —— 满了就分（长出第二层），其余节点满了才分。"
          "漏了这一条 → 插入超过 M 个点后<b>所有点挤在一个节点里</b> → 退化成链表。"
          "<b>验收</b>：小树全量验证里断言「树高 ≤ 理论最小高度 + 1」。")
)
B.append(sec("二 · MBR 数学与 R-tree 页布局 <span>页大小 4096</span>", "", layout))

# IO 模型
B.append(sec(
    "三 · 剪枝的 IO 模型 <span>这一版的性能判据怎么算</span>",
    "",
    """<pre>
<b>窗口查询</b>（选择率 s = 1% = 0.01，10 万点）：

  全扫：      读全部叶子页 = <b>493 页</b>
  R-tree 剪枝：命中 100 个点，但每个命中的点分散在不同的叶子页
                （点按插入顺序落页 → 窗口内的点散落全树）
                → 实际读的叶子页 ≈ <b>100 页左右</b>（几乎每页命中 1 个）
                + 内部节点页 ≈ 10 页
                → 约 <b>110 页</b>

  收益 ≈ 493 / 110 ≈ <b>4.5×</b>，达不到「1/10」
</pre>
"""
    + card("为什么 R-tree 在「插入顺序随机」时剪枝收益有限 <span class=\"pill\">这是必须如实记录的</span>",
           """<p style="color:var(--tx2);font-size:13.5px">
这是 R-tree 的<b>真实特性</b>，不是实现问题：<b>R-tree 的空间局部性取决于点的插入顺序</b>。</p>
<pre>
<b>如果点按空间顺序插入</b>（例如按 x 再按 y 排序后插入）：
  窗口内的点连续落在少数几个叶子页 → 读 3~5 页就够
  收益 ≈ 493 / 5 ≈ <b>100×</b>

<b>如果点随机插入</b>（本版的测试方式）：
  窗口内的点散落全树 → 读 ~100 页
  收益 ≈ <b>4.5×</b>
</pre>
<div class="callout">
<h3>这正是 Z-order 对照实验的价值</h3>
<p><b>Z-order 塞 B+tree 的强项恰恰是随机插入</b> —— Morton 码把空间邻近性编码进一维键，"
"所以窗口内的点在 B+tree 里也是连续的。<br>"
"但它的弱项是<b>聚簇分布</b>（Z-order 对聚簇数据不友好）和<b>长条形窗口</b>（区间分解碎）。</p>
<p><b>结论要可解释</b>：均匀分布 → R-tree 略优；聚簇分布 → R-tree 明显优；"
"高斯基组 → Z-order 略优（因为 Z-order 天然处理高维退化）。"
"<b>具体数字由 bench 测出，每组配一句解释。</b></p>
</div>
<div class="ok"><b>验收 A4 的口径要写清楚</b>：判据是「页读数 &lt; 全扫的 1/10」—— "
"<b>要在<b>聚簇分布</b>或<b>按空间顺序插入</b>的语料上测</b>，"
"随机分布下这个数字达不到。<b>如实记录实测值和分布条件</b>，不要为了让数字好看而换语料。</div>""")
    + bug("<b>坑 3（只测了随机分布，就宣布「剪枝没生效」）</b>："
          "随机插入 + 1% 窗口 → 收益只有 4.5× → 误以为 R-tree 实现错了。"
          "<b>修法</b>：bench 必须跑<b>三组分布</b>（均匀 / 聚簇 / 高斯基组），"
          "每组分别看 IO 和结论。<b>单组数据得不出结论。</b>")
))

# KNN
B.append(sec(
    "四 · mindist KNN <span>精确算法：靠下界剪枝</span>",
    "",
    """<pre>
KNN 查询（q, k）：
  优先队列 Q（按 mindist 最小堆）；结果集 R（按真实距离最大堆，容量 k）
  把根节点压入 Q

  while Q 非空:
     e = Q 弹出最小 mindist 的条目
     <b>if R 已满 k 且 e.mindist ≥ R 中第 k 近的真实距离: break</b>   ← 提前终止
     if e 是叶子条目:
         d = 真实距离(e.point, q)
         if R 未满 或 d < R 中最远: R 插入 d，弹出最远
     else:
         for 每个子条目 c: Q 压入 (mindist(c.MBR, q), c)

  return R 中 k 个（或不足 k 个）
</pre>
"""
    + card("提前终止条件为什么正确", "",
           """<p style="color:var(--tx2);font-size:13.5px">
因为 <b>mindist 是下界</b>：Q 里任何条目的真实距离 ≥ 它的 mindist。
当堆顶的 mindist 已经 ≥ 当前第 k 近的真实距离时，<b>Q 里剩下的所有条目都不可能进入结果集</b>。</p>
<pre>
  Q 堆顶 mindist = 5.0
  R 中第 k 近的真实距离 = 4.8
  → Q 里任何条目的真实距离 ≥ 5.0 &gt; 4.8 → 都不可能更近 → <b>可以停</b>
</pre>
<div class="ok"><b>这是精确算法与近似算法的根本差别</b>：
HNSW 用「多看一些（ef）换召回」，R-tree 用「下界保证不漏」换精确。"
"<b>一个牺牲精度换速度，一个牺牲速度换精度</b> —— 两条路都能走通，取决于需求。</div>""")
    + bug("<b>坑 4（mindist 提前终止条件写错 → 漏结果）</b>："
          "把 <code>&gt;=</code> 写成 <code>&gt;</code>，或者把「R 中第 k 近」写成「R 中最远」。"
          "<b>症状</b>：KNN 结果比暴力扫少几条，但<b>少的那些不是最远的</b>（所以用「离得近的都在」这种弱检查抓不到）。"
          "<b>修法</b>：<b>A3 全等兜底</b> —— KNN 结果必须与暴力排序<b>逐条相等</b>（含 tie-break）。"
          "终止条件附论文伪代码对照注释。")
    + ok("<b>验收数字</b>：10 万随机点 + 网格点，R-tree KNN == 暴力排序（并列按 row_id 升序）；"
         "窗口结果 == 暴力过滤结果（<b>含恰在边界上的点</b>）。")
))

# 事务
B.append(sec(
    "五 · 事务与恢复 <span>删除 = 标记死条目 + reindex</span>",
    "",
    tbl(["方面", "语义", "对照"], [
        ['<span class="mono">可见性</span>', '<code>visible()</code> 恒真', 'heap 按快照判 xmin/xmax'],
        ['<span class="mono">隔离</span>', '写者互斥（全局 latch）+ 读不加锁', 'heap 用版本链让读写互不阻塞'],
        ['<span class="mono">提交</span>', '语句级', 'heap 支持 BEGIN/COMMIT 多语句'],
        ['<span class="mono">WAL</span>', '改页即 <code>WAL_PAGEDATA</code> 整页镜像', 'heap 行级 undo-redo'],
        ['<span class="mono">删除</span>', '<b>标记死条目（不 rebalance）+ reindex 重建</b>', 'heap 打 xmax + vacuum'],
    ])
    + ok("<b>为什么不 rebalance</b>：R-tree 的删除要 rebalance（重新插入被删点的邻居），"
         "这和 B+tree 的删除先例一致 —— <b>标记死条目 + 定期重建</b>。"
         "reindex 走「导出所有活点 → 清空树 → 重新插入」，顺便解决空间碎片。")
    + bug("<b>坑 5（死条目还在树上 → 窗口查询返回已删的点）</b>："
          "死条目的 MBR 仍然参与剪枝和遍历。<b>修法</b>："
          "查询时检查条目的 alive 标记，<b>跳过死条目</b>（但 MBR 仍然用于剪枝 —— 它概括的是子树，"
          "死条目在里面不影响「这棵子树可能有命中点」的判断）。"
          "<b>注意区别</b>：叶子条目要跳过，MBR 照用。")
    + ok("<b>验收数字</b>：删除后窗口/KNN 不返回死点；reindex 重建后结果与暴力全等；"
         "崩溃恢复后树页重放可用（或走 reindex 路径）。")
))

# SQL 侧
B.append(sec(
    "六 · SQL 侧与能力协商 <span>空间谓词同样不进 SQL</span>",
    "",
    tbl(["入口", "内容", "支持", "理由"], [
        ['<span class="mono">db_geo_window</span>', '窗口查询（矩形相交）', '<b>主路径</b>',
         '相交语义不是标量比较'],
        ['<span class="mono">db_geo_knn</span>', 'KNN（精确）', '<b>主路径</b>',
         '排序键是计算出来的距离'],
        ['<span class="mono">geo_points</span>', '(row_id, x, y, minx, …) 标量列', '<b>SELECT</b>',
         'row_id / x / y 等值与范围下推'],
        ['<span class="mono">ST_Intersects 等</span>', '空间谓词进 SQL', '<b>否决</b>',
         '与 v1.3 的 match 同一边界：几何语义不进标量谓词系统'],
        ['<span class="mono">写操作</span>', 'INSERT / UPDATE / DELETE on geo_points', '<b>否决</b>',
         '本版只读；装载走专用 API → E_NOTSUP'],
    ])
    + callout("四条模态同款判决的边界（v1.4 结束后写进 DESIGN 复盘）", [
        "<b>进 SQL 的</b>：值可以映射成<b>通用标量</b>、执行器能独立重判的谓词 —— "
        "比如 JSON 路径值（v1.6）、普通列的等值/范围。",
        "<b>不进 SQL 的</b>：执行器无法独立重判的语义 —— "
        "<b>图遍历</b>（v0.11）、<b>向量 knn</b>（v1.0）、<b>相关性评分</b>（v1.3）、"
        "<b>空间窗口与 KNN</b>（v1.4）、<b>时间分区裁剪</b>（v1.5）。",
        "<b>判据只有一条</b>：<b>执行器能不能独立重判</b>。"
        "这一条判据在 v1.6 得到了最强的验证 —— <code>json_match</code> <b>能</b>进 SQL，"
        "因为路径提取值映射成了通用标量。",
    ])
    + ok("<b>验收</b>：<code>grep geo src/query/</code> 零命中（执行器零改动）；"
         "写操作报 E_NOTSUP；<b>IO 可证</b>：<code>db_geo_page_reads()</code> 前后对比。")
))

# 验收清单
B.append(sec(
    "全部做完后的验收清单 <span>能全过，这个模态就真的能用了</span>",
    "",
    checklist([
        "<b>MBR 数学</b>：相交 / 包含 / mindist 纯函数对照手算（含退化：点矩形、共边、负坐标）",
        "<b>窗口全等</b>：10 万随机点 + 网格点，R-tree 窗口结果 == 暴力（<b>含恰在边界上的点</b>）",
        "<b>KNN 全等</b>：R-tree KNN == 暴力排序（并列按 tie-break 规则）",
        "<b>剪枝可证</b>：1% 选择率窗口查询页读数 &lt; 全扫 1/10（<b>在聚簇/有序插入语料上</b>）",
        "<b>Z-order 对照</b>：均匀 / 聚簇 / 高斯基组，三组 IO 对比 + 每组一句解释",
        "<b>树高健康</b>：小树全量验证 —— 每个内部节点的 MBR 正确概括子树；树高 ≤ 理论最小高度 + 1",
        "<b>删除 + reindex</b>：删除后窗口/KNN 不返回死点；reindex 重建后全等",
        "<b>崩溃恢复</b>：树页 WAL 重放 + reindex 路径",
        "<b>执行器零改动</b>：<code>grep geo src/query/</code> 零命中；旧 152+ 项验收全绿",
    ])
))

# 后续
B.append(sec(
    "做完之后接什么 <span>三个方向，按你的目标选</span>",
    "",
    """<div class="card"><div class="blk">
<ul>
<li><b>想继续做多模 → 时序（v1.5）</b>：分区 + 列式压缩 + 裁剪。
<b>裁剪</b>和<b>剪枝</b>是同一件事的两个方向 —— 一个是「跳过整个分区」，一个是「跳过整棵子树」。
学完 R-tree 的剪枝，再看时序的分区裁剪会更容易理解「为什么要给 AM 足够的信息」。</li>
<li><b>想把空间做深 → 不排期池</b>：R*-tree / STR 批量加载、多边形与线串、
空间 JOIN、坐标系与投影（PostGIS 的 geography 模式）。
<b>这些都是「把 R-tree 换成更高级的变体」或「加更多几何类型」</b>，
机制层的新增量小于全文检索和向量。</li>
<li><b>想回头做向量 → v1.1</b>：v1.0 的 HNSW 是近似 KNN，本版的 R-tree 是精确 KNN。
<b>把两版的验收方法对照着看</b>（recall vs 全等），
是理解「近似 vs 精确」这个取舍最直接的方式。</li>
</ul>
</div></div>"""
))

B.append(foot(
    "整体设计见 <code>db/docs/DESIGN.md</code>（§5.3 模态同构性原则、§7 验收标准）；"
    "版本执行计划见 <code>db/docs/PLAN-v1.4.md</code>（四个关键判决 + 8 项验收 + 风险清单）；"
    "本规格是字节级施工图，三者冲突时以代码和 PLAN 的执行记录为准。",
    "R-tree 的插入 / ChooseSubtree / 二次分裂出自 Guttman, <i>R-Trees: A Dynamic Index Structure "
    "for Spatial Searching</i>, ACM SIGMOD Record 14(1), 1984；"
    "mindist 下界剪枝出自 Cheung &amp; Rote 的 R-tree 最近邻工作；"
    "Morton 码（Z-order 曲线）出自 Morton, <i>A Computer-Oriented Geometric Approach to "
    "Graphics</i>, CACM 1966；Z-order 窗口的 BigMin 区间分解出自 Rigoult et al., <i>Optimal "
    "Querying of R-Trees</i>, 1994。"
))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "地理空间模态实现规格.html")
n = write_spec(path,
               "地理空间模态实现规格 · 从零到能跑",
               "Build Spec · Spatial Modality",
               "地理空间模态：<em>1900 行</em>怎么拆",
               "R-tree + MBR 剪枝 + 精确 KNN。这是第五个独立模态的施工图，"
               "核心路径约 1900 行，测试约 780 行。对标 Guttman 原始 R-tree 与 PostGIS 的 GiST 思路 —— "
               "三个判据：结果与暴力全等、剪枝真的发生、Z-order 对照有可解释的结论。",
               "\n".join(B))
print("OK", path, n, "字符")
