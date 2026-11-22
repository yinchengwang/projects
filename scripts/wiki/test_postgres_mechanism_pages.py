# -*- coding: utf-8 -*-
"""A 组 20 页的低层契约测试：refs 必须与 manifest 完全一致。

writer 只负责正文，refs 是写作合同的一部分。测试扫描 A 组 feature 页，解析
front-matter，并逐页比对 page_file / feature / title / refs，避免只看 lint 通过却
悄悄把 writer 的 refs 改偏。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_frontmatter import parse_frontmatter
from wiki_manifest import load_manifest

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
POSTGRES_PAGES = os.path.join(PROJECT_ROOT, "docs", "wiki", "projects", "postgres")
MANIFEST_PATH = os.path.join(POSTGRES_PAGES, "_manifest.yml")


class TestPostgresMechanismPages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_manifest(MANIFEST_PATH)
        cls.pages = [page for page in cls.manifest["pages"]
                     if page["feature_group"] == "A-存储与IPC"]

    def test_a_group_has_twenty_mechanisms(self):
        self.assertEqual(len(self.pages), 20)

    def test_every_a_group_page_exists(self):
        for page in self.pages:
            path = os.path.join(POSTGRES_PAGES, page["page_file"])
            self.assertTrue(os.path.isfile(path), page["page_file"])

    def test_page_frontmatter_matches_manifest_exactly(self):
        for page in self.pages:
            path = os.path.join(POSTGRES_PAGES, page["page_file"])
            with open(path, encoding="utf-8") as stream:
                metadata, _body = parse_frontmatter(stream.read())
            with self.subTest(page=page["page_file"]):
                self.assertEqual(metadata.get("project"), "postgres")
                self.assertEqual(metadata.get("feature"), page["feature"])
                self.assertEqual(metadata.get("page_type"), "feature")
                self.assertEqual(metadata.get("schema_version"), 1)
                self.assertEqual(metadata.get("refs"), page["refs"])
