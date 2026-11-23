# -*- coding: utf-8 -*-
"""檢索與抽取式問答 API。"""
from __future__ import annotations

import time
from typing import Any

from core.extractive import answer
from fastapi import APIRouter, HTTPException, Request
from pydantic import BaseModel, Field

router = APIRouter(prefix="/api")


class SearchRequest(BaseModel):
    query: str = Field(min_length=1, max_length=2000)
    projects: list[str] | None = None
    limit: int = Field(default=10, ge=1, le=50)


class AskRequest(SearchRequest):
    answer_limit: int = Field(default=4, ge=1, le=10)


def _search(app: Any, payload: SearchRequest) -> dict[str, Any]:
    started = time.perf_counter()
    try:
        result = app.state.store.search(payload.query, payload.projects, payload.limit)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    result["retrieval"]["elapsed_ms"] = round((time.perf_counter() - started) * 1000, 3)
    return result


@router.post("/search")
def search(request: Request, payload: SearchRequest) -> dict[str, Any]:
    return _search(request.app, payload)


@router.post("/ask")
def ask(request: Request, payload: AskRequest) -> dict[str, Any]:
    result = _search(request.app, payload)
    generated = answer(payload.query, result["results"], request.app.state.documents, payload.answer_limit)
    return {**generated, "retrieval": result["retrieval"], "results": result["results"]}
