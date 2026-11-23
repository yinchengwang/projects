# -*- coding: utf-8 -*-
"""純 NumPy BM25：中英混排，以穩定 block_id 關聯元數據。"""
from __future__ import annotations

from collections import Counter
from math import log
from typing import Any

from core.embedder import tokenize


class BM25:
    """BM25Okapi 小型自實現，適合百級頁、千級塊。"""

    def __init__(self, documents: list[dict[str, Any]], k1: float = 1.5, b: float = 0.75):
        self.documents = documents
        self.k1 = k1
        self.b = b
        self.tokens = [tokenize(doc.get("text", "")) for doc in documents]
        self.lengths = [len(value) for value in self.tokens]
        self.avg_length = sum(self.lengths) / max(1, len(self.lengths))
        self.term_freqs = [Counter(value) for value in self.tokens]
        self.doc_freq: Counter[str] = Counter()
        for terms in self.term_freqs:
            self.doc_freq.update(terms.keys())

    def search(self, query: str, limit: int = 10) -> list[tuple[str, float]]:
        terms = tokenize(query)
        document_count = len(self.documents)
        scores: list[tuple[str, float]] = []
        for index, document in enumerate(self.documents):
            score = 0.0
            for term in terms:
                frequency = self.term_freqs[index].get(term, 0)
                if not frequency:
                    continue
                document_frequency = self.doc_freq[term]
                inverse_frequency = log(1 + (document_count - document_frequency + 0.5)
                                        / (document_frequency + 0.5))
                normalization = frequency + self.k1 * (
                    1 - self.b + self.b * self.lengths[index] / max(1, self.avg_length))
                score += inverse_frequency * frequency * (self.k1 + 1) / normalization
            if score > 0:
                scores.append((str(document["block_id"]), score))
        return sorted(scores, key=lambda item: (-item[1], item[0]))[:limit]
