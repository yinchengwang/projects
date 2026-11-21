> ⚠️ **退役说明**：本文引用的 `engine/src/db/`（PostgreSQL 风格 CMake 内核）已于 **2026-11-18 退役**，源码、公开头与测试全部删除；文中出现的 `engine/src/db/...`、`engine/include/db/...`、`engine/test/db/...` 路径均为**历史路径**，现已不存在。
> 当前唯一数据库内核是 `engine/db/`。

# vdb_cli 架构设计

## 1. 架构概览

```mermaid
flowchart TB
    subgraph 入口层
        A[命令行参数解析]
        B[vdb_cli_create<br/>创建 CLI 实例]
    end

    subgraph 命令分发层
        C[vdb_cli_exec<br/>命令分发器]
        D{命令类型判断}
    end

    subgraph 命令处理层
        E1[cmd_create<br/>创建集合]
        E2[cmd_drop<br/>删除集合]
        E3[cmd_list<br/>列出集合]
        E4[cmd_info<br/>集合信息]
        E5[cmd_insert<br/>插入向量]
        E6[cmd_query<br/>查询向量]
        E7[cmd_delete<br/>删除向量]
        E8[cmd_benchmark<br/>性能测试]
        E9[cmd_shell<br/>交互模式]
    end

    subgraph 输出格式层
        F1[VDB_OUTPUT_TABLE<br/>表格输出]
        F2[VDB_OUTPUT_JSON<br/>JSON 输出]
        F3[VDB_OUTPUT_SILENT<br/>静默模式]
    end

    subgraph 数据库层
        G[vdb_api.h<br/>数据库 API]
        H[vdb_handle_t<br/>数据库句柄]
    end

    A --> B
    B --> C
    C --> D
    D --> E1
    D --> E2
    D --> E3
    D --> E4
    D --> E5
    D --> E6
    D --> E7
    D --> E8
    D --> E9

    E1 --> F1
    E1 --> F2
    E1 --> F3
    E2 --> F1
    E2 --> F2
    E2 --> F3
    E3 --> F1
    E3 --> F2
    E3 --> F3
    E4 --> F1
    E4 --> F2
    E4 --> F3
    E5 --> F1
    E5 --> F2
    E5 --> F3
    E6 --> F1
    E6 --> F2
    E6 --> F3
    E7 --> F1
    E7 --> F2
    E7 --> F3
    E8 --> F1
    E8 --> F2
    E8 --> F3

    F1 --> G
    F2 --> G
    F3 --> G
    G --> H
```

## 2. 命令体系

```mermaid
classDiagram
    class vdb_cli_config_t {
        +const char data_dir
        +const char prompt
        +size_t history_size
        +bool echo
        +bool show_timing
        +bool json_output
        +bool color_output
    }

    class vdb_cli_t {
        +vdb_cli_config_t config
        +vdb_handle_t db
        +vdb_output_format_t output_format
        +char last_collection
    }

    class vdb_output_format_t {
        <<enumeration>>
        VDB_OUTPUT_TABLE
        VDB_OUTPUT_JSON
        VDB_OUTPUT_SILENT
    }

    class Command {
        <<interface>>
        +name: string
        +aliases: string[]
        +description: string
        +execute(cli, argc, argv) int
    }

    class cmd_create {
        +name: create
        +aliases: []
        +--collection: string
        +--dim: int
        +--index: string
        +--metric: string
    }

    class cmd_drop {
        +name: drop
        +aliases: []
        +--collection: string
        +--force: bool
    }

    class cmd_list {
        +name: list
        +aliases: ls
    }

    class cmd_info {
        +name: info
        +aliases: []
        +--collection: string
    }

    class cmd_insert {
        +name: insert
        +aliases: []
        +--collection: string
        +--vector: string
        +--file: string
        +--batch: bool
    }

    class cmd_query {
        +name: query
        +aliases: search
        +--collection: string
        +--vector: string
        +-k: int
    }

    class cmd_delete {
        +name: delete
        +aliases: del
        +--collection: string
        +--id: int64
    }

    class cmd_benchmark {
        +name: benchmark
        +aliases: bench
        +--collection: string
        +--mode: string
        +-n: int
    }

    class cmd_shell {
        +name: shell
        +aliases: repl
    }

    vdb_cli_config_t --> vdb_cli_t
    vdb_output_format_t --> vdb_cli_t
    Command <|-- cmd_create
    Command <|-- cmd_drop
    Command <|-- cmd_list
    Command <|-- cmd_info
    Command <|-- cmd_insert
    Command <|-- cmd_query
    Command <|-- cmd_delete
    Command <|-- cmd_benchmark
    Command <|-- cmd_shell
```

**命令列表：**

| 命令 | 别名 | 功能 | 核心参数 |
|------|------|------|----------|
| `create` | - | 创建集合 | `--collection`, `--dim`, `--index`, `--metric` |
| `drop` | - | 删除集合 | `--collection`, `--force` |
| `list` | `ls` | 列出所有集合 | - |
| `info` | - | 显示集合信息 | `--collection` |
| `insert` | - | 插入向量 | `--collection`, `--vector`, `--file` |
| `query` | `search` | 查询向量 | `--collection`, `--vector`, `-k` |
| `delete` | `del` | 删除向量 | `--collection`, `--id` |
| `benchmark` | `bench` | 性能测试 | `--collection`, `--mode`, `-n` |
| `shell` | `repl` | 交互式模式 | - |

## 3. 输出格式

```mermaid
flowchart TB
    subgraph 输入
        A[命令执行结果]
    end

    subgraph 格式判断
        B{output_format}
    end

    subgraph TABLE 输出
        C1[print_table_header]
        C2[print_table_row]
        C3[彩色文本输出]
        C4[对齐表格格式]
    end

    subgraph JSON 输出
        D1[print_json_start]
        D2[json_escape_string]
        D3[print_json_end]
        D4[结构化 JSON]
    end

    subgraph SILENT 输出
        E1[无输出]
        E2[仅返回状态码]
    end

    A --> B
    B -->|VDB_OUTPUT_TABLE| C1
    C1 --> C2
    C2 --> C3
    C3 --> C4
    B -->|VDB_OUTPUT_JSON| D1
    D1 --> D2
    D2 --> D3
    D3 --> D4
    B -->|VDB_OUTPUT_SILENT| E1
    E1 --> E2
```

**TABLE 输出示例：**

```
+------------------+------------------+------------------+
| 集合名称         | 向量数           | 维度             |
+------------------+------------------+------------------+
| test             | 1000             | 128              |
| myvec            | 500              | 256              |
+------------------+------------------+------------------+
共 2 个集合。
```

**JSON 输出示例：**

```json
{
  "success": true,
  "command": "list",
  "collections": ["test", "myvec"],
  "count": 2
}
```

**格式切换：**

```c
// 设置输出格式
void vdb_cli_set_output_format(vdb_cli_t *cli, vdb_output_format_t format);

// 获取当前格式
vdb_output_format_t vdb_cli_get_output_format(vdb_cli_t *cli);
```

## 4. 命令执行流程

### 4.1 单次命令执行

```mermaid
sequenceDiagram
    participant U as 用户
    participant M as main()
    participant C as vdb_cli_create
    participant E as vdb_cli_exec
    participant Cmd as 命令处理器
    participant V as vdb_api
    participant D as 数据库

    U->>M: vdb_cli create --collection test --dim 128
    M->>C: 创建 CLI 实例
    C->>V: vdb_open(data_dir)
    V-->>C: 返回数据库句柄
    C-->>M: 返回 CLI 实例
    M->>E: vdb_cli_exec(cli, "create", argc, argv)
    E->>E: 解析命令名
    E->>Cmd: cmd_create(cli, argc, argv)
    Cmd->>Cmd: 解析参数 (--collection, --dim)
    Cmd->>V: vdb_create_collection(db, name, &config)
    V->>D: 创建集合
    D-->>V: 成功
    V-->>Cmd: VDB_OK
    Cmd->>Cmd: 格式化输出
    Cmd-->>E: 返回状态码
    E-->>M: 返回状态码
    M->>M: vdb_cli_destroy
```

### 4.2 交互模式 (shell)

```mermaid
sequenceDiagram
    participant U as 用户
    participant S as cmd_shell
    participant L as read_line
    participant E as vdb_cli_exec
    participant Cmd as 命令处理器

    U->>S: vdb_cli shell
    S->>S: 打印欢迎信息
    loop 交互循环
        S->>L: read_line("vdb> ")
        L-->>S: 用户输入
        alt 内置命令
            S->>S: exit/quit/q → 退出
            S->>S: help/? → 打印帮助
            S->>S: clear/cls → 清屏
        else 外部命令
            S->>S: 解析参数 (strtok)
            S->>E: vdb_cli_exec(cli, cmd, argc, argv)
            E->>Cmd: 执行具体命令
            Cmd-->>E: 返回状态
            E-->>S: 返回状态
        end
    end
    S->>S: 用户输入 exit
    S-->>U: 退出交互模式
```

### 4.3 向量查询流程

```mermaid
sequenceDiagram
    participant U as 用户
    participant C as cmd_query
    participant P as parse_float_array
    participant V as vdb_api
    participant D as 数据库

    U->>C: query -c test --vector 1.0,2.0,3.0 -k 10
    C->>C: 解析参数
    C->>C: 复用 last_collection (如果未指定)
    C->>V: vdb_get_collection(db, name)
    V-->>C: 返回集合句柄
    C->>P: parse_float_array("1.0,2.0,3.0")
    P-->>C: 返回 float 数组 + dim
    C->>C: 构建 vdb_search_params_t
    C->>V: vdb_search(coll, vector, dim, &params)
    V->>D: 执行向量搜索
    D-->>V: 返回结果集
    V-->>C: 返回 vdb_results_t
    C->>C: 格式化输出 (TABLE/JSON)
    C->>V: vdb_results_free(results)
    C->>C: 更新 last_collection
```

## 5. 核心数据结构

### 5.1 CLI 配置

```c
typedef struct vdb_cli_config_s {
    const char *data_dir;        // 数据目录路径
    const char *prompt;          // 命令提示符
    size_t history_size;         // 历史记录条数
    bool echo;                   // 回显命令
    bool show_timing;            // 显示执行时间
    bool json_output;            // JSON 输出模式
    bool color_output;           // 彩色输出
} vdb_cli_config_t;

// 默认配置
#define VDB_CLI_DEFAULT_CONFIG { \
    .data_dir = "./vdb_data",   \
    .prompt = "vdb> ",          \
    .history_size = 100,        \
    .echo = true,               \
    .show_timing = true,        \
    .json_output = false,       \
    .color_output = true        \
}
```

### 5.2 CLI 实例

```c
struct vdb_cli_s {
    vdb_cli_config_t config;           // 配置
    vdb_handle_t *db;                  // 数据库句柄
    vdb_output_format_t output_format; // 输出格式
    char *last_collection;             // 上次使用的集合名
};
```

### 5.3 输出格式枚举

```c
typedef enum {
    VDB_OUTPUT_TABLE,   // 表格输出
    VDB_OUTPUT_JSON,    // JSON 输出
    VDB_OUTPUT_SILENT   // 静默模式
} vdb_output_format_t;
```

## 6. 关键代码位置

| 模块 | 文件路径 | 核心功能 |
|------|----------|----------|
| CLI 接口定义 | `engine/apps/vdb_cli/vdb_cli.h` | 配置结构体、上下文、输出格式、公共 API |
| CLI 实现 | `engine/apps/vdb_cli/vdb_cli.c` | 9 个命令处理函数、JSON/表格输出、命令分发 |
| 数据库 API | `engine/include/db/vdb_api.h` | vdb_open/close、集合管理、向量操作 |
| 日志模块 | `engine/include/db/log.h` | 日志输出 |

**vdb_cli.c 核心函数：**

| 函数名 | 行号 | 功能 |
|--------|------|------|
| `cmd_create` | 187-264 | 创建集合命令处理 |
| `cmd_drop` | 270-310 | 删除集合命令处理 |
| `cmd_list` | 316-381 | 列出集合命令处理 |
| `cmd_info` | 387-443 | 集合信息命令处理 |
| `cmd_insert` | 449-584 | 插入向量命令处理 |
| `cmd_query` | 590-701 | 查询向量命令处理 |
| `cmd_delete` | 707-761 | 删除向量命令处理 |
| `cmd_benchmark` | 767-882 | 性能测试命令处理 |
| `cmd_shell` | 901-955 | 交互式模式 |
| `vdb_cli_exec` | 1125-1159 | 命令分发器 |
| `vdb_cli_create` | 1086-1112 | 创建 CLI 实例 |
| `vdb_cli_destroy` | 1114-1119 | 销毁 CLI 实例 |
| `parse_float_array` | 82-112 | 解析逗号分隔的浮点数组 |
| `print_table_header/row` | 158-181 | 表格输出辅助函数 |
| `print_json_start/end` | 146-152 | JSON 输出辅助函数 |