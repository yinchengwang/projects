# -*- coding: utf-8 -*-
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_refs import MAX_FILE_SIZE, check_refs, locate_symbol, main

PAGE = """---
project: demo
feature: buffer-pool
page_type: feature
schema_version: 1
refs:
  - path: src/bufmgr.c
    symbols: [ReadBufferExtended, PinBuffer]
  - path: src/freelist.c
    symbols: [StrategyGetBuffer]
---

# 占位
"""

BUFMGR = """/* 伪造的 bufmgr.c，用于测试符号匹配 */
void *ReadBufferExtended(void);
static void PinBuffer(void);
"""

FREELIST = """/* 伪造的 freelist.c */
int StrategyGetBuffer(int flags);
"""


def _write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    return path


def _write_bytes(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


def _build_repo(root):
    _write(os.path.join(root, "src", "bufmgr.c"), BUFMGR)
    _write(os.path.join(root, "src", "freelist.c"), FREELIST)


def _try_link_outside(repo, outside_file):
    """在 repo 内建一条指向仓外的链接，返回 refs 可用的相对路径。

    优先用文件 symlink；Windows 上普通账户没有创建 symlink 的权限时，
    退回目录 junction（mklink /J 不需要提权），避免这个安全用例被整片跳过。
    两种都建不出来才返回 None，由调用方 skip。
    """
    try:
        os.symlink(outside_file, os.path.join(repo, "src", "linked.c"))
        return "src/linked.c"
    except (AttributeError, NotImplementedError, OSError):
        pass
    if os.name != "nt":
        return None
    junction = os.path.join(repo, "src", "linked_dir")
    try:
        completed = subprocess.run(
            ["cmd", "/c", "mklink", "/J", junction, os.path.dirname(outside_file)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    if completed.returncode != 0 or not os.path.isdir(junction):
        return None
    return "src/linked_dir/" + os.path.basename(outside_file)


class TestCheckRefs(unittest.TestCase):
    def test_all_refs_hit(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            page = _write(os.path.join(d, "a04-buffer-pool.md"), PAGE)
            _build_repo(repo)
            self.assertEqual(check_refs(page, repo), [])

    def test_missing_file(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            page = _write(os.path.join(d, "a04-buffer-pool.md"), PAGE)
            os.makedirs(repo)
            errors = check_refs(page, repo)
            self.assertTrue(any("不存在: src/bufmgr.c" in e for e in errors), errors)

    def test_missing_symbol_hard_fails(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("symbols: [ReadBufferExtended, PinBuffer]", "symbols: [ReadBufferExtended, NoSuchSymbol]")
            page = _write(os.path.join(d, "a04-buffer-pool.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("未命中: NoSuchSymbol" in e for e in errors), errors)

    def test_symbol_must_be_in_its_own_file(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("symbols: [StrategyGetBuffer]", "symbols: [ReadBufferExtended]")
            page = _write(os.path.join(d, "a04-buffer-pool.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("未命中: ReadBufferExtended (src/freelist.c)" in e for e in errors), errors)

    def test_no_refs_field(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            page = _write(
                os.path.join(d, "a04-buffer-pool.md"),
                PAGE.replace(
                    "refs:\n  - path: src/bufmgr.c\n    symbols: [ReadBufferExtended, PinBuffer]\n"
                    "  - path: src/freelist.c\n    symbols: [StrategyGetBuffer]\n",
                    "",
                ),
            )
            errors = check_refs(page, repo)
            self.assertTrue(any("refs 缺失或不是列表" in e for e in errors), errors)

    def test_utf8_content_with_bom_tolerated(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            page = _write(os.path.join(d, "a04-buffer-pool.md"), PAGE)
            path = os.path.join(repo, "src", "bufmgr.c")
            with open(path, "wb") as f:
                f.write(b"\xef\xbb\xbf" + BUFMGR.encode("utf-8"))
            self.assertEqual(check_refs(page, repo), [])

    def test_symbol_matches_with_word_boundary_only(self):
        """PinBuffer 不得命中 PinBufferX：词边界是硬性要求。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            _write(os.path.join(repo, "src", "bufmgr.c"), "void PinBufferX(void);\n")
            page = _write(os.path.join(d, "page.md"), PAGE)
            errors = check_refs(page, repo)
            self.assertTrue(any("未命中: PinBuffer" in e for e in errors), errors)
            self.assertFalse(any("未命中: PinBufferX" in e for e in errors), errors)

    def test_symbol_hit_inside_longer_identifier_does_not_count(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            # 只出现「内嵌在其他标识符里」的形态：词边界决定它不能算命中。
            _write(
                os.path.join(repo, "src", "freelist.c"),
                "int my_StrategyGetBuffer_wrapper(int flags);\n",
            )
            page = _write(os.path.join(d, "page.md"), PAGE)
            errors = check_refs(page, repo)
            self.assertTrue(any("未命中: StrategyGetBuffer" in e for e in errors), errors)

    def test_non_utf8_bytes_are_tolerated(self):
        """混合编码是真实上游仓的常态：坏字节不能被当成符号缺失。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            path = os.path.join(repo, "src", "bufmgr.c")
            data = (
                b"/* comment: caf\xe9 \xff\xfe */\n"
                + b"void *ReadBufferExtended(void);\n"
                + b"static void PinBuffer(void);\n"
            )
            _write_bytes(path, data)
            page = _write(os.path.join(d, "page.md"), PAGE)
            self.assertEqual(check_refs(page, repo), [])

    def test_crlf_source_file_is_matched(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            path = os.path.join(repo, "src", "bufmgr.c")
            _write_bytes(path, BUFMGR.replace("\n", "\r\n").encode("utf-8"))
            page = _write(os.path.join(d, "page.md"), PAGE)
            self.assertEqual(check_refs(page, repo), [])

    def test_backslash_reference_path_resolves(self):
        """Windows 上手写 refs 容易填反斜杠，不能因分隔符判成不存在。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("src/bufmgr.c", "src\\bufmgr.c").replace("src/freelist.c", "src\\freelist.c")
            page = _write(os.path.join(d, "page.md"), content)
            self.assertEqual(check_refs(page, repo), [])

    def test_parent_path_outside_repo_fails_even_when_symbol_exists(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            outside = _write(os.path.join(d, "outside.c"), "int ReadBufferExtended(void);\n")
            content = PAGE.replace("src/bufmgr.c", "../outside.c").replace(
                "symbols: [ReadBufferExtended, PinBuffer]", "symbols: [ReadBufferExtended]"
            )
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("超出仓库根目录" in e for e in errors), errors)
            self.assertTrue(os.path.isfile(outside))

    def test_absolute_path_outside_repo_fails_even_when_symbol_exists(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            outside = _write(os.path.join(d, "outside.c"), "int ReadBufferExtended(void);\n")
            # 仓外绝对路径：os.path.join 给的就是绝对形式，跨平台可用。
            content = PAGE.replace("src/bufmgr.c", outside).replace(
                "symbols: [ReadBufferExtended, PinBuffer]", "symbols: [ReadBufferExtended]"
            )
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("超出仓库根目录" in e for e in errors), errors)
            self.assertTrue(os.path.isfile(outside))

    def test_link_outside_repo_fails(self):
        """仓内的链接指向仓外：realpath 解析后不在仓根内，必须 FAIL。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            outside = _write(os.path.join(d, "outside.c"), "int ReadBufferExtended(void);\n")
            rel = _try_link_outside(repo, outside)
            if rel is None:
                self.skipTest("平台不支持 symlink/junction")
            content = PAGE.replace("src/bufmgr.c", rel).replace(
                "symbols: [ReadBufferExtended, PinBuffer]", "symbols: [ReadBufferExtended]"
            )
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("超出仓库根目录" in e for e in errors), errors)
            self.assertTrue(os.path.isfile(outside))

    def test_nul_after_binary_probe_window_fails_hard(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            # 符号在头部窗口内可见，NUL 落在 _BINARY_PROBE 之后：
            # 只探头部会把它当源码，符号命中就误放行了。
            data = b"int ReadBufferExtended(void);\n" + b"/* pad */" + b" " * 8192 + b"\x00"
            _write_bytes(os.path.join(repo, "src", "nul-late.c"), data)
            content = PAGE.replace("path: src/bufmgr.c", "path: src/nul-late.c")
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("无法校验" in e and "NUL" in e for e in errors), errors)

    def test_directory_path_is_not_a_file(self):
        """path 指向目录必须判为不存在（不能误当成源码文件）。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("path: src/bufmgr.c", "path: src")
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("不存在: src" in e for e in errors), errors)

    def test_binary_ref_fails_hard(self):
        """二进制文件里不可能有源码符号，按硬失败处理，不放行。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            _write_bytes(os.path.join(repo, "src", "blob.bin"), b"\x00\x01\x02ReadBufferExtended")
            content = PAGE.replace("path: src/freelist.c", "path: src/blob.bin").replace(
                "symbols: [StrategyGetBuffer]", "symbols: [ReadBufferExtended]"
            )
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("无法校验" in e for e in errors), errors)

    def test_oversized_ref_fails_hard(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            _write_bytes(
                os.path.join(repo, "src", "big.c"),
                b"ReadBufferExtended\n" + b" " * (MAX_FILE_SIZE + 1024),
            )
            content = PAGE.replace("path: src/freelist.c", "path: src/big.c").replace(
                "symbols: [StrategyGetBuffer]", "symbols: [ReadBufferExtended]"
            )
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("无法校验" in e for e in errors), errors)

    def test_empty_symbols_list_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("symbols: [ReadBufferExtended, PinBuffer]", "symbols: []")
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("symbols 必须是非空列表" in e for e in errors), errors)

    def test_empty_symbol_element_rejected(self):
        """行内列表里的空字符串元素不是合法符号，不能被静默当成「未命中符号」。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace(
                "symbols: [ReadBufferExtended, PinBuffer]", 'symbols: [ReadBufferExtended, ""]'
            )
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("含非法符号" in e for e in errors), errors)

    def test_non_list_symbols_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("symbols: [ReadBufferExtended, PinBuffer]", "symbols: 42")
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("symbols 必须是非空列表" in e for e in errors), errors)

    def test_missing_path_field(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            content = PAGE.replace("  - path: src/freelist.c\n    symbols: [StrategyGetBuffer]\n", "  - symbols: [StrategyGetBuffer]\n")
            page = _write(os.path.join(d, "page.md"), content)
            errors = check_refs(page, repo)
            self.assertTrue(any("path 缺失" in e for e in errors), errors)

    def test_invalid_frontmatter_returns_error(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(os.path.join(d, "page.md"), "not frontmatter\n")
            errors = check_refs(page, d)
            self.assertTrue(any("front-matter 解析失败" in e for e in errors), errors)

    def test_refs_not_a_list(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(
                os.path.join(d, "page.md"),
                PAGE.replace(
                    "refs:\n  - path: src/bufmgr.c\n    symbols: [ReadBufferExtended, PinBuffer]\n"
                    "  - path: src/freelist.c\n    symbols: [StrategyGetBuffer]\n",
                    "refs: src/bufmgr.c\n",
                ),
            )
            errors = check_refs(page, d)
            self.assertTrue(any("refs 缺失或不是列表" in e for e in errors), errors)


class TestLocateSymbol(unittest.TestCase):
    def test_finds_file_and_line(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            hits = locate_symbol(repo, "ReadBufferExtended")
            self.assertEqual(len(hits), 1)
            rel, lineno, text = hits[0]
            self.assertEqual(rel, "src/bufmgr.c")
            self.assertEqual(lineno, 2)
            self.assertEqual(text, "void *ReadBufferExtended(void);")

    def test_skips_dot_git(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            _write(os.path.join(repo, ".git", "fake.c"), "ReadBufferExtended\n")
            hits = locate_symbol(repo, "ReadBufferExtended")
            self.assertEqual([h[0] for h in hits], ["src/bufmgr.c"])

    def test_skips_vendored_and_build_dirs(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            for dirname in ("build", "node_modules", "target", "vendor", "third_party", "deps", "dist"):
                _write(os.path.join(repo, dirname, "fake.c"), "ReadBufferExtended\n")
            hits = locate_symbol(repo, "ReadBufferExtended")
            self.assertEqual([h[0] for h in hits], ["src/bufmgr.c"])

    def test_respects_max_hits(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            for i in range(5):
                _write(os.path.join(repo, "src", "f%d.c" % i), "int Dup(int x);\n")
            hits = locate_symbol(repo, "Dup", max_hits=2)
            self.assertEqual(len(hits), 2)

    def test_max_hits_caps_even_when_many_more_exist(self):
        """上限必须在遍历过程中生效，而不是扫完全仓再截断。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            for i in range(200):
                _write(os.path.join(repo, "src", "f%03d.c" % i), "int Dup(int x);\n")
            hits = locate_symbol(repo, "Dup", max_hits=3)
            self.assertEqual(len(hits), 3)

    def test_symbol_not_found_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            self.assertEqual(locate_symbol(repo, "NoSuchSymbol"), [])

    def test_word_boundary_applies_to_locate(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write(os.path.join(repo, "src", "a.c"), "void PinBufferX(void);\n")
            _write(os.path.join(repo, "src", "b.c"), "static void PinBuffer(void);\n")
            hits = locate_symbol(repo, "PinBuffer")
            self.assertEqual([h[0] for h in hits], ["src/b.c"])

    def test_only_code_extensions_are_scanned(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write(os.path.join(repo, "README.md"), "ReadBufferExtended\n")
            _write(os.path.join(repo, "src", "bufmgr.c"), BUFMGR)
            hits = locate_symbol(repo, "ReadBufferExtended")
            self.assertEqual([h[0] for h in hits], ["src/bufmgr.c"])

    def test_exts_parameter_narrows_scan(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write(os.path.join(repo, "src", "a.c"), "int Dup(void);\n")
            _write(os.path.join(repo, "src", "b.go"), "func Dup() {}\n")
            hits = locate_symbol(repo, "Dup", exts=(".go",))
            self.assertEqual([h[0] for h in hits], ["src/b.go"])

    def test_binary_file_is_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write_bytes(os.path.join(repo, "src", "blob.c"), b"\x00\x01TargetSymbol" + b"\x00" * 100)
            _write(os.path.join(repo, "src", "real.c"), "int TargetSymbol(void);\n")
            hits = locate_symbol(repo, "TargetSymbol")
            self.assertEqual([h[0] for h in hits], ["src/real.c"])

    def test_binary_file_with_late_nul_is_skipped(self):
        """NUL 落在头部窗口之后也要跳过：否则 locate 会报假的源码命中。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write_bytes(
                os.path.join(repo, "src", "blob.c"),
                b"int TargetSymbol(void);\n" + b" " * 8192 + b"\x00",
            )
            _write(os.path.join(repo, "src", "real.c"), "int TargetSymbol(void);\n")
            hits = locate_symbol(repo, "TargetSymbol")
            self.assertEqual([h[0] for h in hits], ["src/real.c"])

    def test_oversized_file_is_skipped(self):
        """超大生成文件必须跳过，否则一次 --locate 会被拖到分钟级。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write_bytes(
                os.path.join(repo, "src", "huge.c"),
                b"int TargetSymbol(void);\n" + b" " * (MAX_FILE_SIZE + 1024),
            )
            _write(os.path.join(repo, "src", "real.c"), "int TargetSymbol(void);\n")
            hits = locate_symbol(repo, "TargetSymbol")
            self.assertEqual([h[0] for h in hits], ["src/real.c"])

    def test_non_utf8_file_is_scanned(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write_bytes(
                os.path.join(repo, "src", "a.c"),
                b"/* caf\xe9 \xff */\nint Dup(void);\n",
            )
            hits = locate_symbol(repo, "Dup")
            self.assertEqual([h[0] for h in hits], ["src/a.c"])

    def test_relative_path_uses_forward_slashes(self):
        """返回的相对路径必须与契约一致，用正斜杠，跨平台可比。"""
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write(os.path.join(repo, "src", "deep", "a.c"), "int Dup(void);\n")
            hits = locate_symbol(repo, "Dup")
            self.assertEqual([h[0] for h in hits], ["src/deep/a.c"])

    def test_empty_symbol_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write(os.path.join(repo, "src", "a.c"), "int Dup(void);\n")
            self.assertEqual(locate_symbol(repo, ""), [])

    def test_max_hits_zero_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _write(os.path.join(repo, "src", "a.c"), "int Dup(void);\n")
            self.assertEqual(locate_symbol(repo, "Dup", max_hits=0), [])

    def test_missing_repo_root_returns_empty(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(locate_symbol(os.path.join(d, "nope"), "Dup"), [])


class TestMainCli(unittest.TestCase):
    """CLI：退出码 0 / 1 与目录模式汇总。"""

    def _run(self, argv):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = main(argv)
        return code, buf.getvalue()

    def test_single_good_page_exit_zero(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            page = _write(os.path.join(d, "a04-buffer-pool.md"), PAGE)
            code, out = self._run([page, "--repo-root", repo, "--quiet"])
            self.assertEqual(code, 0)
            self.assertIn("-- 检查 1 页，失败 0 页", out)

    def test_single_bad_page_exit_one(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            content = PAGE.replace("symbols: [ReadBufferExtended, PinBuffer]", "symbols: [ReadBufferExtended, NoSuchSymbol]")
            page = _write(os.path.join(d, "a04-buffer-pool.md"), content)
            code, out = self._run([page, "--repo-root", repo, "--quiet"])
            self.assertEqual(code, 1)
            self.assertIn("-- 检查 1 页，失败 1 页", out)
            self.assertIn("未命中: NoSuchSymbol", out)

    def test_directory_mode_recurses(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            _write(os.path.join(d, "ok", "a04-buffer-pool.md"), PAGE)
            page = os.path.join(d, "bad", "a04-buffer-pool.md")
            _write(page, PAGE.replace("symbols: [StrategyGetBuffer]", "symbols: [ReadBufferExtended]"))
            code, out = self._run([d, "--repo-root", repo, "--quiet"])
            self.assertEqual(code, 1)
            self.assertIn("-- 检查 2 页，失败 1 页", out)

    def test_directory_mode_skips_hidden_dirs(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            _write(os.path.join(d, "a04-buffer-pool.md"), PAGE)
            _write(os.path.join(d, ".hidden", "bad.md"), "# 没有 front-matter\n")
            code, out = self._run([d, "--repo-root", repo, "--quiet"])
            self.assertEqual(code, 0)
            self.assertIn("-- 检查 1 页，失败 0 页", out)

    def test_locate_prints_file_and_line(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            code, out = self._run(["--locate", "ReadBufferExtended", "--repo-root", repo])
            self.assertEqual(code, 0)
            self.assertIn("src/bufmgr.c:2", out)

    def test_locate_missing_symbol_exits_one(self):
        with tempfile.TemporaryDirectory() as d:
            repo = os.path.join(d, "repo")
            _build_repo(repo)
            code, out = self._run(["--locate", "NoSuchSymbol", "--repo-root", repo])
            self.assertEqual(code, 1)
            self.assertIn("未找到符号 NoSuchSymbol", out)

    def test_locate_requires_repo_root(self):
        with self.assertRaises(SystemExit):
            main(["--locate", "Dup"])

    def test_check_requires_paths(self):
        with self.assertRaises(SystemExit):
            main([])

    def test_check_requires_repo_root(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(os.path.join(d, "page.md"), PAGE)
            with self.assertRaises(SystemExit):
                main([page])

    def test_check_rejects_non_directory_repo_root(self):
        with tempfile.TemporaryDirectory() as d:
            page = _write(os.path.join(d, "page.md"), PAGE)
            with self.assertRaises(SystemExit):
                main([page, "--repo-root", os.path.join(d, "nope")])


if __name__ == "__main__":
    unittest.main()
