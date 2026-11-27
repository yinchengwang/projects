# -*- coding: utf-8 -*-
"""解析 docs/wiki/project-catalog-review.md 的候选表。

只做一件事：把 review markdown 中「分类键 | 目标新增 | 候选」表格的候选条目
解析为 {name, model, repo} 列表，供测试校验候选数量、唯一性和与 catalog 的冲突。
"""
from __future__ import annotations

import re
from pathlib import Path
from urllib.parse import urlparse

# 候选条目形如：`name` — `https://github.com/owner/repo` — T1 — 理由
_ENTRY_RE = re.compile(r"`([^`]+)`\s+—\s+`(https?://[^`]+)`")
# 表格行形如：| key | 目标新增 | 候选列表 |
_ROW_RE = re.compile(r"^\| ([^|]+?) \| (\d+) \| (.*) \|$")

DB_SECTION_START = "## 2. 数据库 / 数据基础设施候选"
DB_SECTION_END = "### DB 分类数量的审阅规则"
AI_SECTION_START = "## 3. AI / Agent / RAG / LLM 候选"
AI_SECTION_END = "## 3. 候选规范与风险"


def normalize_repo(url):
    """归一化 GitHub repo：小写 host + 去 .git 的 path。"""
    parsed = urlparse(url.strip().rstrip("/"))
    path = parsed.path.lower().strip("/")
    if path.endswith(".git"):
        path = path[:-4]
    return parsed.netloc.lower(), path


def parse_review(path):
    """返回 review 文件中的全部候选，跳过合计行。"""
    text = Path(path).read_text(encoding="utf-8")
    sections = [
        (DB_SECTION_START, DB_SECTION_END),
        (AI_SECTION_START, AI_SECTION_END),
    ]
    entries = []
    for start, end in sections:
        section = text.split(start, 1)[1].split(end, 1)[0]
        for line in section.splitlines():
            match = _ROW_RE.match(line)
            if match is None:
                continue
            model = match.group(1).strip()
            if model.startswith("**"):
                # 合计行不是候选行
                continue
            for name, repo in _ENTRY_RE.findall(match.group(3)):
                entries.append({"name": name, "model": model, "repo": repo})
    return entries
