# -*- coding: utf-8 -*-
"""catalog stars 回填的回归测试：占位口径、断点续跑与不伪造。"""
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fill_stars as fs


class TestPlaceholderDetection(unittest.TestCase):
    def test_placeholder_is_zero_with_epoch_date(self):
        self.assertTrue(fs.is_placeholder({"stars": 0, "stars_as_of": "1970-01-01"}))

    def test_real_zero_stars_is_not_placeholder(self):
        # 真实取到 0 star 的仓库（如 db-bench）不再是占位，不能被反复重取。
        self.assertFalse(fs.is_placeholder({"stars": 0, "stars_as_of": "2026-11-23"}))

    def test_nonzero_stars_is_not_placeholder(self):
        self.assertFalse(fs.is_placeholder({"stars": 1, "stars_as_of": "1970-01-01"}))


class TestRepoSlug(unittest.TestCase):
    def test_github_https_url(self):
        self.assertEqual(fs.repo_slug("https://github.com/microsoft/autogen"), "microsoft/autogen")

    def test_github_url_with_git_suffix_and_trailing_slash(self):
        self.assertEqual(fs.repo_slug("https://github.com/postgres/postgres.git"), "postgres/postgres")
        self.assertEqual(fs.repo_slug("https://github.com/postgres/postgres/"), "postgres/postgres")

    def test_non_github_host_returns_none(self):
        # gitee 等非 GitHub 托管：明确返回 None，保持占位而不是猜一个 slug。
        self.assertIsNone(fs.repo_slug("https://gitee.com/opengauss/openGauss-server"))


class TestCacheRoundtrip(unittest.TestCase):
    def test_cache_save_and_load(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cache.json"
            cache = {"version": 1, "entries": {"a/b": {"name": "x", "status": "ok", "stars": 12}}}
            fs.save_cache(cache, path)
            loaded = fs.load_cache(path)
            self.assertEqual(loaded["entries"]["a/b"]["stars"], 12)
            self.assertEqual(loaded["version"], 1)

    def test_missing_cache_starts_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cache.json"
            loaded = fs.load_cache(path)
            self.assertEqual(loaded, {"version": 1, "entries": {}})

    def test_corrupt_cache_raises_instead_of_silent_reset(self):
        # 缓存被写坏时必须报错：静默重建会丢掉已取到的真实数据。
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cache.json"
            path.write_text("{not json", encoding="utf-8")
            with self.assertRaises(RuntimeError):
                fs.load_cache(path)


class TestDryRunDoesNotWrite(unittest.TestCase):
    def test_dry_run_leaves_catalog_and_cache_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            catalog = Path(tmp) / "catalog.yml"
            catalog.write_text(
                "projects:\n"
                "  - name: demo\n"
                "    model: relational\n"
                "    tier: T1\n"
                "    repo: https://github.com/demo/demo\n"
                "    license: MIT\n"
                "    language: unknown\n"
                "    stars: 0\n"
                "    stars_as_of: 1970-01-01\n",
                encoding="utf-8",
            )
            cache = Path(tmp) / "cache.json"
            before = catalog.read_text(encoding="utf-8")
            code = fs.main(["--catalog", str(catalog), "--cache", str(cache), "--dry-run"])
            self.assertEqual(code, 0)
            self.assertEqual(catalog.read_text(encoding="utf-8"), before)
            self.assertFalse(cache.exists())


class TestLimitGuard(unittest.TestCase):
    def test_limit_above_max_rejected(self):
        code = fs.main(["--limit", str(fs.MAX_REQUESTS + 1)])
        self.assertEqual(code, 2)

    def test_negative_limit_rejected(self):
        code = fs.main(["--limit", "-1"])
        self.assertEqual(code, 2)


if __name__ == "__main__":
    unittest.main()
