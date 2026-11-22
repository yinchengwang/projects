# -*- coding: utf-8 -*-
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wiki_catalog import (CATALOG_FIELDS, CatalogError, dump_catalog,
                          parse_catalog, validate_entry)

SAMPLE = """projects:
  - name: postgres
    model: relational
    tier: T1
    repo: https://github.com/postgres/postgres
    license: PostgreSQL
    language: C
    stars: 13500
    stars_as_of: 2026-11-22
  - name: qdrant
    model: vector
    tier: T1
    repo: https://github.com/qdrant/qdrant
    license: Apache-2.0
    language: Rust
    stars: 35659
    stars_as_of: 2026-11-22
"""


class TestParseCatalog(unittest.TestCase):
    def test_parses_two_entries(self):
        entries = parse_catalog(SAMPLE)
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[0]["name"], "postgres")
        self.assertEqual(entries[1]["name"], "qdrant")

    def test_scalar_types(self):
        entries = parse_catalog(SAMPLE)
        pg = entries[0]
        self.assertEqual(pg["model"], "relational")
        self.assertEqual(pg["tier"], "T1")
        self.assertEqual(pg["stars"], 13500)
        self.assertEqual(pg["stars_as_of"], "2026-11-22")

    def test_missing_projects_key_raises(self):
        with self.assertRaises(CatalogError):
            parse_catalog("items:\n  - name: x\n")

    def test_empty_catalog_is_allowed(self):
        self.assertEqual(parse_catalog("projects:\n"), [])

    def test_duplicate_name_raises(self):
        dup = SAMPLE + """  - name: postgres
    model: relational
"""
        with self.assertRaises(CatalogError):
            parse_catalog(dup)


class TestDumpCatalog(unittest.TestCase):
    def test_round_trip(self):
        entries = parse_catalog(SAMPLE)
        text = dump_catalog(entries)
        again = parse_catalog(text)
        self.assertEqual(entries, again)

    def test_dump_is_lf_and_has_header(self):
        text = dump_catalog(parse_catalog(SAMPLE))
        self.assertNotIn("\r", text)
        self.assertTrue(text.startswith("projects:\n"))


class TestValidateEntry(unittest.TestCase):
    def test_valid_entry_has_no_errors(self):
        self.assertEqual(validate_entry(parse_catalog(SAMPLE)[0]), [])

    def test_missing_field_reported(self):
        bad = dict(parse_catalog(SAMPLE)[0])
        bad.pop("stars_as_of")
        errors = validate_entry(bad)
        self.assertTrue(any("stars_as_of" in e for e in errors), errors)

    def test_tier_must_be_t1_t2_t3(self):
        bad = dict(parse_catalog(SAMPLE)[0])
        bad["tier"] = "core"
        errors = validate_entry(bad)
        self.assertTrue(any("tier" in e for e in errors), errors)

    def test_repo_must_be_http_url(self):
        bad = dict(parse_catalog(SAMPLE)[0])
        bad["repo"] = "git@github.com:x/y.git"
        errors = validate_entry(bad)
        self.assertTrue(any("repo" in e for e in errors), errors)

    def test_catalog_fields_constant_is_complete(self):
        self.assertIn("stars_as_of", CATALOG_FIELDS)
        self.assertIn("tier", CATALOG_FIELDS)
        self.assertEqual(len(CATALOG_FIELDS), 8)


if __name__ == "__main__":
    unittest.main()
