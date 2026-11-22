# -*- coding: utf-8 -*-
"""wiki 文档契约的常量定义。

这是全仓唯一权威：改这里的任何一项等于改契约，必须同时升 SCHEMA_VERSION，
并在 docs/wiki-schema.md 记一节「本次改了什么、为什么」。
设计文档 §5.4 把升版预算定为 3 次。
"""

SCHEMA_VERSION = 1

# 六个必含章节（缺一即 FAIL）。顺序即 _index.md 与 RAG 切块的默认顺序。
REQUIRED_SECTIONS = (
    "一句话职责",
    "机制是什么",
    "怎么实现的",
    "关键取舍",
    "源码落点",
    "参考",
)

# 一个条件章节：内容可为空，但标题必须在。
CONDITIONAL_SECTIONS = ("与主核 / AI 的关系",)

# 特性页（page_type=feature|compare）必填字段。
REQUIRED_FIELDS = (
    "project",
    "feature",
    "page_type",
    "schema_version",
    "refs",
)

# 项目总览页（page_type=overview，即 _index.md）必填字段。
INDEX_REQUIRED_FIELDS = (
    "project",
    "page_type",
    "display_name",
    "repo",
    "license",
    "language",
    "stars",
    "stars_as_of",
)

PAGE_TYPES = ("overview", "feature", "compare")

# page_type=compare 的页必须放在该名字的目录下。
COMPARE_DIR_NAME = "_compare"


def required_fields_for(page_type: str) -> tuple[str, ...]:
    """按页面类型返回必填字段。未知类型按特性页口径（从严）。"""
    if page_type == "overview":
        return INDEX_REQUIRED_FIELDS
    return REQUIRED_FIELDS
