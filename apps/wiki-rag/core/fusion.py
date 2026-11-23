# -*- coding: utf-8 -*-
"""RRF 雙路名次融合。"""
from __future__ import annotations

from collections.abc import Iterable, Mapping
from typing import Any


def rrf_fuse(*rankings: Iterable[str], k: int = 60,
             weights: Iterable[float] | None = None) -> list[tuple[str, float]]:
    """Reciprocal Rank Fusion，預設平滑常數 k=60。"""
    lists = [list(ranking) for ranking in rankings]
    weight_values = list(weights) if weights is not None else [1.0] * len(lists)
    if len(weight_values) != len(lists):
        raise ValueError("weights 數量必須與 rankings 一致")
    scores: dict[str, float] = {}
    for ranking, weight in zip(lists, weight_values):
        for rank, key in enumerate(dict.fromkeys(ranking), 1):
            scores[key] = scores.get(key, 0.0) + float(weight) / (k + rank)
    return sorted(scores.items(), key=lambda item: (-item[1], item[0]))


def fuse_hits(vector_hits: list[tuple[str, float]], bm25_hits: list[tuple[str, float]],
              k: int = 60, vector_weight: float = 0.5, bm25_weight: float = 0.5,
              documents: Mapping[str, dict[str, Any]] | None = None,
              overview_weight: float = 0.55) -> list[dict[str, Any]]:
    """融合名次，保留兩路原始分數並下調 overview。"""
    vector_rank = [key for key, _ in vector_hits]
    bm25_rank = [key for key, _ in bm25_hits]
    fused = rrf_fuse(vector_rank, bm25_rank, k=k, weights=[vector_weight, bm25_weight])
    vector_scores, bm25_scores = dict(vector_hits), dict(bm25_hits)
    vector_positions = {key: rank for rank, key in enumerate(vector_rank, 1)}
    bm25_positions = {key: rank for rank, key in enumerate(bm25_rank, 1)}
    results = []
    for block_id, score in fused:
        document = (documents or {}).get(block_id, {})
        adjusted = score * (overview_weight if document.get("is_overview") else 1.0)
        results.append({
            "block_id": block_id,
            "score": adjusted,
            "rrf_score": score,
            "vector_score": vector_scores.get(block_id),
            "bm25_score": bm25_scores.get(block_id),
            "vector_rank": vector_positions.get(block_id),
            "bm25_rank": bm25_positions.get(block_id),
        })
    return sorted(results, key=lambda item: (-item["score"], item["block_id"]))
