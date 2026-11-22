# scripts/wiki/test_wiki_schema_doc.py
# -*- coding: utf-8 -*-
"""契约文档与常量必须一致：文档说漏了章节名，等于契约漂移。"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wiki_schema

SCHEMA_DOC = os.path.normpath(
    os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "docs", "wiki", "wiki-schema.md"
    )
)


class TestSchemaDocMatchesConstants(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not os.path.isfile(SCHEMA_DOC):
            raise AssertionError("wiki-schema.md 必须存在: %s" % SCHEMA_DOC)
        with open(SCHEMA_DOC, encoding="utf-8") as f:
            cls.doc = f.read()

    def test_doc_mentions_schema_version(self):
        self.assertIn("SCHEMA_VERSION = %d" % wiki_schema.SCHEMA_VERSION, self.doc)

    def test_doc_mentions_every_required_section(self):
        for name in wiki_schema.REQUIRED_SECTIONS:
            self.assertIn(name, self.doc, "契约文档漏了必含章节: %s" % name)

    def test_doc_mentions_conditional_section(self):
        for name in wiki_schema.CONDITIONAL_SECTIONS:
            self.assertIn(name, self.doc, "契约文档漏了条件章节: %s" % name)

    def test_doc_mentions_every_required_field(self):
        for field in wiki_schema.REQUIRED_FIELDS:
            self.assertIn(field, self.doc, "契约文档漏了必填字段: %s" % field)

    def test_doc_mentions_every_index_field(self):
        for field in wiki_schema.INDEX_REQUIRED_FIELDS:
            self.assertIn(field, self.doc, "契约文档漏了索引页字段: %s" % field)

    def test_doc_mentions_every_page_type(self):
        for t in wiki_schema.PAGE_TYPES:
            self.assertIn(t, self.doc, "契约文档漏了页面类型: %s" % t)

    def test_doc_states_no_line_numbers_rule(self):
        self.assertIn("禁止行号", self.doc)

    def test_doc_states_hard_failure_rule(self):
        self.assertIn("硬失败", self.doc)

    def test_doc_states_bump_budget(self):
        self.assertIn("3 次", self.doc)


if __name__ == "__main__":
    unittest.main()
