# Engineering Track

工程轨道 —— α 工程作品集。多模态数据库内核、SDK、CI/CD、Docker。

## 数据库主线

本轨道内的**唯一长期维护的数据库内核**是 [`db/`](db/)（从零手写的独立多模态内核，自带 Makefile、L0–L4 分层依赖门禁与独立测试协议）：

- 构建与测试走 `db/` 自带的 MinGW Make + shell 测试（见 `db/README.md`），**不入根工程 CMake 图**，这是有意的例外
- 严格 `-Werror` 零警告门禁，改完必跑 `sh tools/check_deps.sh`
- 主核的公开 API 是 `db/include/db.h`

原先并列的 `src/db/`（PostgreSQL 风格 CMake 工程内核）已于 **2026-11-18 退役** ——
源码、公开头、测试、CMake 接线（含 `cmake/ModalityProfiles.cmake` / `MultimodalConfig.cmake`）
全部删除。**本轨道现在只有一个数据库内核**：`db/`。

嵌入式 SDK（`src/sdk/`）是另一条独立线：不链接主核，自己也只依赖 SQLite 一个外部库。

## 构建与测试

> 本节的 CMake 命令覆盖 `src/` `tools/` `test/` 等子工程；**主数据库 `db/` 不在该图内**，它的构建与测试命令见 `db/README.md`。

```bash
# Configure & build
cmake -B ../build/engineering -S . -DBUILD_TESTING=ON
cmake --build ../build/engineering --parallel 4

# Run tests
ctest --test-dir ../build/engineering --output-on-failure
ctest --test-dir ../build/engineering -R <name> --output-on-failure

# Coverage (GCC/lcov)
cmake -B ../build/engineering -S . -DENABLE_COVERAGE=ON
```

```bash
# 主数据库内核（engine/db）—— 需 MinGW + Git Bash
export PATH="/c/mingw64/bin:$PATH"
cd db
mingw32-make clean && mingw32-make all   # -Wall -Wextra -Werror，零警告
sh tools/check_deps.sh                   # L0–L4 依赖方向门禁
sh test/run.sh                           # unit + integration
mingw32-make check                       # 全套
```

编译产物输出到 `build/engine/`，测试/运行产物输出到 `test-results/engine/`。

## Canonical 内容

| 目录 | 说明 |
|---|---|
| `src/` | 库代码（algo-prod/, cpp/, ds/, redis/, sdk/） |
| `include/` | 公共头文件 |
| `test/` | gtest 测试 |
| `sdk/` | 多语言 SDK |
| `tools/` | 工程专用工具 |
| `cmake/` | CMake 工具（ProjectUtils.cmake） |
| `test_data/` | 测试夹具 |

## 资产归属

`engine/` 是以下所有内容的唯一 canonical 位置：
- 顶层 `sdk/`、Docker 文件——已迁入
- RAG 系统——随冻结轨一并退役（原在 `engine/src/db/executor/rag/`）
- 根目录 `include/` 中工程头文件——已审计归入 `engine/include/`
- 根目录 `test_data/` 中复用测试夹具——已迁入
- 工程专用工具——从根 `tools/` 拆分迁入 `engine/tools/`

不可引用 `learning/` 任何路径（该目录已切出为独立仓 `yinchengwang/learning`）。

## 微信小程序归属

微信小程序**不属于** engineering track。

所有微信小程序统一存放在仓库根的 `../wechat-miniprogram/`，每个小程序一个子目录：

| 项目 | 目录 | 技术栈 |
|---|---|---|
| 足迹（游玩记录） | `apps/trip-log/` | 原生小程序 |
| DevBench（程序员工作台） | `apps/programmer-workbench/` | Taro 3.6 + React 18 + TS |

详见 `../wechat-miniprogram/README.md`。