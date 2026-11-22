# -*- coding: utf-8 -*-
"""postgres 332 篇 manifest 生成器测试。

关键约束：332 这个数字不是随便定的，它等于 A–K 十一组实测篇数之和；
每条 refs 必须命中真实源码。因此这里既测「数量与分组」，也测「符号提取
只认函数定义」，还要用真实 postgres 源码做一次端到端校验（源码不存在则跳过）。
"""
import os
import re
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_postgres_manifest as gen
from wiki_manifest import load_manifest
from wiki_manifest_lint import lint_manifest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".cache", "repos", "postgres"))

# 设计文档实测枚举：A–K 十一组，逐组篇数是硬约束。
EXPECTED_COUNTS = {
    "A-存储基础": 29,
    "B-事务与WAL": 28,
    "C-堆表与元组": 35,
    "D-索引族": 33,
    "E-优化器": 39,
    "F-执行器": 41,
    "G-目录与缓存": 28,
    "H-内存与排序": 28,
    "I-复制与高可用": 22,
    "J-服务端与命令面": 35,
    "K-横切主题": 14,
}


class TestGroupSpecs(unittest.TestCase):
    def test_eleven_groups(self):
        self.assertEqual(len(gen.GROUP_SPECS), 11)

    def test_group_labels_match_expected(self):
        labels = [spec["label"] for spec in gen.GROUP_SPECS]
        self.assertEqual(labels, list(EXPECTED_COUNTS))

    def test_counts_match_expected(self):
        actual = {spec["label"]: spec["count"] for spec in gen.GROUP_SPECS}
        self.assertEqual(actual, EXPECTED_COUNTS)

    def test_total_is_332(self):
        self.assertEqual(sum(spec["count"] for spec in gen.GROUP_SPECS), 332)

    def test_every_group_has_dirs(self):
        for spec in gen.GROUP_SPECS:
            self.assertTrue(spec["dirs"], spec)
            for rel in spec["dirs"]:
                self.assertTrue(rel.startswith("src/backend/"), spec)


class TestExtractSymbols(unittest.TestCase):
    def _write(self, text):
        handle = tempfile.NamedTemporaryFile(
            "w", suffix=".c", delete=False, encoding="utf-8", newline="\n"
        )
        handle.write(text)
        handle.close()
        self.addCleanup(os.unlink, handle.name)
        return handle.name

    def test_finds_function_definitions(self):
        path = self._write(
            "void Foo(int x)\n{\n    return;\n}\n\nstatic int Bar(void)\n{\n    return 1;\n}\n"
        )
        self.assertEqual(gen.extract_symbols(path), ["Foo", "Bar"])

    def test_skips_declarations_with_semicolon(self):
        path = self._write("void Foo(int x);\nint Bar(void);\nvoid Baz(void)\n{\n}\n")
        self.assertEqual(gen.extract_symbols(path), ["Baz"])

    def test_skips_control_keywords(self):
        path = self._write("if (x)\n{\n}\nfor (i = 0)\n{\n}\nReal(void)\n{\n}\n")
        self.assertEqual(gen.extract_symbols(path), ["Real"])

    def test_deduplicates(self):
        path = self._write("void Foo(void)\n{\n}\nvoid Foo(void)\n{\n}\n")
        self.assertEqual(gen.extract_symbols(path), ["Foo"])

    def test_respects_limit(self):
        text = "".join("void Func%d(void)\n{\n}\n" % i for i in range(10))
        path = self._write(text)
        self.assertEqual(len(gen.extract_symbols(path, limit=4)), 4)

    def test_rejects_macro_names(self):
        """全大写的标识符是宏，不是函数。"""
        path = self._write("LOCK_DEBUG_ENABLED(void)\n{\n}\nRealFunc(void)\n{\n}\n")
        self.assertEqual(gen.extract_symbols(path), ["RealFunc"])

    def test_rejects_too_short_names(self):
        path = self._write("Ab(void)\n{\n}\nRealFunc(void)\n{\n}\n")
        self.assertEqual(gen.extract_symbols(path), ["RealFunc"])

    def test_skips_indented_lines(self):
        """定义在列 0；缩进行里的调用点不算。"""
        path = self._write("RealFunc(void)\n{\n    Helper(1);\n}\n")
        self.assertEqual(gen.extract_symbols(path), ["RealFunc"])

    def test_prefers_declared_symbols(self):
        """给了头文件声明集合时，优先返回对外接口而非内部小工具。"""
        path = self._write(
            "InternalHelper(void)\n{\n}\nPublicApi(void)\n{\n}\n"
        )
        symbols = gen.extract_symbols(path, declared={"PublicApi"})
        self.assertEqual(symbols, ["PublicApi"])

    def test_empty_file_returns_empty(self):
        self.assertEqual(gen.extract_symbols(self._write("")), [])


class TestSlugAndTitle(unittest.TestCase):
    def test_slug_from_filename(self):
        self.assertEqual(
            gen._slug_for("src/backend/storage/buffer/buf_init.c"), "buf-init"
        )

    def test_slug_is_lowercase_with_dashes(self):
        slug = gen._slug_for("src/backend/catalog/pg_class.c")
        self.assertEqual(slug, "pg-class")
        self.assertNotIn("_", slug)

    def test_known_title_is_chinese(self):
        self.assertEqual(gen._title_for("bufmgr"), "Buffer Pool 主体")

    def test_unknown_title_falls_back(self):
        self.assertEqual(gen._title_for("zzz_unknown"), "zzz_unknown 模块")


class TestManifestShape(unittest.TestCase):
    """不依赖真实源码：用临时造的小仓验证结构与门禁。"""

    def _build_fake_repo(self, root):
        # 只造 A 组够用的文件，其余组用空目录触发错误——这里只测 A 组切片。
        target = os.path.join(root, "src", "backend", "storage", "buffer")
        os.makedirs(target)
        for index in range(3):
            path = os.path.join(target, "f%d.c" % index)
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("void Sym%d(void)\n{\n}\n" % index)

    def test_collect_group_pages_shape(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._build_fake_repo(tmp)
            spec = {"group": "A", "label": "A-存储基础", "count": 3,
                    "dirs": ["src/backend/storage"]}
            pages = gen.collect_group_pages(tmp, spec)
        self.assertEqual(len(pages), 3)
        self.assertEqual(pages[0]["feature_group"], "A-存储基础")
        self.assertTrue(pages[0]["page_file"].startswith("a01-"))
        self.assertEqual(pages[0]["refs"][0]["symbols"], ["Sym0"])

    def test_page_file_tail_matches_feature(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._build_fake_repo(tmp)
            spec = {"group": "A", "label": "A-存储基础", "count": 2,
                    "dirs": ["src/backend/storage"]}
            pages = gen.collect_group_pages(tmp, spec)
        for page in pages:
            stem = os.path.splitext(page["page_file"])[0]
            tail = stem.split("-", 1)[1]
            self.assertEqual(tail, page["feature"])

    def test_insufficient_files_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            self._build_fake_repo(tmp)
            spec = {"group": "A", "label": "A-存储基础", "count": 99,
                    "dirs": ["src/backend/storage"]}
            with self.assertRaises(gen.ManifestGenError):
                gen.collect_group_pages(tmp, spec)

    def test_missing_dir_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            spec = {"group": "Z", "label": "Z-空", "count": 1,
                    "dirs": ["src/backend/nonexistent"]}
            with self.assertRaises(gen.ManifestGenError):
                gen.collect_group_pages(tmp, spec)


@unittest.skipUnless(
    os.path.isdir(os.path.join(REPO_ROOT, "src")),
    "需要真实 postgres 源码（.cache/repos/postgres）",
)
class TestAgainstRealPostgres(unittest.TestCase):
    """端到端：真实源码上生成 332 页，且每条 refs 命中。"""

    @classmethod
    def setUpClass(cls):
        cls.manifest = gen.build_manifest(REPO_ROOT)

    def test_page_count_is_332(self):
        self.assertEqual(len(self.manifest["pages"]), 332)

    def test_project_and_version(self):
        self.assertEqual(self.manifest["project"], "postgres")
        self.assertEqual(self.manifest["schema_version"], 1)

    def test_group_distribution_matches(self):
        actual = {}
        for page in self.manifest["pages"]:
            label = page["feature_group"]
            actual[label] = actual.get(label, 0) + 1
        self.assertEqual(actual, EXPECTED_COUNTS)

    def test_feature_slugs_unique(self):
        slugs = [page["feature"] for page in self.manifest["pages"]]
        self.assertEqual(len(slugs), len(set(slugs)))

    def test_page_files_unique(self):
        files = [page["page_file"] for page in self.manifest["pages"]]
        self.assertEqual(len(files), len(set(files)))

    def test_every_page_file_ends_with_feature(self):
        for page in self.manifest["pages"]:
            stem = os.path.splitext(page["page_file"])[0]
            tail = stem.split("-", 1)[1]
            self.assertEqual(tail, page["feature"], page)

    def test_every_ref_path_exists(self):
        for page in self.manifest["pages"]:
            for ref in page["refs"]:
                full = os.path.join(REPO_ROOT, ref["path"].replace("/", os.sep))
                self.assertTrue(os.path.isfile(full), ref["path"])

    def test_every_symbol_hits_real_source(self):
        failures = gen.verify_refs(self.manifest, REPO_ROOT)
        self.assertEqual(failures, [])

    def test_manifest_passes_lint(self):
        self.assertEqual(lint_manifest(self.manifest), [])

    def test_written_manifest_loads_back_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "_manifest.yml")
            gen.write_manifest(self.manifest, path)
            again = load_manifest(path)
        self.assertEqual(len(again["pages"]), 332)
        self.assertEqual(again["pages"][0]["refs"][0]["symbols"],
                         self.manifest["pages"][0]["refs"][0]["symbols"])

    def test_written_manifest_is_lf(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "_manifest.yml")
            gen.write_manifest(self.manifest, path)
            with open(path, "rb") as handle:
                data = handle.read()
        self.assertNotIn(b"\r\n", data)
        self.assertFalse(data.startswith(b"\xef\xbb\xbf"))


if __name__ == "__main__":
    unittest.main()
