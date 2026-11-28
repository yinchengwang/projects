# -*- coding: utf-8 -*-
"""从 catalog 与候选审阅矩阵生成项目状态账本。

账本只记录「项目到哪一步、下一步做什么」，不重复 catalog 的 stars/license
等元数据。已完成的四个项目直接登记为 indexed，其余 catalog 条目为 planned，
审阅矩阵里的 118 个候选同样是 planned 并标注待取 metadata。
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECTS_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

from candidate_slate import parse_review
from wiki_catalog import parse_catalog

CATALOG_PATH = PROJECTS_ROOT / "docs" / "wiki" / "catalog-projects.yml"
REVIEW_PATH = PROJECTS_ROOT / "docs" / "wiki" / "project-catalog-review.md"
STATUS_PATH = PROJECTS_ROOT / "docs" / "wiki" / "wiki-project-status.yml"

# 已完成 RAG 入库的项目：wiki 目录 slug → 下一动作。
INDEXED_PROJECTS = {
    "postgres": "保持 CI 与 RAG 增量复核",
    "redis": "阶段 0B 入 wiki-rag 后复核引用回源",
    "sqlite": "阶段 0B 入 wiki-rag 后复核引用回源",
    "mysql-server": "阶段 0B 入 wiki-rag 后复核引用回源",
}

# 已完成机制 wiki 但尚未入 RAG 的项目：wiki 目录 slug → 下一动作。
GATES_PASS_PROJECTS = {
    "langgraph": "待阶段 0B 扩展后入 wiki-rag",
    "autogen": "待阶段 0B 扩展后入 wiki-rag",
    "crewai": "待阶段 0B 扩展后入 wiki-rag",
    "metagpt": "待阶段 0B 扩展后入 wiki-rag",
    "openai-agents": "待阶段 0B 扩展后入 wiki-rag",
    "pydantic-ai": "待阶段 0B 扩展后入 wiki-rag",
    "smolagents": "待阶段 0B 扩展后入 wiki-rag",
    "mcp-python-sdk": "待阶段 0B 扩展后入 wiki-rag",
    "browser-use": "待阶段 0B 扩展后入 wiki-rag",
    "openhands": "待阶段 0B 扩展后入 wiki-rag",
    "goose": "待阶段 0B 扩展后入 wiki-rag",
    "open-interpreter": "待阶段 0B 扩展后入 wiki-rag",
    "langchain": "待阶段 0B 扩展后入 wiki-rag",
    "llama-index": "待阶段 0B 扩展后入 wiki-rag",
    "haystack": "待阶段 0B 扩展后入 wiki-rag",
    "lightrag": "待阶段 0B 扩展后入 wiki-rag",
    "graphrag": "待阶段 0B 扩展后入 wiki-rag",
    "ragas": "待阶段 0B 扩展后入 wiki-rag",
}

WIKI_ROOT = PROJECTS_ROOT / "docs" / "wiki" / "projects"

# catalog 名与 wiki 目录名不同时，把 catalog 名登记为别名，供 ingest 与 CI 反查。
def _aliases_for(name, slug):
    return [name] if name != slug else []


# catalog 名与 wiki 目录名不一致且无法靠后缀规则推出的显式映射。
WIKI_DIR_ALIASES = {
    "sqlite3": "sqlite",
    "mysql": "mysql-server",
    "haystack-ai": "haystack",
}


def _wiki_dir_for(name):
    """catalog 名 → 实际 wiki 目录 slug；没有可用 wiki 目录时返回 None。"""
    candidates = [WIKI_DIR_ALIASES.get(name), name, name + "-server"]
    for slug in candidates:
        if slug and (WIKI_ROOT / slug / "_manifest.yml").is_file():
            return slug
    # catalog 用了带 -server 的名字、wiki 目录用了裸名（或反之）时兜一层。
    stripped = name[:-7] if name.endswith("-server") else None
    if stripped and (WIKI_ROOT / stripped / "_manifest.yml").is_file():
        return stripped
    return None


def build_entries():
    catalog = parse_catalog(CATALOG_PATH.read_text(encoding="utf-8"))
    candidates = parse_review(REVIEW_PATH)

    entries = []
    for item in catalog:
        name = item["name"]
        # 状态由磁盘上的 wiki 目录事实推出，不靠手写清单断言「完成」。
        slug = _wiki_dir_for(name) or name
        if slug in INDEXED_PROJECTS:
            state, next_action = "indexed", INDEXED_PROJECTS[slug]
        elif slug in GATES_PASS_PROJECTS:
            state, next_action = "gates-pass", GATES_PASS_PROJECTS[slug]
        else:
            next_action, state = "待源码盘点与机制 wiki 生产", "planned"
        entries.append({
            "slug": slug,
            "repo": item["repo"],
            "state": state,
            "next_action": next_action,
            "aliases": _aliases_for(name, slug),
            "_model": item["model"],
        })

    for item in candidates:
        entries.append({
            "slug": item["name"],
            "repo": item["repo"],
            "state": "planned",
            "next_action": "待取真实 GitHub metadata 后合并 catalog",
            "aliases": [],
            "_model": item["model"],
        })
    # 候选里若出现已由 wiki 项目占用的 slug，视为冲突需人工裁定，此处不静默合并。
    return entries


def dump_status(entries):
    lines = ["projects:"]
    for entry in sorted(entries, key=lambda e: (_model_rank(e["_model"]), e["slug"])):
        lines.append("  - slug: %s" % entry["slug"])
        lines.append("    model: %s" % entry["_model"])
        lines.append("    repo: %s" % entry["repo"])
        lines.append("    state: %s" % entry["state"])
        lines.append("    next_action: %s" % entry["next_action"])
        if entry["aliases"]:
            lines.append("    aliases: [%s]" % ", ".join(entry["aliases"]))
    return "\n".join(lines) + "\n"


def _model_rank(model):
    return (model.startswith("ai/"), model)


def main(argv=None):
    parser = argparse.ArgumentParser(description="生成项目状态账本")
    parser.add_argument("--status", type=Path, default=STATUS_PATH)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    entries = build_entries()
    text = dump_status(entries)
    if args.dry_run:
        print(text)
        return 0
    args.status.write_text(text, encoding="utf-8", newline="\n")
    print("已写账本条目:", len(entries), "→", args.status)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
