#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
gt_check.py — 向量模态的 ground truth 交叉验证（v1.0 步骤 6 · A3 退化方案）

【为什么需要这个文件】

PLAN §1.7 的 A3 要求"对照 hnswlib"。本机 pip 找不到 hnswlib 的 wheel
（该平台没有预编译包），所以按 R3 走退化方案。

但退化方案不能退化成"**和自己比**"——那等于什么都没验。
这里的做法是：

    用 numpy 写一份**完全独立的**精确 top-k 实现，
    和 C 侧算出来的 GT 逐条比对。

两份实现用的是不同的语言、不同的库、不同的算法路径
（C 侧是最大堆 + 堆排序，numpy 侧是矩阵乘 + argpartition），
所以它们同时算错同一个结果的概率极低。

【验三件事，不只一件】

  1. GT 对不对   —— numpy 重算的 top-k 和 C 侧逐条比（id + 距离）
  2. recall 对不对 —— 用 numpy 的 GT 去算 C 侧 ANN 结果的 recall，
                     和 C 侧自报的 recall 比
  3. 顺序对不对 —— ANN 结果的距离必须单调不降

第 3 条是单独列出来的，因为 v1.0 步骤 5 踩过一次：
"集合对、顺序错"的 bug 用普通的断言验不出来
（断言只比集合），只有逐条比距离序列才看得见。

【用法】

    python tools/gt_check.py test/tmp/vec_bench_data.bin.uniform
    python tools/gt_check.py test/tmp/vec_bench_data.bin.clustered

（不带后缀的话会自动试 .uniform 和 .clustered 两个）

【依赖】

    numpy。本机用的是 anaconda 自带的（numpy 2.1.3）。
    没装的话：pip install numpy
"""

import os
import sys

try:
    import numpy as np
except ImportError:
    print("需要 numpy。安装：pip install numpy")
    sys.exit(2)


def load_data(path):
    """读 .data 文件 → (dim, n_vec, n_query, k, vecs, queries)"""
    with open(path, "rb") as f:
        hdr = np.fromfile(f, dtype=np.uint32, count=4)
        dim, n_vec, n_query, k = (int(x) for x in hdr)
        data = np.fromfile(f, dtype=np.float32,
                           count=(n_vec + n_query) * dim)
    if data.size != (n_vec + n_query) * dim:
        raise ValueError("数据文件长度不对")
    vecs = data[: n_vec * dim].reshape(n_vec, dim)
    queries = data[n_vec * dim:].reshape(n_query, dim)
    return dim, n_vec, n_query, k, vecs, queries


def load_result(path):
    """读 .result 文件 → 各种结果"""
    with open(path, "rb") as f:
        hdr = np.fromfile(f, dtype=np.uint32, count=3)
        n_query, k, ef = (int(x) for x in hdr)
        c_recall = float(np.fromfile(f, dtype=np.float32, count=1)[0])
        n_curve = int(np.fromfile(f, dtype=np.uint32, count=1)[0])
        curve_efs = np.fromfile(f, dtype=np.uint16, count=n_curve)
        curve_recalls = np.fromfile(f, dtype=np.float32, count=n_curve)
        gt_ids = np.fromfile(f, dtype=np.uint32, count=n_query * k)
        ann_ids = np.fromfile(f, dtype=np.uint32, count=n_query * k)
        ann_d = np.fromfile(f, dtype=np.float32, count=n_query * k)
    return dict(n_query=n_query, k=k, ef=ef, c_recall=c_recall,
                curve_efs=curve_efs, curve_recalls=curve_recalls,
                gt_ids=gt_ids.reshape(n_query, k),
                ann_ids=ann_ids.reshape(n_query, k),
                ann_d=ann_d.reshape(n_query, k))


def numpy_topk(vecs, queries, k):
    """
    numpy 的精确 top-k（平方 L2）。

    【为什么用 float64 算距离】
    C 侧是 float32 累加，numpy 这边用 float64 ——
    这样"两边的微小差异"只可能来自 C 侧的 float32 舍入，
    而不是 numpy 的算法差异。比出来的误差上界因此是**可解释的**。
    """
    v = vecs.astype(np.float64)
    q = queries.astype(np.float64)

    # ||q - x||² = ||q||² + ||x||² - 2 q·x
    d2 = (q * q).sum(axis=1)[:, None] + (v * v).sum(axis=1)[None, :]
    d2 -= 2.0 * (q @ v.T)

    # argpartition 取前 k，再对这 k 个排序
    idx = np.argpartition(d2, kth=k - 1, axis=1)[:, :k]
    rows = np.arange(queries.shape[0])[:, None]
    order = np.argsort(d2[rows, idx], axis=1)
    idx = idx[rows, order]

    dists = np.take_along_axis(d2, idx, axis=1)
    return idx, dists


def check(base):
    data_path = base + ".data"
    result_path = base + ".result"

    if not os.path.exists(data_path) or not os.path.exists(result_path):
        print("找不到 %s / %s" % (data_path, result_path))
        return 2

    dim, n_vec, n_query, k, vecs, queries = load_data(data_path)
    res = load_result(result_path)
    gt_ids = res["gt_ids"]
    ann_ids = res["ann_ids"]
    ann_d = res["ann_d"]
    c_recall = res["c_recall"]

    if res["n_query"] != n_query or res["k"] != k:
        print("数据文件与结果文件对不上")
        return 2

    print("向量模态 GT 交叉验证（numpy 独立实现）")
    print("  数据: %s" % data_path)
    print("  dim=%d, n_vec=%d, n_query=%d, k=%d, ef=%d"
          % (dim, n_vec, n_query, k, res["ef"]))
    print()

    # ---------- 1. GT 逐条比对 ----------
    np_ids, np_dists = numpy_topk(vecs, queries, k)

    id_diff = (np_ids != gt_ids)
    n_id_diff = int(id_diff.sum())

    # 距离比对：用 numpy 算的距离 vs C 侧的 id 对应的距离
    # C 侧只导了 ANN 的距离，GT 的距离由 numpy 反查自己的向量算。
    # 这里比的是"两边选的 id 对应的距离"——
    # 如果 id 一致，距离必然一致（同一批数据）；如果 id 不同，
    # 距离可能仍然一致（并列），那就是可接受的差异。
    c_gt_d = np.empty((n_query, k), dtype=np.float64)
    for i in range(n_query):
        for j in range(k):
            c_gt_d[i, j] = ((vecs[gt_ids[i, j]].astype(np.float64)
                             - queries[i].astype(np.float64)) ** 2).sum()

    rel = np.abs(c_gt_d - np_dists) / np.maximum(
        np.abs(np_dists), np.finfo(np.float64).tiny)
    max_rel = float(rel.max()) if rel.size else 0.0

    print("【1】GT 逐条比对（numpy 重算 vs C 侧计算）")
    print("    row_id 不同的条数: %d / %d" % (n_id_diff, n_query * k))
    print("    对应距离的最大相对误差: %.3e" % max_rel)

    # id 不同但距离一致 = 距离并列，可接受
    tie_ok = 0
    real_bad = 0
    for i in range(n_query):
        for j in range(k):
            if not id_diff[i, j]:
                continue
            if rel[i, j] <= 1e-5:
                tie_ok += 1
            else:
                real_bad += 1

    if real_bad == 0:
        print("    → PASS（%d 处是距离并列，可接受）" % tie_ok)
    else:
        print("    → FAIL（%d 处距离也不同）" % real_bad)

    gt_pass = (real_bad == 0)

    # ---------- 2. recall 复算 ----------
    print()
    print("【2】recall 复算（用 numpy 的 GT 去算 C 侧 ANN 结果）")
    hits = 0
    for i in range(n_query):
        gset = set(np_ids[i].tolist())
        for j in range(k):
            if int(ann_ids[i, j]) in gset:
                hits += 1
    np_recall = hits / float(n_query * k)
    print("    numpy 复算 recall@%d = %.4f" % (k, np_recall))
    print("    C 侧自报 recall@%d = %.4f" % (k, c_recall))
    # 容差取 1e-6：C 侧的 recall 是 float32 存的，精度只有 ~1e-7。
    # 用 1e-9 去比会误报（第一版就踩了这个 —— 两边都显示 0.8650，
    # 差值 9.5e-9 却报 FAIL）。
    diff = abs(np_recall - c_recall)
    print("    差值: %.3e" % diff)
    recall_ok = diff < 1e-6
    print("    → %s" % ("PASS（两边一致）" if recall_ok
                        else "FAIL（两边的 recall 不一样 —— 量错了）"))

    # ---------- 3. 顺序 ----------
    print()
    print("【3】ANN 结果的顺序（距离必须单调不降）")
    bad_order = 0
    for i in range(n_query):
        row = ann_d[i]
        for j in range(1, k):
            if row[j] < row[j - 1]:
                bad_order += 1
                break
    print("    顺序错乱的查询数: %d / %d" % (bad_order, n_query))
    print("    → %s" % ("PASS" if bad_order == 0
                        else "FAIL（集合可能对，但顺序错了 —— 见步骤 5 的坑）"))

    # ---------- 4. 距离对不对 ----------
    print()
    print("【4】ANN 结果的距离与 numpy 重算的一致性")
    c_ann_d = np.empty((n_query, k), dtype=np.float64)
    for i in range(n_query):
        for j in range(k):
            c_ann_d[i, j] = ((vecs[ann_ids[i, j]].astype(np.float64)
                              - queries[i].astype(np.float64)) ** 2).sum()
    ann_rel = np.abs(c_ann_d - ann_d) / np.maximum(
        np.abs(c_ann_d), np.finfo(np.float64).tiny)
    max_ann_rel = float(ann_rel.max()) if ann_rel.size else 0.0
    print("    最大相对误差: %.3e" % max_ann_rel)
    dist_ok = max_ann_rel < 1e-4
    print("    → %s" % ("PASS" if dist_ok else "FAIL"))

    print()
    all_ok = gt_pass and recall_ok and bad_order == 0 and dist_ok
    print("总判定: %s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


def main(argv):
    if len(argv) > 1:
        bases = argv[1:]
    else:
        bases = ["test/tmp/vec_bench_data.bin.uniform",
                 "test/tmp/vec_bench_data.bin.clustered"]

    rc = 0
    for base in bases:
        if not os.path.exists(base + ".data"):
            continue
        print("=" * 60)
        r = check(base)
        rc = rc or r
        print()
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
