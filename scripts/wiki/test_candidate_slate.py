# -*- coding: utf-8 -*-
"""候选审阅矩阵解析测试：先红后绿。

守住三个语义边界：
1. 从 review markdown 解析出的候选数必须等于 118（DB 54 + AI 64）；
2. 每条候选都有 name / model / repo 三个字段，且 repo 是 GitHub URL；
3. 候选名称与规范化 repo 在整批内唯一，且已全部并入 catalog（同名同 repo）。
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

    def test_candidates_are_merged_into_catalog(self):
        """候选矩阵已于 2026-11-29 并入 catalog，所以现在是「必须在册」而不是「不得冲突」。

        合并前这个测试守的是「候选与既有 catalog 零 name/repo 冲突」；合并后
        同一批候选的 repo 必然出现在 catalog 里，于是断言方向必须反过来：
        每条候选都要能在 catalog 里找到同名同 repo 的条目。
        """
        catalog = parse_catalog(CATALOG_PATH.read_text(encoding="utf-8"))
        names = {e["name"].lower(): e for e in catalog}
        repos = {normalize_repo(e["repo"]): e for e in catalog}
        for entry in parse_review(REVIEW_PATH):
            by_name = names.get(entry["name"].lower())
            self.assertIsNotNone(by_name, entry)
            self.assertEqual(normalize_repo(by_name["repo"]), normalize_repo(entry["repo"]), entry)
            self.assertIn(normalize_repo(entry["repo"]), repos, entry)

    def test_catalog_entry_count(self):
        """catalog 目标 270 项；剔除失效条目后由替补补齐。"""
        catalog = parse_catalog(CATALOG_PATH.read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(catalog), 268)
        self.assertLessEqual(len(catalog), 270)


if __name__ == "__main__":
    unittest.main()
