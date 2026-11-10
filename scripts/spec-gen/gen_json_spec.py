# -*- coding: utf-8 -*-
"""生成《文档JSON模态实现规格.html》"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spec_tpl import *

B = []

B.append(sec(
    "和关系模态的根本差异 <span>第七种形态：寄生模态，不建存储</span>",
    "",
    tbl(["维度", "关系模态（heap）", "文档 / JSON 模态", "为什么不同"], [
        ['<span class="mono">位置</span>', '独立的存储引擎', '<b>寄生在 heap 上的列类型</b>',
         'schema-less 文档的行还是普通行'],
        ['<span class="mono">值表示</span>', '定长类型 / 变长 TEXT', '<b>JSONB</b>（长度前缀树形序列化）',
         '解析一次反复用，比裸文本快'],
        ['<span class="mono">结构</span>', '列在 CREATE TABLE 时定死', '<b>文档内结构自由</b>',
         '这是 schema-less 的定义'],
        ['<span class="mono">寻址</span>', 'RID = (page, slot)', '<b>路径表达式</b>（<code>$.a.b[0].c</code>）',
         '文档内的定位方式'],
        ['<span class="mono">索引</span>', 'B+tree 索引列值', '<b>路径索引</b>（提取标量 → 走已有 B+tree）',
         '把「文档内的值」变成可索引的键'],
        ['<span class="mono">事务</span>', 'MVCC 版本链', '<b>继承 MVCC</b>（文档是 heap 行）',
         '整文档替换走 UPDATE 两段式'],
    ])
    + callout("这一版要证明什么：schema-less 的难点不在存储", [
        "前六个模态各有独立存储；文档模态的判决是<b>寄生</b> —— 不建存储，做 heap 上的列类型。"
        "要证明的是：<b>schema-less 的真正难点在值表示与索引</b>。",
        "<b>自描述文档怎么变成可寻址、可索引、可比较的数据</b> —— "
        "JSONB（解析一次反复用）+ 路径索引（把声明过的路径物化成标量键）。",
        "三个判据：① <b>外部值的最终检验</b>（大 JSON 走页链外置 = 外部值机制的第 3 个用户）；"
        "② <b>下推超集契约的闭环</b>（路径索引给候选 → 执行器重新提取路径值再比较）；"
        "③ <b>执行器改动最小化</b>（非零但有硬边界）。",
    ])
))

B.append(sec(
    "模块与行数 <span>按依赖排序，不是按重要性</span>",
    "",
    tbl(["模块", "职责", "行数", "依赖"], [
        ['<span class="mono">json_parse.c</span>', "递归下降 JSON parser（解析成 JSONB）", '<span class="n">~340</span>', "—"],
        ['<span class="mono">jsonb.c</span>', "JSONB 二进制格式的编解码 + 按路径访问", '<span class="n">~380</span>', "json_parse"],
        ['<span class="mono">json_path.c</span>', "路径表达式解析（<code>$.a.b[0].c</code>）+ 标量映射", '<span class="n">~300</span>', "jsonb"],
        ['<span class="mono">json_col.c</span>', "heap 的 JSON 列类型 + 大文档外置页链", '<span class="n">~280</span>', "jsonb, heap"],
        ['<span class="mono">json_index.c</span>', "路径索引：写时同步维护 + <code>json_extract_scalar</code>", '<span class="n">~320</span>', "json_path, btree"],
        ['<span class="mono">spi/json_am.c</span>', "SPI 适配：json_docs 虚拟表 + json_match 求值入口", '<span class="n">~280</span>', "spi"],
        ['<span class="mono">db_json.c</span>', "公开 API（全部持 latch）", '<span class="n">~170</span>', "json_am, latch"],
        ['<span class="mono">测试</span>', "test_json_parse / test_jsonb / test_json_index / test_json_it / test_json_crash", '<span class="n">~880</span>', "—"],
    ])
    + ok("<b>占便宜的部分</b>：<b>不建存储</b>（行还是 heap 的行，事务直接继承 MVCC）；"
         "路径索引直接走已有 B+tree + <code>index_key</code>（<b>零新索引机制</b>）；"
         "WAL 复用 <code>WAL_PAGEDATA</code>。<b>新东西只在「值表示」和「路径求值」上</b>。")
))

B.append(sec(
    "六步实现顺序 <span>每步都有可量测的验收，不通过不进下一步</span>",
    "",
    steps([
        ("JSON parser + JSONB 编解码 <span class=\"pill\">纯内存</span>",
         "递归下降解析 → JSONB 二进制 → 按路径取值。"
         "验收：<b>对照 SQLite JSON1 逐条全等</b>（转义、Unicode 代理对、嵌套、数字精度、重复键保留首个）。",
         "~720"),
        ("路径表达式 + 标量映射",
         "<code>$.a.b[0].c</code> 解析 + <code>json_extract_scalar</code> 映射到通用标量。"
         "验收：映射规则表全覆盖；<b>索引键与执行器重判用同一个函数</b>（不允许分叉）。",
         "~300"),
        ("heap 列类型 + 大文档外置",
         "小文档内联进 heap 行；大文档走 <code>PT_JSON_OVF</code> 页链（外部值）。"
         "验收：<b>只取路径字段时不读全文</b>（页读计数对比 —— v0.9 IO 断言模式的第 3 次应用）。",
         "~280"),
        ("路径索引（写时维护）",
         "<code>CREATE INDEX ON t (json '$.a.b')</code>；文档更新时删旧键、插新键。"
         "验收：更新后<b>旧键不残留</b>（索引项计数对账）、新键可命中。",
         "~320"),
        ("执行器接线（最小化）+ json_match",
         "<code>json_match(doc, '$.a.b = 5')</code> 求值：有索引走索引（候选集），执行器重判。"
         "验收：<b>走索引 vs 无索引全扫，结果集全等</b>；执行器改动<b>先定义后审计</b>。",
         "~340"),
        ("MVCC + 崩溃 + 反向验证 + 文档收官",
         "验收：并发读 + 文档替换，RR 快照下文档不撕裂；崩溃后页链 + 路径索引（reindex）一致。",
         "~400"),
    ])
    + callout("关键提醒", [
        "<b>SQLite JSON1 当语法裁判</b>：JSON 的语法边角极多（转义、代理对、"
        "大整数精度、<code>1e999</code>）。<b>不要凭记忆写 parser</b> —— "
        "用 SQLite 的 JSON1 做逐条对照，差异就是学习点。"
        "<b>「抄业界实现，不抄教科书」这条方针在这里同样成立。</b>",
        "<b>索引键与执行器重判必须共用同一个函数</b>："
        "如果索引用 <code>json_extract_scalar</code> 建键，而执行器用另一个函数重判，"
        "两边语义一旦分叉 → <b>超集契约当场失效</b> → 静默漏数据。"
        "<b>这是本版最核心的结构性约束</b>，A3 的全等断言就是它的守卫。",
    ])
))

# 判决
B.append(sec(
    "一 · 四个关键判决 <span>先把边界划死，再写代码</span>",
    "",
    card("判决 1（本版核心）：路径提取值映射为通用标量 —— 超集契约的闭环 <span class=\"pill p\">全版最重要的一条</span>", "",
         """<p style="color:var(--tx2);font-size:13.5px"><b>问题</b>：
下推契约（v0.9）要求「AM 给候选行，执行器重新求值 WHERE」。
但 WHERE 里的 <code>json_match(doc, '$.a.b = 5')</code> 要求执行器能<b>独立</b>判断文档。</p>
<pre>
<b>映射规则</b>：

  JSONB 整体      → <b>不进通用类型系统</b>（blob 同款：字节串 / 外部值）

  路径提取值      → <b>映射为通用标量</b>：
      string        → DbValue(TEXT)
      number        → DbValue(INT 或 REAL)
      true / false  → DbValue(INT 1 / 0)
      null / 路径不存在 / 类型不匹配 → DbValue(NULL)
      object / array（非标量路径）    → NULL（语义写死）
</pre>
<div class="callout">
<h3>于是超集契约完整成立</h3>
<p><b>索引按提取值建的键，执行器用同一套提取规则重判</b> ——
两边的映射函数是<b>同一个</b>（<code>json_extract_scalar</code>），<b>不可能语义分叉</b>。</p>
<p>v1.3 §1.2 说过「相关性评分执行器无法重求值」（所以 <code>match</code> 不进 SQL）——
<b>JSON 路径值可以</b>，所以 <code>json_match</code> 进 SQL 是合法的。</p>
<p><b>一进一不进，判据是同一条：执行器能不能独立重判。</b></p>
</div>""")
    + card("判决 2（修正总览预判）：路径索引写时同步维护，不存在「快照过期」 <span class=\"pill p\">与总览不一致的地方</span>", "",
           """<table>
<thead><tr><th>路径</th><th>分析</th></tr></thead>
<tbody>
<tr><td>快照过期 + 重判兜底</td>
    <td>过期意味着<b>索引里躺着死键</b>：没人删它，越积越多 ——
    那不是「保守的超集」，是<b>泄漏</b></td></tr>
<tr><td><b>写时同步维护</b>（采纳）</td>
    <td>文档更新 = 整替换 → 写路径删旧键、插新键 ——
    <b>v0.5 的写路径维护索引原样适用</b>，B+tree 既有能力，<b>零新机制</b></td></tr>
</tbody></table>
<div class="ok"><b>执行器重判仍然保留</b> —— 它是超集契约的一半，"
"防的是<b>索引实现 bug</b>，而不是「过期快照」这个不存在的问题。"
"<b>修正记录进执行记录。</b></div>""")
    + card("判决 3：JSONB 格式 <span class=\"pill\">长度前缀树形序列化</span>", "",
           """<pre>
JSONB = 长度前缀树形序列化（PG JSONB / MongoDB BSON 同思路）：

  [tag][payload]：
    tag ∈ { null, true, false, int32, float64, string, array, object }
    string  ：[len][utf-8 bytes]
    array   ：[count][element]×count              （顺序即语义）
    object  ：[count][[klen][key][element]]×count  （<b>键按长度+字典序存放</b> → 二分可查）

  小文档（≤ 行内限）→ 内联 heap 行的列值
  大文档            → 外部值：PT_JSON_OVF 页链（ext_key = 首页号，ext_fn 沿链读）
</pre>
<div class="ok"><b>键排序是 JSONB 相对裸文本的真实收益之一</b>：
对象键二分查找，路径 <code>$.a.b</code> 的访问是「两次二分」，<b>不是全文扫描</b>。</div>""")
    + card("判决 4：事务语义 —— 三足鼎立的收官 <span class=\"pill p\">整个项目的账在这一版结清</span>", "",
           """<table>
<thead><tr><th>模态</th><th>事务语义</th><th>出处</th></tr></thead>
<tbody>
<tr><td>heap（+ 本版 JSON 列）</td><td><b>MVCC 版本链</b>（RC / RR）</td><td>v0.7</td></tr>
<tr><td>blob</td><td><b>不可变对象</b></td><td>v0.8</td></tr>
<tr><td>graph / vector / ft / ts</td><td><b>锁 + WAL</b>（语句级）</td><td>v0.10 – v1.5</td></tr>
</tbody></table>
<p style="color:var(--tx2);font-size:13.5px">文档是 heap 行 → <b>继承 MVCC</b>：
整文档替换走 UPDATE 两段式（旧版本打 xmax + 插新版本），
RR 快照下读到的文档<b>不撕裂</b>。</p>
<div class="ok"><b>三种语义到齐 —— 这张表是 DESIGN 复盘的素材</b>。
三个模态族各有其道理：<b>关系型需要 MVCC</b>（多版本共存）；
<b>对象存储天然不可变</b>（PUT 语义）；
<b>图/向量/全文/时序的邻接结构与全局索引无法版本化</b>（Neo4j 做了同样的选择）。</div>""")
))

# JSONB 布局
layout = card(
    "JSONB 字节布局 <span class=\"pill p\">tag + payload</span>",
    "",
    """<pre>
<b>标量</b>：
  null      : [0x01]
  true      : [0x02]      false : [0x03]
  int32     : [0x04][4B little-endian]
  float64   : [0x05][8B IEEE754]
  string    : [0x06][len:u32][utf-8 bytes]

<b>数组</b>：
  [0x07][count:u32][element]×count        ← <b>顺序即语义</b>

<b>对象</b>：
  [0x08][count:u32][[klen:u32][key][element]]×count
                     ↑ <b>键按长度+字典序存放 → 可二分查找</b>

<b>空对象的 count = 0</b>，<b>空数组的 count = 0</b> —— 靠 tag 区分两者
</pre>
"""
    + tbl(["项", "值", "说明"], [
        ['<span class="mono">最小文档</span>', '<b>5 B</b>', '{"a":1} → 1+4+1+1+4+1+1+4 = 约 17 B'],
        ['<span class="mono">tag 大小</span>', '<b>1 B</b>', '8 种类型够用（int64 可拆成 int32 + 变长）'],
        ['<span class="mono">键排序</span>', '<b>长度优先，字典序次之</b>', '「长度+字典序」→ 定长前缀可比 → 二分'],
        ['<span class="mono">重复键</span>', '<b>保留首个</b>', '语义写死，解析器和 JSONB 二分行为一致'],
    ])
)
layout += card(
    "路径访问 <span class=\"pill\">两次二分，不是全文扫描</span>",
    "",
    """<pre>
路径： <b>$.a.b</b>

步骤：
  ① 解析路径表达式 → 路径段列表 [ "a", "b" ]
  ② 在根对象里二分查找 "a"        ← 键已排序 → O(log n)
  ③ 在 a 的值里二分查找 "b"        ← 键已排序 → O(log n)
  ④ 把结果映射为通用标量（json_extract_scalar）

<b>对比裸文本 JSON</b>：
  要在整个文档里找 "a" 这个键 → 全文扫描 → O(文档大小)
  <b>路径访问是 O(段数 × log(键数))</b> —— 与文档大小基本无关
</pre>
<div class="ok"><b>这是 JSONB 相对 TEXT 的核心收益</b>，也是「解析一次反复用」这句话的具体含义。</div>""")
layout += card(
    "大文档外置（页链） <span class=\"pill p\">PT_JSON_OVF</span>",
    "",
    """<pre>
heap 行里的列值：
  ┌────────────────────────────────┬──────────────────────┐
  │ 列值 = 内联 JSONB（小文档）      │ 或 列值 = 外部值句柄    │
  │   [tag][payload...]             │   ext_key = 首页号     │
  │                                │   ext_len = 总字节数    │
  └────────────────────────────────┴──────────────────────┘
                                            ↓
                              PT_JSON_OVF 页链（每页一份数据）
                              第一页 → next 页 → … → 末页
</pre>
<div class="ok"><b>外部值机制的第 3 个用户</b>：
v0.9 设计了 <code>ext_fn</code>（按帧读），blob 是第 1 个用户；
向量页是第 2 个（列式页天然可以当外部值）；<b>大 JSON 是第 3 个</b>。
<b>契约一行不改，只换实现</b> —— 这是 v0.9 设计 foresight 的收官考试。</div>
<div class="bug"><b>坑 1（页链断裂 → 读到半个文档）</b>：外置文档跨多页，"
"崩溃可能发生在「写完第 1 页、写第 2 页时断电」→ 恢复后页链指向不存在的页。"
"<b>修法</b>：页链每页都写 <code>WAL_PAGEDATA</code> 整页镜像（next 指针在页里）；"
"读时校验 next 页号在文件范围内；<b>A6 崩溃恢复专项覆盖</b>。</div>""")
B.append(sec("二 · JSONB 的字节布局与路径访问 <span>页大小 4096，列内限可配</span>", "", layout))

# 路径索引
B.append(sec(
    "三 · 路径索引：写时同步维护 <span>零新索引机制</span>",
    "",
    """<pre>
<b>建索引</b>：CREATE INDEX ON t (json '$.a.b')

<b>写入路径（UPDATE / INSERT）</b>：
  ① 旧版本若有该路径的键 → <b>删旧键</b>
  ② 用新文档的 <code>json_extract_scalar(doc, '$.a.b')</code> → <b>插新键</b>
  ③ 键的编码走 <b>已有的 <code>index_key</code></b>（列值 → 一维可比字节串）

<b>查询路径</b>：
  ① AM 的 push 收到谓词 → 提取路径 → 映射为标量 → 查 B+tree → <b>得到候选行集合</b>
  ② 执行器拿到候选行 → <b>重新执行 json_extract_scalar → 比较</b>（超集契约的另一半）
</pre>
"""
    + card("为什么零新机制", "",
           """<p style="color:var(--tx2);font-size:13.5px">
路径索引最终落到的是<b>普通 B+tree 索引</b>，键是「提取出来的标量」。
所以它<b>复用了 v0.5 的全部索引能力</b>：分裂、合并、页分裂、WAL、崩溃恢复。</p>
<div class="ok"><b>本版真正的新东西只有一处</b>：
<b>怎么把「文档里的某个路径」变成「一个可以进 index_key 的标量」</b> —— "
"也就是 <code>json_extract_scalar</code> 这一个函数。<br>"
"<b>而这一个函数必须同时被两处调用</b>：索引建键时、执行器重判时。"
"<b>这是全版最重要的结构性约束。</b></div>""")
    + bug("<b>坑 2（两处映射语义分叉 → 超集契约失效 → 静默漏数据）</b>："
          "索引侧写 <code>if (v &gt; 0) 记为 INT else 记为 TEXT</code>，"
          "执行器侧写 <code>if (v &gt; 1) 记为 INT else 记为 TEXT</code> —— "
          "<b>边界值 1 会被索引成 TEXT、执行器判成 INT</b> → 该行永远不被返回。"
          "<b>症状：查询少了一条数据，但没有任何报错</b>。<br>"
          "<b>修法</b>：<b>物理上只允许一份实现</b>（同一个头文件里的同一个函数），"
          "并且 A3 的「走索引 vs 全扫全等」断言作为守卫。"
          "<b>这不是「小心一点」能解决的，必须靠结构。</b>")
    + bug("<b>坑 3（索引项泄漏 → 索引越用越大）</b>：文档更新时漏删旧键 → "
          "<b>索引里躺着死键</b>，越积越多 —— 这正是判决 2 推翻「快照过期」方案的原因。"
          "<b>修法</b>：写路径的索引维护照抄 v0.5 的「UPDATE 时删旧键 + 插新键」，"
          "并加一条<b>对账断言</b>：<code>索引项数 == 活跃文档中该路径有值的行数</code>。")
    + ok("<b>验收数字</b>：更新文档后，<b>索引项计数对账通过</b>（无残留）；"
         "新键可命中；走索引与全扫<b>结果集全等</b>。")
))

# 执行器改动
B.append(sec(
    "四 · 执行器改动审计 <span>本版唯一必然碰执行器的模态</span>",
    "",
    """<table>
<thead><tr><th>允许的落点</th><th>内容</th><th>为什么允许</th></tr></thead>
<tbody>
<tr><td><b>① 通用值层的路径变体</b></td>
    <td><code>DbValue</code> 增加「从 JSONB 按路径取标量」的入口</td>
    <td>这是<b>值层的通用能力</b>，不是「JSON 模态」的概念</td></tr>
<tr><td><b>② <code>json_match</code> 的求值入口</b></td>
    <td>谓词求值时调用通用值层的入口</td>
    <td>求值入口是<b>契约的一部分</b>，不是模态特判</td></tr>
</tbody></table>
"""
    + callout("零容忍：模态名硬编码", [
        "<b>验收 A7 的核心禁止项</b>：<code>grep -i json src/query/</code> 的命中"
        "<b>必须限于</b>上面两个落点，"
        "<b>不允许出现任何形如 <code>if (strcmp(col_type, \"json\"))</code> 的模态名硬编码</b>。",
        "<b>为什么这条要零容忍</b>：因为一旦执行器里出现「如果列类型是 json 就走这套逻辑」，"
        "<b>下一个模态就会要求再开一个分支</b>（比如 geo、ts、vector）—— "
        "执行器会变成一堆模态特判的集合，同构性原则彻底破产。",
        "<b>纪律：改动清单先定义后实现。</b>动手之前先把「允许落在哪两处」写死，"
        "实现完逐行审计。<b>事后审计容易自我宽容，事前定义才有约束力。</b>",
    ])
    + ok("<b>验收</b>：<code>grep -i json src/query/</code> 命中数 ≤ 2 处且都在允许落点内；"
         "改动清单逐行列进执行记录；旧 152+ 项验收全绿。")
))

# 事务
B.append(sec(
    "五 · 事务与恢复 <span>继承 MVCC + 外部值的第 3 个用户</span>",
    "",
    tbl(["方面", "语义", "说明"], [
        ['<span class="mono">可见性</span>', '<b>继承 heap 的 MVCC</b>（xmin/xmax + 快照）',
         '文档是 heap 行 → 版本链直接生效'],
        ['<span class="mono">整文档替换</span>', 'UPDATE 两段式：旧版本打 xmax + 插新版本',
         'RR 快照下读到的文档<b>不撕裂</b>'],
        ['<span class="mono">路径索引</span>', '写时同步维护（删旧键 + 插新键）',
         '与 <code>index_key</code> 共用编码'],
        ['<span class="mono">大文档</span>', '外部值：<code>PT_JSON_OVF</code> 页链',
         '<b>外部值机制第 3 个用户</b>'],
        ['<span class="mono">WAL</span>', 'heap 行整页镜像 + 页链每页镜像', '复用 <code>WAL_PAGEDATA</code>'],
        ['<span class="mono">恢复</span>', '页链重放 + 路径索引 <b>reindex</b>',
         '索引不一致时走重建路径'],
    ])
    + ok("<b>验收数字</b>：并发读 + 文档替换，<b>RR 快照下文档不撕裂</b>"
         "（复用 test_mvcc 的场景骨架）；"
         "大文档（&gt; 页大小）走页链外置，<b>只取路径字段时不读全文</b>（页读计数对比）；"
         "崩溃后文档页链 + 路径索引恢复一致。")
))

# SQL 侧
B.append(sec(
    "六 · SQL 侧与能力协商 <span>json_match 进 SQL（与全文检索对照）</span>",
    "",
    tbl(["入口", "内容", "支持", "理由"], [
        ['<span class="mono">json_match(doc, \'$.a.b = 5\')</span>', "谓词求值：走路径索引 + 执行器重判",
         '<b>支持</b>', '<b>路径值映射成通用标量 → 执行器能独立重判</b>'],
        ['<span class="mono">db_json_extract</span>', "按路径取标量", '<b>提供</b>', "API 级路径访问"],
        ['<span class="mono">json_docs</span>', "虚拟表（文档元数据 + 路径值列）", '<b>SELECT</b>',
         "标量列下推"],
        ['<span class="mono">JSONB 整体进类型系统</span>', "把 JSONB 当成一等列类型参与比较", '<b>否决</b>',
         '结构不可比；blob 同款（字节串 / 外部值）'],
        ['<span class="mono">$set 部分更新</span>', "文档内局部更新", '<b>否决</b>',
         '本版只做整文档替换（MongoDB 的文档级替换同款起点）'],
        ['<span class="mono">多值索引</span>', "数组展开成多键", '<b>否决</b>',
         '数组路径提取<b>返回 NULL</b>（语义写死）'],
        ['<span class="mono">写操作</span>', "INSERT / UPDATE / DELETE on json_docs", '<b>否决</b>',
         '本版只读；写入走专用 API → E_NOTSUP'],
    ])
    + callout("和全文检索（v1.3）的正面对照", [
        "<b>v1.3 判决</b>：<code>match(col, 'term')</code> <b>不进</b> SQL 谓词 —— "
        "相关性评分依赖全局统计（idf / avgdl），<b>执行器无法独立重求值</b>。",
        "<b>v1.6 判决</b>：<code>json_match(doc, '$.a.b = 5')</code> <b>进</b> SQL 谓词 —— "
        "路径提取值<b>映射成了通用标量</b>，执行器可以独立重判。",
        "<b>一进一不进，判据是同一条：执行器能不能独立重判。</b>"
        "<b>这个对照是整个项目「下推边界」问题的最终答案</b> —— "
        "写在 DESIGN 复盘里，作为八模态收官的判据总结。",
    ])
    + ok("<b>验收</b>：走索引 vs 无索引全扫<b>结果集全等</b>；"
         "数组路径提取返回 NULL（不是首元素）；路径不存在返回 NULL；"
         "类型不匹配返回 NULL（不报错）。")
))

# 验收清单
B.append(sec(
    "全部做完后的验收清单 <span>能全过，这个模态就真的能用了</span>",
    "",
    checklist([
        "<b>JSONB 编解码</b>：parse → binary → 按路径取值，<b>对照 SQLite JSON1 逐条全等</b>"
        "（转义、Unicode 代理对、嵌套、数字精度、重复键保留首个）",
        "<b>外部值收官</b>：大文档（&gt; 页大小）走页链外置；<b>只取路径字段时不读全文</b>（页读计数对比）",
        "<b>json_match 两路全等</b>：同一谓词，走路径索引 vs 无索引全扫过滤，<b>结果集全等</b>",
        "<b>索引维护</b>：文档更新后旧键不残留（索引项计数对账）、新键可命中",
        "<b>MVCC</b>：并发读 + 文档替换，RR 快照下文档不撕裂",
        "<b>崩溃恢复</b>：文档页链 + 路径索引（reindex）恢复后一致",
        "<b>执行器改动审计</b>：<code>grep -i json src/query/</code> 命中限于两个允许落点，"
        "<b>无模态名硬编码</b>（<code>if (strcmp(col_type, \"json\"))</code> 这类零容忍）",
        "<b>语义写死</b>：数组路径 → NULL；路径不存在 → NULL；类型不匹配 → NULL（都不报错）",
        "<b>旧验收全绿</b>；依赖方向正确；新文件 &lt; 800 行",
    ])
))

# 后续
B.append(sec(
    "做完之后接什么 <span>收官复盘 + 三个方向</span>",
    "",
    """<div class="card"><div class="blk">
<ul>
<li><b>先做收官复盘（DESIGN §11 实现顺序）</b>：把八模态的账结清 ——
<b>事务语义三足鼎立</b>（MVCC / 不可变 / 锁+WAL）、
<b>三条贯穿线索</b>（不变文件+后台合并 × 4、外部值句柄+惰性物化 × 3、下推只能给超集 × 7）、
<b>下推边界判据</b>（执行器能不能独立重判）。</li>
<li><b>想做内核收尾 → v1.2</b>：copy-out 重构、细粒度 latch、页内 compact、checkpoint + 日志截断。
<b>v1.2 是让前六个模态的并发模型真正立住的一步</b> —— 现在只有一把全局锁。</li>
<li><b>想做向量化的执行层 → v1.1</b>：批式执行 + 规则 CBO + EXPLAIN。
<b>八个模态的数据都在了，缺的是让执行器高效地用它们</b>。</li>
<li><b>文档模态的进阶（不排期池）</b>：JSON Schema 校验、JSON 聚合 / 生成函数、
MongoDB 查询语法族、键字典 / 重复键压缩、位置更新。</li>
</ul>
</div></div>"""
))

B.append(foot(
    "整体设计见 <code>db/docs/DESIGN.md</code>（§5.1 RID 约定、§5.3 同构性原则、§7 验收标准、§11 实现顺序）；"
    "版本执行计划见 <code>db/docs/PLAN-v1.6.md</code>（四个关键判决 + 8 项验收 + 风险清单）；"
    "八模态盘点与三条贯穿线索见 <code>db/docs/PLAN-modality-expansion.md</code>；"
    "本规格是字节级施工图，三者冲突时以代码和 PLAN 的执行记录为准。",
    "JSONB 的 tag + payload 长度前缀树形格式对标 PostgreSQL 的 jsonb 内部格式"
    "（<code>jsonb.c</code>，键按长度+字典序排序以支持二分）与 MongoDB 的 BSON；"
    "JSON 语法与边界语义以 SQLite 的 JSON1 函数为裁判基准（<code>json_extract</code> / "
    "<code>json_type</code> / <code>json_valid</code>）；"
    "外部值句柄 + 惰性物化的机制出自 v0.9 设计的 SPI 外置值契约。"
))

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "文档JSON模态实现规格.html")
n = write_spec(path,
               "文档 JSON 模态实现规格 · 从零到能跑",
               "Build Spec · Document / JSON Modality",
               "文档 JSON 模态：<em>2200 行</em>怎么拆",
               "JSONB + 路径索引 + 超集契约闭环。这是第八个、也是最后一个独立模态的施工图，"
               "核心路径约 2200 行，测试约 880 行。对标 PostgreSQL 的 jsonb 与 MongoDB 的 BSON —— "
               "核心判据：路径提取值映射成通用标量，让下推超集契约真正闭环。",
               "\n".join(B))
print("OK", path, n, "字符")
