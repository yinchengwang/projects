# -*- coding: utf-8 -*-
"""已入庫的知識庫與頁面導航 API。"""
from __future__ import annotations

from typing import Any

from fastapi import APIRouter, HTTPException, Request

router = APIRouter(prefix="/api")


@router.get("/projects")
def projects(request: Request) -> dict[str, Any]:
    results = []
    store = request.app.state.store
    for name in store.list_projects():
        metadata = store.metadata(name)
        gates = metadata.get("gates", {})
        passed = bool(gates) and all(code == 0 for code in gates.values())
        results.append({
            "name": name,
            "page_count": metadata.get("page_count", 0),
            "block_count": metadata.get("block_count", 0),
            "gates": gates,
            "validation": "PASS" if passed else "FAIL",
        })
    return {"projects": results}


@router.get("/projects/{name}")
def project_detail(name: str, request: Request) -> dict[str, Any]:
    try:
        return request.app.state.store.library(name)
    except (ValueError, OSError) as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
