---
project: browser-use
feature: skills-packaging
title: 技能包与安装
page_type: feature
schema_version: 1
refs:
  - path: browser_use/skills/browser_use.py
    symbols: [as_browser_use_skill, skill_text]
  - path: browser_use/skills/install.py
    symbols: [handle, _build_parser]
---

# 技能包与安装

## 一句话职责

把 browser-harness 的 agent 技能**改写成 browser-use 自己的技能**（换名字、换描述、替换安装指引、换反馈通道），再把它安装到十来个 AI 助手的技能目录里——而重写是**逐行变换**而非复制，因为两份技能文本必须保持同步。

## 机制是什么

两块：

- `skills/browser_use.py`：`skill_text()` 取本包的 `SKILL.md`（随 wheel 发布），取不到就退回 **browser-harness 包里的 `SKILL.md` 并经 `as_browser_use_skill()` 改写**。
- `skills/install.py`：`handle(argv)` 是 `browser-use skill show|install` 的实现，`_build_parser()` 定义 CLI 形状。

`as_browser_use_skill(text)` 的变换清单（按执行顺序）：替换 frontmatter 里的 `name` 与 `description`、补 `homepage` 与 `metadata`（OpenClaw 安装声明）、改标题、替换安装指引链接、重命名正文里所有 `browser-harness` 提及、**删掉 harness 版本里的 Feedback 节**并插入 browser-use 版本、若没有 `## Gotchas` 节则追加到末尾。

## 怎么实现的

**「保留一个宿主技能的身份」而不是 fork 一份正文。** 模块开头有一段注释：`OPENCLAW_METADATA_LINES` 是 **Browser Use 独有的 frontmatter，在生成两份已入库的 `SKILL.md` 副本时加上**，且「**把它当作唯一真值源；`scripts/sync_browser_harness_skill.py` 会验证输出**」。也就是说这里的代码不是「运行时改写一份副本」，而是**定义了那份副本应该长什么样**，并有脚本校验。这是一个把「生成器」与「产物」绑在一起的模式。

**frontmatter 的处理是「替换 + 补齐」而不是重写整块。** `as_browser_use_skill` 逐行扫 frontmatter：`name:` 开头就换成 `name: browser-use` 并记 `saw_name`；`description:` 开头就换成统一描述并记 `saw_description`；其它行原样保留。扫完若仍没见到 name/description 就**插入**到最前（name 在第 0 行、description 在第 1 行）。然后按需补 `homepage:` 与 `metadata:`。

这几步的细节说明一个问题：**输入的 frontmatter 形状不由本包控制**（它来自另一个包），所以不能假定字段顺序或存在性。`saw_name` / `saw_description` 两个标志加「替换而非追加」的做法，保证无论输入是完整、部分缺失、还是字段乱序，输出都恰好有一个 name 和一个 description。

**`SKILL_DESCRIPTION` 的措辞是刻意写的。** 注释解释：**这是 agent 在决定要不要打开技能之前看到的那段描述**，所以里面要**同时点出「hosted cloud browsers for AI agents」这个用例**，让「正在写 agent 代码」的 agent 也会打开它——注释列的触发场景是 browser-use 库、Playwright、Puppeteer、CDP。这是一个把「检索可达性」写进产品文案的例子：描述不只描述内容，还负责在错误的语境里被正确召回。

**正文重命名用的是负向后顾而不是简单替换。** `re.sub(r'(?<!/)browser-harness', 'browser-use', body)`，注释：**除了仓库 URL（`github.com/browser-use/browser-harness/...`）之外，每一处提及都改名**。`(?<!/)` 只在前面不是斜杠时匹配，恰好保住 URL 路径里的 `browser-harness`。这个字符级技巧很窄但正确——用一个宽松规则会破坏文档里的链接。

**安装指引必须换掉，因为它指向的命令在 1.0 里不存在。** `HARNESS_INSTALL_LINK` 是那句「read https://github.com/browser-use/browser-harness/blob/main/install.md」，被替换成 `SETUP_HELP = 'run \`browser-use --doctor\` and follow its output and the error message'`。代码注释给出理由：**browser-harness 的 install.md 讲的是安装并运行 `browser-harness`，而 `uv tool install browser-use` 并不提供那个命令**。也就是说这里拒绝把一个「装了但跑不起来」的指令留在文档里。

**Feedback 节整节替换，理由也是「通道不同」。** `_HARNESS_FEEDBACK` 是一个匹配 `\n## Feedback\n.*?(?=\n## |\Z)` 的正则（`re.DOTALL`），把 harness 版本的整节删掉，再用 `FEEDBACK_SECTION` 插入到 `## Gotchas` 之前（没有 Gotchas 则追加到末尾）。

代码注释说明了原因：**它替换掉 harness 技能里那个 Feedback 节——那一节发送时不经过 browser-use CLI**。而 `FEEDBACK_SECTION` 的内容里给了两条通道：CLI 用 `browser-use feedback --category ...`，Cloud API / SDK 的集成用 `curl` 打 `/api/v4/feedback`。两条都带「第一行写 `kind: integration_report | feature_request | bug`」的协议，以及一条明确的禁令：**不要放密钥、密码或客户数据**。

**`skill_text()` 是「优先用自带副本，退回改写别人那份」。** 它先找 `Path(__file__).parent / 'browser-use' / 'SKILL.md'`，存在就直接读（本包发布时打进去的那份）；不存在才用 `importlib.resources` 从 `browser_harness` 包里读并改写。若那个包都不在，抛 `RuntimeError` 并给出修复命令：**`uv tool install --force browser-use`**。

这条降级路径存在的意义是「装了 browser-use 但没拿到自带 SKILL.md」的情形（例如开发态、或某种打包差异），此时它仍然能产出一个正确的技能——代价是要跑一遍改写。

**`_install_browser_use_tool` 里有一串注释解释「为什么不能盲目 `--force`」。** 它做四件事：

1. 找不到 `uv` 就报错（`Installing the Browser Use skill requires \`uv\``）。
2. `get_browser_use_version()` 返回 `unknown` 就拒（否则无从决定装哪个版本）。
3. **跑 `uv tool dir` 判断「当前进程是不是就从那个 uv tool 目录里跑的」**，是则直接告知「已经装好，不重装」。注释解释了两次真实事故：**从这个 tool 本身运行时，那个版本就是已安装的 tool，`--force` 会删掉本进程运行所依赖的环境；Windows 上正在运行的 exe 被锁住，于是 uv 装到一半留下一个没有 `browser_use` 模块的环境**。
4. 装的时候**把版本钉住**（`browser-use=={version}`）且**指定当前 Python 版本**。两条注释各自对应一类事故：**不钉版本时 uv 会挑最新稳定版，从而把 1.0 rc 的安装换成 0.x**；**不指定 Python 时一次强制的 3.12 会把一个 3.13 的 tool 环境重建成 3.12，删掉本进程还在读的文件**。

这四步合起来是一句工程结论：**「重装自己」是一个危险操作，必须先确认「我是不是我自己」以及「重装会不会杀掉正在运行的东西」**。

**`--install-cli` 是显式 opt-in。** 注释：**它会替换用户自己那份 browser-use tool 安装**。与之对照，`--force` 与 `--no-install` 两个参数是**接受但无作用**的兼容参数（`--force` 的帮助文本直接写「install 默认就覆盖已有 `SKILL.md`」，`--no-install` 写「这现在就是默认」）。这是把「旧脚本还在传这些参数」的成本降到零，而不是让它们报错。

**安装路径表体现的是「各助手的技能目录约定」。** `TARGET_DIR_BUILDERS` 八个目标：七个是 `~/.<assistant>/skills/browser-use`（agents、claude、codex、copilot、cursor、gemini、openclaw），opencode 特殊——它走 `XDG_CONFIG_HOME/opencode/skills/browser-use`，且有 `_xdg_config_home()` 处理未设该变量时退回 `~/.config`。

**`_all_target_skill_paths` 的过滤规则是「共享目录总是装，其它的只在已安装时装」。** 注释写明了：**`~/.agents`（共享技能目录）总是装；其它的只对那些已经装过的助手（`~/.claude`、`~/.codex` 等）装**。实现是 `path == paths[0] or path.parents[2].is_dir()`——`parents[2]` 就是 `~/.<assistant>`。它还把 `~/.config/opencode/...` 的**遗留路径**（老版本用的位置）加进列表并去重，于是 `--target all` 会顺手把旧位置也更新掉。

**路径校验分两层。** `_validate_output_paths` 先拒绝「目标已存在但是目录」（因为期望的是 `SKILL.md` 文件路径），再向上找到第一个存在的祖先，若它不是目录也拒绝。第二层的实现值得看：`while not ancestor.exists()` 循环到根（`ancestor.parent == ancestor`）为止。这样「父路径里有文件挡住」这种输入会在写之前就报清晰错误，而不是让 `mkdir` 抛一个难懂的 `NotADirectoryError`。

**写失败不中断后续目标。** `handle` 里的循环注释：**一个不可写的助手目录不该跳过其它的**。所以它逐目标 `try/except OSError`，失败的打印 `exc.strerror`（而不是整个异常），全部处理完后若有一个失败则返回 1。这个「尽力而为 + 汇总失败」的模式对一个要写八到十个目录的安装器是必要的。

**`skill show` 的 stdout 处理不是小事。** 代码先 `sys.stdout.flush()` 再 `sys.stdout.buffer.write(text.encode('utf-8'))`。注释给出理由：**无论控制台代码页是什么都用 UTF-8，因为 Windows 上 `skill show > SKILL.md` 会写成 cp1252（或在一个它缺的字符上抛错）**。这是这条链上最容易被忽略的一处：重定向的输出是**文件内容**而不是屏幕显示，所以编码必须固定而不是跟随 locale。

**`handle` 的默认命令是 `show`。** `command = args.command or 'show'`——不带子命令就打印技能文本。这是一个合理默认：`browser-use skill` 最常见的用途是看一眼内容或重定向到文件。

## 关键取舍

**源码事实：** `as_browser_use_skill` 做的是逐项变换而非复制正文，且 `OPENCLAW_METADATA_LINES` 被声明为「生成两份已入库副本的唯一真值源」，由同步脚本校验。

**源码事实：** frontmatter 处理用「替换 + 缺则插入」而不是重写整块，因为输入形状不受本包控制。

**源码事实：** 正文重命名用负向后顾 `(?<!/)` 保住仓库 URL 里的 `browser-harness`。

**源码事实：** harness 的安装指引被替换成 `browser-use --doctor`，因为 `uv tool install browser-use` 不提供 `browser-harness` 命令。

**源码事实：** harness 的 Feedback 整节被正则删除并替换成 browser-use 版本，因为旧节不经过 browser-use CLI；新节给出 CLI 与 Cloud API 两条通道，并禁止包含密钥与客户数据。

**源码事实：** `skill_text()` 优先用包内自带的 `SKILL.md`，缺失时才退回改写 browser-harness 的。

**源码事实：** 自动重装自身（`--install-cli`）前先判断「当前进程是否就在那个 uv tool 目录里」，并钉住版本与 Python 版本。三条都有真实事故记录。

**源码事实：** `--force` 与 `--no-install` 是接受但无作用的兼容参数。

**源码事实：** `--target all` 对 `~/.agents` 总是写，对其它助手只在对应目录已存在时写；`~/.config/opencode` 的遗留路径也一并更新。

**源码事实：** 写入失败不中断后续目标，最终以退出码汇总。

**源码事实：** `skill show` 直接往 `sys.stdout.buffer` 写 UTF-8，避免 Windows 重定向写成 cp1252。

**架构推论：** 这个模块处理的不是「安装一个文件」，而是**一个包的文档产物在多个消费方之间的身份一致性**。同一个技能要被十来个助手读到、被同步脚本校验、被 `show` 打印、被 `install` 写盘，而它的正文继承自另一个包。于是一切设计都围绕「怎样让一份继承来的文本在每个出口上都自称 browser-use，且不携带对不上的指令」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `browser_use/skills/browser_use.py` | `as_browser_use_skill` | 把 harness 技能变换成 browser-use 技能 |
| `browser_use/skills/browser_use.py` | `skill_text` | 取包内副本，缺失时退回改写 harness 的 |
| `browser_use/skills/browser_use.py` | `SKILL_DESCRIPTION` | 面向 agent 召回的描述，含 hosted cloud 用例 |
| `browser_use/skills/browser_use.py` | `FEEDBACK_SECTION` | 替换后的反馈节，含 CLI 与 Cloud API 两条通道 |
| `browser_use/skills/browser_use.py` | `_HARNESS_FEEDBACK` | 整节删除 harness Feedback 的正则 |
| `browser_use/skills/browser_use.py` | `OPENCLAW_METADATA_LINES` | OpenClaw 安装声明，生成副本的真值源 |
| `browser_use/skills/install.py` | `handle` | `show` / `install` 两条子命令的实现 |
| `browser_use/skills/install.py` | `_build_parser` | CLI 形状，含两个兼容参数 |
| `browser_use/skills/install.py` | `TARGET_DIR_BUILDERS` | 八个助手技能目录的构造表 |
| `browser_use/skills/install.py` | `_all_target_skill_paths` | 「共享目录总是装、其它按已安装过滤」 |
| `browser_use/skills/install.py` | `_resolve_output_paths` | 目标 / 自定义路径 / `.md` 文件三种解析 |
| `browser_use/skills/install.py` | `_validate_output_paths` | 写前校验目标与祖先类型 |
| `browser_use/skills/install.py` | `_install_browser_use_tool` | 自装自的危险操作的四重防护 |
| `browser_use/skills/install.py` | `_load_skill_text_from_package` | 只用本包的技能，绝不调用 PATH 上的别的程序 |

## 参考

- [browser-use 源码：skills/browser_use.py](https://github.com/browser-use/browser-use/blob/main/browser_use/skills/browser_use.py)
- [browser-use 源码：skills/install.py](https://github.com/browser-use/browser-use/blob/main/browser_use/skills/install.py)
- [Agent Skills 规范](https://docs.claude.com/en/docs/claude-code/skills)
- [browser-harness 仓库](https://github.com/browser-use/browser-harness)

## 与主核 / AI 的关系

这个模块与主核里的「扩展安装器」是同一类东西：把一个包的附带产物（文档、扩展脚本、模板）安装到宿主约定的位置。主核的扩展安装同样要处理路径校验、幂等覆盖、以及「别人的目录可能不可写」；而「自装自」的危险在数据库世界里对应的是 `ALTER EXTENSION ... UPDATE`——同样是「我在运行中替换我自己」。

对 agent 工程的直接价值有三条。其一，**技能描述是召回接口，不是文档摘要**。写「它做什么」会让该打开它的 agent 找到它，但不会让「正在写 Playwright 代码」的 agent 想到它；`SKILL_DESCRIPTION` 里那句 hosted cloud 用例就是为后一类召回写的。其二，**继承来的文档必须重写指令，不能只改名字**。一份说「去读 X 的 install.md」的技能，在一个不提供 X 命令的包装里就是错的——这类错误不会报错，只会让 agent 走进死胡同。其三，**「重装自己」必须先自检**。判断自己是不是就在将被删除的那个环境里、钉住版本、钉住解释器版本，这三条各自防一类灾难性后果，且都是事后才想得到、事前写进注释才能保住的。
