# -*- coding: utf-8 -*-
"""项目状态账本解析与校验。

阶段 0A 需要一份机器可读账本，回答「每个项目现在到哪一步、下一步做什么」，
避免再次出现「做完一个试点就丢失全局进度」。本模块保持标准库实现，
不引入第三方 YAML 依赖，只解析本仓约定的受限子集 schema。

账本结构（docs/wiki/wiki-project-status.yml）：

    projects:
      - slug: postgres
        repo: https://github.com/postgres/postgres
        state: indexed
        next_action: 保持 CI 与 RAG 增量复核
        aliases: [postgresql]
"""
from __future__ import annotations

import re
from urllib.parse import urlparse

# 项目阶段。catalog 只是登记，不等于任何 wiki 内容已完成。
STATES = frozenset({
    "catalog",
    "planned",
    "source-ready",
    "inventoried",
    "pages-written",
    "gates-pass",
    "indexed",
    "blocked",
    "excluded-with-reason",
})

REQUIRED_FIELDS = ("slug", "repo", "state", "next_action")

_ENTRY_START = re.compile(r"^  - slug: (\S+)$")
_FIELD = re.compile(r"^    ([a-z_]+): (.*)$")
_ALIASES = re.compile(r"^    aliases: \[(.*)\]$")


class StatusError(Exception):
    """账本结构非法。"""


def normalize_repo(url):
    parsed = urlparse(url.strip().rstrip("/"))
    path = parsed.path.lower().strip("/")
    if path.endswith(".git"):
        path = path[:-4]
    return parsed.netloc.lower(), path


def _strip_comment(value):
    """去掉行尾注释，保留值本身。"""
    idx = value.find(" #")
    return value[:idx].strip() if idx >= 0 else value.strip()


def parse_status(text):
    """解析账本文本，返回条目列表。"""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "projects:":
        raise StatusError("账本必须以 projects: 开头")

    entries = []
    current = None
    for line in lines[1:]:
        if not line.strip():
            continue
        start = _ENTRY_START.match(line)
        if start:
            if current is not None:
                entries.append(current)
            current = {"slug": start.group(1), "aliases": []}
            continue
        if current is None:
            raise StatusError("字段出现在任何条目之前: %r" % line)
        aliases = _ALIASES.match(line)
        if aliases:
            body = aliases.group(1).strip()
            current["aliases"] = [a.strip() for a in body.split(",") if a.strip()]
            continue
        field = _FIELD.match(line)
        if not field:
            raise StatusError("无法解析的行: %r" % line)
        key, value = field.group(1), _strip_comment(field.group(2))
        if key in current:
            raise StatusError("字段重复: %s in %s" % (key, current["slug"]))
        current[key] = value

    if current is not None:
        entries.append(current)
    return entries


def validate_status(entries):
    """返回问题列表；空列表表示通过。"""
    problems = []
    slugs = set()
    repos = {}
    alias_owner = {}

    for entry in entries:
        slug = entry.get("slug", "<missing>")
        for field in REQUIRED_FIELDS:
            if not entry.get(field):
                problems.append("%s: 缺少 %s" % (slug, field))

        state = entry.get("state")
        if state and state not in STATES:
            problems.append("%s: 未知 state %s" % (slug, state))

        if slug in slugs:
            problems.append("%s: slug 重复" % slug)
        slugs.add(slug)

        repo = entry.get("repo")
        if repo:
            key = normalize_repo(repo)
            if key in repos:
                problems.append("%s: repo 与 %s 重复" % (slug, repos[key]))
            repos[key] = slug

    for entry in entries:
        for alias in entry.get("aliases", []):
            if alias in alias_owner:
                problems.append("%s: alias %s 与 %s 冲突" % (entry["slug"], alias, alias_owner[alias]))
            alias_owner[alias] = entry["slug"]

    for alias, owner in alias_owner.items():
        if alias in slugs:
            problems.append("%s: alias %s 与已有 slug 冲突" % (owner, alias))

    return problems
