# -*- coding: utf-8 -*-
"""manifest 门禁（设计文档 §6.2 Phase 1）。

清单是契约与写作之间的唯一接口：Phase 1 产 manifest，Phase 2 的 writer 只读
自己条目内的页。因此 manifest 必须先过硬门禁，否则 332 篇会按错的结构铺开。

退出码 0 = 全部通过，1 = 有任一 manifest 失败。一个坏 manifest 只记一条失败，
不中断目录扫描——否则一份坏文件的格式错误会掩护其余 manifest 的问题。
"""
import argparse
import os
import re
import sys

import wiki_schema
from wiki_manifest import ManifestError, load_manifest, manifest_feature_slugs

REQUIRED_PAGE_KEYS = ("feature", "feature_group", "title", "page_file", "refs")

# 与 wiki_lint 同一规则：文件名 `a04-buffer-pool.md` 的尾段是 `buffer-pool`。
_PAGE_PREFIX_RE = re.compile(r"^[A-Za-z]\d+-(.+)$")

# 目录模式只收这两种扩展名，与 wiki_manifest.MANIFEST_FILENAME 的 .yml 一致。
_MANIFEST_SUFFIXES = (".yml", ".yaml")


def _page_tail(page_file):
    """`a04-buffer-pool.md` -> `buffer-pool`。

    取 basename 再切扩展名，与 wiki_lint 的取法一致：page_file 允许带子目录，
    但尾段比较只看文件名。
    """
    stem = os.path.splitext(os.path.basename(page_file))[0]
    match = _PAGE_PREFIX_RE.match(stem)
    return match.group(1) if match else stem


def lint_manifest(manifest, project_dir=None):
    """返回错误列表，空列表表示通过。"""
    errors = []

    project = manifest.get("project")
    if not isinstance(project, str) or not project.strip():
        errors.append("缺 project 或不是非空字符串")

    version = manifest.get("schema_version")
    if version != wiki_schema.SCHEMA_VERSION:
        errors.append(
            "schema_version=%r，当前契约版本为 %d" % (version, wiki_schema.SCHEMA_VERSION)
        )

    pages = manifest.get("pages")
    if not isinstance(pages, list) or not pages:
        errors.append("pages 必须是非空列表")
        return errors

    for index, page in enumerate(pages):
        if not isinstance(page, dict):
            errors.append("pages[%d] 不是映射" % index)
            continue
        for key in REQUIRED_PAGE_KEYS:
            if key not in page:
                errors.append("pages[%d] 缺字段: %s" % (index, key))

        page_file = page.get("page_file")
        if isinstance(page_file, str):
            if not page_file.endswith(".md"):
                errors.append("pages[%d].page_file 必须以 .md 结尾: %r" % (index, page_file))
            feature = page.get("feature")
            if isinstance(feature, str) and page_file.endswith(".md"):
                tail = _page_tail(page_file)
                if tail != feature:
                    errors.append(
                        "pages[%d].page_file 尾段(%r) 与 feature(%r) 不一致"
                        % (index, tail, feature)
                    )
            if project_dir:
                conflict = os.path.join(project_dir, page_file)
                if os.path.exists(conflict):
                    errors.append(
                        "pages[%d].page_file 已存在同名的既有文件: %s" % (index, page_file)
                    )

        refs = page.get("refs")
        if not isinstance(refs, list) or not refs:
            errors.append("pages[%d].refs 必须是非空列表" % index)
            continue
        for ridx, ref in enumerate(refs):
            if not isinstance(ref, dict):
                errors.append("pages[%d].refs[%d] 不是映射" % (index, ridx))
                continue
            if not ref.get("path"):
                errors.append("pages[%d].refs[%d] 缺 path" % (index, ridx))
            symbols = ref.get("symbols")
            if not isinstance(symbols, list) or not symbols:
                errors.append("pages[%d].refs[%d].symbols 必须是非空列表" % (index, ridx))

    # feature 唯一性跨全部 pages，而不是单页内自洽。非映射的页已在上面单独报错，
    # 这里先滤掉，避免往 manifest_feature_slugs 里塞无法取键的东西。
    slugs = manifest_feature_slugs({"pages": [p for p in pages if isinstance(p, dict)]})
    seen = set()
    for slug in slugs:
        if slug in seen:
            errors.append("feature 重复: %s" % slug)
        seen.add(slug)

    return errors


def _iter_manifests(target):
    """目录递归收集 manifest，跳隐藏目录；单文件原样返回。"""
    if not os.path.isdir(target):
        return [target]
    paths = []
    for dirpath, dirnames, filenames in os.walk(target):
        # 跳过 .git / .scratch 这类隐藏目录，别把临时文件当 manifest 检查。
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if name.endswith(_MANIFEST_SUFFIXES):
                paths.append(os.path.join(dirpath, name))
    return paths


def main(argv=None):
    parser = argparse.ArgumentParser(description="wiki manifest 门禁")
    parser.add_argument("manifests", nargs="+", help="manifest 文件或目录（目录会递归）")
    parser.add_argument("--quiet", action="store_true", help="只打印失败项")
    args = parser.parse_args(argv)

    failed = 0
    checked = 0
    for target in args.manifests:
        for path in _iter_manifests(target):
            checked += 1
            try:
                manifest = load_manifest(path)
            except ManifestError as exc:
                # 坏 manifest 只记一次失败：异常一旦冒泡就会中断整目录扫描。
                failed += 1
                print("FAIL %s" % path)
                print("     - %s" % exc)
                continue
            errors = lint_manifest(manifest)
            if errors:
                failed += 1
                print("FAIL %s" % path)
                for error in errors:
                    print("     - %s" % error)
            elif not args.quiet:
                print("PASS %s" % path)

    print("-- 检查 %d 个 manifest，失败 %d 个" % (checked, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
