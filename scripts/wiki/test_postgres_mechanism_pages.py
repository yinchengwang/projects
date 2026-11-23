# -*- coding: utf-8 -*-
"""postgres 151 页的低层契约测试：refs 必须与 manifest 完全一致。

writer 只负责正文，refs 是写作合同的一部分。测试扫描全部机制 feature 页，解析
front-matter，并逐页比对 page_file / feature / title / refs 与七个必含 H2，
避免只看 lint 通过却悄悄把 writer 的 refs 或章节结构改偏。
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

REQUIRED_H2 = [
    "一句话职责",
    "机制是什么",
    "怎么实现的",
    "关键取舍",
    "源码落点",
    "参考",
    "与主核 / AI 的关系",
]


class TestPostgresMechanismPages(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = load_manifest(MANIFEST_PATH)
        cls.pages = list(cls.manifest["pages"])

    def test_all_groups_have_151_mechanisms(self):
        self.assertEqual(len(self.pages), 151)

    def test_every_manifest_page_exists(self):
        for page in self.pages:
            path = os.path.join(POSTGRES_PAGES, page["page_file"])
            self.assertTrue(os.path.isfile(path), page["page_file"])

    def test_page_frontmatter_matches_manifest_exactly(self):
        for page in self.pages:
            path = os.path.join(POSTGRES_PAGES, page["page_file"])
            with open(path, encoding="utf-8") as stream:
                metadata, body = parse_frontmatter(stream.read())
            # front-matter 与正文之间有一个空行，比对前先剥掉前导空行。
            body_lines = body.lstrip("\n").splitlines()
            with self.subTest(page=page["page_file"]):
                self.assertEqual(metadata.get("project"), "postgres")
                self.assertEqual(metadata.get("feature"), page["feature"])
                self.assertEqual(metadata.get("page_type"), "feature")
                self.assertEqual(metadata.get("schema_version"), 1)
                self.assertEqual(metadata.get("refs"), page["refs"])
                self.assertEqual(body_lines[0], "# " + page["title"])
                # 章节顺序不在契约里：只要求七个 H2 齐备且各出现一次。
                sections = [line[3:] for line in body_lines if line.startswith("## ")]
                self.assertEqual(sorted(sections), sorted(REQUIRED_H2))

    def test_group_counts_match_specs(self):
        expected = {
            "A-存储与IPC": 20,
            "B-事务与WAL": 19,
            "C-堆表与元组": 15,
            "D-索引族": 17,
            "E-优化器": 13,
            "F-执行器": 13,
            "G-目录与缓存": 7,
            "H-内存与排序": 5,
            "I-复制与高可用": 10,
            "J-服务端与命令面": 12,
            "K-SQL处理与安全": 11,
            "L-SQL类型与内建函数": 9,
        }
        actual = {}
        for page in self.pages:
            actual[page["feature_group"]] = actual.get(page["feature_group"], 0) + 1
        self.assertEqual(actual, expected)

    def test_page_files_are_unique(self):
        names = [page["page_file"] for page in self.pages]
        self.assertEqual(len(names), len(set(names)))

    def test_features_are_globally_unique(self):
        features = [page["feature"] for page in self.pages]
        self.assertEqual(len(features), len(set(features)))
