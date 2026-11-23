# -*- coding: utf-8 -*-
"""库无关的机制 manifest 生成器。

P3 阶段的 `gen_postgres_manifest.py` 把 postgres 的机制清单序列化成 manifest，
但它有 5 处 postgres 专用耦合（模块名写死、project 名写死、分组表写死、
refs 前缀必须是 `src/`、title 不能用「模块」结尾）。P4 要写 sqlite / mysql /
redis 等其它库，这些耦合会让「换一个库就得复制一份脚本」。

本模块把这些耦合全部参数化：

- **机制条目从外部 spec 模块导入**，由 `--spec-module` 指定；
  模块须暴露 `MECHANISM_SPECS`（列表）与 `GROUP_ORDER`（分组顺序）。
- **project 名由 `--project` 指定**。
- **分组表由 spec 模块提供**，不再写死。
- **refs 路径前缀由 `--src-prefix` 指定**（默认 `src`，可用逗号分隔多个）。
  SQLite 的源码在仓根、MySQL 在 `sql/` + `storage/` + `plugin/`，都靠这条放开。
- **伪标题检测升级为通用规则**：title 不得等于文件名去扩展名，也不得以「模块」结尾。

生成器仍然只做三件事：校验 spec 自洽、把 spec 序列化成 manifest、对着真实源码核对 refs。
**它不决定「拆几篇」** —— 篇数是机制盘点的结果，不是生成器的产物。
"""

import argparse
import importlib
import io
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from wiki_manifest import dump_manifest          # noqa: E402
from wiki_manifest_lint import lint_manifest     # noqa: E402

# 页面标题不允许退化成文件名兜底：这是 P1 最刺眼的症状（78% 的标题是
# 「<文件名> 模块」）。除「以『模块』结尾」外，还要拦「title 就是文件名」。
_GENERIC_TITLE_SUFFIX = "模块"

# refs.path 里不允许出现省略号：那说明 spec 写的是缩略路径，无法校验。
_ABBREVIATED_PATH = "..."


class ManifestGenError(Exception):
    """清单自身不自洽（重复 feature、缺 refs 等）时抛出。"""


def load_specs(spec_module):
    """按模块名导入机制清单，返回 (specs, group_order)。

    `spec_module` 可以是 `redis_mechanism_specs` 这样的裸模块名（从本脚本
    所在目录找），也可以带包路径。模块里必须有 MECHANISM_SPECS 与 GROUP_ORDER。

    这里**不**吞异常：导入失败就让人看到 ImportError 原文，比「找不到清单」
    这种含糊报错有用得多。
    """
    module = importlib.import_module(spec_module)
    specs = getattr(module, "MECHANISM_SPECS", None)
    group_order = getattr(module, "GROUP_ORDER", None)
    if specs is None:
        raise ManifestGenError(
            "spec 模块 %s 没有暴露 MECHANISM_SPECS" % spec_module)
    if group_order is None:
        raise ManifestGenError(
            "spec 模块 %s 没有暴露 GROUP_ORDER" % spec_module)
    if not isinstance(specs, list) or not specs:
        raise ManifestGenError(
            "spec 模块 %s 的 MECHANISM_SPECS 必须是非空列表" % spec_module)
    if not isinstance(group_order, (list, tuple)) or not group_order:
        raise ManifestGenError(
            "spec 模块 %s 的 GROUP_ORDER 必须是非空列表" % spec_module)
    return specs, list(group_order)


def _stem_of(page_file):
    """`c14-heap-page-layout.md` -> `c14-heap-page-layout`。"""
    if not page_file:
        return ""
    return page_file[:-3] if page_file.endswith(".md") else page_file


def _is_pseudo_title(title, page_file):
    """伪标题判定：标题只是文件名，或只是「<文件名> 模块」，或以「模块」结尾。

    这些形态都意味着 spec 没有真的起标题，而是拿文件名凑数 —— 正是 P1 的
    失败模式（78% 的标题退化成「<文件名> 模块」）。规则与库无关，所以在这里
    而不是在各库的 spec 模块里。
    """
    text = (title or "").strip()
    if not text:
        return True
    # 任何以「模块」结尾的标题都是文件名兜底的痕迹：真实机制标题不会这么收尾。
    if text.endswith(_GENERIC_TITLE_SUFFIX):
        return True
    stem = _stem_of(page_file)
    if not stem:
        return False
    if text == stem:
        return True
    return False


def check_specs(specs, group_order, src_prefixes):
    """清单自洽性检查，返回问题列表；空列表表示干净。

    查的是「清单写得对不对」，不查源码：源码层面由 `verify_refs` 负责。
    `src_prefixes` 是该库允许的 refs 路径前缀集合（如 `["src"]` 或 `["sql", "storage"]`）。
    """
    problems = []
    seen_features = {}
    seen_files = {}
    prefixes = tuple(src_prefixes or ("src",))

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
        if _is_pseudo_title(title, page_file):
            problems.append("%s 的 title 是伪标题（等于文件名或以「模块」结尾，文件名兜底）: %r"
                            % (feature, title))

        if spec.get("feature_group") not in group_order:
            problems.append("%s 的 feature_group 不在分组表里: %r"
                            % (feature, spec.get("feature_group")))

        if not (spec.get("boundary") or "").strip():
            problems.append("%s 缺 boundary（机制边界是审核依据）" % feature)

        refs = spec.get("refs") or []
        if not refs:
            problems.append("%s 的 refs 为空" % feature)
        for ref in refs:
            path = ref.get("path")
            if not path:
                problems.append("%s 的 refs.path 为空" % feature)
            elif _ABBREVIATED_PATH in path:
                problems.append("%s 的 refs.path 是缩写，不可校验: %r"
                                % (feature, path))
            elif not any(path.startswith(prefix.rstrip("/") + "/")
                         for prefix in prefixes):
                problems.append(
                    "%s 的 refs.path 必须以 %s/ 开头（该库的源码前缀目录）: %r"
                    % (feature, ", ".join(prefix.rstrip("/") for prefix in prefixes), path))
            symbols = ref.get("symbols") or []
            if not symbols:
                problems.append("%s 的 refs.symbols 为空: %s" % (feature, path))
    return problems


def build_manifest(project, specs, group_order):
    """把机制清单转成 manifest dict。

    生成的 manifest 只含 `project` / `schema_version` / `pages` 三个顶层键，
    与 `wiki_manifest_lint` 的契约一致。分组顺序不写进 manifest —— 它由
    spec 模块的 `GROUP_ORDER` 表达，manifest 里保留每页的 `feature_group` 即可。
    """
    if not project or not str(project).strip():
        raise ManifestGenError("project 名不能为空")
    pages = []
    for spec in specs:
        pages.append({
            "feature": spec["feature"],
            "feature_group": spec["feature_group"],
            "title": spec["title"],
            "page_file": spec["page_file"],
            "refs": [{"path": ref["path"], "symbols": list(ref["symbols"])}
                     for ref in spec["refs"]],
        })
    return {
        "project": str(project).strip(),
        "schema_version": 1,
        "pages": pages,
    }


def group_counts(manifest, group_order):
    """返回 {组名: 篇数}，键序与 group_order 一致（缺的组记 0）。"""
    counts = {}
    for page in manifest["pages"]:
        counts[page["feature_group"]] = counts.get(page["feature_group"], 0) + 1
    return {label: counts.get(label, 0) for label in group_order}


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
            try:
                with open(full, "rb") as stream:
                    text = stream.read().decode("utf-8", errors="replace")
            except OSError as error:
                failures.append((page["feature"], rel, "无法读取: %s" % error))
                continue
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


def _split_prefixes(text):
    """`src,sql,storage` -> ["src", "sql", "storage"]，空项丢弃。"""
    return [item.strip() for item in (text or "").split(",") if item.strip()]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="按机制清单生成任一项目的 manifest（库无关）")
    parser.add_argument("--project", required=True, help="项目名（写进 manifest 的 project 字段）")
    parser.add_argument("--spec-module", required=True,
                        help="机制清单模块名，须暴露 MECHANISM_SPECS 与 GROUP_ORDER")
    parser.add_argument("--src-prefix", default="src",
                        help="refs.path 允许的源码前缀，逗号分隔（默认 src）")
    parser.add_argument("--repo-root", help="该项目源码根目录（给了才做 refs 校验）")
    parser.add_argument("--output", help="输出 manifest 路径；给了就写出")
    parser.add_argument("--verify", action="store_true",
                        help="逐条核对 refs 是否命中真实源码（需要 --repo-root）")
    args = parser.parse_args(argv)

    try:
        specs, group_order = load_specs(args.spec_module)
    except (ImportError, ManifestGenError) as error:
        print("-- 载入 spec 失败：%s" % error)
        return 1

    prefixes = _split_prefixes(args.src_prefix)
    if not prefixes:
        parser.error("--src-prefix 不能为空")

    problems = check_specs(specs, group_order, prefixes)
    if problems:
        print("-- 清单自检：%d 个问题" % len(problems))
        for problem in problems:
            print("     - %s" % problem)
        return 1

    manifest = build_manifest(args.project, specs, group_order)
    counts = group_counts(manifest, group_order)
    print("-- 产出 %d 页，%d 组（project=%s，源码前缀=%s）"
          % (len(manifest["pages"]), len(counts), args.project, "/".join(prefixes)))
    for label in group_order:
        print("     %-24s %3d" % (label, counts[label]))

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
