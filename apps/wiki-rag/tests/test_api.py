from fastapi.testclient import TestClient

from index.build_index import IndexStore
from server.app import create_app
from server.config import Settings


def test_health_and_static_page(tmp_path):
    settings = Settings(corpus_root=tmp_path / "corpus", data_root=tmp_path / "data", use_lancedb=False,
                        embedding_model="missing/local/model")
    app = create_app(settings)
    client = TestClient(app)
    assert client.get("/api/health").status_code == 200
    page = client.get("/")
    assert page.status_code == 200
    assert all(name in page.text for name in ("left-rail", "center-pane", "right-pane"))


def test_search_and_ask(tmp_path):
    corpus, data = tmp_path / "corpus", tmp_path / "data"
    project = corpus / "sample"
    project.mkdir(parents=True)
    page = project / "a01-buffer-pool.md"
    page.write_text("---\nproject: sample\nfeature: buffer-pool\npage_type: feature\nschema_version: 1\nrefs: []\n---\n\n# 緩衝池\n\n## 機制是什么\n共享緩衝池按 clock sweep 選擇可替換候選頁。", encoding="utf-8")
    store = IndexStore(data, "missing/local/model", offline=True, use_lancedb=False, chunk_min=1)
    meta = store.build_project(corpus, "sample", {"wiki_lint": 0, "wiki_refs": 0})
    app = create_app(Settings(corpus_root=corpus, data_root=data, use_lancedb=False, chunk_min=1,
                              embedding_model="missing/local/model"))
    app.state.documents.update(store.load_project("sample")["by_id"])
    client = TestClient(app)
    assert client.get("/api/projects").json()["projects"][0]["validation"] == "PASS"
    assert client.get("/api/projects/sample").json()["block_count"] == meta["block_count"]
    search = client.post("/api/search", json={"query": "緩衝池", "projects": ["sample"]})
    assert search.status_code == 200
    assert search.json()["results"][0]["page_file"] == "a01-buffer-pool.md"
    ask = client.post("/api/ask", json={"query": "緩衝池如何選候選", "projects": ["sample"]})
    assert ask.status_code == 200
    assert ask.json()["citations"][0]["block_id"] in app.state.documents


def test_missing_project_and_bad_query(tmp_path):
    settings = Settings(corpus_root=tmp_path / "corpus", data_root=tmp_path / "data", use_lancedb=False)
    client = TestClient(create_app(settings))
    assert client.get("/api/projects/not-here").status_code == 404
    assert client.post("/api/search", json={"query": ""}).status_code == 422
