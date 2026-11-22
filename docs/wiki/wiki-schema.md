# wiki 文档契约 v1

> `SCHEMA_VERSION = 1`
> 本文件是契约的人类可读版；机器可读版是 `scripts/wiki_schema.py`。
> 两者由 `scripts/test_wiki_schema_doc.py` 保证不漂移。
> 设计依据：`../docs/superpowers/specs/2026-11-22-multimodal-wiki-and-rag-design.md`

## 1. 一条铁律：禁止行号

源码与文档路径统一禁止行号引用，包括常见代码文件、配置文件和 Markdown 文档；只引用项目相对路径与符号名。

行号腐化的实测依据：`reference/reference/catalog.yml` 已记录 5/68 篇的引用失效
（`time-series/tdengine.md` 单篇 75 处），腐化率 7.3%。乘以 2.5 万篇的规模不成立。

正确写法：`src/backend/storage/buffer/bufmgr.c` + `ReadBufferExtended`
错误写法：`src/backend/storage/buffer/bufmgr.c:1042`

## 2. 校验是硬失败

`scripts/wiki_refs.py` 对每个 `refs[].path`：

1. 在本地浅克隆中确认文件存在 —— 不存在即 FAIL
2. 对 `refs[].symbols` 逐个在该文件内匹配 —— **任一未命中即 FAIL**

**无豁免机制。** 宏与 `static inline` 函数的正确做法是把 refs 指向其真实定义文件
（通常是 `.h`），而不是给校验加白名单。

## 3. 页面类型（page_type）

| 值 | 用途 | 位置 |
|---|---|---|
| `overview` | 项目总览页，即 `_index.md` | 项目目录根部 |
| `feature` | 一个机制单元一篇 | 项目目录下 |
| `compare` | 跨项目对比页 | 必须在 `_compare/` 下 |

## 4. front-matter 字段

### 4.1 特性页必填（page_type = feature / compare）

| 字段 | 说明 |
|---|---|
| `project` | 项目唯一键，跨类不重名 |
| `feature` | 特性唯一键，必须与文件名尾段一致（`a04-buffer-pool.md` → `buffer-pool`） |
| `page_type` | 见 §3 |
| `schema_version` | 当前为 1 |
| `refs` | 结构化引用列表，校验脚本只读此字段 |

`refs` 单项结构：

```yaml
refs:
  - path: src/backend/storage/buffer/bufmgr.c
    symbols: [ReadBufferExtended, PinBuffer, FlushBuffer]
```

### 4.2 总览页必填（page_type = overview）

| 字段 | 说明 |
|---|---|
| `project` | 项目唯一键 |
| `display_name` | 展示名 |
| `repo` | 上游仓库 URL |
| `license` | 许可证标识 |
| `language` | 主要开发语言 |
| `stars` | 取数时的 star 数 |
| `stars_as_of` | **取数日期，必填** —— star 会变，一年后需能解释分层依据 |

## 5. 章节契约

六个必含章节（缺一即 FAIL）+ 一个条件章节（内容可为空，标题必须在）。

| 章节 | 要求 |
|---|---|
| `一句话职责` | 一句话，不含术语堆砌 |
| `机制是什么` | 解决什么、不解决什么、边界在哪；与相邻机制的上游/下游关系 |
| `怎么实现的` | 核心流程、关键数据结构、并发与生命周期。**不设字数上限** |
| `关键取舍` | 放弃了什么、为什么；至少一个「换成 XXX 方案会怎样」 |
| `源码落点` | 路径 + 符号名的表格，**不写行号** |
| `参考` | 论文 / 官方文档 / benchmark，**至少 2 个**外部链接 |
| `与主核 / AI 的关系` | 条件章节，标题必须在，内容可为空 |

完整样例见 `scripts/test_wiki_lint.py` 的 `GOOD_PAGE` 常量。

## 6. 升版预算

`schema_version` 从 1 起，**最多升 3 次**。每次升版必须在本文件追加一节：

```
## v2 变更（YYYY-MM-DD）
- 改了什么：
- 为什么：
```

已归档的项目目录不回溯，只在其 `_index.md` 标记自身 `schema_version`。
第 4 次想升版说明前 3 次没改对，必须停下重审契约。

## 7. 工具

| 命令 | 作用 |
|---|---|
| `python wiki_lint.py <path>...` | 结构门禁：章节、字段、禁止行号、参考数量 |
| `python wiki_refs.py <path>... --repo-root <dir>` | 引用门禁：路径存在 + 符号命中，硬失败 |
| `python wiki_refs.py --locate <SYMBOL> --repo-root <dir>` | 反查符号真实位置，先定位再填 refs |

三个命令退出码 0 = 通过，1 = 有失败，可直接接 CI 与并行流水线。
