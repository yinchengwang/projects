# -*- coding: utf-8 -*-
"""项目状态账本解析与校验：先红后绿。

守住四个语义边界：
1. 状态表 schema 合法：每项都有 slug / repo / state / next_action；
2. state 只能取已登记的阶段集合；
3. slug 与规范化 repo 在整本账本内唯一；
4. 别名不得同时出现在 slug 与 alias 中，也不能与已有 slug 冲突。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_project_status import (
    STATES,
    StatusError,
    parse_status,
    validate_status,
)

HEADER = "projects:\n"


def build(*blocks):
    return HEADER + "".join(blocks)


def entry(slug, repo, state="planned", next_action="等待取数", alias=None):
    lines = [
        "  - slug: %s\n" % slug,
        "    repo: %s\n" % repo,
        "    state: %s\n" % state,
        "    next_action: %s\n" % next_action,
    ]
    if alias is not None:
        lines.append("    aliases: [%s]\n" % alias)
    return "".join(lines)


class TestParseStatus(unittest.TestCase):
    def test_parses_minimal_entry(self):
        text = build(entry("postgres", "https://github.com/postgres/postgres"))
        entries = parse_status(text)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["slug"], "postgres")
        self.assertEqual(entries[0]["state"], "planned")


class TestValidateStatus(unittest.TestCase):
    def test_accepts_valid_state(self):
        text = build(entry("postgres", "https://github.com/postgres/postgres"))
        self.assertEqual(validate_status(parse_status(text)), [])

    def test_rejects_unknown_state(self):
        text = build(entry("postgres", "https://github.com/postgres/postgres", state="done"))
        problems = validate_status(parse_status(text))
        self.assertEqual(len(problems), 1)
        self.assertIn("done", problems[0])

    def test_rejects_duplicate_slug(self):
        text = build(
            entry("postgres", "https://github.com/postgres/postgres"),
            entry("postgres", "https://github.com/other/postgres"),
        )
        problems = validate_status(parse_status(text))
        self.assertTrue(any("slug" in p for p in problems))

    def test_rejects_duplicate_repo(self):
        text = build(
            entry("postgres", "https://github.com/postgres/postgres"),
            entry("postgres2", "https://github.com/postgres/postgres.git"),
        )
        problems = validate_status(parse_status(text))
        self.assertTrue(any("repo" in p for p in problems))

    def test_rejects_missing_next_action(self):
        text = HEADER + "  - slug: postgres\n    repo: https://github.com/postgres/postgres\n    state: planned\n"
        problems = validate_status(parse_status(text))
        self.assertTrue(any("next_action" in p for p in problems))

    def test_rejects_alias_colliding_with_slug(self):
        text = build(
            entry("postgres", "https://github.com/postgres/postgres"),
            entry("mysql-server", "https://github.com/mysql/mysql-server", alias="sqlite"),
            entry("sqlite", "https://github.com/sqlite/sqlite"),
        )
        problems = validate_status(parse_status(text))
        self.assertTrue(any("alias" in p for p in problems))

    def test_parse_error_raises(self):
        with self.assertRaises(StatusError):
            parse_status("not-a-status-document\n")

    def test_states_are_frozen_set(self):
        self.assertEqual(len(STATES), 9)
        self.assertIn("indexed", STATES)
        self.assertIn("excluded-with-reason", STATES)


if __name__ == "__main__":
    unittest.main()
