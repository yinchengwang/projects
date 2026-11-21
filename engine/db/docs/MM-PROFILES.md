# MM profile 文件布局说明

`mm_profiles.mk` 是 MM 编译期裁剪的唯一真相源。它定义 8 个 profile、每个 profile
开启的可变模态、模态源文件清单，以及模态依赖闭包。

## 固定规则

- `relational` 始终编译。它横跨 storage、catalog、index、SPI，是数据库底座，不是可选插件。
- `full` = relational + 7 个可裁剪模态：blob、graph、vector、ft、geo、ts、doc。
- 其余 profile 是 relational 加一个可变模态，例如 `MM=vector`。
- `doc` 目前只有 JSON/JSONB 底层实现，尚未接入 `db_doc.c` 或 SQL 执行入口；它仍然纳入
  profile 机制，避免未来接入时重新设计裁剪边界。
- `gstore*.c` 是底层通用存储，不属于 graph；graph 与 vector 都可以依赖它。
- 源文件路径全部相对 `engine/db/`。

## Profile 清单

| Profile | 常驻关系模态 | 开启的可变模态 |
|---|---|---|
| `full` | 是 | blob、graph、vector、ft、geo、ts、doc |
| `relational` | 是 | 无 |
| `blob` | 是 | blob |
| `graph` | 是 | graph |
| `vector` | 是 | vector |
| `ft` | 是 | ft |
| `geo` | 是 | geo |
| `ts` | 是 | ts |
| `doc` | 是 | doc |

## 构建接入状态

### B1 已做（数据契约与对象目录隔离）

- `MM` 默认为 `full`，未知 profile 在 make 解析阶段直接失败；
- 非 `full` profile 使用独立的 `build_mm_<profile>/` 对象目录；
- 每个 profile 都生成恒定的 `DB_HAS_*` 宏；
- `make check` 先跑 `tools/mm_profiles_test.sh`，再跑完整测试。

### B2 已做（真正裁剪源文件）

- `SRCS_ALL` 収全量，`SRCS := $(filter-out $(MM_CROP_SRCS), $(SRCS_ALL))`，
  未激活模态的源文件**不再进编译**；
- `MM=full` 时 `MM_CROP_SRCS` 为空 ⇒ 与改动前**逐字等价**；
- `DB_HAS_*` 的值由闭包算出：开 = `1`、关 = `0`（恒定定义，可直接写 `#if DB_HAS_FT`）；
- 非 `full` profile 的可执行文件落在自己的对象目录
  （`build_mm_vector/db.exe`），不覆盖仓库根的 `db.exe`；
- `make bench` 只在 `MM=full` 下可用，其他 profile 会**立刻拒绝并说明原因**
  （那些基准各依赖对应模态，裁剪后必然缺符号）。

实测规模（Windows / MinGW）：

| Profile | 编译单元数 |
|---|---:|
| `full` | 144 |
| `relational` | 59 |

⚠️ **B2 阶段 `make MM=vector` 在链接期仍会失败** —— 这是预期的：
`main.c` 还直接调用所有模态入口（`handle_ft` / `handle_geo` / …），
那些 `#if` 守卫属于 B3。B2 的证据是**对象层面**的（见下），不是链接成功。

## 反向验证（证明裁剪真的生效）

```powershell
cd C:\code\projects\engine\db
mingw32-make -n MM=vector all          # ft 编译行必须为 0，vector 必须 > 0
mingw32-make MM=vector all             # B2 阶段预期链接失败（B3 修）
Get-ChildItem build_mm_vector/modalities/* -Directory
```

实测：`modalities/vector/` 有 8 个对象，`modalities/{ft,geo,ts,graph,doc}/` **全部为 0**；
顶层 `db_ft*.o`、`db_geo*.o`、`db_ts.o`、`db_graph*.o`、`repl_{ft,geo,ts}.o` 也全部为 0。

## 使用与校验

```sh
# 在 engine/db 下执行
mingw32-make MM=full
mingw32-make MM=vector
sh tools/mm_profiles_test.sh
```

`mm_profiles_test.sh` 会检查：

1. 9 个 profile 集合是否与预期一致；
2. `MM_FILES_<modality>` 中列出的文件是否真实存在；
3. 每个 `MM_DEP_<modality>` 是否引用了已知模态；
4. 故意改坏文件路径时测试是否会失败。
