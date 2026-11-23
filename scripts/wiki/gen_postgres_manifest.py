# -*- coding: utf-8 -*-
"""postgres 生成器兼容入口（薄封装）。

旧测试与现有脚本按 `gen_postgres_manifest` 导入 `build_manifest()`、
`check_specs()` 等接口，因此保留这些签名，把实现委托给库无关的
`gen_manifest.py`。新库直接使用通用 CLI：

    python gen_manifest.py --project sqlite --spec-module sqlite_mechanism_specs
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_manifest as _generic  # noqa: E402
from postgres_mechanism_specs import GROUP_ORDER, MECHANISM_SPECS  # noqa: E402
from wiki_manifest_lint import lint_manifest  # noqa: E402

ManifestGenError = _generic.ManifestGenError


def build_manifest(repo_root=None):
    """兼容旧签名；repo_root 原本就不参与清单渲染。"""
    return _generic.build_manifest("postgres", MECHANISM_SPECS, GROUP_ORDER)


def group_counts(manifest):
    """兼容旧签名，按 postgres 分组表统计。"""
    return _generic.group_counts(manifest, GROUP_ORDER)


def check_specs(specs=None):
    """兼容旧签名；默认检查 postgres 机制清单。"""
    return _generic.check_specs(
        MECHANISM_SPECS if specs is None else specs,
        GROUP_ORDER,
        ["src"],
    )


def verify_refs(manifest, repo_root):
    return _generic.verify_refs(manifest, repo_root)


def write_manifest(manifest, output_path):
    return _generic.write_manifest(manifest, output_path)


def main(argv=None):
    """保留旧 CLI 参数：--repo-root / --output / --verify。"""
    import argparse

    parser = argparse.ArgumentParser(
        description="按 postgres 机制清单生成 manifest（兼容入口）")
    parser.add_argument("--repo-root", help="postgres 源码根目录（给了才做 refs 校验）")
    parser.add_argument("--output", help="输出 manifest 路径；给了就写出")
    parser.add_argument("--verify", action="store_true",
                        help="逐条核对 refs 是否命中真实源码（需要 --repo-root）")
    args = parser.parse_args(argv)

    problems = check_specs()
    if problems:
        print("-- 清单自检：%d 个问题" % len(problems))
        for problem in problems:
            print("     - %s" % problem)
        return 1

    manifest = build_manifest()
    counts = group_counts(manifest)
    print("-- 产出 %d 页，%d 组" % (len(manifest["pages"]), len(counts)))
    for label in GROUP_ORDER:
        print("     %-20s %3d" % (label, counts[label]))

    lint_errors = lint_manifest(manifest)
    if lint_errors:
        print("-- manifest 门禁：%d 个错误" % len(lint_errors))
        for error in lint_errors[:20]:
            print("     - %s" % error)
        return 1

    if args.verify:
        if not args.repo_root:
            parser.error("--verify 需要 --repo-root")
        if not os.path.isdir(args.repo_root):
            parser.error("--repo-root 不是目录: %s" % args.repo_root)
        failures = verify_refs(manifest, args.repo_root)
        print("-- refs 校验：失败 %d 条" % len(failures))
        for feature, rel, reason in failures[:20]:
            print("     - %s %s %s" % (feature, rel, reason))
        if failures:
            return 1

    if args.output:
        write_manifest(manifest, args.output)
        print("-- 已写出 %s" % args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
