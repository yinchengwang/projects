import io, re

p = r"C:\code\StillEvolving\projects\engine\db\docs\MODALITIES.md"
s = io.open(p, encoding="utf-8").read()

# 只修 5 张 "项 | 内容" 表里的能力位行：把反引号内的竖线转义成 \|
# 反引号内竖线转义后仍能正确渲染为代码内容
fixes = [
    ("`AM_CAP_SCAN | INSERT | UPDATE | REMOVE | PUSHDOWN | VACUUM | INDEX`",
     "`AM_CAP_SCAN \\| INSERT \\| UPDATE \\| REMOVE \\| PUSHDOWN \\| VACUUM \\| INDEX`"),
    ("`AM_CAP_SCAN | INSERT | UPDATE | REMOVE | PUSHDOWN | INDEX`",
     "`AM_CAP_SCAN \\| INSERT \\| UPDATE \\| REMOVE \\| PUSHDOWN \\| INDEX`"),
    ("`AM_CAP_SCAN | PUSHDOWN`",
     "`AM_CAP_SCAN \\| PUSHDOWN`"),
    ("`AM_CAP_SCAN | PUSHDOWN | INDEX`",
     "`AM_CAP_SCAN \\| PUSHDOWN \\| INDEX`"),
    ("`AM_CAP_SCAN | PUSHDOWN | EXTERNAL`",
     "`AM_CAP_SCAN \\| PUSHDOWN \\| EXTERNAL`"),
]
for a, b in fixes:
    n = s.count(a)
    print(f"  replace {n}x: {a[:52]}")
    s = s.replace(a, b)

io.open(p, "w", encoding="utf-8", newline="\n").write(s)

# 复验：所有表格行的竖线数（排除转义）应一致
s2 = io.open(p, encoding="utf-8").read()
lines = s2.split("\n")
cur = []
errs = 0
checked = 0
for ln in lines:
    if ln.startswith("|"):
        cur.append(ln)
    else:
        if len(cur) >= 2:
            checked += 1
            # 统计未转义的竖线
            cols = [len(re.findall(r"(?<!\\)\|", r)) for r in cur]
            if len(set(cols)) != 1:
                errs += 1
                print(f"  FAIL 表列数不齐: {cur[0][:70]} -> {cols}")
        cur = []
if len(cur) >= 2:
    checked += 1
    cols = [len(re.findall(r"(?<!\\)\|", r)) for r in cur]
    if len(set(cols)) != 1:
        errs += 1
print(f"\n检查 {checked} 张表, 列数不齐 {errs} 张")
