# -*- coding: utf-8 -*-
import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_lint import lint_page, main

GOOD_PAGE = """---
project: postgres
feature: buffer-pool
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/buffer/bufmgr.c
    symbols: [ReadBufferExtended, PinBuffer]
---

# Buffer Pool 主体

## 一句话职责
页框缓存的主实现。

## 机制是什么
缓存页框，用 Clock-Sweep 置换。

## 怎么实现的
扫描指针推进，命中即置引用位。

## 关键取舍
放弃 LRU 的精确性，换取扫描友好的近似。

## 源码落点

| 文件 | 符号 | 作用 |
|---|---|---|
| `src/backend/storage/buffer/bufmgr.c` | `ReadBufferExtended` | 读页入口 |

## 与主核 / AI 的关系
对应 engine/db 的 pager 层。

## 参考
- [PostgreSQL buffer manager](https://www.postgresql.org/docs/current/buffer-manager.html)
- [PostgreSQL storage README](https://git.postgresql.org/gitweb/?p=postgresql.git;a=blob_plain;f=src/backend/storage/buffer/README)
"""


def _write(dirpath, name, content):
    path = os.path.join(dirpath, name)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    return path


class TestLintGoodPage(unittest.TestCase):
    def test_good_page_passes(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", GOOD_PAGE)
            self.assertEqual(lint_page(page), [])

    def test_conditional_section_may_have_empty_body(self):
        """条件章节的内容可为空，标题必须在。"""
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace(
                "## 与主核 / AI 的关系\n对应 engine/db 的 pager 层。\n",
                "## 与主核 / AI 的关系\n",
            )
            page = _write(d, "a04-buffer-pool.md", content)
            self.assertEqual(lint_page(page), [])

    def test_duplicate_required_and_conditional_sections_fail(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE + "\n## 一句话职责\n重复\n## 与主核 / AI 的关系\n重复\n"
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("章节标题重复" in e for e in errors), errors)

    def test_compare_page_inside_compare_dir_passes(self):
        with tempfile.TemporaryDirectory() as d:
            compare_dir = os.path.join(d, "_compare")
            os.mkdir(compare_dir)
            content = GOOD_PAGE.replace("feature: buffer-pool", "feature: qdrant-vs-milvus")
            content = content.replace("page_type: feature", "page_type: compare")
            page = _write(compare_dir, "qdrant-vs-milvus.md", content)
            self.assertEqual(lint_page(page), [])


class TestLintFailures(unittest.TestCase):
    def _page_with(self, dirpath, name, content):
        return _write(dirpath, name, content)

    def test_missing_frontmatter(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", "# 没有 front-matter\n")
            errors = lint_page(page)
            self.assertIn("front-matter 解析失败", errors[0])

    def test_unclosed_frontmatter_returns_single_error(self):
        """解析失败要返回单条可读错误，不能抛异常。"""
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", "---\nproject: postgres\n# 缺结束 ---\n")
            errors = lint_page(page)
            self.assertEqual(len(errors), 1)
            self.assertIn("front-matter 解析失败", errors[0])

    def test_missing_required_section(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace("## 关键取舍\n放弃 LRU 的精确性，换取扫描友好的近似。\n", "")
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("缺必含章节: ## 关键取舍" in e for e in errors), errors)

    def test_missing_conditional_section_heading(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace("## 与主核 / AI 的关系\n对应 engine/db 的 pager 层。\n", "")
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("缺条件章节标题" in e for e in errors), errors)

    def test_h3_heading_does_not_satisfy_section(self):
        """章节必须按 `## ` 行精确匹配，`### 参考` 不能冒充。"""
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace("## 参考", "### 参考")
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("缺必含章节: ## 参考" in e for e in errors), errors)

    def test_line_number_reference_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace("读页入口", "读页入口（bufmgr.c:1042）")
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("出现行号引用" in e for e in errors), errors)

    def test_line_number_reference_rejected_for_c_file(self):
        """裸 .c 是最容易被正则漏掉的扩展名（postgres/sqlite/redis 主力语言）。"""
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace("读页入口", "读页入口（bufmgr.c:1042）")
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("bufmgr.c:1042" in e for e in errors), errors)

    def test_line_number_reference_rejects_common_document_extensions(self):
        for name in ("index.ts", "config.json"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as d:
                page = _write(d, "a04-buffer-pool.md", GOOD_PAGE.replace("读页入口", "读页入口（%s:%d）" % (name, 42 if name.endswith("ts") else 12)))
                self.assertTrue(any(name in e for e in lint_page(page)))

    def test_line_number_reference_long_extension_not_truncated(self):
        """长扩展名必须完整体现在错误消息里（hpp 不能被截成 h，cc 不能被截成 c）。"""
        with tempfile.TemporaryDirectory() as d:
            for name in ("buffer_pool.hpp", "buffer_pool.cc", "buffer_pool.cxx", "buffer_pool.cpp"):
                content = GOOD_PAGE.replace("读页入口", "读页入口（%s:42）" % name)
                page = _write(d, "a04-buffer-pool.md", content)
                errors = lint_page(page)
                self.assertTrue(any("%s:42" % name in e for e in errors), (name, errors))

    def test_feature_filename_mismatch(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", GOOD_PAGE.replace("feature: buffer-pool", "feature: bufferpool"))
            errors = lint_page(page)
            self.assertTrue(any("与文件名尾段" in e for e in errors), errors)

    def test_missing_required_field(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", GOOD_PAGE.replace("page_type: feature\n", ""))
            errors = lint_page(page)
            self.assertTrue(any("缺必填字段: page_type" in e for e in errors), errors)

    def test_missing_refs_field(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace(
                "refs:\n  - path: src/backend/storage/buffer/bufmgr.c\n    symbols: [ReadBufferExtended, PinBuffer]\n",
                "",
            )
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("缺必填字段: refs" in e for e in errors), errors)

    def test_only_one_reference_link(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace(
                "- [PostgreSQL storage README](https://git.postgresql.org/gitweb/?p=postgresql.git;a=blob_plain;f=src/backend/storage/buffer/README)\n",
                "",
            )
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("至少需要 2 个" in e for e in errors), errors)

    def test_relative_link_does_not_count_as_reference(self):
        """只数 http(s) 外部链接，相对路径不算。"""
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace(
                "- [PostgreSQL storage README](https://git.postgresql.org/gitweb/?p=postgresql.git;a=blob_plain;f=src/backend/storage/buffer/README)\n",
                "- [内部页](../_index.md)\n",
            )
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("至少需要 2 个" in e for e in errors), errors)

    def test_bad_page_type(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", GOOD_PAGE.replace("page_type: feature", "page_type: article"))
            errors = lint_page(page)
            self.assertTrue(any("page_type 非法" in e for e in errors), errors)

    def test_compare_page_outside_compare_dir(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "_compare-qdrant-vs-milvus.md", GOOD_PAGE.replace("page_type: feature", "page_type: compare"))
            errors = lint_page(page)
            self.assertTrue(any("必须位于 _compare" in e for e in errors), errors)

    def test_version_mismatch(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", GOOD_PAGE.replace("schema_version: 1", "schema_version: 99"))
            errors = lint_page(page)
            self.assertTrue(any("schema_version=99" in e for e in errors), errors)

    def test_empty_symbols_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            content = GOOD_PAGE.replace("    symbols: [ReadBufferExtended, PinBuffer]", "    symbols: []")
            page = _write(d, "a04-buffer-pool.md", content)
            errors = lint_page(page)
            self.assertTrue(any("symbols 必须是非空列表" in e for e in errors), errors)


class TestMainCli(unittest.TestCase):
    """CLI：退出码 0 / 1 与目录模式汇总。"""

    def _run(self, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = main(argv)
        return code, buf.getvalue()

    def test_single_good_page_exit_zero(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", GOOD_PAGE)
            code, out = self._run([page, "--quiet"])
            self.assertEqual(code, 0)
            self.assertIn("-- 检查 1 页，失败 0 页", out)

    def test_single_bad_page_exit_one(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "a04-buffer-pool.md", "# 没有 front-matter\n")
            code, out = self._run([page, "--quiet"])
            self.assertEqual(code, 1)
            self.assertIn("-- 检查 1 页，失败 1 页", out)

    def test_directory_mode_summarizes(self):
        with tempfile.TemporaryDirectory() as d:
            _write(d, "a04-buffer-pool.md", GOOD_PAGE)
            _write(d, "a05-bad.md", "# 没有 front-matter\n")
            code, out = self._run([d, "--quiet"])
            self.assertEqual(code, 1)
            self.assertIn("-- 检查 2 页，失败 1 页", out)

    def test_directory_mode_rejects_duplicate_features_on_both_pages(self):
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, "project-a"))
            os.mkdir(os.path.join(d, "project-b"))
            p1 = _write(os.path.join(d, "project-a"), "buffer-pool.md", GOOD_PAGE)
            p2 = _write(os.path.join(d, "project-b"), "buffer-pool.md", GOOD_PAGE)
            code, out = self._run([d, "--quiet"])
            self.assertEqual(code, 1)
            self.assertIn(p1, out)
            self.assertIn(p2, out)
            self.assertEqual(out.count("feature 全局重复"), 2)

    def test_different_features_across_projects_pass(self):
        with tempfile.TemporaryDirectory() as d:
            for project, feature in (("a", "buffer-pool"), ("b", "query-engine")):
                folder = os.path.join(d, project)
                os.mkdir(folder)
                _write(folder, feature + ".md", GOOD_PAGE.replace("feature: buffer-pool", "feature: " + feature))
            code, out = self._run([d, "--quiet"])
            self.assertEqual(code, 0, out)

    def test_single_file_mode_explains_global_feature_limit(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(d, "buffer-pool.md", GOOD_PAGE)
            _, out = self._run([page, "--quiet"])
            self.assertIn("feature 全局唯一性须用目录模式", out)

    def test_directory_mode_skips_hidden_dirs(self):
        with tempfile.TemporaryDirectory() as d:
            _write(d, "a04-buffer-pool.md", GOOD_PAGE)
            hidden = os.path.join(d, ".hidden")
            os.mkdir(hidden)
            _write(hidden, "a05-bad.md", "# 没有 front-matter\n")
            code, out = self._run([d, "--quiet"])
            self.assertEqual(code, 0)
            self.assertIn("-- 检查 1 页，失败 0 页", out)


if __name__ == "__main__":
    unittest.main()
