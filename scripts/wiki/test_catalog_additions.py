# -*- coding: utf-8 -*-
"""catalog_additions 的测试：先红后绿。

守住三个语义边界：
1. 每条补充条目字段齐全（恰好八个），且都能过 validate_entry；
2. stars 只来自 GitHub REST API 实取，不留占位值（不编数）；
3. apply_additions 按 name 去重且 existing 优先，结果按 tier/model/name 排序。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from catalog_additions import ADDITIONS, apply_additions
from wiki_catalog import validate_entry


class TestAdditionsData(unittest.TestCase):
    def test_additions_is_non_empty(self):
        self.assertGreater(len(ADDITIONS), 0)

    def test_every_addition_has_eight_fields(self):
        for entry in ADDITIONS:
            self.assertEqual(len(entry), 8, entry)
            self.assertEqual(set(entry), {
                "name", "model", "tier", "repo", "license",
                "language", "stars", "stars_as_of",
            })

    def test_every_addition_validates(self):
        for entry in ADDITIONS:
            self.assertEqual(validate_entry(entry), [], entry)

    def test_names_are_unique(self):
        names = [e["name"] for e in ADDITIONS]
        self.assertEqual(len(names), len(set(names)))

    def test_stars_are_non_negative_ints(self):
        for entry in ADDITIONS:
            self.assertIsInstance(entry["stars"], int)
            self.assertGreaterEqual(entry["stars"], 0)

    def test_tiers_are_known(self):
        for entry in ADDITIONS:
            self.assertIn(entry["tier"], ("T1", "T2", "T3"))


class TestNoPlaceholderData(unittest.TestCase):
    """build_catalog 的占位值（language=unknown / stars=0 / 1970-01-01）一个都不许出现。"""

    def test_stars_as_of_is_the_fetch_date(self):
        for entry in ADDITIONS:
            self.assertEqual(entry["stars_as_of"], "2026-11-22", entry["name"])

    def test_no_entry_keeps_placeholder_marks(self):
        for entry in ADDITIONS:
            self.assertNotEqual(entry["stars_as_of"], "1970-01-01", entry["name"])
            self.assertNotEqual(entry["language"], "unknown", entry["name"])

    def test_stars_are_positive(self):
        # 候选都是知名库，实取必为正数；为 0 说明取数链路坏了
        for entry in ADDITIONS:
            self.assertGreater(entry["stars"], 0, entry["name"])

    def test_repos_are_github_urls(self):
        for entry in ADDITIONS:
            self.assertTrue(entry["repo"].startswith("https://github.com/"), entry["name"])


class TestApplyAdditions(unittest.TestCase):
    def test_appends_new_entries(self):
        existing = [{"name": "postgres", "model": "relational", "tier": "T1",
                     "repo": "https://github.com/postgres/postgres",
                     "license": "PostgreSQL", "language": "C",
                     "stars": 13500, "stars_as_of": "2026-11-22"}]
        merged = apply_additions(existing, ADDITIONS[:2])
        self.assertEqual(len(merged), 3)

    def test_does_not_duplicate_existing(self):
        existing = [ADDITIONS[0]]
        merged = apply_additions(existing, ADDITIONS[:2])
        names = [e["name"] for e in merged]
        self.assertEqual(len(names), len(set(names)))

    def test_existing_wins_on_conflict(self):
        dup = dict(ADDITIONS[0], stars=1)
        existing = [dict(ADDITIONS[0], stars=999)]
        merged = apply_additions(existing, [dup])
        by_name = {e["name"]: e for e in merged}
        self.assertEqual(by_name[ADDITIONS[0]["name"]]["stars"], 999)

    def test_sorted_by_tier_model_name(self):
        merged = apply_additions([], ADDITIONS)
        keys = [(e["tier"], e["model"], e["name"]) for e in merged]
        self.assertEqual(keys, sorted(keys))

    def test_does_not_mutate_inputs(self):
        existing = [dict(ADDITIONS[0], stars=999)]
        snapshot = [dict(e) for e in existing]
        apply_additions(existing, ADDITIONS[:2])
        self.assertEqual(existing, snapshot)

    def test_empty_additions_returns_existing(self):
        existing = [dict(ADDITIONS[0])]
        self.assertEqual(apply_additions(existing, []), existing)


if __name__ == "__main__":
    unittest.main()
