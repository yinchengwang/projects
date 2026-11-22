# -*- coding: utf-8 -*-
"""postgres 机制 manifest 生成器测试。

P1 的生成器按「`.c` 文件体积降序、一个文件一页」产出，篇数被写死成 332，
结果 78% 标题退化成「<文件名> 模块」，还出现跨组重复与测试辅助符号当主题。
P2a 把生成源换成人工审核的机制清单，因此这里的断言重点整体换了方向：

- 篇数跟着清单走，不是跟着目录文件数走；
- 每页都能说出机制边界（`boundary`），标题不许退化成文件名；
- refs 是机制证据而不是文件枚举，同一机制允许跨文件；
- 清单层面能挡住重复、伪标题、缩写路径、空 refs 这些已知病症。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_postgres_manifest as gen
import postgres_mechanism_specs as specs_module
from wiki_manifest import load_manifest
from wiki_manifest_lint import lint_manifest

HERE = os.path.dirname(os.path.abspath(__file__))


def _repo_root():
    """定位 postgres 源码缓存。

    `.cache/` 是每个工作树自己的运行时目录，所以主仓有缓存、worktree 不一定有。
    依次试「本工作树」和「主仓 projects」，任一命中即可；都不在就跳过真实源码
    相关用例，而不是让整个套件失败。
    """
    candidates = [
        os.path.normpath(os.path.join(HERE, "..", "..", ".cache", "repos", "postgres")),
        # worktree 场景：C:\code\.worktrees\<name>\scripts\wiki → C:\code\projects
        os.path.normpath(os.path.join(HERE, "..", "..", "..", "..", "projects",
                                      ".cache", "repos", "postgres")),
    ]
    for path in candidates:
        if os.path.isdir(path):
            return path
    return candidates[0]


REPO_ROOT = _repo_root()

# 评审定稿的分组与篇数：这是盘点结果，不是给源码文件数留的配额。
EXPECTED_COUNTS = {
    "A-存储与IPC": 20,
    "B-事务与WAL": 19,
    "C-堆表与元组": 15,
    "D-索引族": 17,
    "E-优化器": 13,
    "F-执行器": 13,
    "G-目录与缓存": 7,
    "H-内存与排序": 5,
    "I-复制与高可用": 10,
    "J-服务端与命令面": 12,
    "K-SQL处理与安全": 11,
    "L-SQL类型与内建函数": 9,
}


class TestGroupOrder(unittest.TestCase):
    def test_groups_match_reviewed_labels(self):
        self.assertEqual(list(specs_module.GROUP_ORDER), list(EXPECTED_COUNTS))

    def test_every_group_has_mechanisms(self):
        counts = gen.group_counts(gen.build_manifest())
        for label in specs_module.GROUP_ORDER:
            self.assertGreater(counts[label], 0, label)

    def test_group_counts_match_reviewed_inventory(self):
        self.assertEqual(gen.group_counts(gen.build_manifest()), EXPECTED_COUNTS)

    def test_total_equals_mechanism_count(self):
        self.assertEqual(
            len(gen.build_manifest()["pages"]),
            len(specs_module.MECHANISM_SPECS),
        )


class TestManifestShape(unittest.TestCase):
    def test_manifest_has_project_and_schema_version(self):
        manifest = gen.build_manifest()
        self.assertEqual(manifest["project"], "postgres")
        self.assertEqual(manifest["schema_version"], 1)

    def test_features_are_unique(self):
        features = [page["feature"] for page in gen.build_manifest()["pages"]]
        self.assertEqual(len(features), len(set(features)))

    def test_page_files_are_unique(self):
        files = [page["page_file"] for page in gen.build_manifest()["pages"]]
        self.assertEqual(len(files), len(set(files)))

    def test_page_file_tail_equals_feature(self):
        """契约要求 feature 与文件名尾段一致，否则 wiki_lint 会拒。"""
        for page in gen.build_manifest()["pages"]:
            stem = os.path.splitext(page["page_file"])[0]
            tail = stem.split("-", 1)[1]
            self.assertEqual(tail, page["feature"], page["page_file"])

    def test_page_files_are_numbered_from_one_within_group(self):
        """组内序号从 01 起连续，便于索引排序与失败页定位。"""
        seen = {}
        for page in gen.build_manifest()["pages"]:
            prefix, _tail = page["page_file"].split("-", 1)
            letter, number = prefix[0], int(prefix[1:])
            self.assertEqual(letter, page["feature_group"][0].lower())
            expected = seen.get(letter, 0) + 1
            self.assertEqual(number, expected, page["page_file"])
            seen[letter] = expected

    def test_every_page_has_refs(self):
        for page in gen.build_manifest()["pages"]:
            self.assertTrue(page["refs"], page["feature"])


class TestMechanismQuality(unittest.TestCase):
    def test_no_generic_module_titles(self):
        """P1 的 78% 伪标题不许回来。"""
        bad = [p["feature"] for p in gen.build_manifest()["pages"]
               if p["title"].endswith(" 模块")]
        self.assertEqual(bad, [])

    def test_every_mechanism_has_boundary(self):
        for spec in specs_module.MECHANISM_SPECS:
            self.assertTrue(spec["boundary"].strip(), spec["feature"])

    def test_titles_are_unique_per_group(self):
        seen = {}
        for spec in specs_module.MECHANISM_SPECS:
            key = (spec["feature_group"], spec["title"])
            self.assertNotIn(key, seen, spec["feature"])
            seen[key] = spec["feature"]

    def test_no_test_helper_symbols_as_evidence(self):
        """`pgaio_worker_pm_test_grow*` 这类测试辅助函数不能当机制主题。"""
        symbols = {s for p in gen.build_manifest()["pages"]
                   for ref in p["refs"] for s in ref["symbols"]}
        offenders = {s for s in symbols if "test_grow" in s or s.startswith("test_")}
        self.assertEqual(offenders, set())

    def test_rmgrdesc_mechanism_owns_description_files(self):
        """rmgrdesc 只归一个机制页，不再在 B/C 各收一遍。"""
        owners = [p["feature"] for p in gen.build_manifest()["pages"]
                  if any("rmgrdesc/" in ref["path"] for ref in p["refs"])]
        self.assertEqual(owners, ["wal-rmgr-description"])

    def test_aio_backends_are_one_mechanism(self):
        """worker / io_uring 是 AIO 的实现后端，不再按文件拆三页。"""
        owners = [p["feature"] for p in gen.build_manifest()["pages"]
                  if any("storage/aio/" in ref["path"] for ref in p["refs"])]
        self.assertIn("aio-io-methods", owners)
        self.assertNotIn("method-worker", owners)
        self.assertNotIn("method-io-uring", owners)

    def test_local_buffer_is_its_own_mechanism(self):
        """共享缓冲池与 backend-local 池的并发语义不同，分开成页。"""
        owners = [p["feature"] for p in gen.build_manifest()["pages"]
                  if any("buffer/localbuf.c" in ref["path"] for ref in p["refs"])]
        self.assertEqual(owners, ["local-buffer-pool"])

    def test_refs_may_span_multiple_files(self):
        """一个机制跨多个文件实现是常态，不再一文件一页。"""
        multi = [p for p in gen.build_manifest()["pages"] if len(p["refs"]) > 1]
        self.assertGreater(len(multi), 20)


class TestCheckSpecs(unittest.TestCase):
    def _spec(self, **overrides):
        spec = {
            "feature_group": "A-存储与IPC",
            "feature": "buffer-pool",
            "title": "共享缓冲池",
            "page_file": "a01-buffer-pool.md",
            "boundary": "共享 buffer 分配与替换",
            "refs": [{"path": "src/backend/storage/buffer/bufmgr.c",
                      "symbols": ["ReadBufferExtended"]}],
        }
        spec.update(overrides)
        return spec

    def test_good_spec_passes(self):
        self.assertEqual(gen.check_specs([self._spec()]), [])

    def test_duplicate_feature_is_rejected(self):
        problems = gen.check_specs([self._spec(), self._spec(page_file="a02-x.md")])
        self.assertTrue(any("feature 重复" in p for p in problems))

    def test_duplicate_page_file_is_rejected(self):
        problems = gen.check_specs([
            self._spec(),
            self._spec(feature="other", title="别的机制"),
        ])
        self.assertTrue(any("page_file 重复" in p for p in problems))

    def test_generic_title_is_rejected(self):
        problems = gen.check_specs([self._spec(title="bufmgr 模块")])
        self.assertTrue(any("文件名兜底" in p for p in problems))

    def test_empty_boundary_is_rejected(self):
        problems = gen.check_specs([self._spec(boundary="  ")])
        self.assertTrue(any("boundary" in p for p in problems))

    def test_empty_refs_is_rejected(self):
        problems = gen.check_specs([self._spec(refs=[])])
        self.assertTrue(any("refs 为空" in p for p in problems))

    def test_empty_symbols_is_rejected(self):
        problems = gen.check_specs([self._spec(
            refs=[{"path": "src/backend/storage/buffer/bufmgr.c", "symbols": []}])])
        self.assertTrue(any("symbols 为空" in p for p in problems))

    def test_abbreviated_path_is_rejected(self):
        problems = gen.check_specs([self._spec(
            refs=[{"path": "src/backend/storage/.../bufmgr.c",
                   "symbols": ["ReadBufferExtended"]}])])
        self.assertTrue(any("缩写" in p for p in problems))

    def test_absolute_path_is_rejected(self):
        problems = gen.check_specs([self._spec(
            refs=[{"path": "C:/code/postgres/src/backend/x.c", "symbols": ["Foo"]}])])
        self.assertTrue(any("src/ 开头" in p for p in problems))

    def test_unknown_group_is_rejected(self):
        problems = gen.check_specs([self._spec(feature_group="Z-不存在")])
        self.assertTrue(any("feature_group" in p for p in problems))

    def test_bad_slug_is_rejected(self):
        problems = gen.check_specs([self._spec(feature="Buffer_Pool")])
        self.assertTrue(any("feature 非法" in p for p in problems))


class TestRealSpecsAreClean(unittest.TestCase):
    def test_shipped_specs_pass_self_check(self):
        self.assertEqual(gen.check_specs(), [])

    def test_shipped_manifest_passes_lint(self):
        self.assertEqual(lint_manifest(gen.build_manifest()), [])


@unittest.skipUnless(os.path.isdir(REPO_ROOT), "需要本地 postgres 源码缓存")
class TestAgainstRealPostgres(unittest.TestCase):
    def test_every_ref_hits_real_source(self):
        failures = gen.verify_refs(gen.build_manifest(), REPO_ROOT)
        self.assertEqual(failures, [])

    def test_shipped_manifest_matches_generator_output(self):
        """仓库里的 `_manifest.yml` 必须与生成器输出同构，防止手改漂移。"""
        path = os.path.join(HERE, "..", "..", "docs", "wiki", "projects",
                            "postgres", "_manifest.yml")
        shipped = load_manifest(path)
        generated = gen.build_manifest()
        self.assertEqual(len(shipped["pages"]), len(generated["pages"]))
        self.assertEqual([p["page_file"] for p in shipped["pages"]],
                         [p["page_file"] for p in generated["pages"]])
        self.assertEqual([p["feature"] for p in shipped["pages"]],
                         [p["feature"] for p in generated["pages"]])


if __name__ == "__main__":
    unittest.main()
