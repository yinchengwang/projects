# -*- coding: utf-8 -*-
"""wiki_tree 单元测试。

前 9 项直接取自 Task 4 简报（逐字保留），其余为守住自查要点补的用例：
「只数直接子文件」「隐藏目录不进结果」「depth 边界语义」。
"""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_tree import (SKIP_DIRS, SOURCE_EXTS, count_source_files,
                       list_module_dirs, module_key_from_dir)


def _build(root):
    tree = {
        "src/backend/storage/buffer/bufmgr.c": "int a;",
        "src/backend/storage/buffer/freelist.c": "int b;",
        "src/backend/storage/buffer/README": "doc",
        "src/backend/storage/buffer/t/": None,
        "src/backend/access/heap/heapam.c": "int c;",
        "src/backend/access/heap/heapam_xlog.c": "int d;",
        "src/test/regress/foo.c": "int e;",
        "src/backend/postmaster/main.c": "int f;",
        "build/gen.c": "int g;",
    }
    for rel, content in tree.items():
        path = os.path.join(root, rel.replace("/", os.sep))
        if content is None:
            os.makedirs(path, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)


def _write(root, rel, content="x"):
    """在 root 下写一个文件，自动建父目录。"""
    path = os.path.join(root, rel.replace("/", os.sep))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


class TestConstants(unittest.TestCase):
    def test_source_exts_include_c(self):
        self.assertIn(".c", SOURCE_EXTS)
        self.assertIn(".cc", SOURCE_EXTS)

    def test_skip_dirs_include_build_and_test(self):
        self.assertIn("build", SKIP_DIRS)
        self.assertIn("test", SKIP_DIRS)

    def test_skip_dirs_include_git_contrib_doc(self):
        # 这三个直接决定 postgres 枚举结果：.git 是缓存元数据，
        # contrib 与 doc 不承载内核机制。
        for name in (".git", "contrib", "doc"):
            self.assertIn(name, SKIP_DIRS)


class TestCountSourceFiles(unittest.TestCase):
    def test_counts_only_source_exts(self):
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            self.assertEqual(count_source_files(os.path.join(d, "src", "backend", "storage", "buffer")), 2)

    def test_missing_dir_returns_zero(self):
        self.assertEqual(count_source_files(os.path.join(tempfile.gettempdir(), "no-such-dir")), 0)

    def test_does_not_recurse_into_subdirs(self):
        # src/backend 自己没有源文件，源文件都在更深一层：若实现递归，这里会非 0。
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            self.assertEqual(count_source_files(os.path.join(d, "src", "backend")), 0)

    def test_file_path_returns_zero(self):
        # 传进来的是文件不是目录，视为「没有目录」。
        with tempfile.TemporaryDirectory() as d:
            _write(d, "src/a.c")
            self.assertEqual(count_source_files(os.path.join(d, "src", "a.c")), 0)


class TestListModuleDirs(unittest.TestCase):
    def test_lists_dirs_with_source_count(self):
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            modules = list_module_dirs(d, depth=3)
        keys = {m["path"] for m in modules}
        self.assertIn("src/backend/storage/buffer", keys)
        self.assertIn("src/backend/access/heap", keys)

    def test_skips_build_and_test_dirs(self):
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            modules = list_module_dirs(d, depth=3)
        joined = " ".join(m["path"] for m in modules)
        self.assertNotIn("build", joined)
        self.assertNotIn("src/test", joined)

    def test_source_files_field_matches_count(self):
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            modules = {m["path"]: m for m in list_module_dirs(d, depth=3)}
        self.assertEqual(modules["src/backend/storage/buffer"]["source_files"], 2)

    def test_sorted_by_path(self):
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            modules = list_module_dirs(d, depth=3)
        paths = [m["path"] for m in modules]
        self.assertEqual(paths, sorted(paths))

    def test_empty_dirs_are_omitted(self):
        # 纯文档 / 空目录不构成机制单元，不进结果。
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            modules = {m["path"] for m in list_module_dirs(d, depth=3)}
        self.assertNotIn("src", modules)
        self.assertNotIn("src/backend", modules)

    def test_hidden_dirs_are_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            _write(d, ".git/hooks/sample.c")
            _write(d, ".scratch/note.c")
            _write(d, "src/keep.c")
            modules = list_module_dirs(d, depth=3)
        self.assertEqual([m["path"] for m in modules], ["src"])

    def test_depth_boundary(self):
        # 语义可见化：depth=N 时第 N 层仍可继续下降一级，结果里最深出现第 N+1 层目录。
        # depth=2 → 第 3 层（src/backend/postmaster）可见，第 4 层（.../storage/buffer）不可见。
        with tempfile.TemporaryDirectory() as d:
            _build(d)
            keys = {m["path"] for m in list_module_dirs(d, depth=2)}
        self.assertIn("src/backend/postmaster", keys)
        self.assertNotIn("src/backend/storage/buffer", keys)


class TestModuleKey(unittest.TestCase):
    def test_relative_posix_path(self):
        rel = module_key_from_dir("/repo", "/repo/src/backend/buffer")
        self.assertEqual(rel, "src/backend/buffer")

    def test_uses_forward_slashes(self):
        rel = module_key_from_dir("C:\\repo", "C:\\repo\\src\\access")
        self.assertNotIn("\\", rel)


if __name__ == "__main__":
    unittest.main()
