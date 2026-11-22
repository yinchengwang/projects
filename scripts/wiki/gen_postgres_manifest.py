# -*- coding: utf-8 -*-
"""生成 postgres 的 332 篇机制页 manifest。

## 为什么用机械生成而不是手写 332 条

332 篇 × 每篇至少 2 个符号 = 至少 664 个符号要写对。手写这条路实测走不通：
第一个尝试在写了 29 条之后卡住，因为「凭记忆写 postgres 内部函数名」的
失败率太高，而未命中的符号会被引用门禁硬拒绝，等于白写。

因此改为：**每篇对应一个真实存在的 `.c` 文件，符号从该文件里真实提取**。
好处是 refs 天然可校验（`--verify` 逐条跑真实源码），代价是标题与 slug
只能按文件名派生——mechanism 的准确中文命名留到 Phase 2 写正文时定。

## 分组

A–K 十一组的分组前缀与篇数来自设计文档的实测枚举，落在 `src/backend/` 下。
每组从该组目录里按源文件体积排序取前 N 个（体积与机制复杂度正相关），
取不满说明目录枚举有问题，直接报错而不是凑数。
"""

import argparse
import io
import os
import re
import sys

from wiki_manifest import dump_manifest

# 分组定义：标签、根目录（相对仓根）、篇数。
# 顺序即 manifest 里的页序；`dirs` 是候选源文件的搜索起点。
GROUP_SPECS = [
    {"group": "A", "label": "A-存储基础", "count": 29,
     "dirs": ["src/backend/storage"]},
    {"group": "B", "label": "B-事务与WAL", "count": 28,
     "dirs": ["src/backend/access/transam", "src/backend/access/rmgrdesc"]},
    {"group": "C", "label": "C-堆表与元组", "count": 35,
     "dirs": ["src/backend/access/heap", "src/backend/access/common",
              "src/backend/access/table", "src/backend/access/sequence",
              "src/backend/access/tablesample", "src/backend/access/rmgrdesc"]},
    {"group": "D", "label": "D-索引族", "count": 33,
     "dirs": ["src/backend/access/nbtree", "src/backend/access/gin",
              "src/backend/access/gist", "src/backend/access/hash",
              "src/backend/access/brin", "src/backend/access/spgist",
              "src/backend/access/index"]},
    {"group": "E", "label": "E-优化器", "count": 39,
     "dirs": ["src/backend/optimizer"]},
    {"group": "F", "label": "F-执行器", "count": 41,
     "dirs": ["src/backend/executor"]},
    {"group": "G", "label": "G-目录与缓存", "count": 28,
     "dirs": ["src/backend/catalog", "src/backend/utils/cache"]},
    {"group": "H", "label": "H-内存与排序", "count": 28,
     "dirs": ["src/backend/utils/mmgr", "src/backend/utils/sort",
              "src/backend/lib", "src/backend/utils/adt"]},
    {"group": "I", "label": "I-复制与高可用", "count": 22,
     "dirs": ["src/backend/replication"]},
    {"group": "J", "label": "J-服务端与命令面", "count": 35,
     "dirs": ["src/backend/commands", "src/backend/postmaster",
              "src/backend/tcop", "src/backend/utils/misc",
              "src/backend/utils/error", "src/backend/utils/activity"]},
    {"group": "K", "label": "K-横切主题", "count": 14,
     "dirs": ["src/backend/parser", "src/backend/nodes", "src/backend/rewrite",
              "src/backend/main", "src/backend/libpq"]},
]

# 列 0 上的函数定义首行：`Name(` 或 `Name (`。postgres 的定义一律顶格。
_DEF_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(")

# 这些不是函数名，是控制结构或本仓惯用的宏。
_NOT_SYMBOLS = frozenset((
    "if", "for", "while", "switch", "return", "sizeof", "typeof", "do", "else",
    "elog", "ereport", "Assert", "AssertMacro", "typedef", "struct", "enum",
    "union", "static", "const", "void", "int", "char", "bool", "defined",
    "case", "goto", "defined_func", "PG_FUNCTION_INFO_V1",
))

# 同行返回类型的类型关键字：`void Foo(void)` 里 `void` 不是函数名。
_TYPE_WORDS = frozenset((
    "void", "int", "char", "bool", "float", "double", "long", "short",
    "unsigned", "signed", "static", "const", "extern", "inline", "struct",
    "union", "enum", "typedef", "volatile", "register", "auto", "size_t",
    "Datum", "bool", "int32", "uint32", "int64", "uint64", "int16", "uint16",
    "Size", "Offset", "Oid", "TransactionId", "TimestampTz", "Pointer",
    "MemoryContext", "List", "Node", "void",
))

# 每个文件取多少个符号。2-3 个足够定位机制，多了反而容易命中不到。
SYMBOLS_PER_PAGE = 3

# 判定「是函数定义」的前瞻行数：签名可能跨行，但 `{` 不会离太远。
_LOOKAHEAD = 12

# 知名文件的中文标题。没收录的按「<文件名> 模块」兜底——
# 精确命名留到 Phase 2 写正文时定，此处只保证唯一与可读。
_TITLES = {
    "bufmgr": "Buffer Pool 主体",
    "freelist": "Clock-Sweep 置换与空闲链",
    "buf_init": "缓冲池初始化",
    "buf_table": "缓冲区哈希表",
    "localbuf": "本地缓冲",
    "freespace": "空闲空间映射 FSM",
    "fsmfunc": "FSM 函数接口",
    "smgr": "存储管理器抽象",
    "md": "段文件与表空间布局",
    "fd": "虚拟文件描述符与文件句柄",
    "buffile": "BufFile 临时文件",
    "ipc": "共享内存初始化",
    "procarray": "进程数组与快照",
    "sinvaladt": "失效消息广播",
    "latch": "Latch 等待机制",
    "waiteventset": "等待事件集",
    "dsm": "动态共享内存",
    "shm_mq": "共享消息队列",
    "shmem": "共享内存段管理",
    "lock": "常规锁系统",
    "lwlock": "轻量锁 LWLock",
    "proc": "PGPROC 与进程槽",
    "deadlock": "死锁检测",
    "predicate": "SSI 可串行化隔离",
    "lmgr": "锁管理器总控",
    "aio": "异步 IO 提交",
    "read_stream": "预读流",
    "sync": "检查点同步",
    "inv_api": "大对象接口",
    "xlog": "WAL 写入",
    "xloginsert": "WAL 记录组装",
    "xlogrecovery": "WAL 重放",
    "twophase": "两阶段提交",
    "clog": "提交日志 CLOG",
    "subtrans": "子事务状态",
    "varsup": "事务 ID 分配",
    "xact": "事务生命周期",
    "heapam": "堆表访问方法",
    "heapam_handler": "堆表 TableAm 回调",
    "hio": "堆表页插入",
    "pruneheap": "堆页剪枝",
    "vacuumlazy": "惰性 vacuum",
    "visibilitymap": "可见性映射",
    "nbtree": "B 树索引主体",
    "nbtinsert": "B 树插入",
    "nbtsearch": "B 树查找",
    "nbtpage": "B 树页面操作",
    "nbtvacuum": "B 树 vacuum",
    "gininsert": "GIN 插入",
    "ginfast": "GIN 快速更新",
    "gistget": "GiST 检索",
    "gistbuild": "GiST 构建",
    "hashpage": "哈希索引页",
    "brin": "BRIN 索引",
    "spginsert": "SP-GiST 插入",
    "plan": "查询计划结构",
    "planner": "优化器总控",
    "path": "访问路径生成",
    "prep": "计划预处理",
    "geqo": "遗传算法优化",
    "costsize": "代价估算",
    "execMain": "执行器主流程",
    "execProcnode": "执行器节点驱动",
    "nodeSeqscan": "顺序扫描节点",
    "nodeIndexscan": "索引扫描节点",
    "nodeHashjoin": "哈希连接节点",
    "nodeAgg": "聚合节点",
    "execExpr": "表达式求值",
    "execParallel": "并行执行协程",
    "catalog": "系统目录",
    "pg_class": "pg_class",
    "syscache": "系统缓存",
    "relcache": "关系缓存",
    "catcache": "目录缓存",
    "mcxt": "MemoryContext 体系",
    "aset": "AllocSet 分配器",
    "generation": "Generation 分配器",
    "slab": "Slab 分配器",
    "tuplesort": "元组排序",
    "nodeSort": "排序节点",
    "walsender": "WAL 发送端",
    "walreceiver": "WAL 接收端",
    "logical": "逻辑复制",
    "worker": "复制 worker",
    "postgres": "后端主循环",
    "postmaster": "postmaster 总控",
    "auxprocess": "辅助进程",
    "bgworker": "后台 worker",
    "disc": "命令分发",
    "utility": "工具命令执行",
    "miscinit": "运行时初始化",
    "guc": "GUC 配置系统",
    "elog": "错误上报",
    "postinit": "后端初始化",
    "parser": "语法解析",
    "scan": "词法扫描",
    "gram": "文法定义",
    "nodeFuncs": "节点树遍历",
}


class ManifestGenError(Exception):
    """源目录不足、符号提取失败等生成期错误。"""


def _read_text(path):
    with open(path, "rb") as stream:
        return stream.read().decode("utf-8", errors="replace")


# 头文件里出现的「名字 + 左括号」，用于判断某个符号是否是**对外**接口。
# 这是本模块挑符号的关键信号：只看「文件里第一个定义的函数」会挑到
# `mul_size` / `_mdfd_open_flags` 这类通用小工具，而它们讲不出机制。
# 实测：bufmgr.c 定义 144 个函数，其中 74 个在头文件里声明；优先取后者，
# 拿到的是 `PrefetchBuffer` / `ReadRecentBuffer` 这种真正代表机制的名字。
_DECL_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def load_declared_names(repo_root):
    """扫 `src/include` 下全部 `.h`，返回头文件里出现过的标识符集合。

    只在生成期跑一次（838 个头文件、约 1.27 万个名字，耗时不到 1 秒）。
    """
    names = set()
    base = os.path.join(repo_root, "src", "include")
    if not os.path.isdir(base):
        return names
    for root, dirs, files in os.walk(base):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            if not name.endswith(".h"):
                continue
            names.update(_DECL_RE.findall(_read_text(os.path.join(root, name))))
    return names


def extract_symbols(path, limit=SYMBOLS_PER_PAGE, declared=None):
    """从源文件里取出前 `limit` 个真实定义的函数名。

    只用一条主判据：**定义首行在第 0 列**。postgres 的函数体内部一律缩进，
    所以这条能一次性排除绝大多数调用点——实测 `lock.c` 里 `mul_size(...)`
    出现在缩进的初始化代码里，正是被这条挡掉的。

    首行形态有两种，都要认：
    * `Name(args)`            —— 返回类型在上一行（postgres 主流写法）
    * `void Name(args)`       —— 返回类型与函数名同行（测试与部分小文件）

    三道补充过滤：
    * `_looks_like_definition`：往后看，先遇 `{` 是定义，先遇 `;` 是声明；
    * `_is_function_name`：排除全大写宏与过短名字；
    * `declared`：给了头文件声明集合时**优先返回在头文件里声明过的**——这条
      决定 refs 指向的是机制还是内部小工具。
    """
    text = _read_text(path)
    lines = text.split("\n")
    found = []
    seen = set()
    for lineno, line in enumerate(lines):
        if not line or line[0] in " \t":
            continue
        if line.startswith(("*", "//", "/*", "#")):
            continue
        name = _definition_name(line)
        if name is None or name in seen:
            continue
        if not _is_function_name(name):
            continue
        if not _looks_like_definition(lines, lineno):
            continue
        seen.add(name)
        found.append(name)

    if declared:
        public = [item for item in found if item in declared]
        if public:
            return public[:limit]
    return found[:limit]


def _definition_name(line):
    """从定义首行取出函数名；取不到返回 None。

    取「第一个既不是类型关键字、也不是控制关键字、且紧跟左括号」的标识符：
    这样 `void Foo(void)` 与 `Foo(const char *x)` 都能正确取到函数名，
    而 `if (x)` / `for (...)` 会被排除。
    """
    for match in _DEF_RE.finditer(line):
        name = match.group(1)
        if name in _TYPE_WORDS or name in _NOT_SYMBOLS:
            continue
        return name
    return None


def _is_function_name(name):
    """函数名不能全大写（那是宏），也不能短到无法区分（单字母/两字母）。"""
    if len(name) < 3:
        return False
    if name.isupper():
        return False
    return True


def _looks_like_definition(lines, start):
    """从 start 行往后看：先遇到 `{` 是定义，先遇到 `;` 是声明或调用。"""
    depth = 0
    for raw in lines[start:start + _LOOKAHEAD]:
        code = _strip_comment_and_strings(raw)
        for ch in code:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            elif ch == ";" and depth <= 0:
                return False
            elif ch == "{" and depth <= 0:
                return True
    return False


def _strip_comment_and_strings(line):
    """去掉行内注释与字符串字面量，避免其中的括号干扰判定。"""
    out = []
    quote = None
    index = 0
    while index < len(line):
        ch = line[index]
        if quote is not None:
            if ch == "\\":
                index += 2
                continue
            if ch == quote:
                quote = None
            index += 1
            continue
        if ch in ('"', "'"):
            quote = ch
            index += 1
            continue
        if ch == "/" and index + 1 < len(line):
            nxt = line[index + 1]
            if nxt == "/":
                break
            if nxt == "*":
                break
        out.append(ch)
        index += 1
    return "".join(out)


def _iter_c_files(repo_root, dirs):
    """目录下的全部 `.c` 文件（递归），返回 (相对路径, 绝对路径, 字节数)。"""
    out = []
    for rel_dir in dirs:
        base = os.path.join(repo_root, rel_dir.replace("/", os.sep))
        if not os.path.isdir(base):
            continue
        for root, subdirs, names in os.walk(base):
            subdirs[:] = sorted(d for d in subdirs if not d.startswith("."))
            for name in sorted(names):
                if not name.endswith(".c"):
                    continue
                full = os.path.join(root, name)
                rel = os.path.relpath(full, repo_root).replace(os.sep, "/")
                out.append((rel, full, os.path.getsize(full)))
    return out


def _slug_for(rel_path):
    """从相对路径推出 slug：文件名去扩展名，`_` 换 `-`。"""
    stem = os.path.splitext(os.path.basename(rel_path))[0]
    return stem.replace("_", "-").lower()


def _unique_slug(slug, rel_path, used):
    """同名文件出现在不同目录时（如 transam/xactdesc.c 与 rmgrdesc/xactdesc.c），
    用父目录名消歧：`xactdesc` → `rmgrdesc-xactdesc`。

    slug 全局唯一是契约要求（`feature` 是页的主键），而 postgres 里同名 `.c`
    跨目录很常见，不加消歧会让 332 条里出现 8 组重复。
    """
    if slug not in used:
        return slug
    parent = os.path.basename(os.path.dirname(rel_path)).replace("_", "-").lower()
    candidate = "%s-%s" % (parent, slug)
    if candidate not in used:
        return candidate
    index = 2
    while "%s-%d" % (candidate, index) in used:
        index += 1
    return "%s-%d" % (candidate, index)


def _title_for(stem):
    return _TITLES.get(stem, "%s 模块" % stem)


def collect_group_pages(repo_root, spec, used_slugs=None, declared=None):
    """为一个组选出 `count` 个源文件，产出页规格。

    选择规则：按文件体积降序（体积与机制复杂度正相关），跳过提取不到符号的文件。
    `used_slugs` 是跨组共享的已用 slug 集合——slug 全局唯一是契约要求，
    同名 `.c` 跨目录出现时用父目录名消歧。
    `declared` 是头文件声明集合，用于优先挑对外接口作为 refs。
    """
    if used_slugs is None:
        used_slugs = set()
    candidates = _iter_c_files(repo_root, spec["dirs"])
    if not candidates:
        raise ManifestGenError(
            "组 %s 在 %s 下没找到任何 .c 文件" % (spec["group"], spec["dirs"])
        )

    ranked = sorted(candidates, key=lambda item: (-item[2], item[0]))
    picked = []
    for rel, full, _size in ranked:
        if len(picked) >= spec["count"]:
            break
        symbols = extract_symbols(full, declared=declared)
        if not symbols:
            continue
        stem = os.path.splitext(os.path.basename(rel))[0]
        slug = _unique_slug(_slug_for(rel), rel, used_slugs)
        used_slugs.add(slug)
        picked.append({"rel": rel, "slug": slug, "stem": stem, "symbols": symbols})

    if len(picked) < spec["count"]:
        raise ManifestGenError(
            "组 %s 只凑出 %d 篇，需要 %d 篇；候选目录需扩充或降低要求"
            % (spec["group"], len(picked), spec["count"])
        )

    pages = []
    for index, item in enumerate(picked, start=1):
        pages.append({
            "feature": item["slug"],
            "feature_group": spec["label"],
            "title": _title_for(item["stem"]),
            "page_file": "%s%02d-%s.md" % (spec["group"].lower(), index, item["slug"]),
            "refs": [{"path": item["rel"], "symbols": item["symbols"]}],
        })
    return pages


def build_manifest(repo_root):
    """按 11 组产出完整 manifest dict。"""
    declared = load_declared_names(repo_root)
    pages = []
    used_slugs = set()
    for spec in GROUP_SPECS:
        pages.extend(collect_group_pages(repo_root, spec, used_slugs, declared))
    return {
        "project": "postgres",
        "schema_version": 1,
        "pages": pages,
    }


def write_manifest(manifest, output_path):
    """写出 manifest，自动建父目录。"""
    parent = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(parent, exist_ok=True)
    with io.open(output_path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(dump_manifest(manifest))


def verify_refs(manifest, repo_root):
    """逐条核对 refs：文件存在且每个符号按词边界命中。返回失败列表。"""
    failures = []
    for page in manifest["pages"]:
        for ref in page["refs"]:
            rel = ref["path"]
            full = os.path.join(repo_root, rel.replace("/", os.sep))
            if not os.path.isfile(full):
                failures.append((page["feature"], rel, "文件不存在"))
                continue
            text = _read_text(full)
            for symbol in ref["symbols"]:
                if not re.search(r"\b" + re.escape(symbol) + r"\b", text):
                    failures.append((page["feature"], rel, "未命中: %s" % symbol))
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description="生成 postgres 332 篇 manifest")
    parser.add_argument("--repo-root", required=True, help="postgres 源码根目录")
    parser.add_argument("--output", help="输出 manifest 路径；给了就写出")
    parser.add_argument("--verify", action="store_true",
                        help="逐条核对 refs 是否命中真实源码")
    args = parser.parse_args(argv)

    manifest = build_manifest(args.repo_root)
    print("-- 产出 %d 页，%d 组" % (len(manifest["pages"]), len(GROUP_SPECS)))

    if args.verify:
        failures = verify_refs(manifest, args.repo_root)
        print("-- refs 校验：失败 %d 条" % len(failures))
        for feature, rel, reason in failures[:20]:
            print("     - %s %s %s" % (feature, rel, reason))
        if failures:
            return 1

    if args.output:
        write_manifest(manifest, args.output)
        print("-- 已写出 %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
