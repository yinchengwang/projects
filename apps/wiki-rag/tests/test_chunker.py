from core.chunker import chunk_page


def test_chunks_keep_metadata_merge_short_and_split_long():
    meta = {"project": "p", "feature": "f", "page_type": "feature", "schema_version": 1,
            "refs": [{"path": "src/x.c", "symbols": ["foo"]}]}
    body = "## 一句话职责\n短。\n\n## 机制是什么\n" + "机制内容。" * 120 + "\n\n## 关键取舍\n很短。"
    chunks = chunk_page(meta, body, "a01-f.md", "标题", min_chars=20, max_chars=100)
    assert len(chunks) > 2
    assert chunks[0]["block_id"].startswith("p/a01-f.md#")
    assert chunks[0]["feature"] == "f"
    assert chunks[0]["page_type"] == "feature"
    assert chunks[0]["schema_version"] == 1
    assert chunks[0]["refs"] == meta["refs"]
    assert all(len(item["text"]) <= 100 for item in chunks)


def test_overview_marked():
    chunks = chunk_page({"project": "p", "page_type": "overview"}, "## 机制是什么\n" + "内容" * 100,
                        "_index.md", "总览", min_chars=1)
    assert chunks and chunks[0]["is_overview"]
