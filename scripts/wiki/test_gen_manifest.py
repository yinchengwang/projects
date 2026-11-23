# -*- coding: utf-8 -*-
"""`gen_manifest.py` 的测试：库无关生成器的 5 处解耦。

这些测试刻意不依赖 postgres：它们用**假 spec 模块**（临时目录里现写）验证
「换一个库也能生成」这件事本身，而不是验证 postgres 的 151 条清单对不对
（那是 `test_gen_postgres_manifest.py` 的职责）。
"""
import io
import os
import sys
import tempfile
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_manifest as gm


def _spec(feature, page_file, title, group, path, symbols, boundary="边界说明"):
    """构造一条最小合法 spec。"""
    return {
        "feature": feature,
        "feature_group": group,
        "title": title,
        "page_file": page_file,
        "boundary": boundary,
        "refs": [{"path": path, "symbols": list(symbols)}],
    }


def _install_module(name, specs, group_order):
    """把一个假 spec 模块塞进 sys.modules，供 load_specs 导入。"""
    module = types.ModuleType(name)
    module.MECHANISM_SPECS = specs
    module.GROUP_ORDER = group_order
    sys.modules[name] = module
    return module


class TestLoadSpecs(unittest.TestCase):
    def tearDown(self):
        for name in ("fake_sql_specs", "fake_a_specs", "fake_b_specs",
                     "fake_empty_specs", "fake_no_order_specs"):
            sys.modules.pop(name, None)

    def test_loads_specs_and_group_order(self):
        specs = [_spec("ops-codec", "s01-codec.md", "指令解码", "S-解码",
                       "src/codec.c", ["decode_insn"])]
        _install_module("fake_sql_specs", specs, ["S-解码"])
        loaded_specs, order = gm.load_specs("fake_sql_specs")
        self.assertEqual(len(loaded_specs), 1)
        self.assertEqual(order, ["S-解码"])

    def test_missing_mechanism_specs_raises(self):
        module = types.ModuleType("fake_empty_specs")
        module.GROUP_ORDER = []
        sys.modules["fake_empty_specs"] = module
        with self.assertRaises(gm.ManifestGenError):
            gm.load_specs("fake_empty_specs")

    def test_missing_group_order_raises(self):
        module = types.ModuleType("fake_no_order_specs")
        module.MECHANISM_SPECS = []
        sys.modules["fake_no_order_specs"] = module
        with self.assertRaises(gm.ManifestGenError):
            gm.load_specs("fake_no_order_specs")

    def test_import_error_is_not_swallowed(self):
        with self.assertRaises(ImportError):
            gm.load_specs("definitely_not_a_module_xyz")


class TestCheckSpecs(unittest.TestCase):
    def test_accepts_sql_prefix(self):
        # SQLite / MySQL 的源码不在 src/ 下：sqlite 在仓根、MySQL 在 sql/。
        specs = [_spec("sql-parser", "s01-sql-parser.md", "SQL 解析", "S-SQL",
                       "sql/sql_yacc.cc", ["yyparse"])]
        problems = gm.check_specs(specs, ["S-SQL"], ["sql", "storage"])
        self.assertEqual(problems, [])

    def test_rejects_path_outside_prefixes(self):
        specs = [_spec("sql-parser", "s01-sql-parser.md", "SQL 解析", "S-SQL",
                       "runtime/vm.c", ["execute"])]
        problems = gm.check_specs(specs, ["S-SQL"], ["sql"])
        self.assertTrue(any("必须以" in p for p in problems), problems)

    def test_accepts_multiple_prefixes(self):
        specs = [
            _spec("a", "s01-a.md", "甲机制", "S", "sql/one.cc", ["one"]),
            _spec("b", "s02-b.md", "乙机制", "S", "storage/two.cc", ["two"]),
        ]
        problems = gm.check_specs(specs, ["S"], ["sql", "storage", "plugin"])
        self.assertEqual(problems, [])

    def test_rejects_pseudo_title_equal_to_stem(self):
        specs = [_spec("a", "s01-heap.md", "s01-heap", "S", "src/a.c", ["x"])]
        problems = gm.check_specs(specs, ["S"], ["src"])
        self.assertTrue(any("伪标题" in p for p in problems), problems)

    def test_rejects_pseudo_title_with_module_suffix(self):
        specs = [_spec("a", "s01-heap.md", "缓冲池模块", "S", "src/a.c", ["x"])]
        problems = gm.check_specs(specs, ["S"], ["src"])
        self.assertTrue(any("伪标题" in p for p in problems), problems)

    def test_rejects_prefix_without_path_boundary(self):
        specs = [_spec("a", "s01-heap.md", "堆表管理", "S", "srcfoo/a.c", ["x"])]
        problems = gm.check_specs(specs, ["S"], ["src"])
        self.assertTrue(any("必须以" in p for p in problems), problems)

    def test_accepts_real_title(self):
        specs = [_spec("a", "s01-heap.md", "堆表页与行指针", "S", "src/a.c", ["x"])]
        problems = gm.check_specs(specs, ["S"], ["src"])
        self.assertEqual(problems, [])

    def test_rejects_unknown_group(self):
        specs = [_spec("a", "s01-a.md", "甲机制", "X-不存在", "src/a.c", ["x"])]
        problems = gm.check_specs(specs, ["S"], ["src"])
        self.assertTrue(any("不在分组表里" in p for p in problems), problems)

    def test_rejects_abbreviated_path(self):
        specs = [_spec("a", "s01-a.md", "甲机制", "S", "src/.../a.c", ["x"])]
        problems = gm.check_specs(specs, ["S"], ["src"])
        self.assertTrue(any("缩写" in p for p in problems), problems)


class TestBuildManifest(unittest.TestCase):
    def test_project_name_comes_from_argument(self):
        specs = [_spec("a", "s01-a.md", "甲机制", "S", "src/a.c", ["x"])]
        manifest = gm.build_manifest("sqlite", specs, ["S"])
        self.assertEqual(manifest["project"], "sqlite")
        self.assertEqual(manifest["schema_version"], 1)
        self.assertEqual(len(manifest["pages"]), 1)

    def test_empty_project_rejected(self):
        with self.assertRaises(gm.ManifestGenError):
            gm.build_manifest("   ", [], ["S"])

    def test_group_order_is_independent_per_module(self):
        # 两个不同库的分组表互不影响：库 A 用 A/B，库 B 用 X/Y。
        specs_a = [
            _spec("a1", "a01-a1.md", "甲机制", "A-组", "src/a.c", ["x"]),
            _spec("a2", "a02-a2.md", "乙机制", "B-组", "src/b.c", ["y"]),
        ]
        specs_b = [
            _spec("b1", "b01-b1.md", "丙机制", "X-组", "sql/c.cc", ["z"]),
            _spec("b2", "b02-b2.md", "丁机制", "Y-组", "sql/d.cc", ["w"]),
        ]
        order_a = ["A-组", "B-组"]
        order_b = ["X-组", "Y-组"]
        manifest_a = gm.build_manifest("lib_a", specs_a, order_a)
        manifest_b = gm.build_manifest("lib_b", specs_b, order_b)
        counts_a = gm.group_counts(manifest_a, order_a)
        counts_b = gm.group_counts(manifest_b, order_b)
        self.assertEqual(list(counts_a.keys()), order_a)
        self.assertEqual(list(counts_b.keys()), order_b)
        self.assertEqual(counts_a["A-组"], 1)
        self.assertEqual(counts_b["X-组"], 1)

    def test_group_counts_zero_fills_missing(self):
        specs = [_spec("a", "s01-a.md", "甲机制", "S1", "src/a.c", ["x"])]
        manifest = gm.build_manifest("lib", specs, ["S1", "S2", "S3"])
        counts = gm.group_counts(manifest, ["S1", "S2", "S3"])
        self.assertEqual(counts, {"S1": 1, "S2": 0, "S3": 0})


class TestVerifyRefs(unittest.TestCase):
    def test_refs_hit_and_miss(self):
        with tempfile.TemporaryDirectory() as repo:
            os.makedirs(os.path.join(repo, "sql"))
            target = os.path.join(repo, "sql", "one.cc")
            with io.open(target, "w", encoding="utf-8") as stream:
                stream.write("int yyparse(void) { return 0; }\n")
            manifest = gm.build_manifest(
                "lib",
                [_spec("p", "s01-p.md", "甲机制", "S", "sql/one.cc",
                       ["yyparse", "missing_symbol"])],
                ["S"],
            )
            failures = gm.verify_refs(manifest, repo)
            self.assertEqual(len(failures), 1)
            self.assertIn("missing_symbol", failures[0][2])

    def test_missing_file_reported(self):
        with tempfile.TemporaryDirectory() as repo:
            manifest = gm.build_manifest(
                "lib",
                [_spec("p", "s01-p.md", "甲机制", "S", "sql/nope.cc", ["x"])],
                ["S"],
            )
            failures = gm.verify_refs(manifest, repo)
            self.assertEqual(failures[0][2], "文件不存在")


class TestMainCli(unittest.TestCase):
    def tearDown(self):
        sys.modules.pop("fake_cli_specs", None)

    def test_cli_writes_manifest_for_non_postgres_project(self):
        specs = [_spec("codec", "s01-codec.md", "指令解码", "S-解码",
                       "sql/codec.cc", ["decode_insn"])]
        _install_module("fake_cli_specs", specs, ["S-解码"])
        with tempfile.TemporaryDirectory() as tmp:
            out = os.path.join(tmp, "manifest.yml")
            code = gm.main(["--project", "fake", "--spec-module", "fake_cli_specs",
                            "--src-prefix", "sql", "--output", out])
            self.assertEqual(code, 0)
            self.assertTrue(os.path.isfile(out))
            from wiki_manifest import load_manifest
            loaded = load_manifest(out)
            self.assertEqual(loaded["project"], "fake")

    def test_cli_rejects_outside_prefix(self):
        specs = [_spec("codec", "s01-codec.md", "指令解码", "S-解码",
                       "src/codec.cc", ["decode_insn"])]
        _install_module("fake_cli_specs", specs, ["S-解码"])
        code = gm.main(["--project", "fake", "--spec-module", "fake_cli_specs",
                        "--src-prefix", "sql"])
        self.assertEqual(code, 1)


if __name__ == "__main__":
    unittest.main()
