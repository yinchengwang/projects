# -*- coding: utf-8 -*-
"""状态账本的完整性守护：防止条目数量或阶段分布被悄悄改动。

账本是「270 项 wiki 收集」的进度事实来源，因此它本身必须有测试兜住：
总数 270、已完成 4 项、四个项目别名到 wiki 目录、其余条目都有下一动作。
"""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_project_status import parse_status, validate_status

PROJECTS_ROOT = Path(__file__).resolve().parents[2]
STATUS_PATH = PROJECTS_ROOT / "docs" / "wiki" / "wiki-project-status.yml"


class TestLedger(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.entries = parse_status(STATUS_PATH.read_text(encoding="utf-8"))
        cls.by_slug = {e["slug"]: e for e in cls.entries}

    def test_total_is_270(self):
        self.assertEqual(len(self.entries), 270)

    def test_database_and_ai_split_is_150_and_120(self):
        db = [e for e in self.entries if not e["model"].startswith("ai/")]
        ai = [e for e in self.entries if e["model"].startswith("ai/")]
        self.assertEqual(len(db), 150)
        self.assertEqual(len(ai), 120)

    def test_ledger_is_valid(self):
        self.assertEqual(validate_status(self.entries), [])

    def test_four_projects_are_indexed(self):
        indexed = sorted(e["slug"] for e in self.entries if e["state"] == "indexed")
        self.assertEqual(indexed, ["mysql-server", "postgres", "redis", "sqlite"])

    def test_completed_projects_keep_catalog_alias(self):
        self.assertEqual(self.by_slug["sqlite"]["aliases"], ["sqlite3"])
        self.assertEqual(self.by_slug["mysql-server"]["aliases"], ["mysql"])

    def test_every_entry_has_next_action(self):
        for entry in self.entries:
            self.assertTrue(entry["next_action"], entry["slug"])

    def test_slugs_match_wiki_directories_when_present(self):
        wiki_root = PROJECTS_ROOT / "docs" / "wiki" / "projects"
        for slug in ("postgres", "redis", "sqlite", "mysql-server"):
            self.assertTrue((wiki_root / slug).is_dir(), slug)


if __name__ == "__main__":
    unittest.main()
