# -*- coding: utf-8 -*-
"""FastAPI 组合根：API 与零构建静态前端。"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from core.wiki_source import corpus_page_count
from index.build_index import IndexStore
from server.api_library import router as library_router
from server.api_search import router as search_router
from server.config import get_settings


def create_app(settings=None) -> FastAPI:
    settings = settings or get_settings()
    app = FastAPI(title="Wiki RAG 演示", version="1.0.0")
    app.state.settings = settings
    app.state.store = IndexStore(
        settings.data_root, settings.embedding_model, settings.offline,
        settings.use_lancedb, settings.rrf_k, settings.chunk_min,
        settings.chunk_max, settings.vector_weight, settings.bm25_weight,
        settings.overview_weight,
    )
    app.state.documents: dict[str, dict[str, Any]] = {}
    app.state.load_errors: dict[str, str] = {}
    for name in app.state.store.list_projects():
        try:
            app.state.documents.update(app.state.store.load_project(name)["by_id"])
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            app.state.load_errors[name] = str(exc)
    app.include_router(library_router)
    app.include_router(search_router)

    @app.get("/api/health")
    def health() -> dict[str, Any]:
        store = app.state.store
        projects = store.list_projects()
        warnings = [store.embedder.warning] if store.embedder.warning else []
        if not projects:
            warnings.append("索引尚未就绪，请运行 python tools/ingest.py --project <库名>。")
        if app.state.load_errors:
            warnings.append(
                f"有 {len(app.state.load_errors)} 个库的索引无法加载；"
                "多为嵌入模型维度不一致，请用当前模型重建该库。"
            )
        return {
            "status": "ok",
            "embedding_backend": store.embedder.backend,
            "embedding_model": store.embedder.model_name
            if store.embedder.backend == "sentence-transformers" else None,
            "embedding_warning": store.embedder.warning,
            "vector_backend": store.backend,
            "index_ready": bool(app.state.documents),
            "projects": projects,
            "corpus_page_count": corpus_page_count(settings.corpus_root),
            "indexed_page_count": sum(store.metadata(name).get("page_count", 0) for name in projects),
            "warnings": warnings,
        }

    app.mount("/", StaticFiles(directory=Path(__file__).resolve().parent.parent / "web", html=True), name="web")
    return app


app = create_app()
