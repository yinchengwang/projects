# AlgorithmPractice

C/C++ 算法与数据结构练习项目。CMake 3.20+、C11、C++17，无运行时依赖。

## 构建

### 工程轨道（默认）

```bash
cmake -B build/engine -S engine -DBUILD_TESTING=ON
cmake --build build/engine --parallel 4
ctest --test-dir build/engine --output-on-failure
ctest --test-dir build/engine -R <name> --output-on-failure  # 单个测试
```

### 根入口

```bash
cmake -B build/root -S . -DENGINEERING_BUILD=ON
cmake --build build/root --parallel 4
```

> 学习轨（`learning/`）已切出为独立仓 `yinchengwang/learning`，其构建命令请在该仓内查阅。

`all_tests` 是 `add_custom_target` 包了一层 `ctest`，不是 CTest 原生概念。

## 编译与测试产物规范

- 编译产物统一进入 `build/<项目>/`（如 `build/engine`、`build/root`）
- 测试日志、覆盖率、临时数据库、运行日志等进入 `test-results/<项目或轨道>/`
- **测试二进制不再输出到源码目录**

## 测试结构

GoogleTest vendored 在 `vendor/third_part/googletest/`，由 `engine/CMakeLists.txt` 自动包含，无需系统安装。

测试可执行文件命名（不规则）：
- `self_made_test`、`self_made_cpp_test`、`leet_code_test`、`interview_test`
- `kmeans_gtest` 等位于 `engine/test/algo/`、`engine/test/vector_index/` 下
- `all_in_one_test` —— 合并 self_made + self_made_cpp + leetcode + interview 的单一二进制

## 新增模块

- 新模块用 `engine/cmake/ProjectUtils.cmake`：`add_project_test()`、`add_project_library()`
- 老模块手写 CMakeLists，两种都接受

## 库

- `self_made` — C 数据结构（list、queue、tree、str、common）
- `self_made_cpp` — C++ 数据结构
- `algo` — 算法
- `leetcode` — LeetCode 题解（C 在 `c/`、C++ 在 `cpp/`）
- `interview` — 面试题
- `index` — 向量索引（hnsw、ivf、diskann、BM25 等）

## 代码风格

clang-format，LLVM，`ColumnLimit: 120`，`IndentWidth: 4`。

clang-format **不递归**，需配合 glob：
```bash
clang-format -i $(git ls-files '*.c' '*.cpp' '*.h' '*.hpp')
```

## 编译器标志

- MSVC：`/W4 /WX`
- GCC/Clang：`-Wall -Wextra -Wpedantic -Werror -Wno-sign-compare`
- Debug：`-O0 -g3`；Release：`-O2 -DNDEBUG`

## 前端说明（已合并）

前端已从微信小程序重写为**本机 Web 应用**（2026-11-26 完成合并）。

原先的 4 个微信小程序（programmer-workbench / knowledge-hub / trip-log / games）已整体
移植为原生 Web 页面，合并进 `apps/web/`：

| 模块 | 路径 | 页面数 |
|---|---|---|
| 程序员工作台 | `/workbench/` | 18 |
| 知识中心 | `/knowledge/` | 17 |
| 足迹 | `/trip/` | 3 |
| 游戏 | `/games/` | 5 |

`apps/study-hub/`（学习中枢，5174）与 `apps/wiki-rag/`（Wiki 检索，8000）是**独立进程**，
保持原地不动 —— 它们的前端硬编码了绝对路径，挂到子路径下会失效。

```bash
node apps/web/server/main.js                    # 合并应用，5173
PORT=5174 node apps/study-hub/server/main.js    # 学习中枢，5174
cd apps/wiki-rag && python -m server.main       # Wiki 检索，8000
```

合并应用零 npm 依赖、零构建（原生 ES2020 + DOM + Canvas，浏览器侧由
`apps/web/web/cjs-loader.js` 加载 CommonJS 模块）。测试：`cd apps/web && node tests/run-all.js`。
详见 [`apps/README.md`](apps/README.md)。

## 开发守则

### 先想清楚再动手

- 明确假设，不确定时询问而非猜测。
- 存在歧义时，列出多种解释，不默默选定单一方案。
- 如果任务有明显更简单的做法，直接指出优化思路。
- 发现代码矛盾、逻辑不一致时及时暂停，请求信息澄清。

### 简洁优先

- 用最少的代码解决问题，拒绝冗余实现。
- 不为一次性需求创建抽象层、复杂架构。
- 不盲目增加扩展性、可配置性，应对"未来可能用到"的场景。
- 若代码可大幅精简，主动重写优化。
- 校验标准：以资深工程师视角判断，代码若过于复杂，立即简化。

### 精准修改

- 仅修改与当前任务直接相关的代码内容。
- 不顺手优化相邻代码、注释、排版格式。
- 不重构原本可以正常运行的代码模块。
- 严格匹配项目现有代码风格，保留原有编码习惯。
- 因本次修改产生的无效导入、废弃变量，可直接删除。
- 发现项目中原有的死代码、冗余内容，仅做文字提醒，不擅自删除。

### 目标驱动执行

- 执行任务前，定义清晰、可落地的成功标准。
- 将"修复 Bug"转化为：编写用例复现问题，再调试至用例正常通过。
- 将"新增校验功能"转化为：针对异常输入编写测试用例，保证全部通过。
- 将"代码重构"转化为：完成重构后，确保原有所有测试用例正常运行。
- 多步骤复杂任务，先输出简短执行计划，同时标注每一步的验证方式。
