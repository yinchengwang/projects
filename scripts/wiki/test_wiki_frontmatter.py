# -*- coding: utf-8 -*-
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_frontmatter import FrontmatterError, parse_frontmatter, parse_scalar

GOOD = """---
project: postgres
feature: buffer-pool
page_type: feature
schema_version: 1
stars: 13500
stars_as_of: 2026-11-22
refs:
  - path: src/backend/storage/buffer/bufmgr.c
    symbols: [ReadBufferExtended, PinBuffer, FlushBuffer]
  - path: src/backend/storage/buffer/freelist.c
    symbols: [StrategyGetBuffer]
---

# 正文从这里开始
"""


class TestParseFrontmatter(unittest.TestCase):
    def test_returns_meta_and_body(self):
        meta, body = parse_frontmatter(GOOD)
        self.assertIsInstance(meta, dict)
        self.assertIn("# 正文从这里开始", body)
        self.assertNotIn("project: postgres", body)

    def test_scalar_fields(self):
        meta, _ = parse_frontmatter(GOOD)
        self.assertEqual(meta["project"], "postgres")
        self.assertEqual(meta["feature"], "buffer-pool")
        self.assertEqual(meta["page_type"], "feature")

    def test_pure_digits_become_int(self):
        meta, _ = parse_frontmatter(GOOD)
        self.assertEqual(meta["schema_version"], 1)
        self.assertEqual(meta["stars"], 13500)

    def test_date_like_stays_string(self):
        meta, _ = parse_frontmatter(GOOD)
        self.assertEqual(meta["stars_as_of"], "2026-11-22")

    def test_nested_list_of_maps(self):
        meta, _ = parse_frontmatter(GOOD)
        self.assertEqual(len(meta["refs"]), 2)
        self.assertEqual(meta["refs"][0]["path"], "src/backend/storage/buffer/bufmgr.c")
        self.assertEqual(
            meta["refs"][0]["symbols"],
            ["ReadBufferExtended", "PinBuffer", "FlushBuffer"],
        )
        self.assertEqual(meta["refs"][1]["symbols"], ["StrategyGetBuffer"])

    def test_missing_opening_fence(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("# 没有 front-matter\n")

    def test_missing_closing_fence(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nproject: x\n")

    def test_blank_body_is_allowed(self):
        meta, body = parse_frontmatter("---\nproject: x\n---\n")
        self.assertEqual(meta["project"], "x")
        self.assertEqual(body.strip(), "")

    def test_tab_indentation_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n\t- path: x\n---\n")

    def test_block_scalar_is_rejected(self):
        for scalar in ("|", ">", "|-", ">+"):
            with self.subTest(scalar=scalar), self.assertRaises(FrontmatterError):
                parse_frontmatter("---\nsummary: " + scalar + "\n---\n")

    def test_anchor_and_alias_are_rejected(self):
        for value in ("&shared value", "*shared"):
            with self.subTest(value=value), self.assertRaises(FrontmatterError):
                parse_frontmatter("---\nkey: " + value + "\n---\n")

    def test_inline_mapping_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nkey: {name: value}\n---\n")

    def test_nested_list_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nitems:\n  - - nested\n---\n")

    def test_inconsistent_indentation_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n  - path: x\n   symbols: [s]\n---\n")

    def test_inner_keys_align_with_list_item_content_column(self):
        # 列表项内容从第 6 列开始，内嵌键就应对齐到第 6 列。
        meta, _ = parse_frontmatter(
            "---\nrefs:\n  -   path: a.c\n      symbols: [S]\n---\n"
        )
        self.assertEqual(meta["refs"], [{"path": "a.c", "symbols": ["S"]}])

    def test_misaligned_inner_keys_after_wide_dash_are_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n  -   path: a.c\n    symbols: [S]\n---\n")

    def test_key_with_empty_value_then_no_list_stays_empty_list(self):
        meta, _ = parse_frontmatter("---\nrefs:\nother: x\n---\n")
        self.assertEqual(meta["refs"], [])
        self.assertEqual(meta["other"], "x")

    def test_duplicate_key_in_list_item_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n  - path: a\n    path: b\n---\n")

    def test_list_item_outside_any_key_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\n  - path: a\n---\n")

    def test_crlf_input_is_accepted(self):
        meta, body = parse_frontmatter("---\r\nproject: x\r\n---\r\nline\r\n")
        self.assertEqual(meta["project"], "x")
        self.assertEqual(body, "line\n")

    def test_comment_and_blank_lines_inside_block_are_skipped(self):
        meta, _ = parse_frontmatter("---\n# 注释\n\nproject: x\n---\n")
        self.assertEqual(meta, {"project": "x"})

    def test_trailing_inline_comment_is_rejected(self):
        # 静默把 "# 注释" 留在值里就是误解析，必须报错。
        for text in ("---\nk: v # 注释\n---\n", "---\nk: [a, b] # 注释\n---\n"):
            with self.subTest(text=text), self.assertRaises(FrontmatterError):
                parse_frontmatter(text)

    def test_hash_inside_quotes_is_kept(self):
        meta, _ = parse_frontmatter('---\nk: "a # b"\n---\n')
        self.assertEqual(meta["k"], "a # b")

    def test_negative_number_stays_string(self):
        # 契约里的数字字段（stars / schema_version）不会是负数，
        # 负数按普通字符串处理，不做额外推断。
        self.assertEqual(parse_scalar("-5"), "-5")

    def test_duplicate_top_level_key_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nproject: a\nproject: b\n---\n")

    def test_url_value_keeps_scheme_colon(self):
        # repo 是 overview 页必填字段，值形如 https://...，冒号不能被当分隔符吃掉。
        meta, _ = parse_frontmatter(
            "---\nrepo: https://github.com/postgres/postgres\n---\n"
        )
        self.assertEqual(meta["repo"], "https://github.com/postgres/postgres")

    def test_list_item_url_value_keeps_scheme_colon(self):
        meta, _ = parse_frontmatter(
            "---\nrefs:\n  - path: a.c\n    link: https://x.dev/a.c#L10\n---\n"
        )
        self.assertEqual(meta["refs"][0]["link"], "https://x.dev/a.c#L10")

    def test_colon_without_space_at_top_level_is_rejected(self):
        # YAML 的键值分隔要求冒号后有空格，缺空格就是写错了。
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nproject:x\n---\n")

    def test_colon_without_space_in_list_item_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n  - path:a.c\n---\n")

    def test_space_before_colon_is_accepted(self):
        meta, _ = parse_frontmatter("---\nproject : postgres\n---\n")
        self.assertEqual(meta["project"], "postgres")

    def test_empty_value_inside_list_item_is_rejected(self):
        # 列表项内再出现「键后空值」就意味着三级嵌套，契约里不出现，直接报错。
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n  - path:\n      symbols: [S]\n---\n")

    def test_empty_value_on_indented_key_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nrefs:\n  - path: a\n    symbols:\n---\n")

    def test_quoted_escape_hatch_for_plain_list_item_with_colon(self):
        # 冒号后没空格的裸标量会被当成写错的映射；要写纯文本就加引号。
        meta, _ = parse_frontmatter('---\nrefs:\n  - "参见 http://x.dev/doc"\n---\n')
        self.assertEqual(meta["refs"], ["参见 http://x.dev/doc"])

    def test_quoted_escape_hatch_for_value_with_hash(self):
        meta, _ = parse_frontmatter('---\ntitle: "Issue #5"\n---\n')
        self.assertEqual(meta["title"], "Issue #5")

    def test_negative_scalar_list_item_is_not_treated_as_nested_list(self):
        # `- -5` 是负数标量，只有 `- - x` 才算嵌套列表。
        meta, _ = parse_frontmatter("---\nitems:\n  - -5\n  - -7\n---\n")
        self.assertEqual(meta["items"], ["-5", "-7"])

    def test_inline_list_as_list_item_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter("---\nitems:\n  - [a, b]\n---\n")

    def test_value_containing_colon_keeps_everything_after_first_separator(self):
        meta, _ = parse_frontmatter("---\ntitle: a: b: c\n---\n")
        self.assertEqual(meta["title"], "a: b: c")


class TestParseScalar(unittest.TestCase):
    def test_plain_string(self):
        self.assertEqual(parse_scalar("postgres"), "postgres")

    def test_inline_list(self):
        self.assertEqual(parse_scalar("[a, b, c]"), ["a", "b", "c"])

    def test_empty_inline_list(self):
        self.assertEqual(parse_scalar("[]"), [])

    def test_quoted_string_unwrapped(self):
        self.assertEqual(parse_scalar('"with: colon"'), "with: colon")

    def test_int_coercion(self):
        self.assertEqual(parse_scalar("7"), 7)

    def test_quoted_digits_remain_strings(self):
        self.assertEqual(parse_scalar('"007"'), "007")

    def test_nested_inline_mapping_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_scalar("[{name: value}]")

    def test_unbalanced_brackets_in_inline_list_are_rejected(self):
        for text in ("[a, b]]", "[a, [b]]", "[a, b}"):
            with self.subTest(text=text), self.assertRaises(FrontmatterError):
                parse_scalar(text)

    def test_inline_list_elements_stay_strings(self):
        # 行内列表元素不做 int 推断：symbols 之类字段全是符号名。
        self.assertEqual(parse_scalar("[1, 2]"), ["1", "2"])

    def test_quoted_scalar_with_inner_same_quote_is_rejected(self):
        # 内部的同类引号会骗过「首末是引号」的判断，去引号后得到 `a"junk`
        # 这种歪值。受控子集里不出现这种形态，必须报错而不是静默误解析。
        for text in ('"a"junk"', '"a"b"'):
            with self.subTest(text=text), self.assertRaises(FrontmatterError):
                parse_scalar(text)

    def test_quoted_scalar_inside_frontmatter_with_inner_quote_is_rejected(self):
        with self.assertRaises(FrontmatterError):
            parse_frontmatter('---\ntitle: "a"junk"\n---\n')

    def test_inline_list_rejects_unescaped_matching_quote(self):
        for text in ('["A"junk"]', "['A'junk']"):
            with self.subTest(text=text), self.assertRaises(FrontmatterError):
                parse_scalar(text)

    def test_inline_list_accepts_escaped_and_other_quotes(self):
        self.assertEqual(parse_scalar('["A\\"B"]'), ['A\\"B'])
        self.assertEqual(parse_scalar('["A\'B"]'), ["A'B"])

    def test_quoted_scalar_regressions_are_unchanged(self):
        self.assertEqual(parse_scalar('"ok"'), "ok")
        self.assertEqual(parse_scalar("'ok'"), "ok")

    def test_plain_scalar_regression_is_unchanged(self):
        self.assertEqual(parse_scalar("postgres"), "postgres")

    def test_quoted_scalar_may_contain_the_other_quote(self):
        # 只禁同类引号：双引号里可以有单引号，反之亦然。
        self.assertEqual(parse_scalar("\"a'b\""), "a'b")
        self.assertEqual(parse_scalar("'a\"b'"), 'a"b')


if __name__ == "__main__":
    unittest.main()
