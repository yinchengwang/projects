# -*- coding: utf-8 -*-
"""從檢索結果抽取原文，引用由後端按塊 ID 組成。"""
from __future__ import annotations

from typing import Any


def answer(question: str, hits: list[dict[str, Any]],
           documents: dict[str, dict[str, Any]], limit: int = 4) -> dict[str, Any]:
    """按融合分數取塊並原文拼接，不執行模型改寫。"""
    selected = []
    seen: set[str] = set()
    for hit in sorted(hits, key=lambda row: (-row.get("score", 0.0), row["block_id"])):
        block_id = hit["block_id"]
        if block_id not in documents or block_id in seen:
            continue
        seen.add(block_id)
        selected.append((hit, documents[block_id]))
        if len(selected) >= limit:
            break
    if not selected:
        return {
            "answer": "未检索到相关原文，请换一种问法或检查所选语料库。",
            "citations": [],
            "abstained": True,
        }

    citations = []
    passages = []
    for index, (hit, document) in enumerate(selected, 1):
        citation = {
            "id": index,
            "project": document["project"],
            "page_file": document["page_file"],
            "section": document["section"],
            "block_id": document["block_id"],
            "title": document["title"],
            "preview": document["text"][:240],
            "text": document["text"],
            "score": hit.get("score", 0.0),
            "refs": document.get("refs", []),
        }
        citations.append(citation)
        passages.append(f"[{document['project']}/{document['page_file']} § {document['section']}]\n{document['text']}")
    return {"answer": "\n\n".join(passages), "citations": citations, "abstained": False}
