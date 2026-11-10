# -*- coding: utf-8 -*-
"""生成《时序模态实现规格.html》"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_tpl import *

B = []

B.append(sec(
    "和关系模态的根本差异 <span>第六种存储布局：行式 + 列式共存</span>",
    "",
    tbl(["维度", "关系模态（heap）", "时序模态（ts）", "为什么不同"], [
        ['<span class="mono">写入</span>', '就地更新 + 版本链', '<b>按时间戳路由到分区，追加写</b>',
         '时序几乎不更新，追加是主路径'],
        ['<span class="mono">布局</span>', '永远是行式（slotted page）', '<b>活跃行式 → 封窗后列式</b>',
         '热数据要写得快，冷数据要存得小'],
        ['<span class="mono">分区</span>', '无（表就是表）', '<b>按时间范围分区，自管目录</b>',
         '时间是最强的裁剪维度'],
        ['<span class="mono">删除</span>', '逐行打 xmax', '<b>整分区 TTL 过期 → 页区间回收</b>',
         '时序唯一的删除路径是过期'],
        ['<span class="mono">压缩</span>', '无', '<b>封窗时逐列压缩（三种编码）</b>',
         '压缩必须有「变冷」的时机点'],
        ['<span class="mono">索引</span>', 'B+tree', '<b>无二级索引（时间就是索引）</b>',
         '时间分区本身就是索引'],
    ])
    + callout("这一版的核心验收：执行器零改动", [
        "时序数据的三特点（按时间有序追加 / 几乎不更新 / 旧数据变冷）决定了它的存储形态："
        "<b>为写入优化的布局 + 封窗压缩 + 整分区丢弃</b>。",
        "<b>本版最核心的验收是「执行器零改动」</b>：分区、压缩、裁剪<b>全部</b>藏在 AM 内部 —— "
        "执行器看到的仍然只是「行源」。这是 §5.3 同构性原则的最强测试。",
        "「分区封闭后不可变」是不变文件思想的<b>第三次</b>出现（blob → Lucene segment → 这里）。",
    ])
))

B.append(sec(
    "模块与行数 <span>按依赖排序，不是按重要性</span>",
    "",
    tbl(["模块", "职责", "行数", "依赖"], [
        ['<span class="mono">ts_codec.c</span>', "压缩三件套：delta-of-delta / delta+RLE / Gorilla（纯函数）", '<span class="n">~520</span>', "—"],
        ['<span class="mono">ts_part.c</span>', "分区目录（自管）+ 活跃分区行式写 + 封窗", '<span class="n">~480</span>', "pager, wal"],
        ['<span class="mono">ts_col.c</span>', "封闭分区的列式页读写", '<span class="n">~360</span>', "ts_codec"],
        ['<span class="mono">ts_index.c</span>', "分区目录索引 + 时间谓词解析 → 分区裁剪", '<span class="n">~280</span>', "ts_part"],
        ['<span class="mono">spi/ts_am.c</span>', "SPI 适配：ts_partitions 虚拟表 + 谓词下推 → 裁剪", '<span class="n">~300</span>', "spi, ts_index"],
        ['<span class="mono">db_ts.c</span>', "公开 API：db_ts_insert / query / retention（全部持 latch）", '<span class="n">~190</span>', "ts_am, latch"],
        ['<span class="mono">测试</span>', "test_ts_codec / test_ts_part / test_ts_it / test_ts_crash / ts_bench", '<span class="n">~800</span>', "—"],
    ])
    + ok("<b>占便宜的部分</b>：活跃分区直接复用 heap 的 slotted page 管理（不重写）；"
         "WAL 复用 <code>WAL_PAGEDATA</code>；SPI 复用 v0.9 定稿。"
         "<b>新东西在压缩编解码和分区生命周期上</b>。")
))

B.append(sec(
    "六步实现顺序 <span>每步都有可量测的验收，不通过不进下一步</span>",
    "",
    steps([
        ("压缩三件套 <span class=\"pill\">纯内存</span>",
         "delta-of-delta（时间）、delta+RLE（整数）、Gorilla（浮点）。"
         "验收：随机往返全等 + fuzz（随机长度/极值/负数/NaN）。",
         "~520"),
        ("分区目录 + 活跃分区写",
         "按时间戳路由到分区；分区目录自管（不进 catalog）。"
         "验收：写路径断言 <code>part_start ≤ t &lt; part_end</code>；<code>ts_partitions</code> 可见。",
         "~480"),
        ("封窗压缩 + 读路径",
         "写一组 <code>PT_TS_COL</code> 列式页 → 段头记 <code>sealed_lsn</code> → 旧页回收。"
         "验收：写 → 封窗 → 读，行集与写入全等（含列类型混合）。",
         "~360"),
        ("裁剪 + TTL",
         "时间谓词下推 → 只读命中分区；无谓词 → 全分区（超集安全）。"
         "TTL 过期分区整块回收进 freelist。",
         "~280"),
        ("SPI 接线",
         "<code>ts_am</code> + <code>ts_partitions</code> 虚拟表。"
         "验收：<b>执行器零改动</b>（<code>grep ts src/query/</code> 零命中）。",
         "~300"),
        ("崩溃恢复 + 基准 + 反向验证 + 文档",
         "验收：<b>压缩比可解释</b>（缓变 vs 随机 vs 恒定，三组数字 + 每组一句解释）；"
         "裁剪后页读 ≈ 命中分区大小（与总数据量无关）。",
         "~460"),
    ])
    + callout("关键提醒", [
        "<b>delta-of-delta 的位流读写最容易错</b>：变长编码的读写一旦不对称，"
        "解出来的值全错但<b>不报错</b>。<b>修法</b>：位流读写器<b>独立成文件、纯函数化</b>"
        "（给定字节流 → 唯一结果），然后 fuzz 到随机往返全等。",
        "<b>封窗是「不可逆转换」，所以要有一个明确的判定点</b>："
        "<code>sealed_lsn</code> 落盘即封窗成功。崩溃窗口（新页写完、旧页未回收）"
        "由恢复逻辑收敛：重放后发现新旧并存 → 用新页、旧页进 freelist。"
        "这个手法和 v0.6 的「meta 页是根指针」是同一个思路。",
    ])
))

# 判决
B.append(sec(
    "一 · 四个关键判决 <span>先把边界划死，再写代码</span>",
    "",
    card("判决 1（修正总览预判）：分区元数据自管，不进 catalog <span class=\"pill p\">本版与总览不一致的地方</span>", "",
         """<table>
<thead><tr><th>路径</th><th>分析</th></tr></thead>
<tbody>
<tr><td>进 catalog（sys_partition）</td>
    <td>前提是「要被通用查询裁剪就得让执行器看得见」—— <b>这个前提不成立</b>：
    裁剪发生在 AM 的 <code>push</code> 里（拿到时间谓词后查自己的分区表），
    执行器从头到尾不需要知道分区存在</td></tr>
<tr><td><b>自管目录 + <code>ts_partitions</code> 虚拟表</b></td>
    <td>① <code>blob_id</code> 下推的同款先例（blob 自管对象目录，下推时自己查）；
    ② 硬规则 6 的一致性更强：<b>catalog 不知道任何模态</b>
    （blob / graph / vector / ft 都没进 catalog，时序凭什么进）；
    ③ 用户可见性由虚拟表解决（<code>SELECT * FROM ts_partitions</code>），不需要系统表</td></tr>
</tbody></table>
<div class="ok"><b>这正好是「总览 → 独立 PLAN 细化」环节的价值证明</b>：
预判阶段看起来必须共享的结构，对照既有先例逐条过堂后，发现自管更一致。"
"<b>执行记录里保留这次修正。</b></div>""")
    + card("判决 2：封窗（seal）的时机与原子性 <span class=\"pill\">不可逆转换要有明确的判定点</span>", "",
           """<pre>
活跃分区（裸写）── 满/超时 ──→ 封窗压缩（不可逆）──→ 封闭分区（只读）
   slotted page                    列式压缩页                PT_TS_COL
</pre>
<p style="color:var(--tx2);font-size:13.5px"><b>触发</b>：分区行数 ≥ 阈值（config.h）
或时间窗滑过（封窗 = 最后一次写之后）。</p>
<pre>
<b>封窗五步</b>：
  ① 读活跃分区的所有行（行式 → 内存中的列）
  ② 逐列压缩，写一组新的 <code>PT_TS_COL</code> 页
  ③ <b>段头记 <code>sealed_lsn</code></b>   ← <b>判定规则：sealed_lsn 落盘即封窗成功</b>
  ④ 旧 slotted 页回收进 freelist
  ⑤ 更新分区目录项：state = SEALED，指向新页区间
</pre>
<div class="callout">
<h3>崩溃窗口怎么收敛</h3>
<p>崩在 ② 和 ③ 之间 → 新页写了但 <code>sealed_lsn</code> 没落 → 恢复时看到「新旧并存」，
<b>判定为未封窗</b> → 丢弃新页，继续用旧行式分区（数据没丢，只是没压缩）。</p>
<p>崩在 ③ 和 ④ 之间 → <code>sealed_lsn</code> 已落 → 判定为已封窗 → 用新页，旧页进 freelist。</p>
<p><b>判定规则只有一条，落在 <code>sealed_lsn</code> 上</b> —— 这是 v0.6「meta 页是根指针」手法的复用。</p>
</div>""")
    + card("判决 3：压缩三件套（逐列选型） <span class=\"pill\">每种编码对应一种数据形态</span>", "",
           """<table>
<thead><tr><th>列</th><th>编码</th><th>出处 / 适用</th></tr></thead>
<tbody>
<tr><td>时间戳</td><td><b>delta-of-delta</b>（变长 zigzag：常间隔 → 0x0 一个 bit）</td>
    <td>Gorilla 论文（Facebook 2015）</td></tr>
<tr><td>整数（tag id 等）</td><td><b>delta + RLE</b>（重复值游程）</td>
    <td>通用时序实践</td></tr>
<tr><td>浮点（value）</td><td><b>Gorilla XOR</b>（与前值异或 → 前导零/尾零计数 + 有效位）</td>
    <td>Gorilla 论文</td></tr>
</tbody></table>
<div class="callout">
<h3>为什么是列式（而且只在封窗后）</h3>
<p><b>活跃分区是行式的</b>（slotted page）—— 热数据要写得快，
逐行追加进 slotted page 最直接。<b>封闭分区是列式的</b>（对齐 v1.0 向量列式页的先例）：
每列一段连续页，行号 → 列内偏移有页级换算。</p>
<p><b>「封窗」就是格式转换的时机</b>。这也是学习点：
<b>列式不是 everywhere 的，是「冷数据」的</b>（Lance / ClickHouse 的现实约束同构）。</p>
</div>""")
    + card("判决 4：TTL 的页回收 <span class=\"pill\">整分区过期，不是逐行删</span>", "",
           """<pre>
db_ts_retention(conn, seconds) → 过期分区整块回收：

  分区页区间 → <b>freelist</b>（页号空间共享，回收的页可被任何结构用）
  分区目录项 → 标记 dropped（虚拟表仍可见历史，标 state）
</pre>
<div class="callout">
<h3>回收进 freelist 的合法性（对照 v0.8 的「blob 页不进表 freelist」）</h3>
<p>那条规定说的是「<b>表的</b> freelist」（每表独立、语义是「哪页能放下 N 字节的行」）；
时序回收走的是<b>全局页分配器的空闲回收</b>，与 blob 新分配页同一条路（<code>pager_alloc</code> 的对侧）。</p>
<p><b>两条规则不冲突</b> —— 一条管「表页的复用语义」，一条管「页号空间的回收」。</p>
</div>""")
))

# 压缩三件套细节
codec = card(
    "delta-of-delta（时间列） <span class=\"pill\">常间隔 → 1 bit</span>",
    "",
    """<pre>
第一步 delta： t[i] − t[i−1]          ← 变成「间隔」序列
第二步 delta-of-delta： d[i] − d[i−1]   ← 变成「间隔的变化」序列

<b>zigzag 变长编码</b>（把有符号数映射成无符号，使小负数也只占 1 字节）：
  0        → 0x00            (1 bit)
  1        → 0x02            (2 bit)
  −1       → 0x01            (2 bit)
  2        → 0x04            (3 bit)
  −2       → 0x03            (3 bit)
  …

<b>常间隔序列（dofd 全 0）→ 每项 1 bit</b>
  1000 个点 → 1000 bit = 125 B
</pre>
<div class="ok"><b>对比</b>：原始时间戳是 u64（8 B），1000 个点 = 8000 B。
<b>压缩比 64×</b> —— 这就是 delta-of-delta 的全部价值。</div>"""
)
codec += card(
    "delta + RLE（整数列） <span class=\"pill\">重复值走游程</span>",
    "",
    """<pre>
1) delta 编码： v[i] − v[i−1]        （同 tag id 连成片 → delta 全 0）
2) RLE：连续相同的 delta 合并成 (游程长度, 值)

<b>1000 个点，同一个 tag id</b>：
  delta 全 0 → RLE 压成 (1000, 0) → <b>几个字节</b>
</pre>
<div class="ok"><b>为什么要 delta 再 RLE，而不是直接 RLE</b>：
原始值相同的游程好找，但「缓慢递增」（温度 21.0, 21.1, 21.2…）直接 RLE 效果差；
先 delta 再 RLE，递增变成「全是 1」的游程 → <b>同样被压掉</b>。</div>"""
)
codec += card(
    "Gorilla XOR（浮点列） <span class=\"pill\">Facebook 2015</span>",
    "",
    """<pre>
1) v[i] XOR v[i−1]                  ← 与前值的差异
2) 若 XOR == 0（前导 0）→ 只写一个控制位「没变」
3) 否则写：前导零个数 / 有效位长度 / 有效位

<b>前导零 / 尾零的计数</b>用「控制字节」打包：
  控制字节高位 = 有效位长度（0..64），低位 = 前导零个数
  两者之和 ≤ 64 时可塞进一个字节
</pre>
<div class="ok"><b>关键洞察</b>：浮点数在<b>值接近</b>时，
XOR 结果的<b>高位和低位都是 0</b>（只有中间几位在变）→ 编码后极短。
"这就是 Gorilla 对<b>缓变数据</b>特别有效的原因。</div>""")
B.append(sec("二 · 压缩三件套的字节级机制 <span>逐列选型，各有适用形态</span>", "", codec
    + bug("<b>坑 1（位流读写不对称 → 解出来的全是垃圾，但不报错）</b>："
          "变长编码的读和写必须严格镜像。任何一处「写 3 位读 4 位」→ 位流错位 → "
          "<b>后面所有值全错</b>，而代码<b>不报任何错</b>。<br>"
          "<b>修法</b>：位流读写器独立成文件、纯函数化（<code>decode(encode(x)) == x</code>），"
          "并 fuzz 到随机往返全等。<b>不要把读写器写在同一个函数里</b> —— 那样错误会互相抵消，测不出来。")
    + bug("<b>坑 2（Gorilla 全同值 / 全异值的边界）</b>："
          "全同值时 XOR 恒为 0 → 走「没变」分支（好处理）；"
          "全异值时前导零可能为 0、有效位可能为 64 → "
          "<b>控制字节塞不下（0..64 之和 &gt; 64）→ 需要额外的扩展分支</b>。<br>"
          "<b>修法</b>：论文伪代码逐行对照注释 + 极值用例（0.0 / −0.0 / NaN / ±MAX / 极小值）。"
          "<b>NaN 要特别小心</b>：NaN != NaN，XOR 后结果依赖位模式，不能用相等比较判断「没变」。")
    + ok("<b>验收数字</b>：三组语料各测压缩比 —— "
         "<b>缓变</b>（±1% 抖动）显著优于<b>随机白噪声</b>，恒定值最优；"
         "数字与 Gorilla 论文报告的量级一致（Facebook 报告 ~10×+ 的典型压缩）。"
         "<b>每组配一句解释</b>（为什么这组压得好 / 不好）。")
))

# 布局
layout = card(
    "活跃分区（行式） <span class=\"pill p\">复用 heap 的 slotted page</span>",
    "",
    """<pre>
活跃分区 = 一组 slotted page（PT_HEAP 的变体，或专门的 PT_TS_ACTIVE）
  每行 = (timestamp, tag_id, value)

<b>写入路径</b>：
  ① 按 timestamp 算出落在哪个分区（分区窗口是固定的，如 1 天）
  ② 在该分区内追加一行（复用 heap 的 slot 分配）
  ③ 写 WAL_PAGEDATA 整页镜像
</pre>
<div class="ok"><b>为什么活跃期用行式</b>：热数据的写入模式是「不断追加」，
slotted page 的 slot 分配已经解决得很好了。<b>换列式反而要处理「一行跨越多列」的写入对齐问题</b>，
热路径上没好处。</div>"""
)
layout += card(
    "封闭分区（列式） <span class=\"pill p\">PT_TS_COL</span>",
    "",
    """<pre>
一个封闭分区 = 每列一段连续页：

  ┌──────────────┬──────────────┬──────────────┐
  │ 时间列页段    │ tag 列页段    │ value 列页段  │
  │ (delta-of-   │ (delta+RLE)  │ (Gorilla XOR)│
  │  delta 位流) │              │              │
  ├──────────────┼──────────────┼──────────────┤
  │ 列头：行数 / 列偏移表 / 压缩方式标识          │
  └──────────────┴──────────────┴──────────────┘

<b>行号 → 列内偏移</b>：列内是变长编码，所以不能纯算术。
  需要一张「行偏移表」（每行在位流里的起始偏移）。
  → 偏移表本身是一列<u>未压缩</u>的定长数据（u32 / 行）
</pre>
<div class="ok"><b>列内偏移表是变长编码的必要代价</b>：
delta-of-delta / Gorilla 都是变长的，行 r 的位置无法纯算术定位。
"偏移表用 u32（4 B/行）→ 1000 行 = 4 KB，这部分不压缩。"
"<b>对比</b>：不压缩的话时间列是 8 B/行 = 8 KB，偏移表只占一半 —— "
"<b>压缩后偏移表反而成了大头</b>。这是变长编码的固有代价，必须如实算进压缩比。</div>"""
)
layout += card(
    "分区目录 <span class=\"pill\">自管，不进 catalog</span>",
    "",
    """<pre>
分区目录（ts_partition 自管的一段页）：
  ┌────────────────────────────────────────────┐
  │ 分区项 i:                                  │
  │   part_id      u32                         │
  │   part_start   i64   时间窗下界（含）        │
  │   part_end     i64   时间窗上界（不含）      │
  │   state        u8    ACTIVE / SEALED / DROPPED│
  │   first_page   u32                         │
  │   page_count   u32                         │
  │   sealed_lsn   u64   ← <b>封窗判定点</b>      │
  │   row_count    u32                         │
  └────────────────────────────────────────────┘
</pre>
<div class="ok"><b>用户可见性</b>：<code>SELECT * FROM ts_partitions</code> 能看到全部分区
（含已过期的，标 state = DROPPED）。<b>不需要系统表</b> —— 硬规则 6 的一致性更强。</div>"""
)
B.append(sec("三 · 行式与列式共存的布局 <span>同一个模态里的两种格式</span>", "", layout
    + bug("<b>坑 3（乱序时间戳路由错分区）</b>：写入时算错了时间窗 → 数据进了错误的分区 → "
          "<b>查询结果静默错误</b>（带时间谓词的查询会漏掉这行）。<br>"
          "<b>修法</b>：写路径<b>断言</b> <code>part_start ≤ t &lt; part_end</code>，"
          "越界直接报 E_RANGE；负例进测试（往过去的分区写、往未来的分区写）。")
    + bug("<b>坑 4（封窗中途崩溃 → 新旧页并存）</b>：压缩写了一半崩了。"
          "<b>修法</b>：<code>sealed_lsn</code> 落盘即封窗成功的判定规则（判决 2）；"
          "恢复时发现「同一分区有行式页和列式页并存」→ 按 <code>sealed_lsn</code> 决定用哪边。")
))

# 裁剪
B.append(sec(
    "四 · 分区裁剪 <span>执行器零改动的关键</span>",
    "",
    """<pre>
<b>裁剪发生在 AM 的 push 里</b>（不是执行器里）：

  执行器 → AM.push(谓词) → AM 自己解析时间谓词
                        → AM 查自己的分区目录
                        → 只打开命中的分区页
                        → 返回行源（和普通表一样）
</pre>
"""
    + tbl(["谓词形态", "裁剪行为", "语义"], [
        ['<span class="mono">t BETWEEN a AND b</span>', '<b>精确裁剪</b> → 只读命中的分区',
         '结果精确'],
        ['<span class="mono">t = c</span>', '<b>精确裁剪</b> → 只读一个分区', '结果精确'],
        ['<span class="mono">t &gt; a</span>', '裁剪 → 排除 a 之前的分区', '结果精确'],
        ['<span class="mono">t IN (...)</span>', '裁剪 → 读涉及的分区并集', '结果精确'],
        ['<span class="mono">t &gt; a OR t &lt; b</span>', '<b>无法裁剪</b> → 全分区', '<b>超集，安全</b>'],
        ['<span class="mono">f(t) &gt; a</span>', '<b>无法裁剪</b> → 全分区', '<b>超集，安全</b>'],
        ['<span class="mono">无时间谓词</span>', '<b>无法裁剪</b> → 全分区', '<b>超集，安全</b>'],
    ])
    + ok("<b>超集铁律</b>：裁剪<b>只能少读，不能少给</b>。"
         "解析不了谓词时<b>宁可全分区</b>（多读但结果正确），"
         "也不能猜一个范围（少读 → 漏数据）。"
         "<b>「无法裁剪」不是失败，是一种正确的能力声明</b>。")
    + bug("<b>坑 5（为了裁剪率而猜谓词范围 → 静默漏数据）</b>："
          "看到 <code>OR</code> 就想「取两个区间的并集」—— 但 OR 的两个分支可能跨分区，"
          "拆成并集要非常小心。<b>修法</b>：<b>只对能安全解析的谓词形态做裁剪</b>，"
          "其余一律全分区。<b>两种路径的结果必须全等</b>（验收 A4 明确要求）。")
    + ok("<b>验收数字</b>：裁剪后页读 ≈ 命中分区大小（<b>与总数据量无关</b>）；"
         "<b>两种路径（有时间谓词 / 无谓词）结果都全等</b>。")
))

# 事务
B.append(sec(
    "五 · 事务与恢复 <span>封窗是这一版的原子性难点</span>",
    "",
    tbl(["方面", "语义", "对照"], [
        ['<span class="mono">可见性</span>', '<code>visible()</code> 恒真', 'heap 按快照判 xmin/xmax'],
        ['<span class="mono">隔离</span>', '写者互斥（全局 latch）+ 读不加锁', 'heap 用版本链让读写互不阻塞'],
        ['<span class="mono">提交</span>', '语句级', 'heap 支持 BEGIN/COMMIT 多语句'],
        ['<span class="mono">WAL</span>', '活跃分区整页镜像；封窗重写<b>前后镜像</b>', 'heap 行级 undo-redo'],
        ['<span class="mono">删除</span>', '<b>整分区 TTL</b> → 页区间回收', 'heap 打 xmax + vacuum'],
    ])
    + card("封窗的 WAL 与恢复 <span class=\"pill\">新旧页并存怎么收敛</span>", "",
           """<pre>
<b>WAL</b>：
  活跃分区的每次修改 → 整页镜像（和 heap 一样）
  封窗压缩 → 新页整页镜像 + <code>sealed_lsn</code> 记录

<b>恢复时的三种状态</b>：
  ① 只有旧行式页              → 未封窗，继续用旧分区（数据完整）
  ② 新列式页 + <code>sealed_lsn</code> 已落 → <b>已封窗</b>，用新页，旧页进 freelist
  ③ 新列式页 + <code>sealed_lsn</code> 未落 → <b>未封窗</b>，丢弃新页，继续用旧分区
</pre>
<div class="ok"><b>三种状态里，数据都不会丢</b> —— 最坏情况是「压缩白做了」"
         "（回到旧格式，功能正常，只是不省空间）。<b>这是正确的降级</b>。</div>""")
    + ok("<b>验收数字</b>：活跃分区重放 + 封窗中途断电（判决 2 的收敛规则）+ TTL 后恢复，"
         "三种路径结果全对。")
))

# SQL 侧
B.append(sec(
    "六 · SQL 侧与能力协商 <span>分区完全藏在 AM 内部</span>",
    "",
    tbl(["入口", "内容", "支持", "理由"], [
        ['<span class="mono">db_ts_insert</span>', '按时间戳写入（自动路由分区）', '<b>主路径</b>',
         '写入路径要断言时间窗'],
        ['<span class="mono">db_ts_query</span>', '带时间范围查询', '<b>主路径</b>',
         '范围 + 裁剪'],
        ['<span class="mono">ts_partitions</span>', '分区元数据（part_id / 时间窗 / state / 行数）', '<b>SELECT</b>',
         '用户可见性靠虚拟表，不靠系统表'],
        ['<span class="mono">ts_points</span>', '数据点虚拟表（timestamp / tag / value）', '<b>SELECT</b>',
         '供 SQL 查元数据与数据'],
        ['<span class="mono">时间谓词下推</span>', 'BETWEEN / = / &gt; / IN', '<b>支持</b>',
         '<b>裁剪的唯一入口</b> —— 谓词经 AM 的 push 通道进来'],
        ['<span class="mono">写操作</span>', 'INSERT / UPDATE / DELETE on ts_points', '<b>否决</b>',
         '本版只读；写入走专用 API → E_NOTSUP'],
    ])
    + callout("为什么这一版的「执行器零改动」是最强的同构性检验", [
        "前面的模态大多<b>不碰</b>执行器（blob 完全在 SPI 另一侧；图/向量/全文/空间走专用 API）。"
        "但时序<b>确实要给执行器传时间谓词</b> —— 它要下推。",
        "<b>零改动怎么做到</b>：谓词下推的通道（<code>pushdown.h</code> 的 <code>Predicate</code>）"
        "是 v0.9 定的<b>通用标量谓词</b>（<code>col_idx, op, DbValue</code>）—— "
        "<b>时间谓词正好落在这个既有契约里</b>，执行器<b>一行都不用改</b>。",
        "<b>所以这一版验证的是：通用的下推契约够不够用</b>。"
        "答案是够用 —— 只要谓词是标量比较，AM 就能自己解释。"
        "<b>这和 v1.3（相关性评分）、v1.4（空间相交）的不进 SQL 是同一条判据的两面。</b>",
    ])
    + ok("<b>验收</b>：<code>grep ts src/query/</code> 零命中（执行器零改动）；"
         "旧 152+ 项验收全绿；两种裁剪路径（有时间谓词 / 无谓词）结果全等。")
))

# 验收清单
B.append(sec(
    "全部做完后的验收清单 <span>能全过，这个模态就真的能用了</span>",
    "",
    checklist([
        "<b>压缩三件套</b>：各自单测往返全等 + fuzz（随机长度/极值/负数/NaN）",
        "<b>全链路</b>：写 → 封窗 → 读，行集与写入全等（含列类型混合）",
        "<b>压缩比可解释</b>：缓变（±1% 抖动）vs 随机白噪声 vs 恒定，三组数字 + 每组一句解释",
        "<b>裁剪正确 + 超集</b>：有时间谓词 → 只读命中分区；无 / 不可解析谓词 → 全分区；<b>两种路径结果都全等</b>",
        "<b>执行器零改动</b>：<code>grep ts src/query/</code> 零命中",
        "<b>TTL</b>：过期分区回收后查询无旧数据、页可复用（freelist 计数）、目录标记正确",
        "<b>崩溃恢复</b>：活跃分区重放 + 封窗中途断电（<code>sealed_lsn</code> 收敛规则）+ TTL 后恢复",
        "<b>IO 可证</b>：裁剪后页读 ≈ 命中分区大小（与总数据量无关）",
        "<b>路由正确</b>：写路径断言 <code>part_start ≤ t &lt; part_end</code>；负例进测试",
        "<b>旧验收全绿</b>；依赖方向正确；新文件 &lt; 800 行",
    ])
))

# 后续
B.append(sec(
    "做完之后接什么 <span>三个方向，按你的目标选</span>",
    "",
    """<div class="card"><div class="blk">
<ul>
<li><b>想收尾整个项目 → 文档 / JSON（v1.6）</b>：JSONB + 路径索引。
这是<b>唯一寄生在 heap 上的模态</b>，也是<b>唯一必然要碰执行器的模态</b> ——
所以它的 A7 验收专门定义了「执行器改动审计」：允许的落点<b>先定义后实现</b>。</li>
<li><b>想把时序做深 → 不排期池</b>：乱序写缓冲（OOO buffer）、
块级倒排（时间范围 → 列块索引）、降采样 / 连续查询、多指标宽表。
<b>乱序缓冲值得注意</b> —— 它会降低压缩率，这是如实记录的代价，不是要修的 bug。</li>
<li><b>想回看贯穿线索 → v1.6 收官复盘</b>：三条线索到这时全部攒齐 ——
<b>不变文件 + 后台合并</b>（blob / Lucene segment / 向量列式页 / 时序分区压缩 × 4）、
<b>外部值句柄 + 惰性物化</b>（blob 帧 / 向量 / 大 JSON × 3）、
<b>下推只能给超集</b>（× 7）。这张表是 v1.6 的复盘素材，也是整个项目的收尾总结。</li>
</ul>
</div></div>"""
))

B.append(foot(
    "整体设计见 <code>db/docs/DESIGN.md</code>（§5.3 模态同构性原则、§7 验收标准）；"
    "版本执行计划见 <code>db/docs/PLAN-v1.5.md</code>（四个关键判决 + 9 项验收 + 风险清单）；"
    "总览与八模态盘点见 <code>db/docs/PLAN-modality-expansion.md</code>；"
    "本规格是字节级施工图，三者冲突时以代码和 PLAN 的执行记录为准。",
    "delta-of-delta 与 Gorilla XOR 编码出自 Pelkonen et al., <i>Gorilla: A Fast, Scalable, "
    "In-Memory Time Series Database</i>, VLDB 2015（Facebook）；"
    "时序分区 + 冷数据列式压缩的现实约束对标 ClickHouse 与 Prometheus 的分区 + 压缩设计；"
    "「不变文件 + 后台合并」的思想在 Lucene segment 机制中最完整。"
))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "时序模态实现规格.html")
n = write_spec(path,
               "时序模态实现规格 · 从零到能跑",
               "Build Spec · Time-Series Modality",
               "时序模态：<em>2200 行</em>怎么拆",
               "时间分区 + 封窗压缩 + 整分区回收。这是第六个独立模态的施工图，"
               "核心路径约 2200 行，测试约 800 行。对标 Gorilla 的三种编码与 ClickHouse 的分区列式压缩 —— "
               "两个判据：执行器零改动（同构性最强的检验），压缩比可解释（缓变 vs 随机 vs 恒定）。",
               "\n".join(B))
print("OK", path, n, "字符")
