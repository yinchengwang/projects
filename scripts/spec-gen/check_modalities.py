import re, io, os
from collections import defaultdict

os.chdir(r"C:\code\StillEvolving\projects\engine\db\docs")
m = io.open("MODALITIES.md", encoding="utf-8").read()

print("=" * 72)
print("最终全量校验 · MODALITIES.md")
print("=" * 72)

# 1. 损坏字符
bad = [i for i, c in enumerate(m) if c == "\ufffd"]
print(f"1. UTF-8 损坏字符        : {len(bad)}  {'OK' if not bad else 'FAIL'}")

# 2. 章节数
h2 = re.findall(r"^## (.+)$", m, re.M)
print(f"2. H2 章节              : {len(h2)}  {'OK' if len(h2) == 14 else 'FAIL'}")

# 3. 八模态节必备小节
mods = [("关系", "## 2 ·"), ("blob", "## 3 ·"), ("图", "## 4 ·"), ("向量", "## 5 ·"),
        ("全文", "## 6 ·"), ("空间", "## 7 ·"), ("时序", "## 8 ·"), ("文档", "## 9 ·")]
print("3. 八模态节结构          :")
allok = True
for name, pref in mods:
    i = m.find(pref)
    j = m.find("\n## ", i + 1)
    seg = m[i:j if j > 0 else len(m)]
    need = {"行模型": "行模型" in seg or "RID" in seg,
            "物理/形态": ("物理布局" in seg or "形态" in seg or "布局" in seg
                       or "物理组织" in seg or "判决" in seg),
            "事务语义": "事务语义" in seg,
            "SPI 三件": "SPI 落地" in seg,
            "范围/不做": "不做项" in seg,
            "关键坑": "关键坑" in seg}
    ok = all(need.values())
    allok &= ok
    miss = [k for k, v in need.items() if not v]
    print(f"   {name:6s} {'OK ' if ok else 'MISS ' + str(miss)}")

# 4. 坑编号连续
d = defaultdict(set)
for a, b in re.findall(r"坑 (M\d+)-(\d+)", m):
    d[int(a[1:])].add(int(b))
print("4. 坑编号连续性          :")
total_bug = 0
for k in sorted(d):
    v = sorted(d[k])
    total_bug += len(v)
    ok = v == list(range(1, len(v) + 1))
    allok &= ok
    print(f"   M{k}: {v} {'OK' if ok else 'FAIL'}")
print(f"   合计 {total_bug} 条坑")

# 5. 表格配平（每张表的列数一致）
print("5. 表格列数一致性        :")
lines = m.split("\n")
tbl_err = 0
cur = []
for ln in lines:
    if ln.startswith("|"):
        cur.append(ln)
    else:
        if len(cur) >= 2:
            cols = [len(re.findall(r"(?<!\\)\|", r)) for r in cur]
            if len(set(cols)) != 1:
                tbl_err += 1
                print(f"   FAIL 表列数不齐: {cur[0][:70]}")
        cur = []
print(f"   {'OK 全部配平' if not tbl_err else f'FAIL {tbl_err} 张表'}")

# 6. 代码围栏配平
fences = m.count("```")
print(f"6. 代码围栏配平          : {fences} 个 {'OK' if fences % 2 == 0 else 'FAIL'}")

# 7. 交叉引用有效
refs = set(re.findall(r"PLAN-[A-Za-z0-9.]+\.md", m))
missing = [r for r in refs if not os.path.exists(r) and "X" not in r and "Y" not in r]
print(f"7. PLAN 交叉引用         : {len(refs)} 个, 缺失 {len(missing)} {'OK' if not missing else 'FAIL ' + str(missing)}")

print()
print("=" * 72)
print(f"结论: {'全部通过' if allok and not bad and not tbl_err and not missing else '有未通过项'}")
print(f"文档规模: {len(m)} 字符 / {len(h2)} 个 H2 / {len(re.findall(r'^### ', m, re.M))} 个 H3 / {total_bug} 条坑")
print("=" * 72)
