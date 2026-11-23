# -*- coding: utf-8 -*-
"""让 pytest 无论从仓根还是从本应用目录启动，都能找到本应用的顶层包。

测试文件用的是 `from core...` / `from index...` / `from server...` 这种相对本应用根的
绝对导入（与 `python -m server.main` 的运行方式一致），所以需要把应用根放进 `sys.path`。
不这么做时，从 `projects/` 根跑 `pytest apps/wiki-rag/tests` 会报
`ModuleNotFoundError: No module named 'core'`。
"""
import os
import sys

APP_ROOT = os.path.dirname(os.path.abspath(__file__))
if APP_ROOT not in sys.path:
    sys.path.insert(0, APP_ROOT)
