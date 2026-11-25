# -*- coding: utf-8 -*-
"""源码浅克隆缓存。

设计文档 §6.1：实测 raw 拉取 268 KB 需 3005 ms，270 库 × 平均 400 文件会变成
数十小时纯网络等待，因此改为每库整仓一次 --depth 1 克隆。

缓存目录默认 <仓根>/.cache/repos/<name>，已在 .gitignore 里排除。
本模块位于 scripts/wiki/，因此 _HERE 上溯两级才到仓根。

职责边界：本模块只保证「本地有一份该项目的浅克隆」。它不拉取更新、不做增量同步、
不并发、不重试——上游变了就换一个 name 或人工清缓存，以免文档校验在无人值守时
被隐式变化影响。
"""
import os
import shutil
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))
# 本模块位于 <仓根>/scripts/wiki/，上溯两级才是仓根。
_REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))
DEFAULT_CACHE_DIR = os.path.normpath(os.path.join(_REPO_ROOT, ".cache", "repos"))

# 失败信息里附带的 git 输出上限：只留够诊断的尾部信息，不把整段日志塞进异常。
_MAX_DETAIL = 500


class RepoCacheError(Exception):
    """克隆失败或缓存目录不可用。"""


def repo_path(cache_dir: str, name: str) -> str:
    """返回该项目在缓存里的目录路径（不保证存在）。"""
    if not isinstance(name, str) or not name or name in (".", ".."):
        raise RepoCacheError("缓存名称非法: %r" % (name,))
    if os.path.isabs(name) or "/" in name or "\\" in name:
        raise RepoCacheError("缓存名称必须是单一目录名: %r" % name)
    base = os.path.realpath(cache_dir)
    target = os.path.realpath(os.path.join(base, name))
    if os.path.commonpath((base, target)) != base:
        raise RepoCacheError("缓存路径越界: %r" % name)
    return os.path.join(cache_dir, name)


def _normalize_repo_url(url: str) -> str:
    """归一化仓库 URL，用于「同一仓库」判断。

    除大小写、反斜杠、尾部斜杠外，还要抹掉末尾的 ``.git``：
    同一个 GitHub 仓库既可写成 ``https://github.com/x/y``，
    也可写成 ``https://github.com/x/y.git``，git clone 会存留调用者给的写法，
    不归一化会让「已有缓存 origin 与 repo_url 不匹配」把同一仓库误判成不同仓库。
    """
    text = url.rstrip("/").replace("\\", "/").lower()
    if text.endswith(".git"):
        text = text[:-4]
    return text.rstrip("/")


def _git_detail(proc) -> str:
    """从 git 进程输出里取一段可读的诊断信息。"""
    detail = (proc.stderr or proc.stdout or "").strip()
    return detail[:_MAX_DETAIL] or "返回码 %d" % proc.returncode


def _force_remove(path: str) -> None:
    """删除目录，先把只读位去掉再删。

    git 把 pack/*.idx、*.pack 写成 0444，Windows 上直接 shutil.rmtree 会在这些
    文件上报 PermissionError，半成品目录反而清不掉。这里不依赖 rmtree 的
    onerror 钩子（3.12 起已弃用），改成自己先遍历改权限。
    """
    for root, dirs, files in os.walk(path):
        for name in list(dirs):
            entry = os.path.join(root, name)
            if os.path.islink(entry):
                dirs.remove(name)
                continue
            try:
                os.chmod(entry, 0o700)
            except OSError:
                pass
        for name in files:
            entry = os.path.join(root, name)
            try:
                if os.path.islink(entry):
                    continue
                os.chmod(entry, 0o600)
            except OSError:
                pass
    shutil.rmtree(path)


def _cleanup_partial(target: str) -> str:
    """清理本次新建的半成品目录，返回给异常的补充说明（成功则为空串）。

    清理不是「尽力而为」而是必须确认：`git clone` 可能传输成功、检出失败，
    此时 target/.git 已存在，留着它会让下一次调用把半成品当成有效缓存。删不掉
    时必须把这件事写进异常，不能静默。
    """
    if not os.path.exists(target):
        return ""
    try:
        _force_remove(target)
    except OSError as exc:
        return "；半成品目录清理失败，请手工删除 %s（%s）" % (target, exc)
    return ""


def ensure_repo(
    repo_url: str, name: str, cache_dir=None, depth: int = 1, full: bool = False
) -> str:
    """确保本地有一份该项目源码，返回目录路径。

    已存在则直接返回（幂等，不改动既有工作树）。失败抛 RepoCacheError。

    `full=True` 时做**全量克隆**（完整历史 + 全部 tag + 全部分支），
    不加 `--depth` / `--single-branch`。这是机制盘点的默认要求：refs 要能
    对着任意历史版本核验，浅克隆只有 1 个 commit、0 个 tag，做不到。

    `full=False` 时保留原来的浅克隆（`--depth <depth> --single-branch`），
    供只跑门禁、不需要历史的场景使用。

    克隆失败时，若 target 是本函数这次新建的，会把它清理掉：半成品目录留着会让
    后续调用把它当成「已有缓存」，也会掩盖真正的失败原因。target 若是调用方事先
    就有的目录，则一律不动——不能删不属于我们的东西。
    """
    cache_dir = cache_dir or DEFAULT_CACHE_DIR
    target = repo_path(cache_dir, name)
    if os.path.lexists(target):
        probe = subprocess.run(
            ["git", "-C", target, "rev-parse", "--is-inside-work-tree"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if probe.returncode != 0 or probe.stdout.strip() != "true":
            raise RepoCacheError("已有缓存不是有效 Git 工作树 (%s): %s" % (target, _git_detail(probe)))
        origin = subprocess.run(
            ["git", "-C", target, "remote", "get-url", "origin"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        )
        if origin.returncode != 0 or _normalize_repo_url(origin.stdout.strip()) != _normalize_repo_url(repo_url):
            raise RepoCacheError("已有缓存 origin 与 repo_url 不匹配 (%s)" % target)
        return target

    preexisting = os.path.exists(target)
    try:
        os.makedirs(cache_dir, exist_ok=True)
    except OSError as exc:
        raise RepoCacheError("缓存目录不可用 (%s): %s" % (cache_dir, exc))

    cmd = ["git", "clone"]
    if full:
        # partial clone：保留完整提交历史与全部 tag，blob 按需下载。
        # 实测（2026-11-25）：postgres 全量克隆在 25 分钟内 pack 仍为 0 字节
        # 而卡死；改用 --filter=blob:none 后 9 分钟完成（107528 commits / 144 MB）。
        cmd += ["--filter=blob:none"]
    else:
        cmd += ["--depth", str(depth), "--single-branch"]
    cmd += [repo_url, target]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as exc:
        raise RepoCacheError("无法执行 git (%s): %s" % (repo_url, exc))

    if proc.returncode != 0:
        cleanup = ""
        if not preexisting:
            cleanup = _cleanup_partial(target)
        raise RepoCacheError(
            "git clone 失败 (%s): %s%s"
            % (repo_url, _git_detail(proc), cleanup)
        )
    return target
