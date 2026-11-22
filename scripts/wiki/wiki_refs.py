# -*- coding: utf-8 -*-
"""引用门禁：refs 里的 path 必须存在，symbols 必须能在该文件内出现。

硬失败，无豁免（设计文档 §5.2）。宏与内联函数的正确做法是把 refs 指向
其真实定义文件（通常是 .h），而不是给校验加白名单。

--locate 是硬失败的反向工具：在本地浅克隆里全仓查符号，返回真实文件与行号，
让写作者先定位再填 refs，把门禁从惩罚变成反馈（设计文档 §5.3）。

源码一律按行流式读取，且先做大小上限与二进制探测：真实上游仓里有大量
生成文件与二进制产物，整块读入内存会把进程拖死；含 NUL 字节的文件按
二进制跳过，不做符号匹配。NUL 探测覆盖整个文件，不只看头部窗口。

refs.path 必须是仓内相对路径：解析成 canonical 绝对路径后仍须严格落在
repo_root 之内，`../x`、仓外绝对路径、指向仓外的链接一律硬失败。
"""
import argparse
import os
import re
import sys

from wiki_frontmatter import FrontmatterError, parse_frontmatter

# locate 默认扫描的源码扩展名。宽松一些，宁多勿漏。
CODE_EXTS = (
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh",
    ".go", ".rs", ".java", ".kt", ".scala", ".py", ".ts", ".pyx",
    ".sql", ".lua", ".rb", ".php", ".m", ".mm", ".swift",
)

# 全仓遍历时跳过的目录：这些内容没有引用价值，且体量大。
SKIP_DIRS = (
    ".git", "build", "node_modules", "target", "vendor", "third_party",
    "thirdpart", "deps", "dist",
)

# 单个文件的大小上限。超过即视为「生成/打包产物」，不参与符号匹配：
# 再大的源码也不该让一次门禁跑出分钟级耗时。
MAX_FILE_SIZE = 4 * 1024 * 1024
# 二进制探测窗口：头部用于快速拒绝；扫描期间仍需检测后续 NUL。
_BINARY_PROBE = 8192
_BOM = b"\xef\xbb\xbf"
_BINARY_REASON = "含 NUL 字节（二进制）"


class _BinarySourceError(Exception):
    """扫描过程中遇到 NUL 字节：该文件是二进制，不参与符号校验。"""


def _scan_problem(path):
    """返回不能当源码扫的原因，可扫则返回 None。

    先查大小再读头部，避免对超大文件做任何整块读取；这里的头部探测只是
    快速拒绝，真正的 NUL 检测发生在逐行扫描阶段（_iter_text_lines）。
    """
    try:
        if os.path.getsize(path) > MAX_FILE_SIZE:
            return "文件超过 %d 字节" % MAX_FILE_SIZE
        with open(path, "rb") as stream:
            probe = stream.read(_BINARY_PROBE)
    except OSError as error:
        return "无法读取: %s" % error
    if b"\x00" in probe:
        return _BINARY_REASON
    return None


def _iter_text_lines(path):
    """按行流式解码源码，内存只与单行长度相关。

    解码用 errors="replace"：真实上游仓里混合编码是常态（latin-1、GBK 注释
    都出现过），一个坏字节不该让整篇文档的引用校验崩掉。
    首行剥掉 UTF-8 BOM，避免 BOM 影响首个符号的词边界判断。
    逐行检测 NUL：只看头部窗口会把「NUL 在窗口之后」的二进制当源码。
    """
    with open(path, "rb") as stream:
        for lineno, raw in enumerate(stream, 1):
            if b"\x00" in raw:
                raise _BinarySourceError(_BINARY_REASON)
            if lineno == 1 and raw.startswith(_BOM):
                raw = raw[len(_BOM):]
            yield lineno, raw.decode("utf-8", errors="replace").rstrip("\r\n")


def _symbol_pattern(symbol):
    """符号按词边界匹配：否则 PinBuffer 会命中 PinBufferX。"""
    return re.compile(r"\b" + re.escape(symbol) + r"\b")


def _scan_symbols(path, names):
    """单遍扫描全部声明符号，返回 ({符号: (行号, 行文本)}, None)。

    扫描必须走完整个文件：命中之后若还读到 NUL，说明这是二进制文件，
    刚才的命中不算数。任一阶段读到 NUL 就返回 (None, 原因)。
    """
    patterns = [(name, _symbol_pattern(name)) for name in names]
    hits = {}
    try:
        for lineno, line in _iter_text_lines(path):
            for name, pattern in patterns:
                if name not in hits and pattern.search(line):
                    hits[name] = (lineno, line.strip())
    except _BinarySourceError as error:
        return None, str(error)
    except OSError as error:
        return None, "无法读取: %s" % error
    return hits, None


def _resolve_ref_path(repo_root, rel):
    """规范化 refs.path；仓根之外的路径（含逃逸 symlink）返回 None。

    分隔符先统一成本地形式，再求 canonical 绝对路径（realpath 会把
    symlink 解析到真实位置）。只有解析结果严格落在 canonical repo_root
    之内才放行：`../outside.c`、仓外绝对路径、指向仓外的 symlink 全部拒绝。
    """
    repo = os.path.realpath(os.path.abspath(repo_root))
    normalized = rel.replace("\\", os.sep).replace("/", os.sep)
    candidate = os.path.realpath(os.path.abspath(os.path.join(repo, normalized)))
    try:
        common = os.path.commonpath((repo, candidate))
    except ValueError:
        # Windows 不同盘符时 commonpath 会失败，视为仓外。
        return None
    # normcase 让 Windows 的大小写差异不误判（realpath 已给 canonical 形态）。
    if os.path.normcase(common) != os.path.normcase(repo):
        return None
    if os.path.normcase(candidate) == os.path.normcase(repo):
        # refs.path 指向仓根本身：是目录，不是源码文件。
        return None
    return candidate


def check_refs(page_path: str, repo_root: str) -> list[str]:
    """返回错误列表，空列表表示通过。repo_root 必须是该项目源码的本地根。"""
    with open(page_path, encoding="utf-8") as stream:
        text = stream.read()
    try:
        meta, _ = parse_frontmatter(text)
    except FrontmatterError as error:
        return ["front-matter 解析失败: %s" % error]

    refs = meta.get("refs")
    if not isinstance(refs, list):
        return ["refs 缺失或不是列表"]

    errors = []
    for index, item in enumerate(refs):
        if not isinstance(item, dict):
            errors.append("refs[%d] 不是映射" % index)
            continue
        rel = item.get("path")
        if not isinstance(rel, str) or not rel.strip():
            errors.append("refs[%d].path 缺失" % index)
            continue
        abs_path = _resolve_ref_path(repo_root, rel)
        if abs_path is None:
            errors.append("refs[%d].path 超出仓库根目录: %s" % (index, rel))
            continue
        if not os.path.isfile(abs_path):
            errors.append("refs[%d].path 不存在: %s" % (index, rel))
            continue
        symbols = item.get("symbols")
        if not isinstance(symbols, list) or not symbols:
            errors.append("refs[%d].symbols 必须是非空列表" % index)
            continue
        problem = _scan_problem(abs_path)
        if problem is not None:
            # 无法校验就等于校验不过：硬失败，不给「扫不了就放行」的口子。
            errors.append("refs[%d].path 无法校验（%s）: %s" % (index, problem, rel))
            continue
        # 符号只在本条 ref 声明的这个文件里查，绝不跨文件串用。
        names = []
        for symbol in symbols:
            if not isinstance(symbol, str) or not symbol.strip():
                errors.append("refs[%d] 含非法符号 %r" % (index, symbol))
                continue
            names.append(symbol.strip())
        if not names:
            continue
        hits, binary = _scan_symbols(abs_path, names)
        if binary is not None:
            # 头部窗口没看到 NUL，但扫描期间发现了：同样硬失败。
            errors.append("refs[%d].path 无法校验（%s）: %s" % (index, binary, rel))
            continue
        for name in names:
            if name not in hits:
                errors.append("refs[%d].symbol 未命中: %s (%s)" % (index, name, rel))
    return errors


def locate_symbol(repo_root: str, symbol: str, exts=None, max_hits=20) -> list[tuple[str, int, str]]:
    """返回 [(相对路径, 首个命中行号, 该行文本)]，最多 max_hits 条。

    命中数到达上限立即返回，不继续遍历后面的文件：大仓里热门符号会有
    上万处命中，扫完等于白跑。
    """
    if not isinstance(symbol, str) or symbol == "":
        return []
    if max_hits <= 0:
        return []
    extensions = CODE_EXTS if exts is None else exts
    if isinstance(extensions, str):
        extensions = (extensions,)

    hits = []
    for dirpath, dirnames, filenames in os.walk(repo_root):
        # .git 与第三方 vendored 代码没有引用价值；隐藏目录一并跳过。
        dirnames[:] = sorted(
            name for name in dirnames
            if name not in SKIP_DIRS and not name.startswith(".")
        )
        for filename in sorted(filenames):
            if not filename.endswith(tuple(extensions)):
                continue
            abs_path = os.path.join(dirpath, filename)
            if _scan_problem(abs_path) is not None:
                continue
            founds, binary = _scan_symbols(abs_path, (symbol,))
            if binary is not None or founds is None:
                # 头部没有 NUL 但扫描期间发现的二进制，同样不能算源码命中。
                continue
            found = founds.get(symbol)
            if found is None:
                continue
            rel = os.path.relpath(abs_path, repo_root).replace(os.sep, "/")
            hits.append((rel, found[0], found[1]))
            if len(hits) >= max_hits:
                return hits
    return hits


def _iter_pages(root):
    for dirpath, dirnames, filenames in os.walk(root):
        # 跳过 .git / .scratch 这类隐藏目录，别把临时文件当页面检查。
        dirnames[:] = sorted(name for name in dirnames if not name.startswith("."))
        for filename in sorted(filenames):
            if filename.endswith(".md"):
                yield os.path.join(dirpath, filename)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="wiki 引用门禁（设计文档 §5.2）")
    parser.add_argument("paths", nargs="*", help="要检查的 .md 文件或目录（目录会递归）")
    parser.add_argument("--repo-root", help="该项目源码的本地根目录（check 模式必填）")
    parser.add_argument("--locate", metavar="SYMBOL", help="全仓查找符号，打印真实文件与行号")
    parser.add_argument("--quiet", action="store_true", help="只打印失败项")
    args = parser.parse_args(argv)

    if args.locate:
        if not args.repo_root:
            parser.error("--locate 需要 --repo-root")
        hits = locate_symbol(args.repo_root, args.locate)
        if not hits:
            print("未找到符号 %s" % args.locate)
            return 1
        for rel, lineno, text in hits:
            print("%s:%d  %s" % (rel, lineno, text))
        return 0

    if not args.paths:
        parser.error("check 模式需要至少一个 paths")
    if not args.repo_root:
        parser.error("check 模式需要 --repo-root")
    if not os.path.isdir(args.repo_root):
        parser.error("--repo-root 不是目录: %s" % args.repo_root)

    failed = 0
    checked = 0
    for target in args.paths:
        pages = list(_iter_pages(target)) if os.path.isdir(target) else [target]
        for page in pages:
            checked += 1
            errors = check_refs(page, args.repo_root)
            if errors:
                failed += 1
                print("FAIL %s" % page)
                for error in errors:
                    print("     - %s" % error)
            elif not args.quiet:
                print("PASS %s" % page)
    print("-- 检查 %d 页，失败 %d 页" % (checked, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
