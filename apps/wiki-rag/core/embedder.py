# -*- coding: utf-8 -*-
"""纯 NumPy 嵌入，支持可选本地句向量模型。"""
from __future__ import annotations

import hashlib
import os
import re
from typing import Iterable

import numpy as np

_TOKEN_RE = re.compile(r"[A-Za-z0-9_]+|[\u3400-\u9fff]+")


def tokenize(text: str) -> list[str]:
    """英文按词，中文按相邻双字切分。"""
    tokens = []
    for item in _TOKEN_RE.findall(text.lower()):
        if item[0].isascii():
            tokens.append(item)
        elif len(item) == 1:
            tokens.append(item)
        else:
            tokens.extend(item[index:index + 2] for index in range(len(item) - 1))
    return tokens


def hash_embed(text: str, dimension: int = 384) -> np.ndarray:
    """稳定 SHA-256 n-gram 哈希向量；仅作演示降级，不是语义嵌入。"""
    vector = np.zeros(dimension, dtype=np.float32)
    for token in tokenize(re.sub(r"\s+", " ", text.lower()).strip()):
        features = [token]
        for width in (2, 3):
            features.extend(token[index:index + width] for index in range(max(0, len(token) - width + 1)))
        for feature in features:
            digest = hashlib.sha256(feature.encode("utf-8")).digest()
            position = int.from_bytes(digest[:4], "little") % dimension
            vector[position] += 1.0 if digest[4] & 1 else -1.0
    norm = float(np.linalg.norm(vector))
    if norm:
        vector /= norm
    return vector


class Embedder:
    """本地模型可用则加载；不可用时公开标记哈希降级。"""

    def __init__(self, model_name: str = "hash-ngram-demo", offline: bool = True):
        self.model_name = model_name
        self.backend = "hash-fallback"
        self.warning = "未加载语义嵌入模型，当前使用演示用确定性哈希嵌入，不是语义嵌入。"
        self.dimension = 384
        self._model = None
        if model_name.startswith("hash-"):
            self.warning = "当前明确配置为演示用哈希嵌入，不是语义嵌入。"
            return
        try:
            if offline:
                os.environ["HF_HUB_OFFLINE"] = "1"
                os.environ["TRANSFORMERS_OFFLINE"] = "1"
            from sentence_transformers import SentenceTransformer
            self._model = SentenceTransformer(model_name, local_files_only=offline)
            self.backend = "sentence-transformers"
            self.dimension = int(self._model.get_sentence_embedding_dimension())
            self.warning = None
        except Exception as exc:
            self.warning = f"语义模型不可用（{type(exc).__name__}），当前使用演示用确定性哈希嵌入，不是语义嵌入。"

    def encode(self, texts: Iterable[str]) -> np.ndarray:
        values = list(texts)
        if not values:
            return np.empty((0, self.dimension), dtype=np.float32)
        if self._model is not None:
            vectors = self._model.encode(values, normalize_embeddings=True, convert_to_numpy=True, show_progress_bar=False)
            return np.asarray(vectors, dtype=np.float32)
        return np.vstack([hash_embed(value, self.dimension) for value in values])
