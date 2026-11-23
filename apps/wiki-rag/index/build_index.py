# -*- coding: utf-8 -*-
"""按库建索引：切块 → 嵌入 → 双索引落盘 → 写元数据。

本模块只负责「一个库怎么建」，不负责跑门禁，也不负责命令行参数。
门禁退出码由 tools/ingest.py 采集后传进来，写进 metadata.json。
"""
from __future__ import annotations

import json
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any

APP_ROOT = Path(__file__).resolve().parent.parent
if str(APP_ROOT) not in sys.path:
    sys.path.insert(0, str(APP_ROOT))

import numpy as np

try:  # 向量库默认为 lancedb；装不上则走 numpy + .npy 后备分支
    import lancedb
except ImportError:  # pragma: no cover - 只在缺少 lancedb 的环境触发
    lancedb = None

from core.bm25 import BM25
from core.chunker import chunk_page
from core.embedder import Embedder
from core.fusion import fuse_hits
from core.wiki_source import load_project_pages


class IndexStore:
    """按库保存块、向量与双路检索；支持 LanceDB 与 NPY 后备。"""

    def __init__(self, data_root: Path, model_name: str, offline: bool = True,
                 use_lancedb: bool = True, rrf_k: int = 60, chunk_min: int = 160,
                 chunk_max: int = 1200, vector_weight: float = 0.5,
                 bm25_weight: float = 0.5, overview_weight: float = 0.55):
        self.data_root = data_root
        self.data_root.mkdir(parents=True, exist_ok=True)
        self.embedder = Embedder(model_name, offline=offline)
        self.use_lancedb = bool(use_lancedb and lancedb is not None)
        self.backend = "lancedb" if self.use_lancedb else "numpy"
        self.rrf_k = rrf_k
        self.chunk_min = chunk_min
        self.chunk_max = chunk_max
        self.vector_weight = vector_weight
        self.bm25_weight = bm25_weight
        self.overview_weight = overview_weight
        self._cache: dict[str, dict[str, Any]] = {}
        self._db = lancedb.connect(str(data_root / "vectors.lancedb")) if self.use_lancedb else None

    # ---------- 路径 ----------

    def project_dir(self, name: str) -> Path:
        if not name or name in {".", ".."} or any(char in name for char in "/\\"):
            raise ValueError(f"库名非法：{name!r}")
        return self.data_root / "projects" / name

    def list_projects(self) -> list[str]:
        root = self.data_root / "projects"
        return sorted(path.name for path in root.iterdir() if path.is_dir()) if root.is_dir() else []

    def metadata(self, name: str) -> dict[str, Any]:
        path = self.project_dir(name) / "metadata.json"
        if not path.exists():
            raise ValueError(f"库 {name} 尚未入库")
        return json.loads(path.read_text(encoding="utf-8"))

    # ---------- 建索引 ----------

    def build_project(self, corpus_root: Path, project: str,
                      gate_results: dict[str, int] | None = None) -> dict[str, Any]:
        """重建单库索引。只删本库产物，不触碰其他库。"""
        pages = load_project_pages(corpus_root, project)
        blocks: list[dict[str, Any]] = []
        for page in pages:
            page_blocks = chunk_page(page["meta"], page["body"], page["page_file"],
                                     page["title"], self.chunk_min, self.chunk_max)
            # 同页同章节在二次切分后会得到同一基础 ID，追加 part 序号保证唯一。
            counts: dict[str, int] = defaultdict(int)
            for block in page_blocks:
                counts[block["block_id"]] += 1
            seen: dict[str, int] = defaultdict(int)
            for block in page_blocks:
                base = block["block_id"]
                if counts[base] > 1:
                    seen[base] += 1
                    block["block_id"] = f"{base}::part-{seen[base]}"
                block["page_file"] = page["page_file"]
            blocks.extend(page_blocks)

        target = self.project_dir(project)
        target.mkdir(parents=True, exist_ok=True)
        vectors = self.embedder.encode(block["text"] for block in blocks)
        if self.use_lancedb:
            table_name = f"project_{project}"
            if table_name in self._db.table_names():
                self._db.drop_table(table_name)
            if blocks:
                rows = [{**block, "vector": vectors[index].tolist()} for index, block in enumerate(blocks)]
                self._db.create_table(table_name, data=rows, mode="create")
        np.save(target / "vectors.npy", vectors)

        (target / "chunks.json").write_text(
            json.dumps(blocks, ensure_ascii=False, indent=2), encoding="utf-8")
        meta = {
            "project": project,
            "page_count": len(pages),
            "block_count": len(blocks),
            "block_ids": [block["block_id"] for block in blocks],
            "gates": gate_results or {},
            "vector_backend": self.backend,
            "embedding_backend": self.embedder.backend,
            "embedding_model": self.embedder.model_name if self.embedder.backend == "sentence-transformers" else None,
            "embedding_warning": self.embedder.warning,
            "chunk_min": self.chunk_min,
            "chunk_max": self.chunk_max,
            "rrf_k": self.rrf_k,
        }
        (target / "metadata.json").write_text(
            json.dumps(meta, ensure_ascii=False, indent=2), encoding="utf-8")
        self._cache.pop(project, None)
        return meta

    # ---------- 读索引 ----------

    def load_project(self, project: str) -> dict[str, Any]:
        if project in self._cache:
            return self._cache[project]
        target = self.project_dir(project)
        chunks_path = target / "chunks.json"
        if not chunks_path.is_file():
            raise ValueError(f"库 {project} 尚未入库")
        documents = json.loads(chunks_path.read_text(encoding="utf-8"))
        vectors = np.load(target / "vectors.npy")
        data = {
            "documents": documents,
            "by_id": {item["block_id"]: item for item in documents},
            "vectors": vectors,
            "bm25": BM25(documents),
        }
        self._cache[project] = data
        return data

    def library(self, project: str) -> dict[str, Any]:
        data = self.load_project(project)
        meta = self.metadata(project)
        pages: dict[str, dict[str, Any]] = {}
        for doc in data["documents"]:
            pages.setdefault(doc["page_file"], {
                "page_file": doc["page_file"],
                "title": doc["title"],
                "feature": doc["feature"],
                "page_type": doc["page_type"],
                "blocks": [],
            })
            pages[doc["page_file"]]["blocks"].append(doc["block_id"])
        return {**meta, "pages": sorted(pages.values(), key=lambda item: item["page_file"])}

    # ---------- 检索 ----------

    def search(self, query: str, projects: list[str] | None = None, limit: int = 10) -> dict[str, Any]:
        project_names = projects if projects is not None else self.list_projects()
        vector_hits: list[tuple[str, float]] = []
        bm25_hits: list[tuple[str, float]] = []
        docs: dict[str, dict[str, Any]] = {}
        query_vector = self.embedder.encode([query])[0]
        for project in project_names:
            try:
                data = self.load_project(project)
            except ValueError:
                continue
            docs.update(data["by_id"])
            if data["vectors"].shape[1] != query_vector.shape[0]:
                raise ValueError(
                    f"库 {project} 的向量维度 {data['vectors'].shape[1]} 与当前嵌入模型维度 "
                    f"{query_vector.shape[0]} 不一致；请用同一个嵌入模型重建该库索引"
                )
            if self.use_lancedb:
                table = self._db.open_table(f"project_{project}")
                rows = table.search(query_vector.tolist()).limit(limit).to_list()
                hits = [(row["block_id"], float(1 / (1 + row.get("_distance", 0))) ) for row in rows]
            else:
                similarities = data["vectors"] @ query_vector
                order = np.argsort(-similarities)[:limit]
                hits = [(data["documents"][index]["block_id"], float(similarities[index]))
                        for index in order if similarities[index] > 0]
            vector_hits.extend(hits)
            bm25_hits.extend(data["bm25"].search(query, limit))
        combined = fuse_hits(vector_hits, bm25_hits, self.rrf_k, self.vector_weight,
                             self.bm25_weight, docs, self.overview_weight)
        return {
            "results": [{**docs[item["block_id"]], **item} for item in combined[:limit]],
            "retrieval": {
                "vector_recall_count": len(vector_hits),
                "bm25_hit_count": len(bm25_hits),
                "fused_count": len(combined),
            },
        }
