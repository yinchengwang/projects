# -*- coding: utf-8 -*-
"""从 GitHub REST API 真实回填文档 catalog 的 stars/license/language。

默认只处理占位条目（stars=0 且 stars_as_of=1970-01-01），每次最多请求 55 个。
结果写入 catalog_stars_cache.json，成功结果可断点续跑；不伪造失败数据。
"""
from __future__ import annotations

import argparse
import datetime as dt
import http.client
import json
import os
import re
import sys
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlparse
from urllib.request import ProxyHandler, Request, build_opener, urlopen

HERE = Path(__file__).resolve().parent
# HERE = <projects>/scripts/wiki → parents[1] 是 projects 仓根。
PROJECTS_ROOT = HERE.parents[1]
CATALOG_PATH = PROJECTS_ROOT / "docs" / "wiki" / "catalog-projects.yml"
CACHE_PATH = HERE / "catalog_stars_cache.json"

# 绕过环境代理的直连 opener。本机系统代理与本机直连是两个出口 IP，
# GitHub 未认证限流按 IP 计；代理出口被限流时直连通常还有余量。
_DIRECT_OPENER = build_opener(ProxyHandler({}))
PLACEHOLDER_DATE = "1970-01-01"
MAX_REQUESTS = 55
API_ROOT = "https://api.github.com/repos/"


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description="真实回填 GitHub stars/license/language")
    parser.add_argument("--catalog", type=Path, default=CATALOG_PATH)
    parser.add_argument("--cache", type=Path, default=CACHE_PATH)
    parser.add_argument("--dry-run", action="store_true", help="只列出待取条目，不发网络请求、不写文件")
    parser.add_argument("--limit", type=int, default=MAX_REQUESTS, help="本次最多请求数，默认 55，不能超过 55")
    parser.add_argument("--no-write-catalog", action="store_true", help="只更新缓存，不写 catalog")
    parser.add_argument("--direct", action="store_true",
                        help="绕过环境代理直连 GitHub（代理与直连是不同出口 IP，限流按 IP 计）")
    return parser.parse_args(argv)


def load_catalog(path):
    sys.path.insert(0, str(HERE))
    from wiki_catalog import parse_catalog

    return parse_catalog(path.read_text(encoding="utf-8"))


def dump_catalog(entries, path):
    sys.path.insert(0, str(HERE))
    from wiki_catalog import dump_catalog

    path.write_text(dump_catalog(entries), encoding="utf-8", newline="\n")


def load_cache(path):
    if not path.exists():
        return {"version": 1, "entries": {}}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError("缓存无法读取，请人工修复或移走 %s: %s" % (path, exc)) from exc
    if not isinstance(value, dict) or not isinstance(value.get("entries", {}), dict):
        raise RuntimeError("缓存结构非法，期望 {version, entries}: %s" % path)
    value.setdefault("version", 1)
    return value


def save_cache(cache, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(cache, ensure_ascii=False, indent=2) + "\n", encoding="utf-8", newline="\n")


def repo_slug(repo_url):
    parsed = urlparse(repo_url)
    if parsed.scheme not in {"http", "https"} or parsed.netloc.lower() != "github.com":
        return None
    parts = [part for part in parsed.path.split("/") if part]
    if len(parts) < 2:
        return None
    owner, name = parts[0], parts[1]
    if name.endswith(".git"):
        name = name[:-4]
    if not owner or not name:
        return None
    # owner 与 name 各自必须是合法 GitHub 标识符（不能含斜杠）。
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", owner) or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        return None
    return owner + "/" + name


def is_placeholder(entry):
    return entry.get("stars") == 0 and entry.get("stars_as_of") == PLACEHOLDER_DATE


def request_repo(slug, token=None, timeout=20, attempts=3, direct=False):
    """取一次仓库元数据。网络层错误重试，HTTP 403/429 立即返回让调用方停轮。

    `direct=True` 绕过环境代理直连 —— 代理与直连是两个出口 IP，限流按 IP 计，
    一边被限流时另一边常用于继续断点续跑。
    """
    last_error = None
    for attempt in range(1, attempts + 1):
        result = _request_once(slug, token=token, timeout=timeout, use_proxy=not direct)
        if result.get("status") == "ok" or result.get("http_status") in (403, 404, 429, 451):
            return result
        last_error = result
        if attempt < attempts:
            time.sleep(1.5 * attempt)
    return last_error


def _request_once(slug, token=None, timeout=20, use_proxy=True):
    headers = {
        "Accept": "application/vnd.github+json",
        "User-Agent": "projects-wiki-stars-fill/1.0",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if token:
        headers["Authorization"] = "Bearer " + token
    req = Request(API_ROOT + slug, headers=headers, method="GET")
    # 系统里配置的代理与本机直连是两个不同的出口 IP，GitHub 的未认证限流按 IP 计。
    # 代理出口被限流时直连往往还有余量，所以允许绕过环境代理。
    opener = urlopen if use_proxy else _DIRECT_OPENER.open
    fetched_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
    try:
        with opener(req, timeout=timeout) as response:
            raw = response.read().decode("utf-8")
            data = json.loads(raw)
            remaining = response.headers.get("X-RateLimit-Remaining")
            reset = response.headers.get("X-RateLimit-Reset")
            return {
                "status": "ok",
                "fetched_at": fetched_at,
                "http_status": response.status,
                "rate_remaining": remaining,
                "rate_reset": reset,
                "stars": int(data["stargazers_count"]),
                "license": ((data.get("license") or {}).get("spdx_id") or "NOASSERTION"),
                "language": data.get("language") or "unknown",
            }
    except HTTPError as exc:
        body = ""
        try:
            body = exc.read(500).decode("utf-8", errors="replace")
        except OSError:
            pass
        reason = "HTTP %s" % exc.code
        if exc.code in (403, 429):
            reason += "（可能触发 GitHub API 限流）"
        return {"status": "failed", "fetched_at": fetched_at, "http_status": exc.code, "error": reason, "body": body}
    except (URLError, TimeoutError, OSError, ValueError, KeyError, http.client.HTTPException) as exc:
        return {"status": "failed", "fetched_at": fetched_at, "error": "%s: %s" % (type(exc).__name__, exc)}


def main(argv=None):
    args = parse_args(argv)
    if args.limit < 0 or args.limit > MAX_REQUESTS:
        print("错误：--limit 必须在 0..%d 之间" % MAX_REQUESTS, file=sys.stderr)
        return 2

    entries = load_catalog(args.catalog)
    cache = load_cache(args.cache)
    targets = [entry for entry in entries if is_placeholder(entry)]
    invalid = [(entry.get("name"), entry.get("repo")) for entry in targets if repo_slug(entry.get("repo", "")) is None]
    cached_ok = {name for name, item in cache["entries"].items() if item.get("status") == "ok"}
    pending = [entry for entry in targets if repo_slug(entry.get("repo", "")) not in cached_ok]

    print("catalog:", args.catalog)
    print("占位条目:", len(targets), "已缓存成功:", len(targets) - len(pending), "待请求:", len(pending))
    if invalid:
        print("无法解析 GitHub repo（保持占位）:", invalid)
    if args.dry_run:
        for entry in pending[: args.limit]:
            print("DRY-RUN", entry["name"], "->", repo_slug(entry.get("repo", "")))
        print("DRY-RUN 不发请求、不写缓存、不写 catalog。可请求上限:", args.limit)
        return 0

    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    request_count = 0
    success = []
    failures = []
    use_direct = bool(args.direct)
    for entry in pending[: args.limit]:
        slug = repo_slug(entry.get("repo", ""))
        if not slug:
            continue
        result = request_repo(slug, token=token, direct=use_direct)
        cache["entries"][slug] = {"name": entry["name"], "repo": entry["repo"], **result}
        request_count += 1
        if result.get("status") == "ok":
            success.append((entry["name"], slug, result))
            print("OK", entry["name"], slug, "stars=", result["stars"], "language=", result["language"])
            if result.get("rate_remaining") == "0":
                print("检测到剩余请求数为 0，停止本轮。")
                break
        else:
            failures.append((entry["name"], slug, result))
            print("FAIL", entry["name"], slug, result.get("error"))
            if result.get("http_status") in (403, 429):
                print("检测到 GitHub 限流/拒绝，停止本轮，等待下次断点续跑。")
                break
    save_cache(cache, args.cache)

    if not args.no_write_catalog and success:
        today = dt.date.today().isoformat()
        by_slug = {repo_slug(entry.get("repo", "")): entry for entry in entries}
        for name, slug, result in success:
            entry = by_slug.get(slug)
            if entry is None:
                continue
            entry["stars"] = result["stars"]
            entry["stars_as_of"] = today
            entry["language"] = result["language"]
            entry["license"] = result["license"]
        dump_catalog(entries, args.catalog)
        print("已写 catalog 成功条目:", len(success), "取数日期:", today)

    remaining = [entry["name"] for entry in entries if is_placeholder(entry)]
    print("本轮真实请求:", request_count, "成功:", len(success), "失败:", len(failures))
    print("仍为占位条目:", len(remaining), remaining)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
