# -*- coding: utf-8 -*-
"""Python 定义索引测试：先红后绿。

Python 与 C 的定义形态完全不同（缩进、`def`/`async def`、类方法、装饰器、
同名方法分属不同类），用 C 的正则抽会把方法名和调用一起蒙进来。因此单独
写一个索引器，并把这些形态固化成测试。
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dump_python_defs import definitions, index_file

SAMPLE = '''\
"""模块 docstring。"""

import os


TOP = 1


def module_function(a, b=2):
    return a + b


async def async_function(x):
    return x


class Widget:
    """类 docstring。"""

    attr = 1

    def __init__(self, name):
        self.name = name

    async def run(self):
        return self.name

    class Inner:
        def deep(self):
            pass


def bare_annotation(x: int) -> int:
    def nested():
        return 1
    return nested()
'''


class TestDefinitions(unittest.TestCase):
    def test_collects_top_level_function(self):
        found = definitions(SAMPLE)
        self.assertIn("module_function", found)

    def test_collects_async_function(self):
        self.assertIn("async_function", definitions(SAMPLE))

    def test_collects_indented_methods(self):
        found = definitions(SAMPLE)
        self.assertIn("__init__", found)
        self.assertIn("run", found)

    def test_collects_nested_class_method(self):
        self.assertIn("deep", definitions(SAMPLE))

    def test_collects_nested_function(self):
        self.assertIn("nested", definitions(SAMPLE))

    def test_ignores_calls_and_imports(self):
        found = definitions(SAMPLE)
        self.assertNotIn("os", found)
        self.assertNotIn("import", found)

    def test_keeps_class_qualification(self):
        records = definitions(SAMPLE, qualified=True)
        self.assertIn("Widget.run", records)
        self.assertIn("Widget.Inner.deep", records)
        self.assertIn("module_function", records)
        # 输出按出现顺序，便于人工按文件顺序盘点
        self.assertEqual(records[0], "module_function")


class TestIndexFile(unittest.TestCase):
    def test_index_file_returns_line_numbers(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "sample.py")
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write(SAMPLE)
            records = index_file(path)
        by_name = {r["name"]: r for r in records}
        self.assertEqual(by_name["module_function"]["line"], 9)
        self.assertEqual(by_name["Widget.run"]["line"], 25)


if __name__ == "__main__":
    unittest.main()
