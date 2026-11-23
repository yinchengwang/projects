from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path
from typing import Any

from server.config import WIKI_SCRIPTS

_FRONTMATTER_PATH = WIKI_SCRIPTS / "wiki_frontmatter.py"
_spec = importlib.util.spec_from_file_location("wiki_frontmatter", _FRONTMATTER_PATH)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"无法加载 front-matter 解析器：{_FRONTMATTER_PATH}")
_frontmatter = importlib.util.module_from_spec(_spec)
sys.modules.setdefault("wiki_frontmatter", _frontmatter)
_spec.loader.exec_module(_frontmatter)
parse_frontmatter = _frontmatter.parse_frontmatter
FrontmatterError = _frontmatter.FrontmatterError

_H1_RE = re.compile(r"^#\s+(.+?)\s*$", re.MULTILINE)


def load_page(path: Path, project: str) -> dict[str, Any]:
    """读取单页，调用共享契约解析器并返回 front-matter 与正文。"""
    text = path.read_text(encoding="utf-8")
    try:
        meta, body = parse_frontmatter(text)
    except FrontmatterError as exc:
        raise ValueError(f"{path} front-matter 解析失败：{exc}") from exc
    if meta.get("project") != project:
        raise ValueError(f"{path} 中 project={meta.get('project')!r} 与目录库名不一致")
    h1 = _H1_RE.search(body)
    title = h1.group(1).strip() if h1 else path.stem
    return {"meta": meta, "body": body, "title": title, "page_file": path.name, "path": path}


def list_projects(corpus_root: Path) -> list[str]:
    if not corpus_root.is_dir():
        return []
    return sorted(p.name for p in corpus_root.iterdir() if p.is_dir() and not p.name.startswith("."))


def load_project_pages(corpus_root: Path, project: str) -> list[dict[str, Any]]:
    directory = (corpus_root / project).resolve()
    root = corpus_root.resolve()
    if not directory.is_relative_to(root) or not directory.is_dir():
        raise ValueError(f"语料库不存在或路径非法：{project}")
    paths = sorted(path for path in directory.rglob("*.md") if not any(part.startswith(".") for part in path.relative_to(directory).parts))
    return [load_page(path, project) for path in paths]


def corpus_page_count(corpus_root: Path) -> int:
    return sum(len(list((corpus_root / project).rglob("*.md"))) for project in list_projects(corpus_root))


def source_repo_for(project: str, corpus_root: Path, data_root: Path) -> Path | None:
    """根据环境变量或 index 元信息定位本地上游源码仓。"""
    import os
    from server.config import PROJECTS_ROOT

    override = os.getenv(f"WIKI_RAG_{project.upper()}_REPO")
    candidates = [Path(override)] if override else []
    catalog = PROJECTS_ROOT / "docs" / "wiki" / "catalog-projects.yml"
    # 不额外引入 YAML 解析器：显式配置优先，默认尝试仓库缓存目录。
    candidates.append(PROJECTS_ROOT / ".cache" / "repos" / project)
    for candidate in candidates:
        resolved = candidate.expanduser().resolve()
        if resolved.is_dir():
            return resolved
    return None
