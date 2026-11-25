# -*- coding: utf-8 -*-
"""repo_cache 模块测试。

上游一律用临时目录里现造的本地 git 仓（file:// URL），不触网、不依赖远端。
每条克隆行为都用真正执行过的 git 命令做二次验证，避免「函数返回了路径」这种
表面通过：depth 用提交条数验，single-branch 用 fetch refspec + 远端引用清单验，
参数本身再用一次记录器逐字核对（--depth 会隐含单分支，只看结果测不出参数），
幂等用工作树内容验。
"""
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from repo_cache import DEFAULT_CACHE_DIR, RepoCacheError, ensure_repo, repo_path

GIT_ENV = dict(
    os.environ,
    GIT_AUTHOR_NAME="wiki-test",
    GIT_AUTHOR_EMAIL="wiki-test@example.com",
    GIT_COMMITTER_NAME="wiki-test",
    GIT_COMMITTER_EMAIL="wiki-test@example.com",
    GIT_TERMINAL_PROMPT="0",
)

# 测试所在仓库根（projects/），用于校验 .gitignore 现状。
# 本测试位于 scripts/wiki/，故需上溯两级。
_REPO_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)


def _git(args, cwd):
    return subprocess.run(
        ["git"] + args, cwd=cwd, capture_output=True, text=True, env=GIT_ENV
    )


def _make_upstream(root, commits=1, extra_branch=False):
    """造一个带若干次提交的本地上游仓，返回其 file:// URL。"""
    up = os.path.join(root, "upstream")
    os.makedirs(os.path.join(up, "src"))
    _git(["init", "-b", "main"], up)
    for i in range(commits):
        with open(
            os.path.join(up, "src", "a.c"), "w", encoding="utf-8", newline="\n"
        ) as f:
            f.write("int Alpha(void);\n/* rev %d */\n" % i)
        _git(["add", "."], up)
        rc = _git(["commit", "-m", "init %d" % i], up)
        assert rc.returncode == 0, rc.stderr
    if extra_branch:
        _git(["checkout", "-b", "extra"], up)
        with open(
            os.path.join(up, "src", "b.c"), "w", encoding="utf-8", newline="\n"
        ) as f:
            f.write("int Beta(void);\n")
        _git(["add", "."], up)
        _git(["commit", "-m", "extra"], up)
        _git(["checkout", "main"], up)
    # 上游必须真的产生了提交：否则 clone 拿到的是空仓，「克隆成功」验证不足。
    assert _commit_count(up) >= commits, "上游仓没有预期的提交条数"
    return "file:///" + up.replace(os.sep, "/").lstrip("/")


def _commit_count(repo):
    """上游/克隆仓里 HEAD 的提交条数（浅克隆也如实反映 --depth）。"""
    p = _git(["rev-list", "--count", "HEAD"], repo)
    assert p.returncode == 0, p.stderr
    return int(p.stdout.strip())


# 检出必然失败的深路径：文件名 300 字符，Windows 与 POSIX 的单段上限都超了，
# 因此「能传输但不能检出」这条路径在两端都可复现。
_TOO_LONG_NAME = "d" * 300 + ".c"


def _make_upstream_with_uncheckoutable_path(root):
    """造一个「能传输但检出必然失败」的上游仓，返回其 file:// URL。

    本地建不出 300 字符的文件名，所以用 git plumbing（hash-object + mktree +
    commit-tree）直接把树对象塞进去。这类失败发生在 git 已经建好 target 目录和
    .git 之后，正好用来验证「克隆失败后清理半成品目录」。
    """
    up = os.path.join(root, "upstream_bad")
    os.makedirs(up)
    _git(["init", "-b", "main"], up)
    blob = subprocess.run(
        ["git", "hash-object", "-w", "--stdin"],
        cwd=up, input="int Bad;\n", capture_output=True, text=True, env=GIT_ENV,
    )
    assert blob.returncode == 0, blob.stderr
    tree = subprocess.run(
        ["git", "mktree"],
        cwd=up,
        input="100644 blob %s\t%s\n" % (blob.stdout.strip(), _TOO_LONG_NAME),
        capture_output=True, text=True, env=GIT_ENV,
    )
    assert tree.returncode == 0, tree.stderr
    commit = subprocess.run(
        ["git", "commit-tree", tree.stdout.strip(), "-m", "uncheckoutable"],
        cwd=up, capture_output=True, text=True, env=GIT_ENV,
    )
    assert commit.returncode == 0, commit.stderr
    _git(["update-ref", "refs/heads/main", commit.stdout.strip()], up)
    return "file:///" + up.replace(os.sep, "/").lstrip("/")


class TestRepoPath(unittest.TestCase):
    def test_repo_path_is_join_of_cache_and_name(self):
        self.assertEqual(repo_path("/tmp/x", "demo"), os.path.join("/tmp/x", "demo"))


class TestDefaultCacheDir(unittest.TestCase):
    def test_default_cache_dir_points_at_gitignored_cache(self):
        self.assertTrue(DEFAULT_CACHE_DIR.endswith(os.path.join(".cache", "repos")))
        self.assertEqual(
            os.path.normpath(os.path.dirname(os.path.dirname(DEFAULT_CACHE_DIR))),
            os.path.normpath(_REPO_ROOT),
        )

    def test_default_cache_dir_is_ignored_by_gitignore(self):
        with open(
            os.path.join(_REPO_ROOT, ".gitignore"), "r", encoding="utf-8"
        ) as f:
            lines = [line.strip() for line in f]
        self.assertIn(".cache/", lines)


class TestEnsureRepo(unittest.TestCase):
    def test_rejects_unsafe_names(self):
        with tempfile.TemporaryDirectory() as d:
            for name in ("../outside", os.path.abspath(os.path.join(d, "absolute")), "", ".", "..", "a/b", "a\\b"):
                with self.subTest(name=name), self.assertRaises(RepoCacheError):
                    ensure_repo("file:///not-used", name, cache_dir=os.path.join(d, "cache"))

    def test_existing_bad_cache_is_rejected_and_preserved(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "cache")
            target = os.path.join(cache, "demo")
            os.makedirs(os.path.join(target, ".git"))
            marker = os.path.join(target, "keep")
            with open(marker, "w") as f:
                f.write("safe")
            with self.assertRaises(RepoCacheError):
                ensure_repo("file:///unused", "demo", cache_dir=cache)
            self.assertTrue(os.path.isfile(marker))

    def test_origin_mismatch_rejected_and_matching_origin_idempotent(self):
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            other = _make_upstream(os.path.join(d, "other"))
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            self.assertEqual(target, ensure_repo(url, "demo", cache_dir=cache))
            with self.assertRaises(RepoCacheError):
                ensure_repo(other, "demo", cache_dir=cache)

    def test_git_suffix_is_treated_as_same_repo(self):
        # 同一个 GitHub 仓库既可写成 ...://x/y，也可写成 ...://x/y.git。
        # 缓存里存的是调用者给的写法，判断「同一仓库」时必须抹掉 .git 后缀，
        # 否则本地已有缓存会被误判成 origin 不匹配。
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            cache = os.path.join(d, 'cache')
            ensure_repo(url, 'demo', cache_dir=cache)
            self.assertEqual(ensure_repo(url + '.git', 'demo', cache_dir=cache),
                             os.path.join(cache, 'demo'))

    def test_normalize_repo_url_strips_git_suffix(self):
        from repo_cache import _normalize_repo_url
        self.assertEqual(
            _normalize_repo_url('https://github.com/x/y.git'),
            _normalize_repo_url('https://github.com/x/y'),
        )
        self.assertEqual(
            _normalize_repo_url('https://GitHub.com/X/Y.git/'),
            'https://github.com/x/y',
        )

    def test_clones_when_absent(self):
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            self.assertEqual(target, os.path.join(cache, "demo"))
            self.assertTrue(os.path.isdir(os.path.join(target, ".git")))
            self.assertTrue(os.path.isfile(os.path.join(target, "src", "a.c")))

    def test_creates_cache_dir_when_absent(self):
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            cache = os.path.join(d, "deep", "nested", "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            self.assertTrue(os.path.isdir(cache))
            self.assertTrue(os.path.isdir(os.path.join(target, ".git")))

    def test_idempotent_when_present(self):
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            cache = os.path.join(d, "cache")
            first = ensure_repo(url, "demo", cache_dir=cache)
            mtime_before = os.path.getmtime(first)
            second = ensure_repo(url, "demo", cache_dir=cache)
            self.assertEqual(first, second)
            self.assertEqual(mtime_before, os.path.getmtime(second))

    def test_idempotent_does_not_touch_worktree(self):
        """已存在时不得重克隆：工作树里的本地改动必须原样保留。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            marker = os.path.join(target, "src", "local_marker.txt")
            with open(marker, "w", encoding="utf-8", newline="\n") as f:
                f.write("keep me\n")
            commits_before = _commit_count(target)
            ensure_repo(url, "demo", cache_dir=cache)
            self.assertTrue(os.path.isfile(marker))
            self.assertEqual(commits_before, _commit_count(target))

    def test_idempotent_does_not_fetch_new_upstream_commits(self):
        """只做缓存，不做拉取：上游出现在克隆之后的提交不会进入本地。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            up = os.path.join(d, "upstream")
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            with open(
                os.path.join(up, "src", "a.c"), "w", encoding="utf-8", newline="\n"
            ) as f:
                f.write("int Gamma(void);\n")
            _git(["add", "."], up)
            _git(["commit", "-m", "late"], up)
            ensure_repo(url, "demo", cache_dir=cache)
            self.assertEqual(_commit_count(target), 1)
            with open(
                os.path.join(target, "src", "a.c"), "r", encoding="utf-8"
            ) as f:
                self.assertNotIn("Gamma", f.read())

    def test_depth_is_forwarded(self):
        """depth 必须真的传给 --depth：上游 3 次提交，depth=2 只拿到 2 条。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d, commits=3)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache, depth=2)
            self.assertEqual(_commit_count(target), 2)

    def test_default_depth_is_one(self):
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d, commits=3)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            self.assertEqual(_commit_count(target), 1)
            self.assertTrue(os.path.isfile(os.path.join(target, ".git", "shallow")))

    def test_single_branch_only_fetches_one_branch(self):
        """--single-branch 的可观察结果：fetch refspec 不是通配，远端引用只有一条。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d, extra_branch=True)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache)
            spec = _git(["config", "--get", "remote.origin.fetch"], target)
            self.assertEqual(spec.returncode, 0, spec.stderr)
            self.assertNotIn("*", spec.stdout, "fetch refspec 是通配，说明没走单分支")
            refs = _git(
                ["for-each-ref", "refs/remotes/", "--format=%(refname)"], target
            )
            self.assertEqual(refs.returncode, 0, refs.stderr)
            names = [line.strip() for line in refs.stdout.splitlines() if line.strip()]
            self.assertEqual(
                names, ["refs/remotes/origin/HEAD", "refs/remotes/origin/main"]
            )

    def test_git_argv_carries_depth_and_single_branch(self):
        """直接核对传给 git 的参数。

        `--depth` 自身就会隐含单分支，所以单看克隆结果无法证明 `--single-branch`
        真的写着；这里把 subprocess.run 换成记录器，逐字核对命令行。
        """
        captured = {}

        class _Proc:
            returncode = 0
            stdout = ""
            stderr = ""

        def _fake_run(cmd, **kwargs):
            captured["cmd"] = list(cmd)
            captured["kwargs"] = kwargs
            return _Proc()

        with tempfile.TemporaryDirectory() as d, mock.patch(
            "repo_cache.subprocess.run", side_effect=_fake_run
        ):
            cache = os.path.join(d, "cache")
            os.makedirs(cache)
            ensure_repo("file:///up.git", "demo", cache_dir=cache, depth=3)
        cmd = captured["cmd"]
        self.assertEqual(cmd[:2], ["git", "clone"])
        self.assertIn("--depth", cmd)
        self.assertEqual(cmd[cmd.index("--depth") + 1], "3")
        self.assertIn("--single-branch", cmd)
        self.assertEqual(cmd[-2:], ["file:///up.git", os.path.join(cache, "demo")])

    # ---------- full=True：全量克隆 ----------

    def test_full_clone_keeps_all_commits(self):
        """full=True 必须拿到完整历史，不是 depth=1。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d, commits=5)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache, full=True)
            self.assertEqual(_commit_count(target), 5)
            self.assertFalse(
                os.path.isfile(os.path.join(target, ".git", "shallow")),
                "全量克隆不该留下 shallow 标记",
            )

    def test_full_clone_keeps_all_branches(self):
        """full=True 必须拿到全部分支，fetch refspec 应是通配。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d, extra_branch=True)
            cache = os.path.join(d, "cache")
            target = ensure_repo(url, "demo", cache_dir=cache, full=True)
            spec = _git(["config", "--get", "remote.origin.fetch"], target)
            self.assertEqual(spec.returncode, 0, spec.stderr)
            self.assertIn("*", spec.stdout, "fetch refspec 不是通配，说明走了单分支")
            refs = _git(
                ["for-each-ref", "refs/remotes/", "--format=%(refname)"], target
            )
            self.assertEqual(refs.returncode, 0, refs.stderr)
            names = [line.strip() for line in refs.stdout.splitlines() if line.strip()]
            self.assertIn("refs/remotes/origin/extra", names)

    def test_full_clone_argv_has_no_depth_or_single_branch(self):
        """直接核对传给 git 的参数：全量克隆不能带 --depth / --single-branch。

        全量克隆用 `--filter=blob:none`（partial clone）——保留完整提交历史与
        全部 tag，blob 按需下载。实测 postgres 全量 pack 传输会卡死 25 分钟，
        改用 filter 后 9 分钟完成，因此参数里必须有 --filter。
        """
        captured = {}

        class _Proc:
            returncode = 0
            stdout = ""
            stderr = ""

        def _fake_run(cmd, **kwargs):
            captured["cmd"] = list(cmd)
            return _Proc()

        with tempfile.TemporaryDirectory() as d, mock.patch(
            "repo_cache.subprocess.run", side_effect=_fake_run
        ):
            cache = os.path.join(d, "cache")
            os.makedirs(cache)
            ensure_repo("file:///up.git", "demo", cache_dir=cache, full=True)

        cmd = captured["cmd"]
        self.assertEqual(cmd[:2], ["git", "clone"])
        self.assertNotIn("--depth", cmd)
        self.assertNotIn("--single-branch", cmd)
        self.assertIn("--filter=blob:none", cmd)
        self.assertEqual(cmd[-2:], ["file:///up.git", os.path.join(cache, "demo")])

    def test_default_is_still_shallow(self):
        """不传 full 时保持浅克隆——那是只跑门禁场景的快路径。"""
        captured = {}

        class _Proc:
            returncode = 0
            stdout = ""
            stderr = ""

        def _fake_run(cmd, **kwargs):
            captured["cmd"] = list(cmd)
            return _Proc()

        with tempfile.TemporaryDirectory() as d, mock.patch(
            "repo_cache.subprocess.run", side_effect=_fake_run
        ):
            cache = os.path.join(d, "cache")
            os.makedirs(cache)
            ensure_repo("file:///up.git", "demo", cache_dir=cache)

        self.assertIn("--depth", captured["cmd"])
        self.assertIn("--single-branch", captured["cmd"])

    def test_bad_url_raises(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "cache")
            with self.assertRaises(RepoCacheError):
                ensure_repo("file:///no/such/repo.git", "bad", cache_dir=cache)

    def test_error_message_names_repo_url(self):
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "cache")
            with self.assertRaises(RepoCacheError) as ctx:
                ensure_repo("file:///no/such/repo.git", "bad", cache_dir=cache)
            self.assertIn("file:///no/such/repo.git", str(ctx.exception))

    def test_failed_clone_leaves_no_partial_dir(self):
        """克隆失败不得留下半成品目录：下次调用应仍能正常克隆。"""
        with tempfile.TemporaryDirectory() as d:
            cache = os.path.join(d, "cache")
            with self.assertRaises(RepoCacheError):
                ensure_repo("file:///no/such/repo.git", "bad", cache_dir=cache)
            self.assertFalse(os.path.exists(os.path.join(cache, "bad")))
            url = _make_upstream(d)
            target = ensure_repo(url, "bad", cache_dir=cache)
            self.assertTrue(os.path.isdir(os.path.join(target, ".git")))

    def test_failed_cleanup_does_not_touch_external_symlink_target(self):
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream_with_uncheckoutable_path(d)
            cache = os.path.join(d, "cache")
            outside = os.path.join(d, "outside")
            os.mkdir(outside)
            protected = os.path.join(outside, "protected.txt")
            with open(protected, "w") as f:
                f.write("keep")
            target = os.path.join(cache, "demo")
            os.makedirs(target)
            link = os.path.join(target, "external-link")
            try:
                os.symlink(outside, link, target_is_directory=True)
            except (OSError, NotImplementedError) as exc:
                self.skipTest("平台不支持 symlink：%s" % exc)
            before = os.stat(protected).st_mode
            with self.assertRaises(RepoCacheError):
                ensure_repo(url, "demo", cache_dir=cache)
            self.assertEqual(os.stat(protected).st_mode, before)
            with open(protected) as f:
                self.assertEqual(f.read(), "keep")

    def test_cleanup_after_checkout_failure(self):
        """传输成功、检出失败时 target 已含 .git：必须清理，否则被当成有效缓存。

        用一段 300 字符的文件名当上游内容：能克隆传输，但检出必然失败，
        且此时 target/.git 已经建好——正是「半成品目录」的真实形态。
        """
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream_with_uncheckoutable_path(d)
            cache = os.path.join(d, "cache")
            with self.assertRaises(RepoCacheError):
                ensure_repo(url, "demo", cache_dir=cache)
            target = repo_path(cache, "demo")
            self.assertFalse(
                os.path.isdir(os.path.join(target, ".git")),
                "残留的 .git 会让下次调用直接把它当有效缓存返回",
            )
            self.assertFalse(os.path.exists(target))

    def test_stale_dir_without_git_raises_instead_of_reusing(self):
        """目录存在但没有 .git：宁可硬失败，也不拿来路不明的内容当缓存。"""
        with tempfile.TemporaryDirectory() as d:
            url = _make_upstream(d)
            cache = os.path.join(d, "cache")
            target = repo_path(cache, "demo")
            os.makedirs(target)
            with open(
                os.path.join(target, "garbage.txt"), "w", encoding="utf-8", newline="\n"
            ) as f:
                f.write("stale\n")
            with self.assertRaises(RepoCacheError):
                ensure_repo(url, "demo", cache_dir=cache)
            # 不破坏既有内容，留给人工确认。
            self.assertTrue(os.path.isfile(os.path.join(target, "garbage.txt")))


if __name__ == "__main__":
    unittest.main()
