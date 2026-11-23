from __future__ import annotations

import ast
import json
from pathlib import Path

import pytest

from index.build_index import IndexStore
from server.app import create_app
from server.config import Settings

APP_ROOT = Path(__file__).resolve().parent.parent

TEMPLATE = (
    "## 一句话职责\n临时夹具页，用于验证入库行为。\n\n"
    "## 机制是什么\n它把固定文本切成若干块，不涉及任何真实机制。\n\n"
    "## 怎么实现的\n直接复用仓库里的切块函数，不引入额外依赖。\n\n"
    "## 关键取舍\n没有取舍，它只是测试用的临时夹具。\n\n"
    "## 源码落点\n没有源码落点，这一节仅用于占位以满足切块长度要求。\n\n"
    "## 参考\nhttps://example.com/a\nhttps://example.com/b\n\n"
    "## 与主核 / AI 的关系\n无关。\n"
)


def write_page(directory: Path, project: str, feature: str, title: str) -> None:
    text = (
        f"---\nproject: {project}\nfeature: {feature}\npage_type: feature\n"
        f"schema_version: 1\nrefs: []\n---\n\n# {title}\n\n{TEMPLATE}"
    )
    (directory / f"a01-{feature}.md").write_text(text, encoding="utf-8")


def make_store(data: Path) -> IndexStore:
    return IndexStore(data, "missing/local/model", offline=True, use_lancedb=False, chunk_min=1)


def test_ingest_is_idempotent(tmp_path):
    corpus = tmp_path / "corpus"
    write_page(corpus / "one", "one", "cache", "缓存") if (corpus / "one").mkdir(parents=True) is None else None
    store = make_store(tmp_path / "data")
    first = store.build_project(corpus, "one", {"wiki_lint": 0, "wiki_refs": 0})
    second = store.build_project(corpus, "one", {"wiki_lint": 0, "wiki_refs": 0})
    assert first["block_count"] == second["block_count"]
    assert first["block_ids"] == second["block_ids"]
    assert len(first["block_ids"]) == len(set(first["block_ids"]))


def test_rebuild_one_library_keeps_others(tmp_path):
    corpus = tmp_path / "corpus"
    data = tmp_path / "data"
    for name, feature in (("one", "cache"), ("two", "index")):
        (corpus / name).mkdir(parents=True)
        write_page(corpus / name, name, feature, feature)
    store = make_store(data)
    store.build_project(corpus, "one", {"wiki_lint": 0, "wiki_refs": 0})
    store.build_project(corpus, "two", {"wiki_lint": 0, "wiki_refs": 0})
    before = json.loads((data / "projects" / "two" / "metadata.json").read_text(encoding="utf-8"))
    store.build_project(corpus, "one", {"wiki_lint": 0, "wiki_refs": 0})
    after = json.loads((data / "projects" / "two" / "metadata.json").read_text(encoding="utf-8"))
    assert before == after
    assert set(before["block_ids"]).isdisjoint(
        json.loads((data / "projects" / "one" / "metadata.json").read_text(encoding="utf-8"))["block_ids"])


def test_failing_gate_is_reported_as_fail(tmp_path):
    corpus = tmp_path / "corpus"
    data = tmp_path / "data"
    (corpus / "bad").mkdir(parents=True)
    write_page(corpus / "bad", "bad", "broken", "坏页")
    store = make_store(data)
    store.build_project(corpus, "bad", {"wiki_lint": 1, "wiki_refs": 127})
    client = __import__("fastapi.testclient", fromlist=["TestClient"]).TestClient(
        create_app(Settings(corpus_root=corpus, data_root=data, use_lancedb=False,
                            embedding_model="missing/local/model")))
    project = client.get("/api/projects").json()["projects"][0]
    assert project["validation"] == "FAIL"
    assert project["gates"] == {"wiki_lint": 1, "wiki_refs": 127}


def test_hash_fallback_is_disclosed(tmp_path):
    client = __import__("fastapi.testclient", fromlist=["TestClient"]).TestClient(
        create_app(Settings(corpus_root=tmp_path / "corpus", data_root=tmp_path / "data",
                            use_lancedb=False, embedding_model="missing/local/model")))
    health = client.get("/api/health").json()
    assert health["embedding_backend"] == "hash-fallback"
    assert "不是语义嵌入" in health["embedding_warning"]


def test_no_core_kernel_imports():
    """验收第 7 条：应用任何模块都不得 import 主核 engine/db。"""
    offenders = []
    for path in APP_ROOT.rglob("*.py"):
        if "__pycache__" in path.parts:
            continue
        tree = ast.parse(path.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            modules = []
            if isinstance(node, ast.Import):
                modules = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom) and node.module:
                modules = [node.module]
            for module in modules:
                if module == "engine" or module.startswith(("engine.", "engine/db")):
                    offenders.append((str(path.relative_to(APP_ROOT)), module))
    assert offenders == []
