from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path

APP_ROOT = Path(__file__).resolve().parent.parent
PROJECTS_ROOT = APP_ROOT.parent.parent
REPO_ROOT = PROJECTS_ROOT.parent
WIKI_SCRIPTS = PROJECTS_ROOT / "scripts" / "wiki"
DEFAULT_CORPUS = PROJECTS_ROOT / "docs" / "wiki" / "projects"
DEFAULT_DATA = APP_ROOT / "index" / "data"


@dataclass(frozen=True)
class Settings:
    corpus_root: Path = DEFAULT_CORPUS
    data_root: Path = DEFAULT_DATA
    use_lancedb: bool = True
    embedding_model: str = "BAAI/bge-small-zh-v1.5"
    offline: bool = True
    rrf_k: int = 60
    chunk_min: int = 160
    chunk_max: int = 1200
    vector_weight: float = 0.5
    bm25_weight: float = 0.5
    overview_weight: float = 0.55


def _path(name: str, default: Path) -> Path:
    value = os.getenv(name)
    return Path(value).expanduser().resolve() if value else default.resolve()


def get_settings() -> Settings:
    """从环境读取配置，不解析 .env 文件或读取密钥。"""
    return Settings(
        corpus_root=_path("WIKI_RAG_CORPUS", DEFAULT_CORPUS),
        data_root=_path("WIKI_RAG_DATA", DEFAULT_DATA),
        use_lancedb=os.getenv("WIKI_RAG_LANCEDB", "1").lower() not in {"0", "false", "no"},
        embedding_model=os.getenv("WIKI_RAG_EMBEDDING_MODEL", "BAAI/bge-small-zh-v1.5"),
        offline=os.getenv("WIKI_RAG_OFFLINE", "1").lower() not in {"0", "false", "no"},
        rrf_k=max(1, int(os.getenv("WIKI_RAG_RRF_K", "60"))),
        chunk_min=max(1, int(os.getenv("WIKI_RAG_CHUNK_MIN", "160"))),
        chunk_max=max(1, int(os.getenv("WIKI_RAG_CHUNK_MAX", "1200"))),
        vector_weight=max(0.0, float(os.getenv("WIKI_RAG_VECTOR_WEIGHT", "0.5"))),
        bm25_weight=max(0.0, float(os.getenv("WIKI_RAG_BM25_WEIGHT", "0.5"))),
        overview_weight=max(0.0, min(1.0, float(os.getenv("WIKI_RAG_OVERVIEW_WEIGHT", "0.55")))),
    )
