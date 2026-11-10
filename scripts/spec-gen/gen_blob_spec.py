# -*- coding: utf-8 -*-
"""生成《blob 模态实现规格.html》"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_tpl import *

B = []

B.append(sec(
    "和关系模态的根本差异 <span>先看这张表，再写一行代码</span>",
    "",
    tbl(["维度", "关系模态（heap）", "blob 模态（LOB）", "为什么不同"], [
        ['<span class="mono">寻址</span>',
         'RID = (page_id, slot)',
         '<b>blob_id = 头页页号 + 1</b>',
         'blob 没有行概念；对象号就是一个页号'],
        ['<span class="mono">页粒度</span>',
         'slotted page：一页装 N 行变长记录',
         '一整页 = <b>一帧</b>（4096 − 20 = 4076 B）',
         '帧是 IO 的最小单位，一页一帧 = 零浪费'],
        ['<span class="mono">随机访问</span>',
         '读第 3000 <b>字节</b> → 要读前 3000 字节经过的所有 overflow 页',
         '读第 3000 <b>帧</b> → 只读那一帧所在的一页',
         '<b>IO 量 O(1) vs O(位置)</b> —— 这是 blob 存在的全部理由'],
        ['<span class="mono">生命周期</span>',
         'xmin/xmax + MVCC 版本链',
         '<b>不可变</b>，写完定稿',
         '对象存储的标准模型（S3 的 PUT 语义）'],
        ['<span class="mono">页归属</span>',
         '进表的 freelist（语义是「哪页能放下 N 字节的行」）',
         '<b>不进任何表的 freelist</b>',
         '语义不同，混用 → freelist 把 blob 页当行页用 → 写坏数据'],
        ['<span class="mono">元数据</span>',
         'catalog（表/列/索引）',
         '<b>自管目录页</b>（不进 catalog）',
         '模态之间不许互相依赖（SPI 的硬约束）'],
    ])
))

B.append(sec(
    "模块与行数 <span>按依赖排序，不是按重要性</span>",
    "",
    tbl(["模块", "职责", "行数", "依赖"], [
        ['<span class="mono">blob.h</span>', "对象/帧的公开契约 + 大量设计注释（为什么这样定）", '<span class="n">220</span>', "pager, types"],
        ['<span class="mono">blob_hdr.h/.c</span>', "对象头格式：字段偏移、CRC32 解析与写回", '<span class="n">68 + 69</span>', "config, types"],
        ['<span class="mono">blob.c</span>', "对象分配、帧读写、目录页、统计、定位信息", '<span class="n">772</span>', "pager, wal, blob_hdr"],
        ['<span class="mono">db_blob.c</span>', "公开 API 包装（全部持 latch，v0.10 硬规则 8）", '<span class="n">~150</span>', "blob, latch"],
        ['<span class="mono">测试</span>', "test_blob.c / test_blob_crash.c / test_blob_it.c", '<span class="n">~520</span>', "—"],
    ])
    + ok("<b>占便宜的部分</b>：WAL 零新代码（复用 <code>WAL_PAGEDATA</code> 整页镜像）；"
         "页格式复用 20 B 公共页头；缓冲池复用 pager。<b>本模态的新东西只在对象头和帧寻址上</b>。")
))

B.append(sec(
    "六步实现顺序 <span>每步都有可量测的验收，不通过不进下一步</span>",
    "",
    steps([
        ("对象头格式 <span class=\"pill\">blob_hdr</span>",
         "magic / version / nframes / frame_size / total_len / base_page / n_index_blk[] / hdr_crc。"
         "验收：解析-写回-再解析全等；magic 改一个字节 → 必须报 <code>E_CORRUPT</code>。",
         "~140"),
        ("对象分配与帧寻址",
         "一次 <code>pager_alloc</code> 分配 <code>1 + nframes</code> 个<b>连续</b>页；"
         "帧号 → 页号 = <code>base_page + N</code> 的纯算术。验收：读第 3000 帧页读 ≤ 2 次。",
         "~200"),
        ("帧读写与边界",
         "整帧读写、最后一帧不满的处理、越界报错。验收：写 1MB 读回逐字节相同；越界不返回垃圾。",
         "~220"),
        ("目录页与统计",
         "blob 自管目录（不依赖 catalog），页号存内存、重启靠扫描重建。验收：重启后对象列表一致。",
         "~150"),
        ("WAL 与崩溃恢复",
         "blob 页写 <code>WAL_PAGEDATA</code> 整页镜像。验收：写一半崩溃 → 重启后要么完整要么不存在，"
         "<b>不能是半截数据</b>。",
         "~120"),
        ("公开 API + 接线",
         "<code>db_blob_create/write/read_frame/read/stat</code>，全部持 latch。"
         "验收：旧 152 项验收全绿；<code>grep blob src/query/</code> 零命中（执行器零改动）。",
         "~170"),
    ])
    + callout("关键提醒", [
        "第 1 步就<b>先把「什么情况下索引是必需的」想清楚</b>，再决定要不要为它付 IO 代价。"
        "本项目的答案是：<b>帧索引的数据结构照样建（为支持将来增量写），但 v0.8 的快路径恰好能绕开它</b>"
        "——对象一次写完 → 页连续 → 纯算术 → 零索引 IO。"
        "「为扩展留好接口，但不提前付代价」是这一版最重要的设计习惯。",
    ])
))

layout = card(
    "Page Header（20 B）<span class=\"pill p\">公共页头，所有页型共用</span>",
    "",
    tbl(["偏移", "字段", "大小", "说明"], [
        ['<span class="n">0</span>', '<span class="mono">page_type</span>', '<span class="n">1</span>',
         '<code>PT_BLOB = 0x06</code>（独立页类型，heap 扫描只认 <code>PT_HEAP</code>）'],
        ['<span class="n">1</span>', '<span class="mono">flags</span>', '<span class="n">1</span>',
         'bit0 dirty / bit1 has_overflow / bit2 needs_compact'],
        ['<span class="n">2</span>', '<span class="mono">slots</span>', '<span class="n">2</span>',
         'blob 页<b>不用</b>（固定为 0）'],
        ['<span class="n">4</span>', '<span class="mono">free_off</span>', '<span class="n">2</span>',
         'blob 页不用'],
        ['<span class="n">6</span>', '<span class="mono">free_bytes</span>', '<span class="n">2</span>',
         'blob 页不用'],
        ['<span class="n">8</span>', '<span class="mono">next</span>', '<span class="n">4</span>',
         'blob 帧页不用（帧之间靠算术，不靠链）'],
        ['<span class="n">12</span>', '<span class="mono">page_lsn</span>', '<span class="n">4</span>',
         '本页最后修改的 LSN（幂等重放用）'],
        ['<span class="n">16</span>', '<span class="mono">checksum</span>', '<span class="n">2</span>',
         'crc16，检测半写'],
        ['<span class="n">18</span>', '<span class="mono">tid</span>', '<span class="n">2</span>',
         'blob 页不用（0）'],
    ])
)
layout += """
<div class="tbl-scroll"><table>
<thead><tr><th>帧页布局</th><th>字节</th><th>说明</th></tr></thead>
<tbody>
<tr><td>页头</td><td class="n">20</td><td><code>H_TYPE = PT_BLOB</code>，其余字段全 0</td></tr>
<tr><td>帧数据</td><td class="n">4076</td><td><code>BLOB_FRAME_SIZE = PAGE_SIZE − HDR_SIZE</code>，原始字节，无结构</td></tr>
</tbody></table></div>
"""
layout += ok("<b>容量速算</b>：帧 4076 B → <b>1 帧 = 1 页</b>；1 GB 对象 = 263,006 帧 ≈ 263,006 页 ≈ <b>1.03 GB</b>"
             "（开销 0.5%，来自 20 B 页头）。最后一帧不满时按填充 0 处理，浪费 ≤ 4076 B。")

hdr = card(
    "对象头页布局 <span class=\"pill p\">BLOB_HDR_OFF = HDR_SIZE = 20</span>",
    "",
    """<pre>
偏移(相对 hdr 区)  字段          大小   含义
────────────────── ───────────── ───── ──────────────────────────
 0                magic         u32   0x424C4F42 = "BLOB"  ← 魔数
 4                version       u32   格式版本（当前 1）
 8                nframes       u64   总帧数
16                frame_size    u32   每帧字节数（读时校验 == 当前页大小）
20                total_len     u64   实际数据总长（≤ nframes × frame_size）
28                base_page     u32   帧数据起始页号（= 头页页号 + 1）
32                n_index_blk   u32   索引块个数
36                index_blk[]   u32[] 每个索引块的页号（≤ 64 个）
36+4n             hdr_crc       u32   上面所有字段的 crc32
</pre>
"""
    + tbl(["对象头字段", "为什么需要它"], [
        ['<span class="mono">magic</span>', '识别这是一张 blob 对象头页。<b>没有 magic 的页在崩溃恢复后无法与别的页区分</b>'],
        ['<span class="mono">version</span>', '格式演进的空间 —— 以后改布局时老对象还能读'],
        ['<span class="mono">frame_size</span>', '校验用。若有人把页大小从 4096 改成 16384，老对象的帧大小对不上 → 必须报错而不是乱读'],
        ['<span class="mono">total_len</span>', '对象真实长度（≤ nframes × frame_size）。<b>没有它就不知道末帧填了多少 0</b>'],
        ['<span class="mono">base_page</span>', '帧数据起点。<b>有了它帧号→页号就是纯算术</b>'],
        ['<span class="mono">index_blk[]</span>', '帧索引块的位置表。<b>当前快路径不用它，但数据结构建好 = 为增量写留好接口</b>'],
        ['<span class="mono">hdr_crc</span>', '对象头损坏 → 整个对象不可用。crc32 覆盖上面所有字段'],
    ])
)
B.append(sec("一 · 对象头与帧的字节布局 <span>页大小 4096，公共页头 20 B</span>", "", layout + hdr
             + bug("<b>坑 1（blob 独有的坑）</b>：<b>blob_id 直接用页号，首个对象的 id 就是 0</b> —— "
                   "和「无效 id」无法区分。API 的常见用法是「先查有没有，有才用」，"
                   "于是到处要写 <code>if (id == 0) 无效</code> 的特判。<br>"
                   "<b>修法</b>：<code>blob_id = 页号 + 1</code>，反解 <code>页号 = blob_id − 1</code>。"
                   "<code>id = 0</code> 天然表示「没有对象」，全项目零特判。<b>blob_id → base_page 依然是 O(1) 算术</b>。")
             ))

B.append(sec(
    "二 · 帧号 → 页号：为什么是纯算术 <span>blob 模态的核心机制</span>",
    "",
    callout("先纠正一个方向性错误", [
        "初稿按「关系模态的 TEXT 太大 → 外置到 overflow 页」来设计（TOAST 式）。"
        "<b>这是错的</b>，因为验收标准「读 1GB 对象第 3000 帧，IO 量 ≈ 4KB」本身就说不通："
        "行外置模型里<b>没有「对象的第 N 帧」这个概念</b>。",
        "关系模态的大字段解决的是「一行一个 TEXT 太大」；blob 模态解决的是「<b>多个大对象</b>」。"
        "让关系模态背这个包袱，会把两个模态的机制混成一团。",
        "对标业界的是 <b>S3/MinIO 的对象存储</b>、<b>MongoDB GridFS</b>、<b>Parquet/Lance 的分块文件</b> —— "
        "共同机制是「对象 + 帧 + 帧索引」，而不是「数据库里的大字段」。",
    ])
    + """<pre>
对象的物理形态（页连续，因为一次写完）：

  ┌─────────────┬────────────────────────────────────────┐
  │ 对象头页     │  帧数据页（连续的 nframes 页）          │
  │ (base+0)    │  base+1, base+2, ... base+nframes     │
  ├─────────────┼────────────────────────────────────────┤
  │ 索引块 0     │  索引块 1, ... 索引块 m                │
  │ (base+n+1)  │  （帧号 → 页号 的映射，备用）            │
  └─────────────┴────────────────────────────────────────┘

  <b>第 N 帧 → 页号 = base_page + N</b>       ← 纯算术，零索引 IO
</pre>
"""
    + card("为什么帧索引还要建 <span class=\"pill\">为扩展留接口，但不提前付代价</span>", "",
           """<p style="color:var(--tx2);font-size:13.5px">1 GB / 4076 B ≈ 263,006 帧。
一页装不下 263,006 个 u32 页号（要 1 MB），所以索引本身必须能「索引自己」——
拆成多个索引块，每个块覆盖 <code>BLOB_FRAMES_PER_INDEX = 1024</code> 帧。</p>
<pre>
读第 N 帧（当前快路径）：
  ① 读对象头页            → 拿到 base_page          （头页常驻缓存）
  ② 页号 = base_page + N  → <b>纯算术，没有第二次索引查找</b>
  ③ 读那一帧的数据页      → 拿到 4076 字节
  <b>热路径页读 = 2 次</b>（头页 + 数据页）≈ 8 KB

读第 N 帧（将来增量写，页不连续时）：
  ① 读对象头页            → 拿到 index_blk[] 表
  ② blk = N / 1024        → 算出该帧落在哪个索引块
  ③ 读 index_blk[blk] 页  → 拿到第 N 帧的真实页号
  ④ 读那一帧的数据页
  热路径页读 = 3 次 ≈ 12 KB
</pre>
<div class="ok"><b>验收数字</b>：读第 3000 帧，页读次数 ≤ 2；1 GB 对象的页读不随帧号增长（IO 量 O(1)）。</div>""")
    + bug("<b>坑 2（容量不是整数倍，末帧的坑）</b>：1 GB 不是 4076 的整数倍 → 最后一帧不满。"
          "如果用「帧数 × 帧大小」当对象长度，末帧的填充 0 会被当成真实数据返回 → "
          "<b>读出来的对象比写入的长</b>。<br>"
          "<b>修法</b>：对象头必须单独存 <code>total_len</code>（真实字节数），"
          "读整个对象按 <code>total_len</code> 截断，读末帧时 <code>*len_out</code> 返回真实长度。"
          "填充 0 不是数据 —— 没有 total_len 就没有办法区分。")
    + bug("<b>坑 3（页号算错，读到别人的数据）</b>：帧号 → 页号一旦算错，"
          "<b>不会报错，会静默读到另一个对象的数据</b>（或者垃圾）。"
          "这类错误在随机测试里极难发现，因为「读到的东西看起来是合法的字节」。<br>"
          "<b>修法</b>：帧数据页里存自己的帧号（页头 flags 或 next 字段的空位），读时校验；"
          "或者退一步：对象头 CRC + 页级 CRC16（已有）能把「结构损坏」和「寻址错」区分开。")
))

B.append(sec(
    "三 · 页管理的三条边界 <span>blob 页和表页是两套世界</span>",
    "",
    tbl(["边界", "规则", "违反的后果"], [
        ['<span class="mono">页类型隔离</span>',
         '<code>PT_BLOB = 0x06</code>，独立页类型；heap 扫描只认 <code>PT_HEAP</code>',
         'heap 扫描撞上 blob 页 → 读到垃圾行'],
        ['<span class="mono">freelist 隔离</span>',
         "blob 页<b>不进任何表的 freelist</b>（表的 freelist 语义是「哪页能放下 N 字节的行」）",
         '<code>freelist_find</code> 把 blob 页当行页用 → <b>写坏数据</b>'],
        ['<span class="mono">catalog 隔离</span>',
         'blob 自管目录页，<b>不进 catalog</b>',
         'blob 依赖关系模态 → 违反「模态之间不许互相 include」的硬约束'],
    ])
    + card("目录页 <span class=\"pill\">blob 的私有元数据</span>", "",
           """<p style="color:var(--tx2);font-size:13.5px">目录页记录所有对象的 id。
它的页号存在 <code>BlobStore</code> 里（内存），<b>重启后靠扫描 blob 页重建</b>。</p>
<pre>
  ┌────────────────────────────────────────┐
  │ 目录页（PT_BLOB，带目录标志）           │
  │  count: u32                            │
  │  ids[]: u64[]   ← 对象 id = 头页页号 + 1 │
  └────────────────────────────────────────┘
</pre>
<div class="ok"><b>对照</b>：v0.5 的 B+tree 索引、v1.5 的时序分区目录，都是同一款先例 ——
<b>模态自管目录，不进 catalog</b>。这条先例在 v1.5 细化时还被用来推翻总览的预判
（总览预判「分区元数据进 catalog」，细化时按 blob 先例改成自管）。</div>""")
    + bug("<b>坑 4（v0.7 顺延的同类坑，这里是它的 blob 版）</b>："
          "「overflow 页不进表 freelist」这条规则在 v0.7 就定过，blob 页是同一类问题的第二例。"
          "<b>凡是「页的语义不是行」的页，都不能进表的 freelist</b> —— "
          "这是可以推广的规则，写进 README 硬规则。")
))

B.append(sec(
    "四 · 事务与恢复 <span>不可变对象 = WAL 零新代码</span>",
    "",
    tbl(["方面", "语义", "理由"], [
        ['<span class="mono">可见性</span>', '<b>不存在</b>（要么存在要么不存在）', '不可变对象没有中间态，没有 MVCC 可言'],
        ['<span class="mono">版本</span>', '无 xmin/xmax，无版本链', 'S3 的 PUT 语义'],
        ['<span class="mono">WAL</span>', '<code>WAL_PAGEDATA</code> 整页镜像（before/after 各 4 KB）',
         '「这一页变成了这样」 —— 正好是 blob 页需要的，重放 memcpy 即可'],
        ['<span class="mono">恢复</span>', '重放镜像 + 对象头校验', '<b>恢复逻辑零改动</b>'],
        ['<span class="mono">删除</span>', '<b>v0.8 只增不减</b>', '空间回收留给 vacuum（v1.2）'],
    ])
    + ok("<b>不可变对象带来的额外好处</b>：每个页只写一次 ⇒ 日志不需要区分"
         "「首次修改记全页 / 后续只记增量」（PG 需要这个区分）⇒ <b>直接用整页镜像，逻辑大幅简化</b>。")
    + bug("<b>坑 5（反向验证时踩过，v0.8 记录在案）</b>："
          "<b>反向验证必须先确认编译通过再跑</b>。"
          "v0.8 做「blob 不写 WAL → 崩溃恢复测试应该变红」的反向验证时，"
          "改完代码直接跑，<b>跑的是旧二进制</b> —— 测试没红，"
          "差点误判成「WAL 不是必需的」。"
          "<b>纪律</b>：反向验证的第二步永远是「确认它<b>应该</b>变红」而不是「确认它变红了」。"
          "先看编译输出，再看测试输出。")
    + ok("<b>验收数字</b>：写 1 MB 对象 → 任意时刻 <code>db_abandon</code> 断电 → 恢复 → "
         "对象要么完整可读（逐字节全等）、要么根本不存在，<b>绝不能是半截数据</b>；20 轮。")
))

B.append(sec(
    "SQL 侧与能力协商 <span>blob 不进 SQL 的谓词系统</span>",
    "",
    tbl(["入口", "内容", "支持", "理由"], [
        ['<span class="mono">db_blob_*</span>', 'create / write / read_frame / read / stat / frame_info',
         '<b>主路径</b>', '大对象是 API 级操作，不是行级操作'],
        ['<span class="mono">blob_lob</span>', '外部值句柄（页号 + 长度）', '<b>支持</b>',
         'v0.9 的外置值机制；blob 的帧页天然是一个外部值'],
        ['<span class="mono">SQL 谓词</span>', 'WHERE col = &lt;blob&gt;', '<b>否决</b>',
         '比较的是字节内容 → 要读全部帧 → 退回 O(位置)'],
        ['<span class="mono">SQL 写入</span>', 'INSERT INTO t VALUES (大对象)', '<b>否决</b>',
         '对象不可变 + 一次写完 → 只能整替换，不能增量写进行'],
    ])
    + callout("blob 与关系模态的边界（必须写清楚，否则将来一定被问）", [
        "<b>blob 不是关系模态的大字段类型</b>，它是并列的第二个存储引擎。"
        "两者共享底座（pager / WAL / 页格式），但各有自己的物理组织 —— 这正是 SPI 存在的意义。",
        "如果哪天需要「一行里放一个大对象」，正确做法是：<b>行里放外部值句柄</b>"
        "（页号 + 长度），对象本体在 blob 侧。这和 v1.6 的大 JSON 外置是同一个手法 —— "
        "<b>外部值句柄 + 惰性物化</b>，本项目的三条贯穿线索之一。",
    ])
    + ok("<b>验收</b>：<code>grep blob src/query/</code> 零命中（执行器零改动）；"
         "blob 与关系模态共存时表数据和对象数据都完好（页不串）；"
         "写操作报明确错误而不是崩溃。")
))

B.append(sec(
    "全部做完后的验收清单 <span>能全过，这个模态就真的能用了</span>",
    "",
    checklist([
        "<b>往返一致</b>：写 1 MB 对象 → 读回逐字节相同（跨 263 帧）",
        "<b>随机访问</b>：读第 0 / 中间 / 最后 / 越界帧都正确；越界报明确错误不返回垃圾",
        "<b>IO 量 O(1)</b>：读第 3000 帧的页读次数 ≤ 2（头页 + 数据页）",
        "<b>末帧正确</b>：不完整对象的 <code>total_len</code> 正确，填充 0 不被当成数据",
        "<b>跨对象隔离</b>：两个对象的数据不串；页类型隔离有效",
        "<b>崩溃恢复</b>：写一半断电 → 恢复后要么完整要么不存在（不能是半截数据），20 轮",
        "<b>WAL 覆盖</b>：写对象产生 <code>WAL_PAGEDATA</code> 记录；<b>反向验证：去掉 WAL → 恢复测试必须变红</b>",
        "<b>目录一致</b>：重启后对象列表与重启前一致（目录靠扫描重建）",
        "<b>执行器零改动</b>：<code>grep blob src/query/</code> 零命中；旧 152 项验收全绿",
        "<b>并发安全</b>：多线程建对象 + 读写不崩、<code>pager_frames_used</code> 稳定（pin 无泄漏）",
    ])
))

B.append(sec(
    "做完之后接什么 <span>三个方向，按你的目标选</span>",
    "",
    """<div class="card"><div class="blk">
<ul>
<li><b>想继续做多模 → 图模态（v0.11）</b>：定长槽页 + 双向邻接链。
blob 的「页类型隔离 / freelist 隔离 / 自管目录」三条边界在图模态全部复用 ——
图模态也自管、也不进 catalog、也要独立页类型。</li>
<li><b>想把 blob 的价值放大 → 外部值机制</b>：v0.9 设计的 <code>ext_fn</code>
（按帧读）目前只有 blob 一个用户。v1.0 向量、v1.6 大 JSON 会各加一个用户 ——
<b>契约一行不改，只换实现</b>。这是「先设计好扩展点，再写第一个使用者」的兑现。</li>
<li><b>想做对象存储的完整语义 → 后续清单</b>：分块上传 / 断点续传 / 删除与空间回收 /
压缩 / 加密 / 范围读 —— v0.8 明确划掉了。这几项都不难，但要等真空闲做完才有意义。</li>
</ul>
</div></div>"""
))

B.append(foot(
    "整体设计见 <code>db/docs/DESIGN.md</code>（§5.1 RID 约定、§7 验收标准）；"
    "版本执行计划见 <code>db/docs/PLAN-v0.8.md</code>（方案判决表 + 五个决定 + 执行记录）；"
    "本规格是字节级施工图，三者冲突时以代码和 PLAN 的执行记录为准。",
    "对象 + 帧模型对标 S3/MinIO 的对象存储与 MongoDB GridFS；"
    "「页连续 ⇒ 帧号纯算术」的推理依据见 PLAN-v0.8 §1.2 的决定 B；"
    "「不可变 ⇒ 每页只写一次 ⇒ 日志无需区分首次/增量」的推理依据见 PLAN-v0.8 §1.2 的决定 E。"
))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "blob模态实现规格.html")
n = write_spec(path,
               "blob 模态实现规格 · 从零到能跑",
               "Build Spec · Blob Modality",
               "blob 模态：<em>1200 行</em>怎么拆",
               "对象 + 定长帧 + 纯算术寻址。这是第一个独立模态的施工图，"
               "核心路径约 1200 行，测试约 520 行。对标 S3/MinIO 的对象存储与 MongoDB GridFS —— "
               "共同机制是「对象 + 帧 + 随机访问」，不是「数据库里的大字段」。",
               "\n".join(B))
print("OK", path, n, "字符")
