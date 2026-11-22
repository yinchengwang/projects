# -*- coding: utf-8 -*-
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_manifest import (MANIFEST_FILENAME, ManifestError, dump_manifest,
                           load_manifest, manifest_feature_slugs)

MANIFEST = """---
project: postgres
schema_version: 1
modules:
  - path: src/backend/storage/buffer
    source_files: 8
pages:
  - feature: buffer-pool
    feature_group: A-存储基础
    title: Buffer Pool 主体
    page_file: a04-buffer-pool.md
    refs:
      - path: src/backend/storage/buffer/bufmgr.c
        symbols: [ReadBufferExtended, PinBuffer]
  - feature: freelist
    feature_group: A-存储基础
    title: 空闲页框链表
    page_file: a05-freelist.md
    refs:
      - path: src/backend/storage/buffer/freelist.c
        symbols: [StrategyGetBuffer]
---
"""


def _write(text, directory):
    path = os.path.join(directory, MANIFEST_FILENAME)
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def _load(text):
    """写入临时目录再读回，返回 (manifest, 读取用的临时目录路径)。"""
    with tempfile.TemporaryDirectory() as d:
        return load_manifest(_write(text, d))


class TestLoadManifest(unittest.TestCase):
    def test_loads_project_and_version(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, MANIFEST_FILENAME)
            with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(MANIFEST)
            m = load_manifest(path)
        self.assertEqual(m["project"], "postgres")
        self.assertEqual(m["schema_version"], 1)

    def test_loads_pages_with_refs(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, MANIFEST_FILENAME)
            with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(MANIFEST)
            m = load_manifest(path)
        self.assertEqual(len(m["pages"]), 2)
        first = m["pages"][0]
        self.assertEqual(first["feature"], "buffer-pool")
        self.assertEqual(first["page_file"], "a04-buffer-pool.md")
        self.assertEqual(first["refs"][0]["path"], "src/backend/storage/buffer/bufmgr.c")
        self.assertEqual(first["refs"][0]["symbols"], ["ReadBufferExtended", "PinBuffer"])

    def test_missing_file_raises(self):
        with self.assertRaises(ManifestError):
            load_manifest(os.path.join(tempfile.gettempdir(), "no-such-manifest.yml"))

    def test_not_a_mapping_raises(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, MANIFEST_FILENAME)
            with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write("no frontmatter here\n")
            with self.assertRaises(ManifestError):
                load_manifest(path)


class TestDumpManifest(unittest.TestCase):
    def test_round_trip_preserves_pages(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, MANIFEST_FILENAME)
            with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(MANIFEST)
            original = load_manifest(path)
        text = dump_manifest(original)
        self.assertNotIn("\r", text)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, MANIFEST_FILENAME)
            with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            again = load_manifest(path)
        self.assertEqual(again["project"], original["project"])
        self.assertEqual(len(again["pages"]), len(original["pages"]))
        self.assertEqual(
            again["pages"][0]["refs"][0]["symbols"],
            original["pages"][0]["refs"][0]["symbols"],
        )

    def test_dump_ends_with_single_newline(self):
        text = dump_manifest({"project": "x", "schema_version": 1, "pages": []})
        self.assertTrue(text.endswith("\n"))
        self.assertFalse(text.endswith("\n\n"))


class TestFeatureSlugs(unittest.TestCase):
    def test_returns_all_slugs(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, MANIFEST_FILENAME)
            with io.open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(MANIFEST)
            m = load_manifest(path)
        self.assertEqual(manifest_feature_slugs(m), ["buffer-pool", "freelist"])

    def test_empty_pages_returns_empty_list(self):
        self.assertEqual(manifest_feature_slugs({"pages": []}), [])


# ---- 以下为 Task 2 自查条目补充的用例（不在简报 Step 1 的原清单内） ----


class TestLoadErrors(unittest.TestCase):
    def test_body_after_frontmatter_raises(self):
        """front-matter 之后有正文：manifest 不是文档，正文说明写错了文件。"""
        with self.assertRaises(ManifestError):
            _load(MANIFEST + "\n# 这里不该有正文\n")

    def test_missing_closing_fence_raises(self):
        with self.assertRaises(ManifestError):
            _load("---\nproject: postgres\n")

    def test_tab_indentation_raises(self):
        with self.assertRaises(ManifestError):
            _load("---\nproject: postgres\npages:\n\t- feature: x\n---\n")

    def test_duplicate_top_level_key_raises(self):
        with self.assertRaises(ManifestError):
            _load("---\nproject: a\nproject: b\n---\n")

    def test_nested_dash_misaligned_reports_real_cause(self):
        """嵌套块里 `- ` 与子键没对齐：错误必须指向缩进，而不是说「不支持三级嵌套」。

        这是本模块最容易把排查引偏的地方：P0 解析器先报「不支持三级嵌套」，
        但它只是能力边界；真实病因是续行缩进不足。控制器要能一眼看出改哪一行。
        """
        text = (
            "---\n"
            "project: x\n"
            "pages:\n"
            "  - feature: f\n"
            "    refs:\n"
            "        - path: a.c\n"
            "      symbols: [A]\n"
            "---\n"
        )
        with self.assertRaises(ManifestError) as ctx:
            _load(text)
        message = str(ctx.exception)
        self.assertIn("缩进", message)
        self.assertNotIn("不支持三级嵌套", message)

    def test_duplicate_nested_key_reports_real_cause(self):
        """同一页内 refs 重复：报「重复」，而不是报不支持三级嵌套。"""
        text = (
            "---\n"
            "project: x\n"
            "pages:\n"
            "  - feature: f\n"
            "    refs:\n"
            "      - path: a.c\n"
            "        symbols: [A]\n"
            "    refs:\n"
            "      - path: b.c\n"
            "        symbols: [B]\n"
            "---\n"
        )
        with self.assertRaises(ManifestError) as ctx:
            _load(text)
        self.assertIn("重复", str(ctx.exception))

    def test_empty_nested_block_is_allowed_as_empty_list(self):
        """`refs:` 后面没有内容：按空列表处理，与页面解析器口径一致。

        是否允许空 refs 是 Task 3 门禁的判断，不是解析层的判断。
        """
        text = (
            "---\n"
            "project: x\n"
            "pages:\n"
            "  - feature: f\n"
            "    refs:\n"
            "    title: t\n"
            "---\n"
        )
        self.assertEqual(_load(text)["pages"][0]["refs"], [])


class TestNestedStructureIsGeneric(unittest.TestCase):
    def test_nested_list_under_any_key_name(self):
        """切分不认 `refs` 这个名字：任何键下的嵌套列表都要解析出来。"""
        text = (
            "---\n"
            "project: x\n"
            "modules:\n"
            "  - path: src/a\n"
            "    files:\n"
            "      - name: a.c\n"
            "        lines: 100\n"
            "      - name: b.c\n"
            "        lines: 200\n"
            "---\n"
        )
        modules = _load(text)["modules"]
        self.assertEqual(modules[0]["files"][0]["name"], "a.c")
        self.assertEqual(modules[0]["files"][1]["lines"], 200)


class TestRoundTripIdentity(unittest.TestCase):
    def test_round_trip_is_full_identity(self):
        """从文本读 → 写 → 再读，整个 dict 逐层相等。"""
        original = _load(MANIFEST)
        again = _load(dump_manifest(original))
        self.assertEqual(again, original)

    def test_dump_layout_matches_manifest_text(self):
        """dump 出来的版面与原文本逐字一致（refs 缩进 6、其子键 8）。"""
        self.assertEqual(dump_manifest(_load(MANIFEST)), MANIFEST)

    def test_symbols_read_back_as_list_not_str(self):
        page = _load(MANIFEST)["pages"][0]
        self.assertIsInstance(page["refs"][0]["symbols"], list)

    def test_modules_round_trip(self):
        original = _load(MANIFEST)
        again = _load(dump_manifest(original))
        self.assertEqual(again["modules"], [{"path": "src/backend/storage/buffer",
                                            "source_files": 8}])

    def test_empty_pages_round_trip(self):
        manifest = {"project": "x", "schema_version": 1, "pages": []}
        self.assertEqual(_load(dump_manifest(manifest)), manifest)

    def test_int_like_string_stays_string(self):
        """dump 不会把纯数字标题写成整数，否则读回就变了类型。"""
        manifest = {"project": "x", "pages": [{"feature": "f", "title": "123"}]}
        self.assertEqual(_load(dump_manifest(manifest))["pages"][0]["title"], "123")

    def test_hash_in_value_round_trips(self):
        """值里带 ` #` 会被 front-matter 当成尾随注释，dump 必须先加引号。"""
        manifest = {"project": "x", "pages": [{"feature": "f", "title": "见 #3 节"}]}
        self.assertEqual(_load(dump_manifest(manifest))["pages"][0]["title"], "见 #3 节")

    def test_empty_manifest_round_trip(self):
        text = dump_manifest({})
        self.assertEqual(text, "---\n---\n")
        self.assertEqual(_load(text), {})


if __name__ == "__main__":
    unittest.main()
