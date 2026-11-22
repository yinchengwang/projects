# -*- coding: utf-8 -*-
"""front-matter 极简解析器。

只支持 wiki 契约用到的受控 YAML 子集：

    key: 标量
    key:
      - 标量
      - key: 标量            ← 列表项内嵌标量
        symbols: [a, b, c]  ← 行内列表

不支持多行块标量（| / >）、锚点与别名、标签、行内映射（{...}）、
嵌套列表、嵌套映射、值后尾随注释、冒号后缺空格的 ``key:value``。
这些形态契约里不会出现，一旦出现必须抛 FrontmatterError 而不是静默误解析
——宁可写文档的人看到一行中文报错，也不要让 2.5 万篇文档里混进一个被
解析歪了的字段。
"""

import re

# 纯整数字面量（只认 ASCII 十进制），用于标量转 int。
_INT_RE = re.compile(r"[0-9]+")

# 单个标量里不允许作为首字符的 YAML 指示符：锚点 &、别名 *、标签 !、
# 指令 %、保留 @ `、块标量 | >、行内映射 {、闭合的 ] }。
_UNSUPPORTED_PLAIN_START = ("&", "*", "!", "%", "@", "`", "|", ">", "{", "]", "}")


class FrontmatterError(Exception):
    """front-matter 缺失，或其形态超出受控 YAML 子集。"""


def _reject_inline_comment(v):
    """值后尾随注释不在受控子集内——静默留在值里就是误解析，直接报错。

    引号内的 ``#`` 不算注释起点：``"a # b"`` 合法，``a # b`` 不合法。
    """
    quote = None
    for i, ch in enumerate(v):
        if quote is not None:
            if ch == quote:
                quote = None
            continue
        if ch in ('"', "'"):
            quote = ch
            continue
        if ch == "#" and i > 0 and v[i - 1] in (" ", "\t"):
            raise FrontmatterError(
                "不支持值后尾随注释，请把注释独占一行，遇到 %r" % v
            )


def _reject_unescaped_quote(v):
    """引号包裹的标量里不允许出现未转义的同类引号。

    只查「首末是同种引号」会让 ``"a"junk"`` 这种畸形输入静默变成 ``a"junk``；
    内部的同类引号说明引号配对是错的，必须报错而不是猜。
    """
    quote = v[0]
    inner = v[1:-1]
    i = 0
    while i < len(inner):
        ch = inner[i]
        if ch == "\\":
            i += 2          # 反斜杠转义下一个字符（\" \' \\ 等）
            continue
        if ch == quote:
            raise FrontmatterError("引号内出现未转义的同类引号，遇到 %r" % v)
        i += 1
    if i > len(inner):
        # 反斜杠越过末尾，说明收尾的引号被转义掉了，引号其实没闭合。
        raise FrontmatterError("引号未闭合，遇到 %r" % v)


def parse_scalar(v):
    """把单个值转成 str / int / list。

    * 行内列表 ``[a, b, c]`` -> list（元素一律保持 str）
    * ``[]`` -> []
    * 引号包裹 -> 去掉引号（引号内不参与 int 推断）
    * 纯数字 -> int；日期形态 ``2026-11-22`` 保持 str
    """
    v = v.strip()
    if not v:
        return ""
    _reject_inline_comment(v)
    head = v[0]

    if head in ("&", "*"):
        raise FrontmatterError("不支持锚点 / 别名，遇到 %r" % v)
    if head == "!":
        raise FrontmatterError("不支持标签，遇到 %r" % v)
    if head in ("%", "@", "`"):
        raise FrontmatterError("不支持 YAML 指示符，遇到 %r" % v)
    if head in ("|", ">"):
        raise FrontmatterError("不支持多行块标量（| / >），遇到 %r" % v)
    if head == "{":
        raise FrontmatterError("不支持行内映射（{...}），遇到 %r" % v)
    if head == "#":
        raise FrontmatterError("不支持值后尾随注释，请把注释独占一行，遇到 %r" % v)
    if head == "[":
        return _parse_inline_list(v)
    if head in ("]", "}"):
        raise FrontmatterError("值格式非法，遇到 %r" % v)

    # 引号包裹的值：整体去引号，内部不做 int 推断。
    if head in ('"', "'"):
        if len(v) < 2 or v[-1] != head:
            raise FrontmatterError("引号未闭合，遇到 %r" % v)
        _reject_unescaped_quote(v)
        return v[1:-1]

    if _INT_RE.fullmatch(v):
        return int(v)
    return v


def _parse_inline_list(v):
    """解析行内列表 ``[a, b, c]``，元素一律保持 str。"""
    if not v.endswith("]"):
        raise FrontmatterError("行内列表缺少右方括号，遇到 %r" % v)
    inner = v[1:-1].strip()
    if not inner:
        return []
    items = []
    for part in inner.split(","):
        item = part.strip()
        if not item:
            raise FrontmatterError("行内列表含空元素，遇到 %r" % v)
        if item[0] in ("[", "{"):
            raise FrontmatterError("不支持行内列表嵌套 / 行内映射，遇到 %r" % v)
        if item[0] in _UNSUPPORTED_PLAIN_START:
            raise FrontmatterError("行内列表元素形态不受支持，遇到 %r" % v)
        if item[0] in ('"', "'"):
            if len(item) >= 2 and item[-1] == item[0]:
                _reject_unescaped_quote(item)
                items.append(item[1:-1])
                continue
            raise FrontmatterError("引号未闭合，遇到 %r" % v)
        if any(ch in item for ch in "[]{}"):
            # 裸元素里再出现括号，说明行内列表的括号配不上，别猜。
            raise FrontmatterError("行内列表元素含括号，形态不受支持，遇到 %r" % v)
        items.append(item)
    return items


def _reject_empty_value_in_item(k, v, lineno, raw):
    """列表项内的键不允许空值：那意味着三级嵌套，契约里不出现。"""
    if v == "":
        raise FrontmatterError(
            "列表项内键 %r 的值为空，不支持三级嵌套（第 %d 行）: %r"
            % (k, lineno, raw)
        )


def _split_key_value(s, lineno, raw):
    """把一行拆成 (key, value)，并校验「冒号后必须有空白或行尾」。

    YAML 的块映射要求 ``key: value`` 里冒号后跟空格；``key:value`` 在 YAML
    里是一个整体标量而不是映射，落到这里就是写错了，必须报错。
    """
    k, _, v = s.partition(":")
    k = k.strip()
    if not k:
        raise FrontmatterError("键名为空（第 %d 行）: %r" % (lineno, raw))
    if v and not v.startswith((" ", "\t")):
        raise FrontmatterError(
            "键 %r 的冒号后缺少空格（第 %d 行）: %r" % (k, lineno, raw)
        )
    return k, v.strip()


def parse_frontmatter(text):
    """返回 ``(meta, body)``。text 首行必须是 ``---``，其后必须再有一行 ``---``。"""
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
    """解析两条 --- 之间的行。缩进只接受空格，且必须对齐。"""
    meta = {}
    cur_key = None          # 当前等待列表项的顶层键
    cur_item = None         # 当前列表项（dict 或 None）
    item_key_indent = None  # 列表项内嵌键的基准缩进
    list_indent = None      # 当前顶层键的列表项缩进

    for lineno, raw in enumerate(block, start=1):
        if not raw.strip() or raw.strip().startswith("#"):
            continue

        leading = raw[: len(raw) - len(raw.lstrip())]
        if "\t" in leading:
            raise FrontmatterError(
                "front-matter 缩进禁止使用制表符（第 %d 行）: %r" % (lineno, raw)
            )
        stripped = raw[len(leading):]
        indent = len(leading)

        # ---- 列表项 ----
        if stripped[0] == "-":
            if cur_key is None:
                raise FrontmatterError(
                    "列表项没有归属的顶层键（第 %d 行）: %r" % (lineno, raw)
                )
            if not isinstance(meta.get(cur_key), list):
                raise FrontmatterError(
                    "键 %r 不是列表却出现列表项（第 %d 行）: %r"
                    % (cur_key, lineno, raw)
                )
            if not stripped.startswith("- "):
                raise FrontmatterError(
                    "列表项必须写作 '- 内容'（第 %d 行）: %r" % (lineno, raw)
                )
            if list_indent is None:
                list_indent = indent
            elif indent != list_indent:
                raise FrontmatterError(
                    "列表项缩进不一致，不支持嵌套列表（第 %d 行）: %r" % (lineno, raw)
                )

            # 列表项内容所在的列，用于校验内嵌键的对齐。
            after_dash = stripped[1:]
            content_indent = indent + 1 + len(after_dash) - len(after_dash.lstrip(" "))
            payload = after_dash.strip()
            if not payload:
                raise FrontmatterError(
                    "列表项内容为空（第 %d 行）: %r" % (lineno, raw)
                )
            if payload[0] in ("[", "{") or payload == "-" or payload.startswith("- "):
                # `- - x` 是嵌套列表，`- [a, b]` 是行内序列作列表项，
                # 两种都超出契约；注意 `- -5` 只是负数标量，不算嵌套。
                raise FrontmatterError(
                    "不支持嵌套列表 / 行内列表 / 行内映射作列表项（第 %d 行）: %r"
                    % (lineno, raw)
                )

            # 整项是引号包裹的标量：不当成 key: value 拆开。
            if payload[0] in ('"', "'"):
                meta[cur_key].append(parse_scalar(payload))
                cur_item = None
                item_key_indent = None
                continue

            if ":" in payload:
                k, v = _split_key_value(payload, lineno, raw)
                _reject_empty_value_in_item(k, v, lineno, raw)
                cur_item = {k: parse_scalar(v)}
                meta[cur_key].append(cur_item)
                item_key_indent = content_indent
            else:
                meta[cur_key].append(parse_scalar(payload))
                cur_item = None
                item_key_indent = None
            continue

        # ---- key / key: value ----
        if ":" not in stripped:
            raise FrontmatterError(
                "不是 key: value 形式（第 %d 行）: %r" % (lineno, raw)
            )
        k, v = _split_key_value(stripped, lineno, raw)

        if indent == 0:
            if k in meta:
                raise FrontmatterError(
                    "顶层键 %r 重复（第 %d 行）: %r" % (k, lineno, raw)
                )
            cur_key = k
            cur_item = None
            item_key_indent = None
            list_indent = None
            if v == "":
                # 空值视为「后面跟缩进列表」，先占位为 list。
                meta[k] = []
            else:
                meta[k] = parse_scalar(v)
                cur_key = None
        else:
            if cur_item is None:
                raise FrontmatterError(
                    "缩进键 %r 不在任何列表项内，不支持嵌套映射（第 %d 行）: %r"
                    % (k, lineno, raw)
                )
            if indent != item_key_indent:
                raise FrontmatterError(
                    "缩进键 %r 必须与列表项内第一个键对齐（第 %d 行）: %r"
                    % (k, lineno, raw)
                )
            if k in cur_item:
                raise FrontmatterError(
                    "列表项内键 %r 重复（第 %d 行）: %r" % (k, lineno, raw)
                )
            _reject_empty_value_in_item(k, v, lineno, raw)
            cur_item[k] = parse_scalar(v)

    return meta
