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

## 构建接入状态（B1）

B1 已将 profile 真相源接入 Makefile：

- `MM` 默认为 `full`，未知 profile 会在解析阶段直接失败；
- 非 `full` profile 使用独立的 `build_mm_<profile>/` 对象目录；
- 每个 profile 都生成恒定的 `DB_HAS_*` 宏（当前 B1 全部为 `1`，真正裁剪在 B2 启用）；
- `make check` 会先运行 `tools/mm_profiles_test.sh`，再运行完整测试。

B1 的边界是先固定数据契约和对象目录隔离，不提前改变默认 `full` 的源文件集合。
这样 B2 启用源文件裁剪时，profile 清单、宏和构建目录已有独立验证。


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

Makefile 接入 `MM` 后，每个非 `full` profile 使用独立对象目录，避免不同 profile 的 `.o`
文件互相污染。`DB_HAS_*` 宏始终定义为 `0` 或 `1`，供后续组合根裁剪使用。
