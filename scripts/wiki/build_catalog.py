# -*- coding: utf-8 -*-
"""从 reference 仓的镜像清单继承基线，产出文档生产清单。

两份清单语义不同（镜像清单管「怎么克隆」，生产清单管「写哪些库、写多深」），
这里只继承 name / model / tier / repository / license 五个字段，
不继承 commit / pin_status / source / mirror / status / notes —— 那些属于克隆策略，
与文档生产无关。

未取数的条目用占位值（language=unknown、stars=0、stars_as_of=1970-01-01），
交由后续任务实测替换，绝不编造 stars。
"""
import argparse
import os
import re
import sys

from wiki_catalog import CATALOG_FIELDS, dump_catalog, validate_entry

# reference 镜像清单的 tier 口径 → 文档生产清单的 tier 口径
TIER_MAP = {"core": "T1", "representative": "T2", "innovative": "T3"}

# 未取数条目的占位值（1970-01-01 是显式的「从未取过数」标记，不是真实日期）
PLACEHOLDER_LANGUAGE = "unknown"
PLACEHOLDER_STARS = 0
PLACEHOLDER_STARS_AS_OF = "1970-01-01"

# overrides 允许覆盖的字段，其余键一律忽略（避免把镜像清单字段漏进来）
_OVERRIDABLE = ("language", "stars", "stars_as_of")

# 镜像清单是规整的两层结构：2 空格缩进的条目、4 空格缩进的字段
_ENTRY_RE = re.compile(r"^\s{2}-\s+name:\s*(.+?)\s*$")
_FIELD_RE = re.compile(r"^\s{4}([A-Za-z_]+):\s*(.*?)\s*$")


def map_tier(reference_tier):
    """core→T1 / representative→T2 / innovative→T3。未知值抛 ValueError，不静默降级。"""
    try:
        return TIER_MAP[reference_tier]
    except KeyError:
        raise ValueError("未知的 reference tier: %r" % (reference_tier,))


def build_entry(name, model, tier, repo, license, language, stars, stars_as_of):
    """构造一条清单条目。repo 末尾的 .git 会被去掉，字段恰好八个。"""
    if isinstance(repo, str) and repo.endswith(".git"):
        repo = repo[: -len(".git")]
    values = {
        "name": name,
        "model": model,
        "tier": tier,
        "repo": repo,
        "license": license,
        "language": language,
        "stars": stars,
        "stars_as_of": stars_as_of,
    }
    # 按 CATALOG_FIELDS 的固定顺序落字段，保证条目形状与清单 schema 一致
    return {field: values[field] for field in CATALOG_FIELDS}


def _parse_reference(path):
    """解析镜像清单里的 name / model / tier / repository / license。

    只按行取这五个字段，notes 之类的多行列表自然被 4 空格锚定规则排除。
    """
    with open(path, encoding="utf-8") as f:
        text = f.read().replace("\r\n", "\n")

    entries = []
    current = None
    for raw in text.split("\n"):
        m = _ENTRY_RE.match(raw)
        if m:
            current = {"name": m.group(1).strip()}
            entries.append(current)
            continue
        m = _FIELD_RE.match(raw)
        if m and current is not None:
            current[m.group(1)] = m.group(2).strip()
    return entries


def merge_from_reference(reference_catalog_path, overrides):
    """产出完整条目列表；overrides 按 name 覆盖 language / stars / stars_as_of。"""
    out = []
    for raw in _parse_reference(reference_catalog_path):
        name = raw["name"]
        # 只取三个可覆盖字段，overrides 里的多余键（尤其镜像清单专有字段）直接丢弃
        raw_override = overrides.get(name, {})
        override = {k: raw_override[k] for k in _OVERRIDABLE if k in raw_override}
        out.append(build_entry(
            name=name,
            model=raw.get("model", ""),
            tier=map_tier(raw.get("tier", "")),
            repo=raw.get("repository", ""),
            license=raw.get("license", ""),
            language=override.get("language", PLACEHOLDER_LANGUAGE),
            stars=override.get("stars", PLACEHOLDER_STARS),
            stars_as_of=override.get("stars_as_of", PLACEHOLDER_STARS_AS_OF),
        ))
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description="生成文档生产清单（基线继承）")
    parser.add_argument("reference_catalog", help="reference/reference/catalog.yml 路径")
    parser.add_argument("output", help="输出路径（docs/wiki/catalog-projects.yml）")
    args = parser.parse_args(argv)

    entries = merge_from_reference(args.reference_catalog, overrides={})
    bad = 0
    for entry in entries:
        errors = validate_entry(entry)
        if errors:
            bad += 1
            print("FAIL %s" % entry.get("name"))
            for e in errors:
                print("     - %s" % e)
    if bad:
        # 有一条不合格就整体不落地，避免下游读到半成品
        print("-- %d 条不合格，未写出" % bad)
        return 1

    text = dump_catalog(entries)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print("-- 写出 %d 条到 %s" % (len(entries), args.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
