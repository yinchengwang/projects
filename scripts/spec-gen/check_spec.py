# -*- coding: utf-8 -*-
"""规格书结构校验器 —— 与《图模态实现规格》同构检查"""
import sys, re, os

f = sys.argv[1]
s = open(f, encoding="utf-8").read()

print("=" * 72)
print(f"FILE: {f}")
print(f"字符数: {len(s)}   行数: {s.count(chr(10))+1}")

# 1. h2 段落
h2s = re.findall(r"<h2>(.*?)<span>(.*?)</span></h2>", s, re.S)
print(f"\nH2 段数: {len(h2s)}")
for i, (a, b) in enumerate(h2s, 1):
    print(f"  {i:2d}. {a.strip()}  |  {b.strip()}")

# 2. 坑编号
bugs = re.findall(r'<b>坑\s*(\d+)', s)
print(f"\n坑标记: {len(bugs)} 处  编号={bugs}")

# 3. 编号的坑 小节标题
bug_h = re.findall(r'<b>(坑\s*\d+[^<]*)</b>', s)
print(f"坑标题（带描述）: {len(bug_h)}")
for b in bug_h:
    print(f"  - {b[:70]}")

# 4. 验收数字块
ok_n = len(re.findall(r'<b>验收数字</b>', s))
print(f"\n验收数字块: {ok_n} 处")

# 5. 卡片 / ok / bug / callout / step
for cls in ["card", "ok\"", "bug\"", "callout", "step\"", "tbl-scroll"]:
    print(f"  .{cls}: {len(re.findall(r'class=\"' + cls, s))}")

# 6. 标签闭合检查
for tag in ["section", "div", "table", "tbody", "thead", "tr", "pre", "ul", "li"]:
    o = len(re.findall(rf"<{tag}[ >]", s))
    c = len(re.findall(rf"</{tag}>", s))
    flag = "OK " if o == c else "!! "
    print(f"  {flag}<{tag}>: open={o} close={c}")

# 7. 未转义的裸引号（只在 body 里查，跳过 CSS）
body = s[s.find("</style>"):]
bad = re.findall(r'(?<![\w">])"(?![a-zA-Z_\-]*[a-zA-Z_])', body)
print(f"\n可疑裸引号(body): {len(bad)}")
for m in re.finditer(r'(?<![\w">])"(?![a-zA-Z_\-]*[a-zA-Z_])', body):
    print("   ", repr(body[max(0, m.start()-60):m.start()+60]))

# 8. 必须存在的关键元素
must = ["模块与行数", "实现顺序", "验收清单", "做完之后接什么", "foot"]
for m in must:
    print(f"  {'OK ' if m in s else '!! '}包含「{m}」")
