# -*- coding: utf-8 -*-
"""文档生产清单（catalog）的解析与校验。

与 reference 仓的 `reference/catalog.yml` 是两份不同的东西：那份是**第三方源码镜像
清单**（字段含 commit / pin_status / source / mirror，语义是「怎么克隆」）；本文件是
**文档生产清单**（语义是「写哪些库、写多深」）。两者生命周期不同，不合并。

只用标准库：清单结构比 catalog 更规整（两层、无行内列表、无嵌套映射），
手写解析比引入 YAML 依赖更符合本仓既有约定（P0 已确立零第三方依赖）。
"""
import re

CATALOG_FIELDS = (
    "name",
    "model",
    "tier",
    "repo",
    "license",
    "language",
    "stars",
    "stars_as_of",
)

VALID_TIERS = ("T1", "T2", "T3")
_INT_RE = re.compile(r"[0-9]+")
_ENTRY_RE = re.compile(r"^\s{2}-\s+name:\s*(.+?)\s*$")
_FIELD_RE = re.compile(r"^\s{4}([A-Za-z_]+):\s*(.*?)\s*$")


class CatalogError(Exception):
    """清单缺失、结构非法或条目重复。"""


def parse_catalog(text):
    """返回条目列表。空清单返回 []。"""
    lines = text.replace("\r\n", "\n").split("\n")
    start = None
    for i, line in enumerate(lines):
        if line.strip() == "projects:":
            start = i
            break
    if start is None:
        raise CatalogError("缺少顶层 projects: 键")

    entries = []
    current = None
    for raw in lines[start + 1:]:
        if not raw.strip() or raw.strip().startswith("#"):
            continue
        m = _ENTRY_RE.match(raw)
        if m:
            current = {"name": m.group(1).strip()}
            entries.append(current)
            continue
        m = _FIELD_RE.match(raw)
        if m:
            if current is None:
                raise CatalogError("字段出现在任何条目之前: %r" % raw)
            current[m.group(1)] = _coerce(m.group(2))
            continue
        raise CatalogError("无法解析的行: %r" % raw)

    seen = set()
    for entry in entries:
        name = entry["name"]
        if name in seen:
            raise CatalogError("条目重复: %s" % name)
        seen.add(name)
    return entries


def _coerce(value):
    """整数字面量转 int，其余保持 str。日期 `2026-11-22` 不是纯整数，自然保持 str。"""
    if _INT_RE.fullmatch(value):
        return int(value)
    return value


def dump_catalog(entries):
    """按固定字段顺序输出，未提供的字段跳过。返回 LF 文本。"""
    out = ["projects:"]
    for entry in entries:
        out.append("  - name: %s" % entry["name"])
        for field in CATALOG_FIELDS:
            if field == "name" or field not in entry:
                continue
            out.append("    %s: %s" % (field, entry[field]))
    return "\n".join(out) + "\n"


def validate_entry(entry):
    """返回错误列表，空列表表示通过。"""
    errors = []
    for field in CATALOG_FIELDS:
        if field not in entry:
            errors.append("缺字段: %s" % field)

    tier = entry.get("tier")
    if tier is not None and tier not in VALID_TIERS:
        errors.append("tier 必须是 %s 之一，实际 %r" % (list(VALID_TIERS), tier))

    repo = entry.get("repo")
    if isinstance(repo, str) and not repo.startswith(("https://", "http://")):
        errors.append("repo 必须是 http(s) URL，实际 %r" % repo)

    stars = entry.get("stars")
    if stars is not None and not isinstance(stars, int):
        errors.append("stars 必须是整数，实际 %r" % (stars,))

    as_of = entry.get("stars_as_of")
    if isinstance(as_of, str) and not re.fullmatch(r"\d{4}-\d{2}-\d{2}", as_of):
        errors.append("stars_as_of 必须是 YYYY-MM-DD，实际 %r" % as_of)

    return errors
