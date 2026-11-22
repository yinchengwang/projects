# -*- coding: utf-8 -*-
"""build_catalog 的测试：先红后绿。

守住三个语义边界：
1. 只继承镜像清单的 name / model / tier / repository / license 五个字段；
2. 未取数的条目用占位值，不编造 stars；
3. overrides 能覆盖 language / stars / stars_as_of。
"""
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_catalog import (TIER_MAP, build_entry, main, map_tier,
                           merge_from_reference)
from wiki_catalog import parse_catalog, validate_entry

# 镜像清单片段：故意带上 commit / pin_status / source / mirror / status / notes
# 这些生产清单不该继承的字段，用来证明继承范围被严格限制。
REFERENCE_CATALOG = """
projects:
  - name: postgres
    model: relational
    tier: core
    repository: https://github.com/postgres/postgres.git
    mirror: https://github.com/postgres/postgres.git
    license: PostgreSQL
    status: active
    commit: ea9c9432c828c6ba9e38bdd0bccd3f63507594fc
    pin_status: valid
    source: mirror
    notes:
      - 关系型数据库典型实现
  - name: qdrant
    model: vector
    tier: representative
    repository: https://github.com/qdrant/qdrant.git
    license: Apache-2.0
    status: active
"""

# 一条 repository 不是 http(s) 的镜像条目，用来驱动 main 的失败分支。
BAD_REFERENCE_CATALOG = """
projects:
  - name: broken
    model: vector
    tier: core
    repository: git@github.com:broken/repo.git
    license: MIT
    status: active
"""

# 生产清单必须恰好有的八个字段（顺序与 wiki_catalog.CATALOG_FIELDS 一致）。
EXPECTED_FIELDS = {
    "name", "model", "tier", "repo", "license",
    "language", "stars", "stars_as_of",
}
# 镜像清单专有字段，一个都不许出现在生产清单里。
MIRROR_ONLY_FIELDS = {
    "commit", "pin_status", "source", "mirror", "status", "notes", "clone_url",
}


def _write(directory, text):
    """把 YAML 文本写成 LF、UTF-8 的临时文件，返回路径。"""
    path = os.path.join(directory, "catalog.yml")
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return path


def _merge(text, overrides=None):
    """在临时目录里建镜像清单并跑 merge_from_reference。"""
    with tempfile.TemporaryDirectory() as d:
        path = _write(d, text)
        return merge_from_reference(path, overrides={} if overrides is None else overrides)


class TestMapTier(unittest.TestCase):
    def test_core_to_t1(self):
        self.assertEqual(map_tier("core"), "T1")

    def test_representative_to_t2(self):
        self.assertEqual(map_tier("representative"), "T2")

    def test_innovative_to_t3(self):
        self.assertEqual(map_tier("innovative"), "T3")

    def test_unknown_tier_raises(self):
        with self.assertRaises(ValueError):
            map_tier("unknown")

    def test_empty_tier_raises(self):
        # 镜像条目缺 tier 字段时会传空串，必须响亮失败而不是静默产出 T?
        with self.assertRaises(ValueError):
            map_tier("")

    def test_tier_map_contents(self):
        self.assertEqual(
            TIER_MAP,
            {"core": "T1", "representative": "T2", "innovative": "T3"},
        )


class TestBuildEntry(unittest.TestCase):
    def test_all_fields_present(self):
        entry = build_entry(
            name="qdrant",
            model="vector",
            tier="T1",
            repo="https://github.com/qdrant/qdrant",
            license="Apache-2.0",
            language="Rust",
            stars=35659,
            stars_as_of="2026-11-22",
        )
        self.assertEqual(entry["name"], "qdrant")
        self.assertEqual(entry["tier"], "T1")
        self.assertEqual(entry["stars"], 35659)

    def test_repo_with_git_suffix_stripped(self):
        entry = build_entry(
            name="x", model="vector", tier="T2",
            repo="https://github.com/x/y.git", license="MIT",
            language="Go", stars=1, stars_as_of="2026-11-22",
        )
        self.assertEqual(entry["repo"], "https://github.com/x/y")

    def test_repo_without_git_suffix_untouched(self):
        entry = build_entry(
            name="x", model="vector", tier="T2",
            repo="https://github.com/x/y", license="MIT",
            language="Go", stars=1, stars_as_of="2026-11-22",
        )
        self.assertEqual(entry["repo"], "https://github.com/x/y")

    def test_exactly_eight_fields(self):
        entry = build_entry(
            name="x", model="vector", tier="T3",
            repo="https://github.com/x/y.git", license="MIT",
            language="unknown", stars=0, stars_as_of="1970-01-01",
        )
        self.assertEqual(set(entry), EXPECTED_FIELDS)
        self.assertEqual(len(entry), 8)


class TestMergeFromReference(unittest.TestCase):
    def test_maps_tier_and_keeps_repo(self):
        entries = _merge(REFERENCE_CATALOG)
        by_name = {e["name"]: e for e in entries}
        self.assertEqual(by_name["postgres"]["tier"], "T1")
        self.assertEqual(by_name["qdrant"]["tier"], "T2")
        self.assertEqual(by_name["postgres"]["repo"], "https://github.com/postgres/postgres")
        self.assertEqual(by_name["qdrant"]["license"], "Apache-2.0")

    def test_overrides_apply(self):
        entries = _merge(
            REFERENCE_CATALOG,
            overrides={"postgres": {"language": "C", "stars": 13500,
                                    "stars_as_of": "2026-11-22"}},
        )
        by_name = {e["name"]: e for e in entries}
        self.assertEqual(by_name["postgres"]["language"], "C")
        self.assertEqual(by_name["postgres"]["stars"], 13500)
        self.assertEqual(by_name["postgres"]["stars_as_of"], "2026-11-22")

    def test_entries_have_eight_fields(self):
        entries = _merge(REFERENCE_CATALOG)
        for entry in entries:
            self.assertEqual(len(entry), 8, entry)

    def test_order_follows_reference(self):
        entries = _merge(REFERENCE_CATALOG)
        self.assertEqual([e["name"] for e in entries], ["postgres", "qdrant"])

    def test_only_five_fields_inherited(self):
        # 镜像清单专有字段一个都不许泄漏到生产清单
        entries = _merge(REFERENCE_CATALOG)
        for entry in entries:
            leaked = MIRROR_ONLY_FIELDS & set(entry)
            self.assertEqual(leaked, set(), entry)

    def test_placeholder_values_for_unfetched(self):
        # 未取数的条目：language=unknown / stars=0 / stars_as_of=1970-01-01
        entries = _merge(REFERENCE_CATALOG)
        by_name = {e["name"]: e for e in entries}
        for name in ("postgres", "qdrant"):
            self.assertEqual(by_name[name]["language"], "unknown")
            self.assertEqual(by_name[name]["stars"], 0)
            self.assertEqual(by_name[name]["stars_as_of"], "1970-01-01")

    def test_override_is_scoped_to_one_entry(self):
        entries = _merge(
            REFERENCE_CATALOG,
            overrides={"postgres": {"language": "C", "stars": 13500,
                                    "stars_as_of": "2026-11-22"}},
        )
        by_name = {e["name"]: e for e in entries}
        self.assertEqual(by_name["qdrant"]["language"], "unknown")
        self.assertEqual(by_name["qdrant"]["stars"], 0)

    def test_override_extra_keys_ignored(self):
        # overrides 只认 language / stars / stars_as_of 三个键，多余键不扩字段
        entries = _merge(
            REFERENCE_CATALOG,
            overrides={"postgres": {"language": "C", "commit": "deadbeef"}},
        )
        for entry in entries:
            self.assertEqual(set(entry), EXPECTED_FIELDS)

    def test_all_entries_pass_validate_entry(self):
        # 生产清单的条目必须能过 wiki_catalog 的门禁
        for entry in _merge(REFERENCE_CATALOG):
            self.assertEqual(validate_entry(entry), [], entry)


class TestMain(unittest.TestCase):
    def test_writes_catalog_and_returns_zero(self):
        with tempfile.TemporaryDirectory() as d:
            ref = _write(d, REFERENCE_CATALOG)
            out = os.path.join(d, "nested", "catalog-projects.yml")
            code = main([ref, out])
            self.assertEqual(code, 0)
            with io.open(out, "rb") as f:
                raw = f.read()
            # 无 BOM、LF
            self.assertFalse(raw.startswith(b"\xef\xbb\xbf"))
            self.assertNotIn(b"\r\n", raw)
            entries = parse_catalog(raw.decode("utf-8"))
            self.assertEqual([e["name"] for e in entries], ["postgres", "qdrant"])
            self.assertEqual(entries[0]["tier"], "T1")
            self.assertEqual(entries[0]["repo"], "https://github.com/postgres/postgres")

    def test_invalid_entry_returns_one_and_writes_nothing(self):
        # repository 不是 http(s) → validate_entry 报错 → 不落地半成品
        with tempfile.TemporaryDirectory() as d:
            ref = _write(d, BAD_REFERENCE_CATALOG)
            out = os.path.join(d, "catalog-projects.yml")
            self.assertEqual(main([ref, out]), 1)
            self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
