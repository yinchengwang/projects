# Stage A0 只读盘点报告

盘点时间：2026-11-15（本次执行，只读）。未执行 fetch/pull、submodule init/update、add/commit/push、删除或移动。

## 一、自有仓

| 仓 | 工作树 | `.git` | HEAD / 分支 | origin HEAD 比对 | 工作区 |
|---|---|---|---|---|---|
| projects | `C:\code\projects` | 真目录 | `82426a54c0947a959e624f81a3293c12ddcb290f` / main | 本地 main == 远程 main | 17 个未跟踪文件：15 个 JSONB 源码/测试，另有本次设计文档与计划 |
| learning | `C:\code\StillEvolving\learning` | 指针：`gitdir: ../.git/modules/learning` | `f527d50289f09f46535a547752514da2275302b0` / main | 本地 main == 远程 main | 干净 |
| ai-docs | `C:\code\StillEvolving\ai-docs` | 指针：`gitdir: ../.git/modules/ai-docs` | `aa55ee63b248678ec20fce8bdad788426e0abb94` / main | 本地 main == 远程 main | 干净 |
| reference | `C:\code\reference` | 真目录 | `4ada98a6aaa4c1baac60939ce261b9fbffc2aa01` / main | 本地 main == 远程 main | 干净 |

远程比对使用 `git ls-remote <origin> refs/heads/<branch>` 直接比较 SHA；没有 fetch 或改写本地 remote-tracking refs。

## 二、projects 15 个 JSONB 文件

以下 15 个文件均为未跟踪源代码/测试，不是生成物；还未提交或推送：

- `engine/db/src/modalities/doc/json_ovf.c`
- `engine/db/src/modalities/doc/json_ovf.h`
- `engine/db/src/modalities/doc/json_parse.c`
- `engine/db/src/modalities/doc/json_parse.h`
- `engine/db/src/modalities/doc/json_path.c`
- `engine/db/src/modalities/doc/json_path.h`
- `engine/db/src/modalities/doc/json_scalar.c`
- `engine/db/src/modalities/doc/jsonb.c`
- `engine/db/src/modalities/doc/jsonb.h`
- `engine/db/src/modalities/doc/jsonb_get.c`
- `engine/db/test/unit/jsonb_test_common.h`
- `engine/db/test/unit/test_json_ovf.c`
- `engine/db/test/unit/test_json_path.c`
- `engine/db/test/unit/test_jsonb.c`
- `engine/db/test/unit/test_jsonb_crash.c`

此前已确认的集成边界：Makefile 的源码 glob 覆盖 `src/*/*/*.c`，但 JSONB 实现在 `src/modalities/doc/` 更深一层，主目标不会因此自动编译这些源文件；测试脚本会发现 `test_*.c`，但链接依赖已有 `build/*.o`。因此须先核实/重建对象文件，再运行完整构建、`check_deps.sh` 和 `test/run.sh`；截至本报告，测试尚未运行。

静态审查提示需关注：头文件对 JSONB 键二分查找的描述与实现线性扫描不一致；点号路径的允许字符文档与实现需核对；overflow 页写失败清理路径需核对。上述都需在提交前复核或明确记为非阻断差异，不能直接宣称测试通过。

## 三、嵌套子模块

### projects

| 路径 | gitfile | HEAD | 工作区 | origin |
|---|---|---|---|---|
| `engine/third_part/uthash` | `gitdir: ../../../.git/modules/engineering/third_part/uthash` | `a49bed0b4abb7dff16c73906dcdc8a9718d582d2` | 干净 | `https://gitee.com/mirrors/uthash.git` |
| `vendor/third_part/googletest` | `gitdir: ../../../.git/modules/third_part/googletest` | `f8d7d77c06936315286eb55f8de22cd23c188571` | 干净 | `https://github.com/google/googletest.git` |

Projects 的 gitlink pins 与上述 HEAD 一致。

### learning

| 路径 | gitfile | HEAD | 工作区 | origin |
|---|---|---|---|---|
| `third_part/googletest` | `gitdir: ../../../.git/modules/learning/modules/third_part/googletest` | `f8d7d77c06936315286eb55f8de22cd23c188571` | 干净 | `https://github.com/google/googletest.git` |

learning 的 gitlink pin 与该 HEAD 一致。

### reference

已初始化且干净的上游子模块：

- `reference/embedded/boltdb`：`fd01fc79c553a8e99d512a07e8e0c63d4a3ccfc5`，origin `https://github.com/boltdb/bolt.git`
- `reference/vector/faiss`：`02ea14372c9983f3eaa15698512180dc62ea234d`，origin `https://github.com/facebookresearch/faiss.git`
- 另有 **93 个未初始化**的上游子模块。本次未初始化它们；这些上游仓不属于四个自有仓，不应为本计划而递归拉取或推送。

## 四、父聚合仓与已确认处理决定

`StillEvolving`：HEAD `b0065c7f06d6a6796a036f15e33ff9f4facf47aa`，origin `https://github.com/yinchengwang/StillEvolving.git`，本地 main 与远程 main 相同。

父仓状态：

- `D projects`：projects 工作树已移到 `C:\code\projects` 的本地效果；该状态未提交，且不得用 destructive reset/checkout 恢复。
- `?? m5-verify/`：293 KB、38 个文件，包含 M5 实现/验证源码、测试、补丁脚本与 README。用户已确认整体迁到 `C:\code\m5-verify` 并在 code 根 `.gitignore` 忽略；迁移前核对清单和目标不存在，迁移后核验文件数与哈希；不提交、不删除。

code 聚合仓尚未在 `C:\code` 初始化；远程 `code` 仓的初始内容须在 A5 只读检查。用户已批准 projects 仓内总设计、实施计划、A0 审计报告三份文档单独精确提交并推送；它们必须与 JSONB 源码分开提交。

## 五、Gate A0 结论

- 四个自有仓当前都没有本地 ahead commits，且 HEAD 与远程 main 相同。
- 上述 15 个 JSONB 文件已通过完整构建、依赖方向和 90 个测试程序；仍未提交/推送，不能声称已远程备份。
- projects 内三份设计/计划/盘点文档须独立提交推送，不能和 JSONB 代码合并提交；用户已授权该独立提交与推送。
- `m5-verify/` 的迁移到 code 根及 ignore 方式已由用户确认；仍需按文件清单执行迁移并核验哈希。
- A0 当前仍未通过：待三份文档推送并验证、JSONB 按白名单提交并推送验证、m5-verify 迁移核验完成。上述动作未完成前不得开始 A3 gitdir 手术或删除 StillEvolving。

## 六、A1 验证补充（2026-11-15）

按用户批准执行 JSONB 验证门禁，结果：

1. `export PATH="/c/mingw64/bin:$PATH" && cd /c/code/projects/engine/db && mingw32-make clean && mingw32-make all`：退出码 0；`-Wall -Wextra -Werror` 严格编译与链接完成；链接行包含 `json_ovf.o`、`json_parse.o`、`json_path.o`、`json_scalar.o`、`jsonb.o`、`jsonb_get.o`。
2. `sh tools/check_deps.sh`：退出码 0，输出 `✓ 依赖方向正确`。
3. `sh test/run.sh`：退出码 0；发现并执行 60 个单元测试 + 30 个集成测试，共 90 个测试程序，输出 117 个 `[PASS]` 标记、0 个 `[FAIL]` / `[COMPILE FAIL]`；新增 `test_json_ovf`、`test_json_path`、`test_jsonb`、`test_jsonb_crash` 均通过。
4. 再运行项目组合门禁 `mingw32-make check`：退出码 0；依赖方向正确，完整测试输出 117 个 `[PASS]`，0 个失败标记。
5. `git diff --check` 退出码 0；构建/测试产物均在 `engine/db/.gitignore` 覆盖的 `build/` 与 `test/tmp/`；`git status` 只显示三份计划文档与 15 个指定 JSONB 文件，未跟踪文件没有变化或额外测试数据库进入版本控制。

审查结论：没有确认到必然阻断 build/test 的问题。仍需记录的非阻断风险：JSONPath 下标长输入的整数累积溢出、裸 UTF-8 校验不足、对象多次 realloc 失败时的状态一致性、外置页写失败回滚与两趟读取并发一致性。提交 JSONB 代码前应决定是否修复/记录这些风险；不得因门禁全绿就声称这些边界已解决。

## 七、A1 风险处理结果（2026-11-16 补记）

四项风险已逐项处理并提交在 projects `78feaa222`：

| 风险 | 结论 |
|---|---|
| JSONPath 下标 u64 回绕 | **证伪**。用探针直接调用 `json_path_parse`：`$[4294967295]` 接受（index=4294967295）；`$[4294967296]`、`$[18446744073709551616]`、`$[99999999999999999999]` 均立即返回 `E_INVAL`「下标太大」。原因是每读一位就检查 `> 0xFFFFFFFF`，在 u64 回绕前退出。保留三条断言为边界回归，不改生产代码。 |
| 裸 UTF-8 未校验 | **已修（TDD）**。新增 7 类非法字节用例（含 3/4 字节截断），旧实现 10 个断言全红且产出树；新增 `utf8_seq_len()` 后转绿，合法中文/emoji/代理对不受影响。`json_parse.h` 合同同步改为严格拒绝。 |
| `jsonb_obj_set` realloc 部分失败 | **复核为非缺陷**。`count` 失败时不变、成员搬移未发生，前 `count` 项始终有效可读/编码/释放；只缺强容量回滚，无逻辑或内存安全缺陷。未新增 allocator seam。 |
| 页链写回滚 | **补真实可达测试**。用 `nframes=1` 真实 Pager 触发第二页 `pager_alloc` 失败：断言返回错误、`ext_key` 为 NIL、已分配页进入空闲栈、之后同池可再写一页。首次运行真实变红（两条断言），修正断言口径后转绿。`pager_unpin` 的 ownership 违规按用户决策定义为不可恢复 API misuse，不伪造注入。 |
| 页链两趟读取并发 | **合同化**。采纳用户选择的「调用方串行化」；`json_ovf.h` 明确 read/write/free 不可并发、free 前需无读者。当前无 SQL 主路径调用点。 |

验证证据（2026-11-16 复跑）：`mingw32-make clean && mingw32-make all` 退出码 0 零 error；`sh tools/check_deps.sh` 输出 `✓ 依赖方向正确`；`sh test/run.sh` 退出码 0，117 个 `[PASS]`、0 个失败标记。

## 八、Stage A 收尾状态（2026-11-16）

已完成并推送：

| 项 | 结果 |
|---|---|
| 四份自有仓代码/README 推送 | projects `3cacdfeac`、learning `84c400d`、ai-docs `7b08d23`、reference `3d2ad47`，均与各自远端 main 一致，工作区干净 |
| code 聚合仓 | `6e9f0bf`，四个直接 submodule 与远端一致，工作区干净 |
| m5-verify 保全 | 已迁至 `C:\code\m5-verify`，38 文件 / 204,968 字节 / manifest sha256 `8a4d87fb…` 与迁移前一致，被 code `.gitignore` 忽略 |
| docs 合并 | `StillEvolving/docs` 22 个文件中 21 个逐字节相同，`architecture.md` 为有意的拓扑改写；旧镜像 README 已由 code 权威 `docs/README.md` 取代 |
| `.superpowers` 合并 | 192 个未跟踪文件全部并入 code 侧 ignored 目录，逐一核验无缺失、无差异 |
| learning/ai-docs gitdir 脱钩 | 已完成并验证：learning 与嵌套 GoogleTest、ai-docs 的 toplevel/HEAD/status 均正常 |
| 旧父仓删除 | `C:\code\StillEvolving` 已删除（9.1 MB）；GitHub 上的仓与历史未动 |
| 脚本门禁 | `check_consistency.py` 退出码 0（仅 6 条已知 clone_url 分歧告警）；Python 单测 38 项通过；`test_fetch_reference.sh` 37 条通过 |
| `update=none` 行为 | **沙盒实测**：裸 `git submodule update --init` 打印 `Skipping submodule 'child'` 并保持未初始化；`-c submodule.<name>.update=checkout` 才初始化且不向下递归 |

**未完成项（阻塞 Stage A 完整验收）：**

- 完整 clean clone 的 submodule 初始化未验证通过。两次尝试均在 `git submodule update --init --depth 1 projects` 处因本机网络超时被终止（10 分钟上限），`projects` 目录保持为空。因此不能声称「干净 clone 能拉到四个直接 submodule」，只验证到「clone code 仓本身成功、四个 gitlink 与 `.gitmodules` 正确、reference 被正确跳过」。
- 该未完成项是环境/网络限制，不是配置缺陷的证据；但按计划 Gate，Stage A 尚不能标为完全通过。


