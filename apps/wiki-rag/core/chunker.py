# -*- coding: utf-8 -*-
"""按 H2 切塊，合併短段落並按原文段落二次切分。"""
from __future__ import annotations

import re
from typing import Any

_SECTION_RE = re.compile(r"^##\s+(.+?)\s*$", re.MULTILINE)


def _split_sections(body: str) -> list[tuple[str, str]]:
    matches = list(_SECTION_RE.finditer(body))
    sections = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(body)
        sections.append((match.group(1).strip(), body[match.end():end].strip()))
    return sections


def _split_long(text: str, max_chars: int) -> list[str]:
    """超長段落按句末 / 換行拆，最後才按字元硬切。"""
    pieces = re.split(r"(?<=[。！？!?])|(?<=\n)", text)
    output = []
    current = ""
    for piece in pieces:
        if not piece:
            continue
        while len(piece) > max_chars:
            if current.strip():
                output.append(current.strip())
                current = ""
            output.append(piece[:max_chars].strip())
            piece = piece[max_chars:]
        if current and len(current) + len(piece) > max_chars:
            output.append(current.strip())
            current = ""
        current = f"{current}{piece}" if current else piece
    if current.strip():
        output.append(current.strip())
    return output


def _paragraph_parts(text: str, max_chars: int, min_chars: int) -> list[str]:
    """保持原文順序，把短段落打包進相鄰塊，避免短句孤立召回。"""
    paragraphs = [part.strip() for part in re.split(r"\n\s*\n", text) if part.strip()]
    packed = []
    current = ""
    for paragraph in paragraphs:
        candidate = f"{current}\n\n{paragraph}" if current else paragraph
        if current and len(candidate) > max_chars:
            packed.append(current)
            current = paragraph
        else:
            current = candidate
    if current:
        packed.append(current)

    split = []
    for paragraph in packed:
        split.extend(_split_long(paragraph, max_chars) if len(paragraph) > max_chars else [paragraph])

    result = []
    queue = list(split)
    while queue:
        piece = queue.pop(0)
        if len(piece) >= min_chars or not queue:
            result.append(piece)
            continue
        adjacent = queue.pop(0)
        merged = _split_long(f"{piece}\n\n{adjacent}", max_chars)
        result.append(merged[0])
        queue[0:0] = merged[1:]
    return result


def chunk_page(meta: dict[str, Any], body: str, page_file: str, title: str,
               min_chars: int = 160, max_chars: int = 1200) -> list[dict[str, Any]]:
    """H2 是塊元資料；短章節與鄰章合併，超長章節按段落切。"""
    sections = _split_sections(body)
    if not sections:
        return []

    grouped: list[tuple[list[str], str]] = []
    pending: list[tuple[str, str]] = []
    for section, content in sections:
        if len(content) < min_chars:
            pending.append((section, content))
            continue
        names = [name for name, _ in pending] + [section]
        text = "\n\n".join(value for _, value in [*pending, (section, content)] if value)
        grouped.append((names, text))
        pending.clear()
    if pending:
        names = [name for name, _ in pending]
        text = "\n\n".join(value for _, value in pending if value)
        if grouped:
            prior_names, prior_text = grouped[-1]
            grouped[-1] = (prior_names + names, "\n\n".join(v for v in (prior_text, text) if v))
        elif text:
            grouped.append((names, text))

    result = []
    project = str(meta.get("project", ""))
    for names, text in grouped:
        section_name = " / ".join(names)
        for part, content in enumerate(_paragraph_parts(text, max_chars, min_chars), 1):
            result.append({
                "block_id": f"{project}/{page_file}#{section_name}",
                "project": project,
                "feature": str(meta.get("feature", "")),
                "page_type": str(meta.get("page_type", "")),
                "schema_version": meta.get("schema_version"),
                "section": section_name,
                "title": title,
                "refs": meta.get("refs", []),
                "is_overview": meta.get("page_type") == "overview",
                "text": content,
                "part": part,
            })
    return result
