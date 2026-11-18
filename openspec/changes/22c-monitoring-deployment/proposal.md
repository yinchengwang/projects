# Task 22-C: 监控 / 部署 / 复制（**已作废**）

**变更名称：** workbench 微服务监控与部署
**创建日期：** 2026-08-23
**状态：** ❌ **已作废（2026-11-18）** —— 目标载体已不存在，无法复核
**关联 spec：** `docs/superpowers/specs/2026-08-17-workbench-micro-service-design.md` §14 / §16

---

## 作废说明（2026-11-18）

本提案要交付的是 **C 语言 workbench 微服务**（`engineering/apps/workbench_server/`）
的监控、部署与复制基础设施。该目录已于 2026-10-23 的 `907301fa9`
（"删除 engineering/apps"）整棵移除，现在仓库里只有
`apps/programmer-workbench/`（原生微信小程序），那是另一套技术栈、另一套产物，
**不是本提案的目标载体**。

复核证据（2026-11-18）：

- 本目录只有 `proposal.md`，**没有** `tasks.md` / `design.md`
- 提案声称的 12 个交付物，在 `907301fa9^`（删目录前最后一个提交）与
  提案提交 `9fdbf12ea` 两处用 `git cat-file` 查都是 **MISSING** ——
  只有 `engineering/apps/workbench_server/src/workbench_metrics.{c,h}`
  曾真实存在，并随整目录删除
- 因此下文"当前状态"里那串 ✅ **在 git 历史中无法复核**

**为什么不重做**：目标载体（C 微服务）已被原生小程序取代，
监控/部署的需求形态完全不同（小程序没有进程、没有 /metrics 端点、
没有 docker-compose）。要在新载体上做，那是另一个提案。

**处置**：
- 本提案标记作废，**不归档进 `openspec/archived/`** ——
  归档意味着"已完成"，而它从未在当前仓库里完成过
- 若将来要为新载体做监控/部署，另开 `workbench-miniprogram-observability`
- 下文保留原文，作为"当时的意图"记录

---

## 背景

Phase 7 Task 22-A（多实例负载均衡）+ Task 22-B（JWT secret 旋转）已完成。当前 workbench 微服务架构已有：

- ✅ web-server + workbench-server 双进程
- ✅ nginx upstream 多实例负载均衡
- ✅ JWT 多密钥 + grace period + rotate-secret 端点
- ✅ SQLite WAL + busy_timeout

**缺监控 / 部署 / 复制基础设施：**

1. 无 metrics 暴露 — Prometheus 无法拉取指标
2. 无 Grafana dashboard — 运维无可视化
3. docker-compose 仅 MiniVecDB 单服务，无 nginx / workbench / web-server
4. 无 k8s manifest — 生产部署缺标准

---

## What Changes

### 必做项（12 文件）
**新建：**
- `engineering/apps/common/metrics.h` + `metrics.c` — Prometheus text exposition format 手写实现（~300 行，无第三方依赖）
- `engineering/apps/web/server/src/metrics_handler.c` + `.h` — `GET /metrics` HTTP handler
- `engineering/apps/workbench_server/src/workbench_metrics.c` + `.h` — workbench 专用指标（DB 慢查询 / card 操作）
- `engineering/docker-compose.workbench.yml` — 多服务部署（nginx + workbench-server + web-server + prometheus + grafana）
- `engineering/prometheus/prometheus.yml` — scrape config
- `engineering/grafana/provisioning/` — datasources + dashboards provisioning
- `engineering/grafana/dashboards/workbench.json` — 6 面板 dashboard
- `engineering/docs/workbench-deployment.md` — standalone / docker-compose / k8s 三模式部署文档
- `engineering/k8s/` — k8s manifest starter 模板

**修改：**
- `engineering/apps/web/server/src/main.c` — 注册 `GET /metrics` 端点 + metrics 中间件
- `engineering/apps/workbench_server/src/main.c` — 注册 `GET /metrics` + DB 埋点
- `scripts/workbench-acceptance.sh` — 新增 phase22c smoke（6 步验证）

---

## Non-Goals（明确范围）

- ❌ 不引入第三方 tracing（Jaeger / Zipkin）
- ❌ 不改鉴权 / rotate 逻辑（Task 22-B 已落地）
- ❌ 不动 workbench 业务路由（card_handler / notes_api 等）
- ❌ 不引入 OpenTelemetry SDK（保持轻量）

---

## 当前状态（⚠️ 以下是提案原文，**未经当前仓库复核**）

> 2026-11-18 复核结论：下面这串 ✅ 在 git 历史里找不到对应交付物（见顶部"作废说明"）。
> 原文保留，仅作意图记录；不要当成"已完成"引用。

- ~~✅ OPSX change 目录已建~~（目录在，但只有 proposal.md）
- ~~✅ 所有 12 个文件已实施完成~~（**12 个文件在 git 历史中均为 MISSING**）
- ~~✅ 编译通过（web-server + workbench-server）~~（载体已删）
- ~~✅ Workbench gtest 30/30 通过~~（载体已删）
- ~~✅ /metrics 端点已注册~~（载体已删）
- ~~✅ Prometheus metrics 手写实现~~
- ~~✅ docker-compose.workbench.yml~~
- ~~✅ prometheus.yml + grafana provisioning~~
- ~~✅ k8s manifest 模板~~
- ~~✅ workbench-deployment.md~~
- ~~✅ workbench-acceptance.sh phase22c smoke 验证~~
- **状态：❌ 已作废 —— 目标载体（C 微服务）已于 2026-10-23 删除**