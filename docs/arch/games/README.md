# Games 游戏子系统架构设计

> ⚠️ **历史文档 · 原实现已删除**
>
> 本文描述的实现位于 `engine/apps/`，该目录已于 2026-10-23 整目录删除。
> 本文档保留作为设计参考；如需找回旧实现，可从 git 历史恢复。
> Web 端现实现见 `apps/web/` 的 `/games/` 模块

本文档描述 Games 游戏子系统的整体架构、核心组件和关键流程。

## 1. 子系统架构概览

Games 子系统采用「游戏中心 + 独立游戏 + 共享组件」的三层架构。

```mermaid
flowchart TB
    subgraph 游戏中心["游戏中心 (mini_program)"]
        MC[main.c<br/>入口程序]
        MENU[menu.h/c<br/>菜单系统]
    end

    subgraph 共享组件["共享组件 (common)"]
        TERM[terminal.h/c<br/>跨平台终端适配]
        MENU --> TERM
    end

    subgraph 贪吃蛇["贪吃蛇 (snake)"]
        SNAKE_H[snake.h]
        SNAKE_C[snake.c<br/>游戏逻辑]
        SNAKE_M[main.c<br/>难度选择+主循环]
    end

    subgraph 2048["2048 (2048)"]
        G2048_H[2048.h]
        G2048_C[2048.c<br/>滑动合并逻辑]
        G2048_M[main.c<br/>难度选择+主循环]
    end

    subgraph 数独["数独 (sudoku)"]
        SUDOKU_H[sudoku.h]
        SUDOKU_CPP[sudoku.cpp<br/>生成/求解算法]
        SUDOKU_M[main.cpp<br/>难度选择+主循环]
    end

    MC --> MENU
    MC --> GAME_LAUNCH[launch_snake<br/>launch_sudoku<br/>launch_2048]

    GAME_LAUNCH --> SNAKE_M
    GAME_LAUNCH --> G2048_M
    GAME_LAUNCH --> SUDOKU_M

    SNAKE_M --> SNAKE_C
    G2048_M --> G2048_C
    SUDOKU_M --> SUDOKU_CPP
```

**设计要点：**

- **游戏中心**：统一入口，通过 `system()` 启动独立游戏进程
- **独立游戏**：每个游戏自成一个可执行文件，独立运行
- **共享组件**：`menu` 和 `terminal` 提供通用能力，被游戏中心和各游戏复用

---

## 2. 游戏中心

游戏中心是用户入口，负责游戏选择和启动。

```mermaid
classDiagram
    class GameEntry {
        +const char *name
        +void (*launch)()
    }

    class Menu {
        +menu_main_select(games, count) int
        +menu_difficulty_select(title, options, count) int
        +menu_show_banner(title)
        +menu_wait_exit()
    }

    class Terminal {
        +terminal_init() TermResult
        +terminal_restore() TermResult
        +terminal_getch() int
        +terminal_kbhit() int
        +terminal_clear()
        +terminal_move_cursor(row, col)
    }

    Menu --> Terminal : 依赖

    note for GameEntry "name: 显示名称如 '1. 贪吃蛇'\nlaunch: 启动函数指针"
```

**启动流程：**

```mermaid
sequenceDiagram
    participant User
    participant Main as main.c
    participant Menu
    participant Terminal
    participant Game as 游戏进程

    Main->>Terminal: terminal_set_utf8()
    loop 游戏选择循环
        Main->>Menu: menu_main_select(GAMES, 3)
        Menu->>Terminal: terminal_clear()
        Menu->>Terminal: terminal_getch()
        Terminal-->>Menu: 按键码
        Menu-->>Main: 游戏索引或 -1
        alt 选择了游戏
            Main->>Terminal: terminal_restore()
            Main->>Game: system("游戏路径")
            Game-->>Main: 游戏结束
            Main->>Terminal: terminal_set_utf8()
        else 退出
            Main->>User: 显示再见
        end
    end
```

---

## 3. 贪吃蛇游戏

### 3.1 核心数据结构

```mermaid
classDiagram
    class Point {
        +int x
        +int y
    }

    class GameState {
        +Point snake[WIDTH*HEIGHT]
        +int snake_len
        +Point food
        +enum Dir dir
        +enum Dir next_dir
        +int score
        +int speed
        +int base_speed
        +int diff
        +int game_over
        +int snake_growth_pending
        +Point prev_tail
        +int prev_snake_len
        +int first_render
    }

    class Dir {
        <<enumeration>>
        DIR_UP
        DIR_DOWN
        DIR_LEFT
        DIR_RIGHT
    }

    class Diff {
        <<enumeration>>
        DIFF_EASY
        DIFF_HARD
        DIFF_HELL
    }

    GameState "1" *-- "5000" Point : snake数组
    GameState --> Dir : 方向
    GameState --> Diff : 难度
```

### 3.2 游戏循环

```mermaid
flowchart TB
    START([游戏开始]) --> INIT[init_game]
    INIT --> SPAWN[spawn_food]
    SPAWN --> LOOP{game_over?}

    LOOP -->|否| INPUT{有按键?}
    INPUT -->|是| PROCESS[处理输入<br/>更新 next_dir]
    INPUT -->|否| UPDATE[update]

    PROCESS --> UPDATE
    UPDATE --> RENDER[render]
    RENDER --> SLEEP[sleep_ms]

    SLEEP --> LOOP

    LOOP -->|是| END([游戏结束])

    subgraph 输入处理
        PROCESS --> KBHIT[排空缓冲区<br/>保留最后有效方向]
        KBHIT --> SPEED_BOOST{方向相同?}
        SPEED_BOOST -->|是| BOOST[速度翻倍]
        SPEED_BOOST -->|否| NORMAL[正常速度]
    end

    subgraph 更新逻辑
        UPDATE --> DIR_CHECK{方向有效?}
        DIR_CHECK -->|是| MOVE[移动蛇头]
        DIR_CHECK -->|否| KEEP[保持原方向]
        MOVE --> COLLISION{碰撞检测}
        COLLISION -->|撞墙/自身| GAME_OVER[game_over = 1]
        COLLISION -->|吃食物| GROW[蛇身增长<br/>得分+5<br/>生成新食物]
        COLLISION -->|正常移动| SLIDE[身体前移]
    end
```

### 3.3 玩家操作响应

```mermaid
sequenceDiagram
    participant User
    participant Main as main.c
    participant Snake as snake.c
    participant Render as render()

    User->>Main: 按下 W/A/S/D 或方向键
    Main->>Main: my_kbhit() 检测按键
    Main->>Main: my_getch() 获取按键码
    Main->>Main: 排空缓冲区，保留最后有效方向

    alt WASD 或方向键
        Main->>Snake: next_dir = DIR_UP/DOWN/LEFT/RIGHT
    else Q 键
        Main->>Snake: game_over = 1
    end

    Main->>Snake: update()
    Snake->>Snake: 方向验证（不能反向）
    Snake->>Snake: 移动蛇头
    Snake->>Snake: 碰撞检测
    Snake->>Snake: 更新速度（得分递增）

    Main->>Render: render()
    Render->>Render: 首次渲染清屏
    Render->>Render: 增量更新蛇身和食物
    Render->>Render: 更新状态栏（得分/长度/速度）
```

---

## 4. 2048 游戏

### 4.1 核心数据结构

```mermaid
classDiagram
    class Game2048 {
        +int board[4][4]
        +int score
        +int best_score
        +bool moved
        +bool game_over
        +bool won
        +bool keep_going
    }

    class Tile {
        <<note>>
        0 = 空格
        2,4,8,...,2048 = 方块值
    }

    Game2048 *-- Tile : 棋盘格子
```

### 4.2 移动合并逻辑

```mermaid
flowchart TB
    START([玩家移动]) --> DIR{移动方向}

    DIR -->|左| LEFT[move_left]
    DIR -->|右| RIGHT[move_right]
    DIR -->|上| UP[move_up]
    DIR -->|下| DOWN[move_down]

    RIGHT --> FLIP_H[水平翻转]
    FLIP_H --> LEFT
    UP --> ROTATE_CCW[逆时针旋转 90°]
    ROTATE_CCW --> LEFT
    DOWN --> ROTATE_CW[顺时针旋转 90°]
    ROTATE_CW --> LEFT

    LEFT --> SLIDE[slide_row 每行处理]

    subgraph 行滑动算法
        SLIDE --> COMPRESS1[压缩：移除 0，左移]
        COMPRESS1 --> MERGE[合并：相邻相同值×2]
        MERGE --> COMPRESS2[再压缩：填补空位]
    end

    SLIDE --> CHANGED{有变化?}
    CHANGED -->|是| MOVED[moved = true<br/>score += points]
    CHANGED -->|否| SKIP[保持 moved = false]

    LEFT --> RESTORE{需要恢复变换?}
    RESTORE -->|右| FLIP_H_BACK[水平翻转恢复]
    RESTORE -->|上| ROTATE_CW_BACK[顺时针旋转恢复]
    RESTORE -->|下| ROTATE_CCW_BACK[逆时针旋转恢复]
    RESTORE -->|左| CHECK

    MOVED --> SPAWN[spawn_tile 生成新块]
    SPAWN --> CHECK
    SKIP --> CHECK

    CHECK --> CAN_MOVE{能继续移动?}
    CAN_MOVE -->|否| GAME_OVER[game_over = true]
    CAN_MOVE -->|是| LOOP([等待下一次输入])
```

**核心技巧：** 通过旋转/翻转变换，所有方向的移动都复用 `move_left` 算法。

---

## 5. 数独游戏

### 5.1 核心数据结构

```mermaid
classDiagram
    class Cell {
        +int value
        +bool is_given
        +bool is_conflict
    }

    class GameState {
        +Cell board[9][9]
        +Cell solution[9][9]
        +int cursor_row
        +int cursor_col
        +int difficulty
        +int empty_count
        +bool game_over
    }

    class DifficultyConfig {
        +const char *name
        +int holes
    }

    GameState "1" *-- "81" Cell : 玩家棋盘
    GameState "1" *-- "81" Cell : 完整解
    GameState --> DifficultyConfig : 难度配置
```

### 5.2 题目生成与求解流程

```mermaid
flowchart TB
    START([生成题目]) --> FULL[generate_full_board<br/>生成完整终盘]

    subgraph 终盘生成
        FULL --> CLEAR[清空棋盘]
        CLEAR --> RECURSE[fill_board_recursive]
        RECURSE --> SHUFFLE[Fisher-Yates 洗牌候选数]
        SHUFFLE --> TRY[尝试 1-9]
        TRY --> VALID{is_valid_placement?}
        VALID -->|是| PLACE[填入数字]
        VALID -->|否| NEXT[尝试下一个]
        PLACE --> NEXT_ROW{填完一行?}
        NEXT_ROW -->|是| RECURSE_NEXT[递归下一行]
        NEXT_ROW -->|否| NEXT_COL[递归下一列]
        RECURSE_NEXT --> SUCCESS{成功?}
        NEXT_COL --> SUCCESS
        SUCCESS -->|是| DONE([终盘完成])
        SUCCESS -->|否| BACKTRACK[回溯：清空当前格]
        BACKTRACK --> NEXT
    end

    DONE --> COPY[复制到玩家棋盘]
    COPY --> HOLES[随机挖 holes 个洞]

    subgraph 挖洞法
        HOLES --> POS_SHUFFLE[Fisher-Yates 打乱坐标]
        POS_SHUFFLE --> DIG_LOOP{挖够了吗?}
        DIG_LOOP -->|否| PICK[取出一个坐标]
        PICK --> BACKUP[备份当前值]
        BACKUP --> CLEAR_CELL[清空格子]
        CLEAR_CELL --> COUNT[count_solutions]
        COUNT --> UNIQUE{唯一解?}
        UNIQUE -->|是| KEEP_HOLE[保留挖洞<br/>is_given = false]
        UNIQUE -->|否| RESTORE[恢复原值]
        KEEP_HOLE --> DIG_LOOP
        RESTORE --> DIG_LOOP
        DIG_LOOP -->|是| PUZZLE([题目生成完成])
    end
```

### 5.3 玩家操作流程

```mermaid
sequenceDiagram
    participant User
    participant Main as main.cpp
    participant Sudoku as sudoku.cpp
    participant Render as render()

    User->>Main: 选择难度
    Main->>Sudoku: init_game(state, difficulty)
    Sudoku->>Sudoku: generate_puzzle(state, holes)

    loop 游戏循环
        User->>Main: 方向键移动光标
        Main->>Main: 更新 cursor_row/cursor_col

        User->>Main: 数字键 1-9
        Main->>Sudoku: place_number(state, num)
        Sudoku->>Sudoku: 检查 is_given（给定格不可改）
        Sudoku->>Sudoku: 更新 empty_count
        Sudoku->>Sudoku: update_conflicts(state)
        Sudoku->>Sudoku: check_win(state)

        User->>Main: 0/Del/Backspace
        Main->>Sudoku: erase_cell(state)
        Sudoku->>Sudoku: place_number(state, 0)

        Main->>Render: render(state)
        Render->>Render: 渲染棋盘（冲突格子红色）
        Render->>Render: 渲染状态栏
    end

    Sudoku-->>Main: game_over = true
    Main->>Main: 显示胜利信息
```

---

## 6. 共享组件

### 6.1 Menu 菜单系统

```mermaid
classDiagram
    class Menu {
        +menu_main_select(games, count) int
        +menu_difficulty_select(title, options, count) int
        +menu_show_banner(title) void
        +menu_show_line(ch, width) void
        +menu_wait_exit() void
    }

    class GameEntry {
        +const char *name
        +void (*launch)()
    }

    Menu ..> GameEntry : 使用

    note for Menu "menu_main_select: 显示游戏列表并获取选择\nmenu_difficulty_select: 显示难度菜单\nmenu_show_banner: 显示标题横幅\nmenu_show_line: 显示分隔线\nmenu_wait_exit: 等待用户退出"
```

### 6.2 Terminal 终端适配器

```mermaid
classDiagram
    class Terminal {
        +terminal_init() TermResult
        +terminal_restore() TermResult
        +terminal_getch() int
        +terminal_kbhit() int
        +terminal_sleep_ms(ms) void
        +terminal_hide_cursor() void
        +terminal_show_cursor() void
        +terminal_clear() void
        +terminal_move_cursor(row, col) void
        +terminal_set_utf8() void
    }

    class TermResult {
        <<enumeration>>
        TERM_OK
        TERM_ERR_INIT_FAILED
        TERM_ERR_RESTORE_FAILED
    }

    Terminal --> TermResult : 返回类型

    note for Terminal "跨平台封装：Windows (_getch, _kbhit, Sleep)\nPOSIX (termios, select, usleep)\nANSI 转义序列控制光标和颜色"
```

**平台适配矩阵：**

| 功能 | Windows | POSIX |
|------|---------|-------|
| UTF-8 编码 | SetConsoleOutputCP(65001) | setlocale(LC_ALL, "en_US.UTF-8") |
| 原始输入 | SetConsoleMode(ENABLE_LINE_INPUT off) | tcsetattr(ICANON off) |
| 非阻塞检测 | _kbhit() | select() with timeout=0 |
| 单字符读取 | _getch() | getchar() in raw mode |
| 毫秒延时 | Sleep(ms) | usleep(ms*1000) |
| ANSI 颜色 | ENABLE_VIRTUAL_TERMINAL_PROCESSING | 原生支持 |

---

## 7. 关键代码位置

| 组件 | 头文件 | 源文件 |
|------|--------|--------|
| **游戏中心** | — | `engine/apps/games/mini_program/main.c` |
| **菜单系统** | `engine/apps/common/menu.h` | `engine/apps/common/menu.c` |
| **终端适配** | `engine/apps/common/terminal.h` | `engine/apps/common/terminal.c` |
| **贪吃蛇** | `engine/apps/games/snake/snake.h` | `engine/apps/games/snake/snake.c`<br/>`engine/apps/games/snake/main.c` |
| **2048** | `engine/apps/games/2048/2048.h` | `engine/apps/games/2048/2048.c`<br/>`engine/apps/games/2048/main.c` |
| **数独** | `engine/apps/games/sudoku/sudoku.h` | `engine/apps/games/sudoku/sudoku.cpp`<br/>`engine/apps/games/sudoku/main.cpp` |

---

## 8. 架构设计要点总结

1. **进程隔离**：每个游戏独立进程，游戏中心通过 `system()` 启动，避免内存泄漏和状态干扰
2. **增量渲染**：贪吃蛇和 2048 采用增量渲染，只更新变化的格子，减少闪烁
3. **平台适配**：通过条件编译封装 Windows/POSIX 差异，上层代码无需关心平台
4. **算法复用**：2048 通过旋转变换复用 `move_left` 算法；数独的生成/求解共享回溯框架
5. **难度分级**：三个游戏都支持 3 级难度，通过参数（速度/初始块数/挖洞数）控制