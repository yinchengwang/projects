# -*- coding: utf-8 -*-
"""源码目录树 → 机制单元候选。

postgres 332 篇不是拍脑袋列的：每篇对应一个真实目录或一组紧密相关源文件。
本模块只回答「哪些目录有源码、各有多少」，把「拆几篇」留给 Phase 1 判定。

职责边界：不做缓存、不读 manifest、不判「拆几篇」——只遍历文件系统。
"""
import os

SOURCE_EXTS = (
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hh",
    ".go", ".rs", ".java", ".kt", ".scala", ".py", ".ts",
    ".sql", ".lua", ".rb", ".php", ".m", ".mm", ".swift",
)

# 这些目录要么是构建产物，要么是测试夹具，不承载「机制」。
# .git / build / test / contrib / doc 是 postgres 枚举的关键排除项：
# 前三者是元数据与产物，后两者是附属组件与文档。
SKIP_DIRS = (
    ".git", "build", "build-msvc", "node_modules", "target", "vendor",
    "third_party", "thirdpart", "deps", "dist", "test", "tests", "regress",
    "examples", "doc", "docs", "contrib",
)


def count_source_files(dir_path: str) -> int:
    """统计目录下（不递归）符合 SOURCE_EXTS 的文件数。目录不存在返回 0。"""
    if not os.path.isdir(dir_path):
        return 0
    total = 0
    for name in os.listdir(dir_path):
        full = os.path.join(dir_path, name)
        if os.path.isfile(full) and name.endswith(SOURCE_EXTS):
            total += 1
    return total


def module_key_from_dir(repo_root: str, dir_path: str) -> str:
    """返回相对仓根、正斜杠分隔的路径。"""
    rel = os.path.relpath(dir_path, repo_root)
    return rel.replace(os.sep, "/")


def _depth_from_root(repo_root: str, dir_path: str) -> int:
    """仓根自身记 -1，仓根的直接子目录记 0。"""
    rel = os.path.relpath(dir_path, repo_root)
    if rel in (".", ""):
        return -1
    return len(rel.split(os.sep)) - 1


def list_module_dirs(repo_root: str, depth: int = 2) -> list[dict]:
    """列出下降层数不超过 depth 的目录，附带其直接源文件数。

    返回 [{"path": "src/backend/storage/buffer", "source_files": 8}, ...]，按 path 排序。
    只返回 source_files > 0 的目录：空目录与纯文档目录不构成机制单元。

    depth 语义（简报参考实现与简报测试在此差一层，本实现取后者）：
    current_depth 以「仓根的直接子目录」为 0 起算，current_depth >= depth 时
    不再下降。因此 depth=3 枚举到的最深目录是仓根下第 4 层（如
    src/backend/access/heap）——postgres 的核心机制目录恰在这一层，332 篇
    的推导依赖它们可见；depth=2（默认）最深到第 3 层。
    """
    repo_root = os.path.abspath(repo_root)
    results = []

    for dirpath, dirnames, _ in os.walk(repo_root):
        dirnames[:] = sorted(
            d for d in dirnames
            if d not in SKIP_DIRS and not d.startswith(".")
        )
        if _depth_from_root(repo_root, dirpath) >= depth:
            dirnames[:] = []

        count = count_source_files(dirpath)
        if count > 0:
            results.append(
                {
                    "path": module_key_from_dir(repo_root, dirpath),
                    "source_files": count,
                }
            )

    return sorted(results, key=lambda item: item["path"])
