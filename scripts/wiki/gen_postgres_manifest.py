# -*- coding: utf-8 -*-
"""从经审核的机制清单生成 PostgreSQL `_manifest.yml`。

P1 版本按「目录里 `.c` 文件体积降序、一个文件一页」生成，结果是文件清单而不是
机制清单：78% 的标题退化成「<文件名> 模块」，A 组把测试辅助函数
（`pgaio_worker_pm_test_grow_*`）当成主题，B/C 两组重复收录同一批 `rmgrdesc`，
旧 H 组把 `utils/adt` 的类型函数塞进「内存与排序」。P2a 据此把生成源换成人工
审核过的 `postgres_mechanism_specs.MECHANISM_SPECS`：

- **一个条目 = 一个机制单元 = 一个页面**，机制边界写在 `boundary` 里；
- 一页可以引用多个文件（同一机制跨文件实现），不再一文件一页；
- `feature` / `title` / `page_file` 全部显式给死，生成期不做任何推导；
- 篇数是盘点结果，不是配额，所以不再有「凑够 N 篇」的失败路径。

生成器仍然只做两件事：把 spec 序列化成 manifest，以及对着真实源码核对 refs。
"""
import argparse
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from postgres_mechanism_specs import GROUP_ORDER, MECHANISM_SPECS  # noqa: E402
from wiki_manifest import dump_manifest  # noqa: E402
from wiki_manifest_lint import lint_manifest  # noqa: E402

# 页面标题不允许退化成文件名兜底：这是 P1 最刺眼的症状，加测试守住。
_GENERIC_TITLE_SUFFIX = " 模块"


class ManifestGenError(Exception):
    """清单自身不自洽（重复 feature、缺 refs 等）时抛出。"""


def build_manifest(repo_root=None):
    """把机制清单转成 manifest dict。

    `repo_root` 只为与旧调用点保持兼容而保留：新生成器不再读源码目录，
    篇数与分组完全由清单决定。传 None 也合法。
    """
    pages = []
    for spec in MECHANISM_SPECS:
        pages.append({
            "feature": spec["feature"],
            "feature_group": spec["feature_group"],
            "title": spec["title"],
            "page_file": spec["page_file"],
            "refs": [{"path": ref["path"], "symbols": list(ref["symbols"])}
                     for ref in spec["refs"]],
        })
    return {
        "project": "postgres",
        "schema_version": 1,
        "pages": pages,
    }


def group_counts(manifest):
    """返回 {组名: 篇数}，键序与 GROUP_ORDER 一致。"""
    counts = {}
    for page in manifest["pages"]:
        counts[page["feature_group"]] = counts.get(page["feature_group"], 0) + 1
    return {label: counts.get(label, 0) for label in GROUP_ORDER}


def check_specs(specs=None):
    """清单自洽性检查，返回问题列表；空列表表示干净。

    这里查的是「清单写得对不对」，不查源码：源码层面由 `verify_refs` 负责。
    """
    specs = MECHANISM_SPECS if specs is None else specs
    problems = []
    seen_features = {}
    seen_files = {}
    for index, spec in enumerate(specs):
        feature = spec.get("feature")
        if not feature or not re.fullmatch(r"[a-z][a-z0-9-]*", feature):
            problems.append("第 %d 条 feature 非法: %r" % (index, feature))
        elif feature in seen_features:
            problems.append("feature 重复: %s" % feature)
        else:
            seen_features[feature] = index

        page_file = spec.get("page_file")
        if not page_file or not page_file.endswith(".md"):
            problems.append("第 %d 条 page_file 非法: %r" % (index, page_file))
        elif page_file in seen_files:
            problems.append("page_file 重复: %s" % page_file)
        else:
            seen_files[page_file] = index

        title = spec.get("title") or ""
        if not title.strip():
            problems.append("%s 缺 title" % feature)
        elif title.endswith(_GENERIC_TITLE_SUFFIX):
            problems.append("%s 的 title 退化成文件名兜底: %r" % (feature, title))

        if spec.get("feature_group") not in GROUP_ORDER:
            problems.append("%s 的 feature_group 不在分组表里: %r"
                            % (feature, spec.get("feature_group")))

        if not (spec.get("boundary") or "").strip():
            problems.append("%s 缺 boundary（机制边界是审核依据）" % feature)

        refs = spec.get("refs") or []
        if not refs:
            problems.append("%s 的 refs 为空" % feature)
        for ref in refs:
            path = ref.get("path")
            if not path or not path.startswith("src/"):
                problems.append("%s 的 refs.path 必须是 src/ 开头的相对路径: %r"
                                % (feature, path))
            elif "..." in path:
                problems.append("%s 的 refs.path 是缩写，不可校验: %r"
                                % (feature, path))
            symbols = ref.get("symbols") or []
            if not symbols:
                problems.append("%s 的 refs.symbols 为空: %s" % (feature, path))
    return problems


def verify_refs(manifest, repo_root):
    """逐条核对 refs：文件存在且每个符号按词边界命中。返回失败列表。

    这是渲染门禁，只证明「符号文本在该文件里出现」。它**不**证明符号是函数定义、
    也不证明该符号代表这一页的机制——后两件事靠生成期的 Ctags 核对与人工审阅。
    """
    failures = []
    for page in manifest["pages"]:
        for ref in page["refs"]:
            rel = ref["path"]
            full = os.path.join(repo_root, rel.replace("/", os.sep))
            if not os.path.isfile(full):
                failures.append((page["feature"], rel, "文件不存在"))
                continue
            with open(full, "rb") as stream:
                text = stream.read().decode("utf-8", errors="replace")
            for symbol in ref["symbols"]:
                if not re.search(r"\b" + re.escape(symbol) + r"\b", text):
                    failures.append((page["feature"], rel, "未命中: %s" % symbol))
    return failures


def write_manifest(manifest, output_path):
    """写出 manifest，自动建父目录。"""
    parent = os.path.dirname(os.path.abspath(output_path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with io.open(output_path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(dump_manifest(manifest))


def main(argv=None):
    parser = argparse.ArgumentParser(description="按机制清单生成 postgres manifest")
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
