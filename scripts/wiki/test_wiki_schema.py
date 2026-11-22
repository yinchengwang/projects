# -*- coding: utf-8 -*-
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wiki_schema


class TestConstants(unittest.TestCase):
    def test_schema_version_is_positive_int(self):
        self.assertIsInstance(wiki_schema.SCHEMA_VERSION, int)
        self.assertGreaterEqual(wiki_schema.SCHEMA_VERSION, 1)

    def test_six_required_sections(self):
        self.assertEqual(len(wiki_schema.REQUIRED_SECTIONS), 6)

    def test_required_section_names_are_unique(self):
        self.assertEqual(len(set(wiki_schema.REQUIRED_SECTIONS)), 6)

    def test_exact_section_names(self):
        self.assertEqual(
            wiki_schema.REQUIRED_SECTIONS,
            ("一句话职责", "机制是什么", "怎么实现的", "关键取舍", "源码落点", "参考"),
        )

    def test_conditional_sections_not_in_required(self):
        for name in wiki_schema.CONDITIONAL_SECTIONS:
            self.assertNotIn(name, wiki_schema.REQUIRED_SECTIONS)

    def test_page_types(self):
        self.assertEqual(set(wiki_schema.PAGE_TYPES), {"overview", "feature", "compare"})

    def test_index_fields_include_stars_as_of(self):
        self.assertIn("stars_as_of", wiki_schema.INDEX_REQUIRED_FIELDS)

    def test_feature_fields_include_refs(self):
        self.assertIn("refs", wiki_schema.REQUIRED_FIELDS)

    def test_required_fields_for_feature(self):
        self.assertEqual(
            wiki_schema.required_fields_for("feature"), wiki_schema.REQUIRED_FIELDS
        )

    def test_required_fields_for_overview(self):
        self.assertEqual(
            wiki_schema.required_fields_for("overview"), wiki_schema.INDEX_REQUIRED_FIELDS
        )

    def test_required_fields_for_compare_includes_refs(self):
        self.assertIn("refs", wiki_schema.required_fields_for("compare"))


if __name__ == "__main__":
    unittest.main()
