# -*- coding: utf-8 -*-
"""生成 postgres _index.md 的 151 页完整导航表（12 组）。

只打印导航表片段，由人合并进 _index.md —— 避免脚本直接改写已审阅的导航散文。
"""
import io
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

sys.path.insert(0, HERE)
from wiki_manifest import load_manifest

MANIFEST = os.path.join(PROJECT_ROOT, "docs", "wiki", "projects", "postgres", "_manifest.yml")

GROUP_LABELS = {
    "A-存储与IPC": "A · 存储与 IPC",
    "B-事务与WAL": "B · 事务与 WAL",
    "C-堆表与元组": "C · 堆表与元组",
    "D-索引族": "D · 索引族",
    "E-优化器": "E · 优化器",
    "F-执行器": "F · 执行器",
    "G-目录与缓存": "G · 目录与缓存",
    "H-内存与排序": "H · 内存与排序",
    "I-复制与高可用": "I · 复制与高可用",
    "J-服务端与命令面": "J · 服务端与命令面",
    "K-SQL处理与安全": "K · SQL 处理与安全",
    "L-SQL类型与内建函数": "L · SQL 类型与内建函数",
}


def main():
    manifest = load_manifest(MANIFEST)
    pages = manifest["pages"]
    groups = {}
    for page in pages:
        groups.setdefault(page["feature_group"], []).append(page)

    lines = []
    lines.append("## 151 个机制页")
    lines.append("")
    lines.append("按 12 个机制领域组织。每页是一个独立的机制单元，"
                 "标题、`feature` 与 `refs` 由 `postgres_mechanism_specs.py` 冻结，"
                 "正文由 writer 逐页写并经 `wiki_lint` + `wiki_refs` 双门禁。")
    lines.append("")
    lines.append("| 领域 | 页数 |")
    lines.append("|---|---:|")
    for group in sorted(groups):
        lines.append("| %s | %d |" % (GROUP_LABELS.get(group, group), len(groups[group])))
    lines.append("| **合计** | **%d** |" % len(pages))
    lines.append("")

    for group in sorted(groups):
        lines.append("### %s（%d 页）" % (GROUP_LABELS.get(group, group), len(groups[group])))
        lines.append("")
        lines.append("| 页 | 机制 | 标题 |")
        lines.append("|---|---|---|")
        for page in groups[group]:
            lines.append("| [`%s`](%s) | `%s` | %s |"
                         % (page["page_file"], page["page_file"],
                            page["feature"], page["title"]))
        lines.append("")

    out = "\n".join(lines)
    print(out)
    print("--- 统计：%d 组 / %d 页" % (len(groups), len(pages)))


if __name__ == "__main__":
    main()
