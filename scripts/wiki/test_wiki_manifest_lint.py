# -*- coding: utf-8 -*-
"""wiki_manifest_lint 的测试：先红后绿。"""
import contextlib
import copy
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_manifest_lint import lint_manifest, main

GOOD = {
    "project": "postgres",
    "schema_version": 1,
    "pages": [
        {
            "feature": "buffer-pool",
            "feature_group": "A-存储基础",
            "title": "Buffer Pool 主体",
            "page_file": "a04-buffer-pool.md",
            "refs": [{"path": "src/backend/storage/buffer/bufmgr.c",
                      "symbols": ["ReadBufferExtended"]}],
        }
    ],
}


def _good():
    # 深拷贝：refs 是嵌套结构，浅拷贝会让各用例共享同一份 refs，
    # 前一个用例改掉的 path / symbols 会污染后面「应当通过」的用例。
    return copy.deepcopy(GOOD)


class TestLintGood(unittest.TestCase):
    def test_good_manifest_passes(self):
        self.assertEqual(lint_manifest(_good()), [])


class TestLintFailures(unittest.TestCase):
    def test_missing_project(self):
        m = _good()
        m.pop("project")
        self.assertTrue(any("project" in e for e in lint_manifest(m)))

    def test_wrong_schema_version(self):
        m = _good()
        m["schema_version"] = 99
        self.assertTrue(any("schema_version" in e for e in lint_manifest(m)))

    def test_empty_pages(self):
        m = _good()
        m["pages"] = []
        self.assertTrue(any("pages" in e for e in lint_manifest(m)))

    def test_missing_required_page_key(self):
        m = _good()
        m["pages"][0].pop("title")
        self.assertTrue(any("title" in e for e in lint_manifest(m)))

    def test_duplicate_feature(self):
        m = _good()
        m["pages"].append(dict(m["pages"][0], page_file="a05-buffer-pool.md"))
        self.assertTrue(any("feature 重复" in e for e in lint_manifest(m)))

    def test_page_file_must_be_markdown(self):
        m = _good()
        m["pages"][0]["page_file"] = "a04-buffer-pool.txt"
        self.assertTrue(any(".md" in e for e in lint_manifest(m)))

    def test_page_file_tail_must_match_feature(self):
        m = _good()
        m["pages"][0]["page_file"] = "a04-other.md"
        self.assertTrue(any("尾段" in e for e in lint_manifest(m)))

    def test_empty_refs_rejected(self):
        m = _good()
        m["pages"][0]["refs"] = []
        self.assertTrue(any("refs" in e for e in lint_manifest(m)))

    def test_ref_without_symbols_rejected(self):
        m = _good()
        m["pages"][0]["refs"][0]["symbols"] = []
        self.assertTrue(any("symbols" in e for e in lint_manifest(m)))

    def test_ref_without_path_rejected(self):
        m = _good()
        m["pages"][0]["refs"][0].pop("path")
        self.assertTrue(any("path" in e for e in lint_manifest(m)))

    def test_page_file_conflicts_with_existing_file(self):
        """给出 project_dir 时，已存在的同名页文件视为冲突。"""
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        with io.open(os.path.join(d, "a04-buffer-pool.md"), "w",
                     encoding="utf-8", newline="\n") as f:
            f.write("# 既有页\n")
        self.assertTrue(any("已存在" in e for e in lint_manifest(_good(), d)))


class TestMainCli(unittest.TestCase):
    def _write(self, manifest_text):
        d = tempfile.mkdtemp()
        path = os.path.join(d, "_manifest.yml")
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(manifest_text)
        return path

    def test_exit_zero_on_good(self):
        text = (
            "---\n"
            "project: postgres\n"
            "schema_version: 1\n"
            "pages:\n"
            "  - feature: buffer-pool\n"
            "    feature_group: A-存储基础\n"
            "    title: Buffer Pool 主体\n"
            "    page_file: a04-buffer-pool.md\n"
            "    refs:\n"
            "      - path: src/backend/storage/buffer/bufmgr.c\n"
            "        symbols: [ReadBufferExtended]\n"
            "---\n"
        )
        self.assertEqual(main([self._write(text), "--quiet"]), 0)

    def test_exit_one_on_bad(self):
        text = "---\nproject: postgres\nschema_version: 99\npages: []\n---\n"
        self.assertEqual(main([self._write(text), "--quiet"]), 1)


GOOD_TEXT = (
    "---\n"
    "project: postgres\n"
    "schema_version: 1\n"
    "pages:\n"
    "  - feature: buffer-pool\n"
    "    feature_group: A-存储基础\n"
    "    title: Buffer Pool 主体\n"
    "    page_file: a04-buffer-pool.md\n"
    "    refs:\n"
    "      - path: src/backend/storage/buffer/bufmgr.c\n"
    "        symbols: [ReadBufferExtended]\n"
    "---\n"
)


class TestMainCliDirectory(unittest.TestCase):
    """目录模式：一个坏 manifest 不能让整目录扫描崩掉。"""

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)

    def _write(self, name, text):
        path = os.path.join(self.dir, name)
        with io.open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return path

    def test_broken_manifest_does_not_abort_scan(self):
        self._write("_manifest.yml", GOOD_TEXT)
        # 缩进错乱的 manifest：load_manifest 会抛 ManifestError。
        self._write("_manifest_bad.yml", "---\nproject: postgres\npages:\n   - feature: x\n ---\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main([self.dir])
        text = out.getvalue()
        self.assertEqual(code, 1)
        self.assertIn("PASS", text)   # 好的那个仍然被检查并报告
        self.assertIn("FAIL", text)   # 坏的那个被记为失败

    def test_directory_all_good_exits_zero(self):
        self._write("_manifest.yml", GOOD_TEXT)
        self._write("_manifest2.yml", GOOD_TEXT.replace("buffer-pool", "wal-writer")
                    .replace("a04-buffer-pool.md", "a05-wal-writer.md"))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = main([self.dir])
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue().count("PASS"), 2)


if __name__ == "__main__":
    unittest.main()
