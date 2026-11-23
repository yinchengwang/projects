from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path
from typing import Any

APP_ROOT = Path(__file__).resolve().parent.parent
PROJECTS_ROOT = APP_ROOT.parent.parent
sys.path.insert(0, str(APP_ROOT))

from index.build_index import IndexStore
from server.config import get_settings


def _run_gate(script: Path, args: list[str], cwd: Path) -> int:
    """运行仓库内真实门禁，并保留其退出码。"""
    result = subprocess.run([sys.executable, str(script), *args], cwd=cwd, check=False)
    return int(result.returncode)


def ingest_project(project: str, corpus_root: Path | None = None,
                   data_root: Path | None = None) -> dict[str, Any]:
    settings = get_settings()
    corpus = corpus_root or settings.corpus_root
    data = data_root or settings.data_root
    pages = corpus / project
    if not pages.is_dir():
        raise ValueError(f"找不到语料库：{pages}")

    scripts = PROJECTS_ROOT / "scripts" / "wiki"
    gates = {"wiki_lint": _run_gate(scripts / "wiki_lint.py", [str(pages)], PROJECTS_ROOT)}
    override = os.getenv(f"WIKI_RAG_{project.upper()}_REPO")
    repo = Path(override).expanduser().resolve() if override else PROJECTS_ROOT / ".cache" / "repos" / project
    if repo.is_dir():
        gates["wiki_refs"] = _run_gate(
            scripts / "wiki_refs.py", [str(pages), "--repo-root", str(repo)], PROJECTS_ROOT)
    else:
        # 无源码仓不做伪校验：记非零状态，使左栏如实显示 FAIL。
        gates["wiki_refs"] = 127

    store = IndexStore(data, settings.embedding_model, settings.offline, settings.use_lancedb,
                       settings.rrf_k, settings.chunk_min, settings.chunk_max,
                       settings.vector_weight, settings.bm25_weight, settings.overview_weight)
    return store.build_project(corpus, project, gates)


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description="按库建立 wiki RAG 索引")
    parser.add_argument("--project", help="只重建指定库")
    parser.add_argument("--all", action="store_true", help="重建现有语料中的全部库")
    args = parser.parse_args(argv)
    settings = get_settings()
    if args.project:
        names = [args.project]
    elif args.all:
        names = sorted(path.name for path in settings.corpus_root.iterdir()
                       if path.is_dir() and not path.name.startswith("."))
    else:
        parser.error("必须指定 --project <库名> 或 --all")
    if not names:
        parser.error("没有可入库的库")

    failed = False
    for name in names:
        try:
            meta = ingest_project(name)
            print(f"完成 {name}：{meta['page_count']} 页，{meta['block_count']} 块，"
                  f"嵌入={meta['embedding_backend']}，向量库={meta['vector_backend']}，门禁={meta['gates']}")
            failed |= any(code != 0 for code in meta["gates"].values())
        except Exception as exc:
            print(f"入库失败 {name}：{exc}")
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
