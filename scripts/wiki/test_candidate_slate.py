# -*- coding: utf-8 -*-
"""候选审阅矩阵解析测试：先红后绿。

守住三个语义边界：
1. 从 review markdown 解析出的候选数必须等于 118（DB 54 + AI 64）；
2. 每条候选都有 name / model / repo 三个字段，且 repo 是 GitHub URL；
3. 候选名称与规范化 repo 在整批内唯一，且不与现有 catalog 冲突。
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from candidate_slate import parse_review, normalize_repo
from wiki_catalog import parse_catalog

PROJECTS_ROOT = Path(__file__).resolve().parents[2]
REVIEW_PATH = PROJECTS_ROOT / "docs" / "wiki" / "project-catalog-review.md"
CATALOG_PATH = PROJECTS_ROOT / "docs" / "wiki" / "catalog-projects.yml"


class TestParseReview(unittest.TestCase):
    def test_total_candidates_is_118(self):
        self.assertEqual(len(parse_review(REVIEW_PATH)), 118)

    def test_database_and_ai_split(self):
        entries = parse_review(REVIEW_PATH)
        db = [e for e in entries if not e["model"].startswith("ai/")]
        ai = [e for e in entries if e["model"].startswith("ai/")]
        self.assertEqual(len(db), 54)
        self.assertEqual(len(ai), 64)

    def test_every_candidate_has_required_fields(self):
        for entry in parse_review(REVIEW_PATH):
            self.assertEqual(set(entry), {"name", "model", "repo"}, entry)
            self.assertTrue(entry["name"])
            self.assertTrue(entry["model"])

    def test_repos_are_github_urls(self):
        for entry in parse_review(REVIEW_PATH):
            self.assertTrue(entry["repo"].startswith("https://github.com/"), entry)

    def test_names_are_unique(self):
        names = [e["name"].lower() for e in parse_review(REVIEW_PATH)]
        self.assertEqual(len(names), len(set(names)))

    def test_repos_are_unique(self):
        repos = [normalize_repo(e["repo"]) for e in parse_review(REVIEW_PATH)]
        self.assertEqual(len(repos), len(set(repos)))

    def test_no_conflict_with_catalog(self):
        catalog = parse_catalog(CATALOG_PATH.read_text(encoding="utf-8"))
        names = {e["name"].lower() for e in catalog}
        repos = {normalize_repo(e["repo"]) for e in catalog}
        for entry in parse_review(REVIEW_PATH):
            self.assertNotIn(entry["name"].lower(), names, entry)
            self.assertNotIn(normalize_repo(entry["repo"]), repos, entry)


if __name__ == "__main__":
    unittest.main()
