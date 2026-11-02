# Engineering Track

工程轨道 —— α 工程作品集。PG 风格存储引擎、MiniVecDB、RAG、SDK、CI/CD、Docker。

## 构建与测试

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

编译产物输出到 `build/engine/`，测试/运行产物输出到 `test-results/engine/`。

## Canonical 内容

| 目录 | 说明 |
|---|---|
| `src/` | 库代码（db/, algo-prod/, cpp/, ds/, redis/, sdk/） |
| `include/` | 公共头文件 |
| `test/` | gtest 测试 |
| `sdk/` | 多语言 SDK |
| `tools/` | 工程专用工具 |
| `cmake/` | CMake 工具（ProjectUtils.cmake） |
| `test_data/` | 测试夹具 |

## 资产归属

`engine/` 是以下所有内容的唯一 canonical 位置：
- 顶层 `sdk/`、Docker 文件——已迁入
- RAG 系统——不在 `engine/rag/`，而在 `engine/src/db/executor/rag/`（随 db 执行器）
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