# Wiki RAG 演示应用

基于本地 wiki 语料构建的三栏混合检索工作台。答案为**抽取式**：只把检索到的原文块拼接，不由模型改写；出处由后端按块 ID 生成。

## 快速开始

要求 Python 3.11+。本应用是 `apps/` 下第一个带 pip 依赖与 HTTP 后端的应用（取舍见文末）。

```powershell
cd C:\code\projects\apps\wiki-rag
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python tools/ingest.py --project postgres
python -m server.main
```

服务默认 `http://127.0.0.1:8000/`。

```powershell
python tools/ingest.py --project postgres   # 只重建指定库
python tools/ingest.py --all                # 重建语料里所有库
```

索引按 `index/data/projects/<库名>/` 分库保存：`chunks.json`（块与元数据）、`vectors.npy`（向量）、`metadata.json`（页数、块数、块 ID 全量清单、门禁退出码、嵌入/向量后端）。向量另存于 `index/data/vectors.lancedb/` 的同名表。按库重建只覆盖该库目录与该库表；同一批语料重跑得到相同的块数与块 ID 集合（幂等）。`index/data/` 已在本目录 `.gitignore` 排除。

## API

| 端点 | 方法 | 说明 |
|---|---|---|
| `/api/health` | GET | 服务存活、嵌入后端、向量后端、索引是否就绪、页数、降级告警 |
| `/api/projects` | GET | 已入库库清单 + 页/块数 + 真实门禁状态（`PASS` / `FAIL`） |
| `/api/projects/{name}` | GET | 单库的页与块清单 |
| `/api/search` | POST | 纯检索：`{"query": "缓冲池", "projects": ["postgres"], "limit": 10}` |
| `/api/ask` | POST | 检索 + 抽取式回答 + 引用列表 + 检索抽屉数据 |
| `/` | GET | 三栏静态页面 |

门禁状态不是写死的：`tools/ingest.py` 实际以子进程运行 `projects/scripts/wiki/wiki_lint.py` 与 `wiki_refs.py`，把**退出码**写进 `metadata.json`，API 原样读出，任一非零即 `FAIL`。`wiki_refs.py` 需要该库的本地源码浅克隆，默认找 `projects/.cache/repos/<库名>`，可用 `WIKI_RAG_<库名大写>_REPO` 覆盖（如 `WIKI_RAG_POSTGRES_REPO`）。找不到源码仓时记 127 而非假装通过。

## 架构与数据流

```text
docs/wiki/projects/<库>/*.md          ← 只读，不修改写作中的语料
  → core/wiki_source.py               复用 projects/scripts/wiki/wiki_frontmatter.py 的 parse_frontmatter
  → core/chunker.py                   按 ## 切；短章节并入邻章；超长章节按段落二次切分
  → index/build_index.py              嵌入 + BM25 索引 + LanceDB/NumPy 向量检索
  → core/fusion.py                    RRF 融合（k 可配置，默认 60），overview 降权
  → core/extractive.py                原文段落拼接 + 后端生成引用
  → server/api_search.py              /api/search、/api/ask
```

块 ID 为 `<project>/<page_file>#<section>`；同一章节二次切分出多段时追加 `::part-N`。块元数据含 `project`、`feature`、`page_type`、`schema_version`、`section`、`title`、`refs`、`is_overview`。主核 `engine/db` 不在依赖图内（测试与代码里都没有 import）。

## 分支与选型（计划 §1.3 要求写明）

**向量库**：默认 `lancedb`（`0.41.0`，本机实测可装可跑）。另保留纯 NumPy + `.npy` 精确暴力检索后备，`WIKI_RAG_LANCEDB=0` 即切走；Windows 上装不上 lancedb 时就走这条。**不引入 chromadb**：依赖树过大，对百级页语料不划算。

**嵌入**：默认 `BAAI/bge-small-zh-v1.5`（离线加载，中文语料实测相关度明显优于英文 MiniLM）。加载失败（无模型缓存、无 torch、离线不下载）时，自动降级为纯 NumPy 的稳定 SHA-256 n-gram 特征哈希向量，并在 `/api/health` 返回 `embedding_backend=hash-fallback` + `embedding_warning`，前端顶栏显示「演示降级 · 哈希嵌入」。**这是演示用降级，不是语义嵌入**。换模型必须重建索引：向量维度或词表不同会让旧索引与查询维度不匹配，检索接口会返回明确的中文错误而不是静默出错。

## 测试

```powershell
python -m pytest tests -q
```

覆盖：切块元数据 / 短块合并 / 超长切分 / overview 标记、RRF 名次与 k 可配置、抽取式引用可回源与无结果回退、API 正常与错误路径、入库幂等、按库重建隔离。测试全部用哈希嵌入，不依赖模型缓存。

## 与 apps/ 既有风格的分歧

现有 `apps/`（`games` / `knowledge-hub` / `programmer-workbench` / `trip-log` / `study-hub`）零 pip 依赖、无 Python 后端。本项目是第一个：

1. **首个 pip 依赖应用**：`requirements.txt` 逐行固定版本，只含检索、服务、测试所需包（FastAPI + uvicorn + numpy + lancedb + pyarrow + sentence-transformers + pytest），没有引入与检索/服务无关的包。
2. **首个 Python HTTP 后端**：`server/` 提供 FastAPI 路由与静态文件托管。理由是混合检索、离线嵌入与索引持久化在纯静态前端里做不到；本应用独立于其他 app，也不接主核 `engine/db`。
3. **保留了既有前端的核心取向**：`web/` 仍是纯静态 HTML/CSS/JS —— 零构建、零 CDN、零外部字体、无框架，与其他 apps 一致。

## 目录

```text
server/  main.py（uvicorn 入口）、app.py、api_search.py、api_library.py、config.py
core/    chunker / embedder / bm25 / fusion / extractive / wiki_source
index/   build_index.py（IndexStore + 检索）、data/（产物，gitignore）
web/     index.html / app.js / style.css
tests/   test_chunker / test_fusion / test_extractive / test_api / test_ingest
tools/   ingest.py（全量/增量入库 CLI，跑真实门禁）
```

## 配置

全部从环境变量读，密钥不入库、不进代码。`.env.example` 只列键名与默认值；应用不解析 `.env` 文件，请由 shell 或进程管理器注入。
