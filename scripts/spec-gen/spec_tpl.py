# -*- coding: utf-8 -*-
"""
模态实现规格书生成器 —— 共享模板

与《关系模态实现规格》《图模态实现规格》同款：
CSS 变量、.card / .bug / .ok / .callout / .steps / .blk 样式完全一致。
章节骨架固定：
  开篇对照表 → 模块与行数 → 实现顺序 → 字节级布局 →
  编号的坑 → 机制推导 → 事务/恢复 → SQL 侧与能力协商 →
  验收清单 → 做完之后接什么
"""

CSS = """  :root{
    --bg:#0b0e14; --panel:#141922; --panel2:#1a2029; --line:#2a323d;
    --tx:#e6edf3; --tx2:#9aa7b4; --tx3:#6b7885;
    --red:#f85149; --amber:#d29922; --green:#3fb950; --blue:#58a6ff; --purple:#bc8cff; --teal:#39c5cf;
    --redbg:rgba(248,81,73,.12); --amberbg:rgba(210,153,34,.12); --greenbg:rgba(63,185,80,.12);
    --bluebg:rgba(88,166,255,.10); --tealbg:rgba(57,197,207,.10);
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--tx);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Hiragino Sans GB","Microsoft YaHei",sans-serif;
    line-height:1.7;font-size:15px;-webkit-font-smoothing:antialiased;padding:48px 24px 80px}
  .wrap{max-width:1060px;margin:0 auto}
  .eyebrow{font-size:12px;letter-spacing:.18em;color:var(--tx3);text-transform:uppercase;margin-bottom:14px}
  h1{font-size:31px;font-weight:700;letter-spacing:-.02em;line-height:1.3;margin-bottom:16px}
  h1 em{font-style:normal;color:var(--purple)}
  .sub{color:var(--tx2);font-size:15px;max-width:780px}
  section{margin-top:52px}
  h2{font-size:13px;letter-spacing:.16em;text-transform:uppercase;color:var(--tx3);font-weight:600;
     padding-bottom:12px;border-bottom:1px solid var(--line);margin-bottom:24px;display:flex;justify-content:space-between;align-items:baseline;gap:16px}
  h2 span{letter-spacing:0;text-transform:none;font-size:12px;color:var(--tx3);font-weight:400;text-align:right}
  h3{font-size:16.5px;font-weight:600;margin:26px 0 10px}
  h3:first-child{margin-top:0}
  code{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12.5px;
       background:var(--panel2);padding:1px 5px;border-radius:3px;color:var(--purple)}
  pre{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12.5px;line-height:1.65;
      background:var(--panel2);border:1px solid var(--line);border-radius:6px;
      padding:16px 18px;overflow-x:auto;color:var(--tx2);margin:12px 0}
  pre b{color:var(--teal);font-weight:600}

  .tbl-scroll{overflow-x:auto;border:1px solid var(--line);border-radius:8px;background:var(--panel)}
  table{border-collapse:collapse;width:100%;font-size:13px;min-width:640px}
  th,td{padding:9px 12px;text-align:left;border-bottom:1px solid var(--line);vertical-align:top}
  thead th{background:var(--panel2);font-size:11px;letter-spacing:.06em;text-transform:uppercase;color:var(--tx3);font-weight:600;white-space:nowrap}
  tbody tr:last-child td{border-bottom:none}
  td.mono{font-family:ui-monospace,monospace;font-size:12px;color:var(--teal);white-space:nowrap}
  td.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap;color:var(--tx2)}

  .card{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:22px 24px;margin-bottom:14px}
  .card-head{display:flex;justify-content:space-between;align-items:flex-start;gap:16px;margin-bottom:6px;flex-wrap:wrap}
  .card-head h3{font-size:17px;margin:0}
  .pill{font-size:11px;padding:3px 9px;border-radius:4px;font-weight:600;white-space:nowrap;
        color:var(--teal);background:var(--tealbg);border:1px solid rgba(57,197,207,.3)}
  .pill.p{color:var(--purple);background:rgba(188,140,255,.1);border-color:rgba(188,140,255,.3)}

  .blk h4{font-size:10.5px;letter-spacing:.1em;text-transform:uppercase;color:var(--tx3);margin-bottom:8px;font-weight:600}
  .blk ul{list-style:none}
  .blk li{position:relative;padding-left:13px;margin-bottom:6px;font-size:13.5px;color:var(--tx2)}
  .blk li::before{content:"";position:absolute;left:0;top:9px;width:4px;height:4px;border-radius:50%;background:var(--tx3)}
  .blk li b{color:var(--tx);font-weight:600}

  .bug{margin-top:10px;padding:12px 14px;background:var(--redbg);border-radius:6px;border-left:2px solid var(--red);font-size:13px;color:var(--tx2)}
  .bug b{color:var(--red)}
  .ok{margin-top:10px;padding:12px 14px;background:var(--bluebg);border-radius:6px;border-left:2px solid var(--blue);font-size:13px;color:var(--tx2)}
  .ok b{color:var(--blue)}

  .callout{background:var(--panel);border:1px solid var(--line);border-radius:8px;padding:18px 20px;margin-bottom:12px}
  .callout h3{font-size:15.5px;color:var(--teal);margin:0 0 8px}
  .callout p{color:var(--tx2);font-size:13.5px}
  .callout p+p{margin-top:8px}
  .callout p b{color:var(--tx)}

  .steps{counter-reset:s;display:grid;gap:0}
  .step{display:flex;gap:15px;padding:14px 0;border-bottom:1px solid var(--line)}
  .step:last-child{border-bottom:none}
  .step .n{counter-increment:s;flex:0 0 26px;height:26px;border-radius:50%;background:var(--panel2);
           border:1px solid var(--line);display:flex;align-items:center;justify-content:center;
           font-size:12px;color:var(--tx2);font-weight:600}
  .step .n::before{content:counter(s)}
  .step .c{flex:1}
  .step .c b{display:block;font-size:14.5px;margin-bottom:2px}
  .step .c p{font-size:13.5px;color:var(--tx2)}
  .step .w{flex:0 0 54px;text-align:right;font-size:12px;color:var(--tx3);font-variant-numeric:tabular-nums}

  .foot{margin-top:56px;padding-top:20px;border-top:1px solid var(--line);font-size:12.5px;color:var(--tx3);line-height:1.9}
  .foot b{color:var(--tx2)}"""


def _normalize_quotes(html):
    """把 HTML 文本节点里的 ASCII 双引号成对转成中文引号。

    HTML 感知：只处理标签之外（文本节点）的引号，
    属性值里的引号不动。这样生成脚本里可以放心写 ASCII 双引号，
    输出统一是中文引号。
    """
    out = []
    i = 0
    n = len(html)
    in_tag = False
    text_buf = []

    def flush():
        if not text_buf:
            return
        t = "".join(text_buf)
        # 成对替换：第 1、3、5… 个引号开，第 2、4、6… 个闭
        res = []
        open_q = True
        for ch in t:
            if ch == '"':
                res.append("「" if open_q else "」")
                open_q = not open_q
            else:
                res.append(ch)
        out.append("".join(res))
        text_buf.clear()

    while i < n:
        ch = html[i]
        if ch == "<":
            flush()
            in_tag = True
            # 复制整个标签（含引号）
            j = i
            while j < n:
                if html[j] == ">":
                    out.append(html[i:j + 1])
                    i = j + 1
                    break
                j += 1
            else:
                out.append(html[i:])
                i = n
            in_tag = False
            continue
        text_buf.append(ch)
        i += 1
    flush()
    return "".join(out)


def write_spec(path, title, eyebrow, h1, sub, body):
    """写出规格书 HTML。body 是 <div class="wrap"> 之后的正文。"""
    html = (
        "<!DOCTYPE html>\n"
        '<html lang="zh-CN">\n'
        "<head>\n"
        '<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
        f"<title>{title}</title>\n"
        "<style>\n" + CSS + "\n</style>\n"
        "</head>\n"
        "<body>\n"
        '<div class="wrap">\n\n'
        f'<div class="eyebrow">{eyebrow}</div>\n'
        f"<h1>{h1}</h1>\n"
        f'<p class="sub">{sub}</p>\n'
        f"{body}\n"
        "</div>\n"
        "</body>\n"
        "</html>\n"
    )
    html = _normalize_quotes(html)
    with open(path, "w", encoding="utf-8") as f:
        f.write(html)
    return len(html)


def sec(title, span, inner):
    """一个 <section>。title 里已含 <span>副标题</span>；span 参数仅用于留空占位。"""
    return f'<section>\n<h2>{title}</h2>\n{inner}\n</section>\n'


def tbl(headers, rows):
    """一张表。headers 是字符串列表；rows 是列表的列表。"""
    out = ['<div class="tbl-scroll"><table>', "<thead><tr>"]
    for h in headers:
        out.append(f"<th>{h}</th>")
    out.append("</tr></thead>\n<tbody>")
    for r in rows:
        out.append("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>")
    out.append("</tbody></table></div>")
    return "\n".join(out)


def steps(items):
    """实现顺序的步骤列表。items = [(标题, 说明, 工作量)]"""
    out = ['<div class="steps">']
    for t, d, w in items:
        out.append(
            f'<div class="step"><div class="n"></div><div class="c"><b>{t}</b><p>{d}</p></div><div class="w">{w}</div></div>'
        )
    out.append("</div>")
    return "\n".join(out)


def card(head, pill=None, inner=None, pill_cls=""):
    """一张卡片。两种调用方式都合法：
       card(head, inner)            —— 无 pill
       card(head, pill, inner)      —— 带 pill
    """
    if inner is None:
        inner = pill
        pill = None
    p = f'<span class="pill {pill_cls}">{pill}</span>' if pill else ""
    return (
        f'<div class="card">\n<div class="card-head"><h3>{head}</h3>{p}</div>\n{inner}\n</div>'
    )


def bug(text):
    return f'<div class="bug">{text}</div>'


def ok(text):
    return f'<div class="ok">{text}</div>'


def callout(head, paras):
    ps = "".join(f"<p>{p}</p>" for p in paras)
    return f'<div class="callout">\n<h3>{head}</h3>\n{ps}\n</div>'


def checklist(items):
    lis = "".join(f"<li>{i}</li>" for i in items)
    return f'<div class="card"><div class="blk">\n<ul>\n{lis}\n</ul>\n</div></div>'


def foot(paired, basis):
    return (
        '<div class="foot">\n'
        f"<b>配套文档</b>：{paired}<br>\n"
        f"<b>规格依据</b>：{basis}<br>\n"
        "生成于 2026-11-01 · 与 v0.10 代码库状态（152 项验收全绿）对齐。\n"
        "</div>"
    )
