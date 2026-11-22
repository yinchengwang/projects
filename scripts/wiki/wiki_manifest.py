# -*- coding: utf-8 -*-
"""机制页清单（manifest）的读写。

manifest 描述「一个项目要写哪些机制页」——它是清单与写作之间的唯一接口：
Phase 1 产 manifest，Phase 2 的 writer 只读自己 manifest 条目内的页，不自由发挥。

格式复用 front-matter：manifest 用 `---` 包裹。值级解析（标量、引号、行内列表
`symbols: [a, b]`、整数推断、全部报错措辞）一律交给 P0 的 `wiki_frontmatter`，
本模块只补两件事：

1. **结构切分**：manifest 的 `pages[].refs[]` 是「列表项内再嵌列表」，比
   `parse_frontmatter` 的受控子集（只认一层列表）深一层。因此遇到这种文本时，
   先按「顶层键 → 列表项」递归切块、去掉缩进，把每一块包成一份两层子文档
   再喂给它。切分只搬运缩进，不解释任何值的语义。
2. **序列化**：`dump_manifest` 排出能被 `load_manifest` 原样读回的文本。
"""

import io
import os
import re

from wiki_frontmatter import FrontmatterError, parse_frontmatter, parse_scalar

MANIFEST_FILENAME = "_manifest.yml"

_INT_RE = re.compile(r"[0-9]+")

# 裸写会被读成别的东西、或直接被解析器拒绝的首字符：
# YAML 指示符 + parse_scalar 明确不支持的形态 + 会被当成列表 / 引号 / 注释的符号。
_PLAIN_START_BLOCKED = ("&", "*", "!", "%", "@", "`", "|", ">", "{", "[",
                        "]", "}", "#", '"', "'")


class ManifestError(Exception):
    """manifest 缺失、格式非法或结构不合法。"""


def load_manifest(path):
    """读取 manifest 并返回 dict。文件不存在或格式非法抛 ManifestError。"""
    if not os.path.isfile(path):
        raise ManifestError("manifest 不存在: %s" % path)
    try:
        with io.open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as exc:
        raise ManifestError("manifest 读取失败 (%s): %s" % (path, exc))

    try:
        meta, body = _parse_manifest_text(text)
    except FrontmatterError as exc:
        raise ManifestError("manifest 解析失败 (%s): %s" % (path, exc))

    if not isinstance(meta, dict):
        raise ManifestError("manifest 顶层不是映射: %s" % path)
    if body.strip():
        raise ManifestError("manifest 在 front-matter 之后不允许有正文: %s" % path)
    return meta


def manifest_feature_slugs(manifest):
    """返回全部 feature slug，保持 manifest 内顺序。"""
    return [page["feature"] for page in manifest.get("pages") or [] if "feature" in page]


# ---------------------------------------------------------------- 解析


def _parse_manifest_text(text):
    """返回 ``(meta, body)``。

    先整体交给 P0 解析器——一层结构它一次就能处理，值级报错措辞也最准。只有它因
    「列表项内再嵌列表」失败、而结构切分能解析成功时，才采用切分结果。

    两条路都失败时，报**行号更靠后**的那个错误：行号越靠后，说明该解析器走得越远、
    越接近真正的病灶。若一律报 P0 的错误，一个写错的嵌套缩进会被报成
    「不支持三级嵌套」，把排查引到格式不支持上，而真实原因是续行缩进不足。
    """
    try:
        return parse_frontmatter(text)
    except FrontmatterError as exc:
        first_error = exc
    try:
        return _split_document(text)
    except FrontmatterError as exc:
        raise _further_error(first_error, exc)


_LINE_IN_MESSAGE = re.compile(r"第\s*(\d+)\s*行")

# P0 解析器「这个形态我不支持」这类错误：它说明的是**能力边界**，不是文本错误。
# manifest 特意要支持比它更深的结构，所以这类错误不能作为最终结论抛出去。
_UNSUPPORTED_MARKERS = ("不支持三级嵌套", "不支持嵌套列表", "不支持嵌套映射")


def _further_error(first_error, second_error):
    """选更可能是真实病因的那个错误。

    优先序：
    1. 若第一条只是 P0 解析器的「不支持这种结构」，而结构切分给出了具体错误
       （缩进错、键重复等），用第二条——那才是文本本身的问题。
    2. 否则取行号更靠后的（走得更远，更接近病灶）；无行号的排在后面。
    """
    first_text = str(first_error)
    if any(marker in first_text for marker in _UNSUPPORTED_MARKERS):
        return second_error

    first_line = _error_line(first_error)
    second_line = _error_line(second_error)
    if first_line is None:
        return second_error
    if second_line is None:
        return first_error
    return second_error if second_line >= first_line else first_error


def _error_line(error):
    """从错误消息里取出「第 N 行」的 N；取不到返回 None。"""
    match = _LINE_IN_MESSAGE.search(str(error))
    return int(match.group(1)) if match else None


def _split_document(text):
    """按结构切块解析更深的嵌套，返回 ``(meta, body)``。"""
    lines = text.replace("\r\n", "\n").split("\n")
    if not lines or lines[0].strip() != "---":
        raise FrontmatterError("缺少 front-matter 起始 ---（文件首行必须是 ---）")
    end = None
    for i in range(1, len(lines)):
        if lines[i].strip() == "---":
            end = i
            break
    if end is None:
        raise FrontmatterError("缺少 front-matter 结束 ---")
    return _parse_block(lines[1:end]), "\n".join(lines[end + 1:])


def _parse_block(block):
    """解析一段已对齐到第 0 列的行：顶层键段 → 标量或列表。"""
    segments = []           # [首行, 行号, [缩进行]]
    for lineno, raw in enumerate(block, start=1):
        if not raw.strip() or raw.strip().startswith("#"):
            continue
        leading = raw[: len(raw) - len(raw.lstrip())]
        if "\t" in leading:
            raise FrontmatterError(
                "front-matter 缩进禁止使用制表符（第 %d 行）: %r" % (lineno, raw)
            )
        if leading:
            if not segments:
                raise FrontmatterError(
                    "缩进内容不在任何顶层键下（第 %d 行）: %r" % (lineno, raw)
                )
            segments[-1][2].append((lineno, raw))
        else:
            segments.append([raw, lineno, []])

    meta = {}
    for first, lineno, children in segments:
        if first.lstrip().startswith("- "):
            # 悬空的列表项（没有归属的键）。P0 解析器同样拒绝，切分层不能松。
            raise FrontmatterError(
                "列表项没有归属的顶层键（第 %d 行）: %r" % (lineno, first)
            )
        key, value = _split_key_value(first)
        if key in meta:
            raise FrontmatterError("顶层键 %r 重复: %r" % (key, first))
        if children:
            if value:
                raise FrontmatterError(
                    "键 %r 同时有值和缩进内容: %r" % (key, first)
                )
            meta[key] = _parse_sequence(children)
        else:
            meta[key] = _parse_value_line(first)
    return meta


def _split_key_value(line):
    """把一行拆成 (key, value)；冒号后没空格就是写错了。"""
    key, _, value = line.partition(":")
    key = key.strip()
    if not key:
        raise FrontmatterError("键名为空: %r" % line)
    if value and not value.startswith((" ", "\t")):
        raise FrontmatterError("键 %r 的冒号后缺少空格: %r" % (key, line))
    return key, value.strip()


def _parse_sequence(children):
    """把某键下的缩进行切成列表项，再逐项解析。"""
    items = []              # [内容列, [行]]
    dash_indent = None
    for lineno, raw in children:
        if not raw.strip() or raw.strip().startswith("#"):
            continue
        leading = raw[: len(raw) - len(raw.lstrip())]
        indent = len(leading)
        stripped = raw[len(leading):]

        if stripped.startswith("- "):
            if dash_indent is None:
                dash_indent = indent
            if indent == dash_indent:
                after = stripped[1:]
                content_col = indent + 1 + len(after) - len(after.lstrip(" "))
                if not after.strip():
                    raise FrontmatterError(
                        "列表项内容为空（第 %d 行）: %r" % (lineno, raw)
                    )
                items.append([content_col, [(lineno, raw)]])
                continue
            if indent > dash_indent and items:
                # 列表项内部的嵌套列表（如 pages[].refs），交给子块继续拆。
                items[-1][1].append((lineno, raw))
                continue
            raise FrontmatterError(
                "列表项缩进不一致（第 %d 行）: %r" % (lineno, raw)
            )

        if not items:
            raise FrontmatterError(
                "列表项没有归属的顶层键（第 %d 行）: %r" % (lineno, raw)
            )
        if indent < items[-1][0]:
            raise FrontmatterError(
                "列表项续行缩进不足（第 %d 行）: %r" % (lineno, raw)
            )
        items[-1][1].append((lineno, raw))

    return [_parse_item(content_col, raws) for content_col, raws in items]


def _parse_item(content_col, raws):
    """去掉列表项的固定缩进后解析：映射项递归拆，标量项取值。"""
    # 首行带 `- ` 前缀，内容列由 _parse_sequence 算好，直接切到内容列即可。
    doc = [raws[0][1][content_col:]]
    for lineno, raw in raws[1:]:
        if not raw.strip():
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        if indent < content_col:
            raise FrontmatterError(
                "列表项续行缩进不足（第 %d 行）: %r" % (lineno, raw)
            )
        doc.append(raw[content_col:])

    if _starts_mapping(doc[0]):
        return _parse_block(doc)
    if len(doc) > 1:
        raise FrontmatterError("标量列表项下不允许有缩进内容: %r" % (doc[0],))
    return parse_scalar(doc[0])


def _starts_mapping(line):
    """判断去缩进后的首行是 `key: value` 还是裸标量。"""
    if line[:1] in ('"', "'"):
        return False
    i = line.find(":")
    if i <= 0:
        return False
    return i + 1 == len(line) or line[i + 1] in " \t"


def _parse_value_line(line):
    """单行 `key: value` 的值交给 P0 解析器，避免重写值级解析。"""
    meta, _ = parse_frontmatter("---\n" + line + "\n---\n")
    if len(meta) != 1:
        raise FrontmatterError("无法解析为单个键: %r" % line)
    return next(iter(meta.values()))


# ---------------------------------------------------------------- 序列化


def dump_manifest(manifest):
    """把 manifest 序列化为 front-matter 文本（LF，单尾换行）。

    键序沿用 dict 的顺序，因此 `dump_manifest(load_manifest(p))` 能逐字还原原文。
    """
    lines = []
    _dump_mapping(lines, manifest, col=0, dash=False)
    body = "\n".join(lines)
    return "---\n" + (body + "\n" if body else "") + "---\n"


def _dump_mapping(lines, mapping, col, dash):
    """写一个映射。`dash` 为真表示首行同时充当列表项的 `- ` 开头。

    列号约定与简报样例一致：列表项 `- ` 位于 col-2，内容列即 col，
    其子列表项再进 4 列。
    """
    first = dash
    for key, value in mapping.items():
        head = " " * (col - 2) + "- " if first else " " * col
        first = False
        if _is_mapping_list(value):
            lines.append("%s%s:" % (head, key))
            for item in value:
                _dump_mapping(lines, item, col + 4, dash=True)
        else:
            lines.append("%s%s: %s" % (head, key, _format_value(value)))


def _is_mapping_list(value):
    """只有「非空且全是映射」的列表才写成块状，其余写成行内列表。"""
    return isinstance(value, list) and bool(value) and all(
        isinstance(item, dict) for item in value
    )


def _format_value(value):
    """映射值：行内列表或标量。"""
    if isinstance(value, list):
        return "[%s]" % ", ".join(_format_inline_item(v) for v in value)
    return _format_scalar(value)


def _format_scalar(value):
    """标量：int 原样写，str 在会被读成别的东西时加引号。"""
    if isinstance(value, bool) or value is None or isinstance(value, dict):
        # front-matter 契约里没有布尔 / 空值 / 嵌套映射，写出去就读不回来。
        raise ManifestError("manifest 值只支持 int / str / list，实际 %r" % (value,))
    if isinstance(value, int):
        return str(value)
    text = str(value)
    return text if not _needs_quote(text) else _quote(text)


def _needs_quote(text):
    """判断裸写会不会被解析成别的值（或直接报错）。"""
    if text == "" or text != text.strip():
        return True
    if text[0] in _PLAIN_START_BLOCKED:
        return True
    if _INT_RE.fullmatch(text):
        return True
    if " #" in text or "\t#" in text:
        return True
    return any(ch in text for ch in "\r\n\t")


def _inline_needs_quote(text):
    """行内列表元素比普通标量多一层限制：括号会破坏 `[a, b]` 的配对。"""
    return _needs_quote(text) or any(ch in text for ch in "[]{}")


def _format_inline_item(value):
    """`[a, b]` 里的单个元素。

    行内列表先按逗号切分、再认引号，所以含逗号的元素无论加不加引号都读不回来；
    这类值在 front-matter 子集里无法表达，直接拒绝而不是写出读不回的文本。
    """
    if isinstance(value, bool) or value is None or isinstance(value, (dict, list)):
        raise ManifestError("行内列表元素只支持 int / str，实际 %r" % (value,))
    if isinstance(value, int):
        return str(value)
    text = str(value)
    if "," in text:
        raise ManifestError("行内列表元素含逗号，front-matter 子集无法表达: %r" % text)
    return text if not _inline_needs_quote(text) else _quote(text)


def _quote(text):
    """挑一个值里没有的引号包住它；两种引号都有则无法表达。"""
    for quote in ("'", '"'):
        if quote not in text:
            return quote + text + quote
    raise ManifestError("值同时含单双引号，front-matter 子集无法表达: %r" % text)
