# -*- coding: utf-8 -*-
"""结构门禁：检查 wiki 页是否满足契约（设计文档 §5.1）。

只查结构与字段，不查源码引用是否真实——那是 wiki_refs.py 的职责。
退出码 0 = 全部通过，1 = 有任一页失败。可整目录递归。
"""
import argparse
import os
import re
import sys

import wiki_schema
from wiki_frontmatter import FrontmatterError, parse_frontmatter

# 禁止形态：<带扩展名的路径>:<数字>。命中即 FAIL。
# 扩展名顺序：长的排在短的之前（cxx 先于 cc，cpp 先于 c，hpp 先于 hh 先于 h），
# 否则短扩展名会抢先匹配掉长扩展名。裸 c 必须在内——postgres / sqlite / redis
# 的主力扩展名就是 .c，漏了它这条门禁在最该拦的地方失明（已由测试抓到过一次）。
_LINE_REF_RE = re.compile(
    r"([A-Za-z0-9_./\-]+\.(?:jsx|tsx|cpp|cxx|hpp|yaml|yml|toml|json|md|js|ts|cc|c|hh|h|rs|go|java|kt|py|scala|sql|lua|rb|php|m|mm|swift)):(\d+(?:[,\-]\d+)*)"
)
# 参考章节的外部链接计数。只认 http(s)，不认相对路径。
_LINK_RE = re.compile(r"https?://")
# 参考章节的条目形态：`- [标题](url)`，允许链接后面跟一句说明。
# 用来发现「链接被截断」这类书写事故——只看 https 出现次数会漏掉 `- [规范](` 这种
# 半截链接（已由 goose f01 抓到一次）。标题里允许出现方括号（例如 `#[default]`），
# 所以方括号部分用贪婪匹配；URL 部分不允许空白与圆括号，全角括号因此也会被拦下
# （全角 `）` 收尾的链接在浏览器里是坏链）。
_LIST_LINK_RE = re.compile(r"^\s*[-*]\s+.*\]\(([^()\s]+)\)")
# 章节标题必须独占一行且以 `## ` 开头：`### 参考` 不算数。
_SECTION_RE = re.compile(r"^##\s+(.+?)\s*$", re.MULTILINE)
# 特性页文件名形如 a04-buffer-pool.md，取「首字母+编号+横线」之后的尾段。
_FILENAME_PREFIX_RE = re.compile(r"^[A-Za-z]\d+-(.+)$")
_MIN_REFERENCES = 2


def _split_sections(body):
    """返回 [(标题, 正文)]，标题不带 ## 前缀。"""
    out = []
    matches = list(_SECTION_RE.finditer(body))
    for i, m in enumerate(matches):
        start = m.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(body)
        out.append((m.group(1).strip(), body[start:end]))
    return out


def _dirname_parts(path):
    return os.path.normpath(path).split(os.sep)


def _check_refs_shape(refs, errors):
    if not isinstance(refs, list):
        errors.append("refs 必须是列表")
        return
    for i, item in enumerate(refs):
        if not isinstance(item, dict):
            errors.append("refs[%d] 不是映射" % i)
            continue
        if not item.get("path"):
            errors.append("refs[%d].path 缺失" % i)
        syms = item.get("symbols")
        if not isinstance(syms, list) or not syms:
            errors.append("refs[%d].symbols 必须是非空列表" % i)


def lint_page(page_path):
    """返回错误列表，空列表表示通过。"""
    errors = []
    with open(page_path, encoding="utf-8") as f:
        text = f.read()

    try:
        meta, body = parse_frontmatter(text)
    except FrontmatterError as e:
        # 解析失败只回一条可读错误：别让一个坏 front-matter 连带 2.5 万篇都崩。
        return ["front-matter 解析失败: %s" % e]

    page_type = meta.get("page_type")
    if page_type is not None and page_type not in wiki_schema.PAGE_TYPES:
        errors.append("page_type 非法: %r，允许 %s" % (page_type, list(wiki_schema.PAGE_TYPES)))

    for field in wiki_schema.required_fields_for(page_type):
        if field not in meta:
            errors.append("缺必填字段: %s" % field)

    # 未知键一律报错：frontmatter 是契约面，多一个键通常意味着抄错了字段名
    # （已发生过两次：`page_file:` 与 `page_page:` 都靠人工核对才发现，
    # 结构门禁当时全放过）。允许清单按 page_type 取并集，再加一个可选 `title`
    # （特性页普遍用它给页面起中文标题，契约里没有强制但全库在用）。
    allowed_keys = (
        set(wiki_schema.INDEX_REQUIRED_FIELDS)
        | set(wiki_schema.REQUIRED_FIELDS)
        | {"title"}
    )
    for key in meta:
        if key not in allowed_keys:
            errors.append("frontmatter 出现未定义字段: %s" % key)

    if "schema_version" in meta and meta["schema_version"] != wiki_schema.SCHEMA_VERSION:
        errors.append(
            "schema_version=%r，当前契约版本为 %d" % (meta["schema_version"], wiki_schema.SCHEMA_VERSION)
        )

    if page_type == "compare":
        if wiki_schema.COMPARE_DIR_NAME not in _dirname_parts(page_path):
            errors.append("page_type=compare 的页必须位于 %s/ 下" % wiki_schema.COMPARE_DIR_NAME)

    feature = meta.get("feature")
    if "feature" in meta:
        if not isinstance(feature, str):
            errors.append("feature 必须是字符串，实际 %r" % (feature,))
        else:
            stem = os.path.splitext(os.path.basename(page_path))[0]
            m = _FILENAME_PREFIX_RE.match(stem)
            tail = m.group(1) if m else stem
            if tail != feature:
                errors.append("feature(%r) 与文件名尾段(%r) 不一致" % (feature, tail))

    if "refs" in meta:
        _check_refs_shape(meta["refs"], errors)

    sections_list = _split_sections(body)
    sections = {}
    for section_name, section_body in sections_list:
        sections.setdefault(section_name, section_body)
    required_names = set(wiki_schema.REQUIRED_SECTIONS) | set(wiki_schema.CONDITIONAL_SECTIONS)
    for name in required_names:
        if sum(1 for section_name, _ in sections_list if section_name == name) > 1:
            errors.append("章节标题重复: ## %s" % name)
    for name in wiki_schema.REQUIRED_SECTIONS:
        if name not in sections:
            errors.append("缺必含章节: ## %s" % name)
    for name in wiki_schema.CONDITIONAL_SECTIONS:
        if name not in sections:
            errors.append("缺条件章节标题: ## %s" % name)

    for m in _LINE_REF_RE.finditer(body):
        errors.append("出现行号引用 %s:%s，契约禁止行号" % (m.group(1), m.group(2)))

    ref_body = sections.get("参考", "")
    n_links = len(_LINK_RE.findall(ref_body))
    if n_links < _MIN_REFERENCES:
        errors.append("参考章节外部链接 %d 个，至少需要 %d 个" % (n_links, _MIN_REFERENCES))

    # 逐条核对列表项：任何以 `- [` 开头的行都必须是一条完整的 `- [标题](url)`。
    # 只数 https 不会发现「括号没闭合」或「链接被截断」，那类页面仍是残缺的。
    for line_no, line in enumerate(ref_body.splitlines(), 1):
        stripped = line.strip()
        if not stripped.startswith(("- [", "* [")):
            continue
        if not _LIST_LINK_RE.match(line):
            errors.append("参考章节第 %d 行不是完整的 [标题](url) 链接: %s" % (line_no, stripped[:60]))

    return errors


def _iter_pages(root):
    for dirpath, dirnames, filenames in os.walk(root):
        # 跳过 .git / .scratch 这类隐藏目录，别把临时文件当页面检查。
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for fn in sorted(filenames):
            if fn.endswith(".md"):
                yield os.path.join(dirpath, fn)


def main(argv=None):
    parser = argparse.ArgumentParser(description="wiki 结构门禁（设计文档 §5.1）")
    parser.add_argument("paths", nargs="+", help="要检查的 .md 文件或目录（目录会递归）")
    parser.add_argument("--quiet", action="store_true", help="只打印失败项")
    args = parser.parse_args(argv)

    failed = 0
    checked = 0
    if any(not os.path.isdir(target) for target in args.paths):
        print("提示：单文件模式仅能校验文件本身，feature 项目内唯一性须用目录模式。")
    # feature 唯一性的作用域是**项目内**，不是全局（契约 wiki-schema.md §4：
    # 「feature｜一个机制单元一篇｜项目目录下」）。
    # 不同库出现同名 feature 是正常的——name-resolution 在 mysql-server 与
    # sqlite 里各有一篇，subquery-transform 在 postgres 与 mysql-server 里各有一篇。
    # 所以键是 (project, feature)，只报同项目内的重复。
    features = {}
    results = {}
    for target in args.paths:
        pages = list(_iter_pages(target)) if os.path.isdir(target) else [target]
        for page in pages:
            checked += 1
            errs = lint_page(page)
            results[page] = errs
            try:
                with open(page, encoding="utf-8") as f:
                    meta, _ = parse_frontmatter(f.read())
                feature = meta.get("feature")
                project = meta.get("project")
                if isinstance(feature, str):
                    key = (project if isinstance(project, str) else "", feature)
                    page_list = features.setdefault(key, [])
                    if page not in page_list:
                        page_list.append(page)
            except (OSError, FrontmatterError):
                pass
    for (project, feature), feature_pages in features.items():
        if len(feature_pages) > 1:
            for page in feature_pages:
                results[page].append(
                    "feature 项目内重复: %s（project=%s）" % (feature, project or "未标注")
                )
    for page, errs in results.items():
        if errs:
            failed += 1
            print("FAIL %s" % page)
            for e in errs:
                print("     - %s" % e)
        elif not args.quiet:
            print("PASS %s" % page)

    print("-- 检查 %d 页，失败 %d 页" % (checked, failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
