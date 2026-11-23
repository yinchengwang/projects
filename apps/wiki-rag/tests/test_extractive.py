from core.extractive import answer


def test_answer_citations_are_from_original_blocks():
    docs = {"p/a.md#机制": {"block_id": "p/a.md#机制", "project": "p", "page_file": "a.md",
            "section": "机制", "title": "测试", "text": "原文段落。", "refs": []}}
    result = answer("问题", [{"block_id": "p/a.md#机制", "score": 0.1}], docs)
    assert "原文段落。" in result["answer"]
    assert result["citations"][0]["block_id"] in docs
    assert result["citations"][0]["preview"] == "原文段落。"


def test_no_hits_abstains():
    result = answer("无关问题", [], {})
    assert result["abstained"] is True
    assert result["citations"] == []
