---
project: openhands
feature: acp-auth-probe
title: ACP 智能体认证探测
page_type: feature
schema_version: 1
refs:
  - path: src/api/acp-service/acp-service.api.ts
    symbols: [AcpAuthStatus, AcpAuthProbe, PROBE_TIMEOUT_SECONDS]
  - path: src/api/acp-service/acp-service.api.ts
    symbols: [classifyClaude, classifyCodex, ACP_AUTH_PROBES, AcpService]
---

# ACP 智能体认证探测

## 一句话职责

ACP 提供商（Claude Code / Codex / Gemini CLI / Pi）是否已经登录，靠**在各家自己的 CLI 上跑一条状态命令**来判断：一张「提供商 → 命令 + 分类器」的探测表，加上一个三值结果（已认证 / 未认证 / 未知），全部跑在本地 agent-server 的 bash 端点里，没有专门的后端接口，也不花任何模型 token。

## 机制是什么

这个设计的前提是一个资源约束：**没有可供询问的「登录状态」接口。** ACP 提供商的登录态不在 Canvas 里、不在 agent-server 里，而是在用户自己机器上的 CLI 配置文件或环境变量里。于是唯一可靠的来源是「让那台机器上的 CLI 自己说」。

所以这一层的形状是「**命令行探测 + 输出分类**」，而它的难点在两个地方：

1. **没有统一输出格式。** Claude 能吐 JSON、Codex 只吐一行人类可读文本、Gemini 与 Pi 根本没有状态命令。所以分类器是按提供商各写一份的，不是通用的解析。
2. **探测结果必须是三值而不是二值。** 「未知」是一个必须存在的状态，因为「CLI 没装」「输出格式变了」「bash 调用失败」这三种情况都会发生，而它们都不等于「未登录」。

`AcpAuthStatus` 就三个值：

```
export type AcpAuthStatus = "authenticated" | "unauthenticated" | "unknown";
```

为什么「未知」不能折成「未认证」：`AcpService` 的 docstring 直说——「无法分类的提供商会以 `unknown` 返回，于是 onboarding 退回 API-key 输入框，而不是给出一个误导性的横幅」。如果折成「未认证」，就会告诉用户「你没登录」；而当 CLI 根本没装时，正确的话是「我不知道，请你自己决定怎么填」。

探测的运行位置也决定了它的适用范围：命令跑在 agent-server 所在的地方。所以它**只对本地后端有意义**——在远端/云端主机上，提供商 CLI 与凭据文件几乎不可能在。这条判断不在本模块里，而在调用它的 hook 里（见下）。

## 怎么实现的

**探测表是「提供商 → 命令 + 分类器」的静态映射。**

```
interface AcpAuthProbe {
  /** Shell command run on the (local) agent-server host to detect login. */
  command: string;
  /** Classify the command's output into an auth status. */
  classify: (out: BashOutput) => AcpAuthStatus;
}
```

把「跑什么」与「怎么读」绑在同一个条目里，而不是让调用方分两步。`ACP_AUTH_PROBES` 的键是 `acp_server` / OnboardingAgentId，注释写明「表里没有的提供商（OpenHands、自定义、未知）报告 `unknown`」。

四个条目对应四种「拿不到状态」的处境，每一种的分类器都不同：

| 提供商 | 命令 | 分类依据 |
|---|---|---|
| `claude-code` | `claude auth status --json` | 解析 JSON 里的 `loggedIn` 布尔 |
| `codex` | `codex login status` | 文本里有没有 `logged in` / `not logged in` |
| `gemini-cli` | `test -f "$HOME/.gemini/oauth_creds.json" && echo present || echo absent` | 逐字匹配 `present` / `absent` |
| `pi` | 环境变量或 `auth.json` 里有条目则 echo `present` | 同上 |

**`classifyClaude` 读的是 JSON 而不是退出码。**

```
function classifyClaude(out: BashOutput): AcpAuthStatus {
  let parsed: unknown;
  try { parsed = JSON.parse((out.stdout ?? "").trim()); } catch { return "unknown"; }
  const loggedIn = (parsed as { loggedIn?: unknown } | null)?.loggedIn;
  if (typeof loggedIn === "boolean") return loggedIn ? "authenticated" : "unauthenticated";
  return "unknown";
}
```

注释交代了为什么必须看 JSON：「CLI 在登出时以非零码退出，所以我们读 JSON，而不是退出码」。这是这一页最典型的一处判断——**用一个字段表达状态，而不是用进程退出码**，因为后者在「未登录」与「命令出错」时会给出同一个信号。

第二层判断也重要：`loggedIn` 必须是**布尔**。若 JSON 能解析但没有这个字段（例如 CLI 换了输出格式），返回 `unknown` 而不是 `unauthenticated`。注释明说：「没有可解析的 `loggedIn`（例如 CLI 没装 → `command not found`、空 stdout）⇒ `unknown`，于是 onboarding 显示 API-key 字段而不是靠猜」。

**`classifyCodex` 的顺序是「先否定」。**

```
function classifyCodex(out: BashOutput): AcpAuthStatus {
  const text = streams(out).toLowerCase();
  if (text.includes("not logged in")) return "unauthenticated";
  if (text.includes("logged in")) return "authenticated";
  return "unknown";
}
```

`not logged in` 里含有 `logged in` 子串，所以否定判断必须在前。这一点写在注释里：「先匹配 `not logged in`，因为它包含 `logged in` 子串」。注释同时说明了另一件事：「（输出到 stderr）」，所以它用 `streams()` 把 stdout 与 stderr 拼起来：

```
function streams(out: BashOutput): string {
  return `${out.stdout ?? ""}\n${out.stderr ?? ""}`;
}
```

汇总的注释解释了为什么要拼：「CLI 对用哪个流不一致（例如 `codex login status` 写到 stderr）」。这是一个纯经验性的适配——没有标准可依，只能按各家实际行为。

注意这里用的是小写化后的 `includes`，而不是正则。原因很实际：输出里有别的上下文（「Logged in using ChatGPT」），精确匹配整行会随措辞变化而失效。

**Gemini 与 Pi 走「凭据是否存在」，且它们的分类器共用一份。**

```
function classifyPresence(out: BashOutput): AcpAuthStatus {
  const text = (out.stdout ?? "").trim();
  if (text === "present") return "authenticated";
  if (text === "absent") return "unauthenticated";
  return "unknown";
}
```

注释写得很细：「命令恰好把 `present` / `absent` 打到 stdout，所以对修剪过的 stdout 做精确匹配。只读 stdout（不读 stderr）意味着一条杂散的 shell 警告不会把真实结果变成 `unknown`」。

这里与 `classifyCodex` 恰好相反：那个必须读两个流（因为目标输出在 stderr），这个必须只读一个（因为目标输出在 stdout，而 stderr 可能有噪音）。两条都是被实际输出形态逼出来的。

「只 echo 结果、两种情况都退出 0」这个设计本身也值得记：注释说「命令回显 present/absent，两种情况都以 0 退出；其它情况（shell 失败）⇒ `unknown`」。于是退出码完全不用管，只有「输出了预期的那两个词」才算有结论。

两个提供商的命令分别处理各自的凭据位置：

- **Gemini**：`test -f "$HOME/.gemini/oauth_creds.json"` —— 文件存在即已登录（Google OAuth）。
- **Pi**：`{ [ -n "$ANTHROPIC_API_KEY" ] || grep -qs '"type"' "${PI_CODING_AGENT_DIR:-$HOME/.pi/agent}/auth.json"; } && echo present || echo absent` —— 注释解释了为什么要 `grep` 而不是 `test -f`：「Pi 首次运行会种一个空的 `{}` auth.json，所以需要一个条目」。`-qs` 的 `-q` 让 grep 静默（只看退出码），`-s` 让它不报错。目录用 `${PI_CODING_AGENT_DIR:-$HOME/.pi/agent}` 支持自定义位置。

Pi 那条还先检查 `ANTHROPIC_API_KEY` 环境变量——因为 Pi 也接受直接给 API key 的登录方式，此时没有 auth.json 里的条目。

**`AcpService.getAuthStatus` 是全部逻辑的入口，它只做三件事。**

```
static async getAuthStatus(server: string): Promise<AcpAuthStatus> {
  const probe = ACP_AUTH_PROBES[server];
  if (!probe) return "unknown";
  const out = await new BashClient(getAgentServerClientOptions()).executeCommand(probe.command, undefined, PROBE_TIMEOUT_SECONDS);
  return probe.classify(out);
}
```

1. 查表，没有就 `unknown`（不抛错）。
2. 用 `BashClient` 在 agent-server 上跑命令，超时 `PROBE_TIMEOUT_SECONDS = 10`。
3. 把输出交给该提供商的分类器。

超时那个常量的注释交代了它的取值理由：「单条探测命令的硬上限（秒）。这些是快速的本地状态检查（没有 npx 下载），所以 10 秒对一个健康的 CLI 绰绰有余，同时让 onboarding 的转圈在退回 `unknown` 之前保持很短」。

「没有 npx 下载」这句是关键——它确保了 10 秒这个数字是安全的，因为命令全是本地文件检查或 CLI 自带的状态子命令，不会触网拉包。而如果将来有一条命令需要下载，这个上限就会变成误判的来源。

`executeCommand` 的第二个参数是 `undefined`（大概是工作目录或环境），说明探测命令不依赖 cwd——它们只用绝对路径与 `$HOME`。

**类层面的 docstring 把整套机制的设计约束讲全了。**

> 检测所选 ACP 提供商是否已登录——**完全在客户端，没有专门的 agent-server 端点**。它通过既有的 agent-server bash 端点跑提供商自己的状态命令（对 Gemini 与 Pi 则是凭据检查），并分类输出。
>
> 由调用方门控到**本地后端**：命令跑在 agent-server 所在的地方，而在用户自己的机器上，提供商的 CLI 与凭据文件位于它们的标准路径。不发送任何 prompt，所以不消耗模型 token。无法分类的提供商——CLI 未安装、输出意外、或 bash 调用失败——以 `unknown` 回来，于是 onboarding 退回 API-key 字段而不是一个误导性的横幅。

三点各有价值：**复用既有通道**（不为一个状态查询加一个端点）、**门控在调用方**（这个模块不知道后端种类，它只假设「命令在 agent-server 上跑」，而「哪类后端才有意义」的判断交给上层）、**零 token**（不发送 prompt）。

第二点尤其值得记：「门控在调用方」意味着这个模块**不防御**——如果把 `getAuthStatus` 直接对着一个云端后端调用，它会真的在云端主机上跑那条命令，得到 `unknown`（因为那里没有 CLI）。这是一个明确的责任划分，而不是疏漏。

**调用侧的包装把「异常」也归进 `unknown`。**

虽然不在本文件，但它是这套机制完整行为的必要部分（`use-acp-auth-status`）：

```
async function probeAcpAuth(providerKey: string): Promise<AcpAuthStatus> {
  try { return await AcpService.getAuthStatus(providerKey); }
  catch { return "unknown"; }   // bash 端点不可达或出错
}
```

注释说明后果：「退回 `unknown`，好让调用方显示 API-key 字段而不是错误地宣称『未登录』」。也就是说即使 bash 端点整个挂了，用户看到的也是「请填 API-key」，而不是一个确定性的错误结论。

同一侧还有一条门控：`const isSupported = isLocal;` —— 只有本地后端才探。以及一个 5 秒的 `staleTime` 与 `refetchOnWindowFocus: true`，注释解释：「登录状态在 Canvas 之外变化（在终端里 `claude auth login`、切换账号），所以结果不能按会话缓存：窗口重新获得焦点或消费界面重新挂载时都会重探，一旦过了那个短暂的 5 秒窗口」。5 秒只是用来合并多个消费方同时挂载时的突发调用。

## 关键取舍

**源码事实：** 探测结果三值，`unknown` 是独立状态；表里没有的提供商、CLI 未装、输出不可解析、bash 调用失败都归 `unknown`，而调用方据此显示 API-key 字段而不是「未登录」。

**源码事实：** 探测不加新端点，复用既有 agent-server bash 端点；不发送 prompt，所以零模型 token。

**源码事实：** `classifyClaude` 读 JSON 字段而非退出码，因为 CLI 在登出时以非零码退出——用退出码无法区分「未登录」与「命令出错」。

**源码事实：** `classifyCodex` 先判 `not logged in` 再判 `logged in`（前者包含后者的子串），且必须拼上 stderr，因为目标输出写在 stderr。

**源码事实：** `classifyPresence` 只读 stdout 且逐字匹配，因为 stderr 上可能出现杂散 shell 警告；它的两个命令都保证两种情况退出 0，所以退出码不参与判断。

**源码事实：** Pi 的检查用 `grep` 而非 `test -f`，因为 Pi 首次运行会种一个空 `{}` 的 auth.json；它还先查 `ANTHROPIC_API_KEY`。

**源码事实：** `PROBE_TIMEOUT_SECONDS = 10`，注释的理由是「这些是快速本地检查、没有 npx 下载」。

**源码事实：** 门控（只对本地后端探）在调用方，不在本模块；因此本模块对「对着云端后端调用」不设防。

**架构推论：** 「用提供商自己的 CLI 来回答提供商自己的状态」是一种**避免重述事实**的设计：Canvas 不维护登录态，也就不会与 CLI 的真实状态漂移。代价是必须接受输出格式各异、必须逐个适配、且必须承受格式变化带来的脆弱性。

**架构推论：** 「未知」作为一等状态的代价是要在 UI 上处理三种情形；收益是**永远不说错误的话**。在一个 onboarding 场景里，「我不知道你是否已登录，请自己选」比「你没登录」或「你已登录」都更安全——两种错误的确定性陈述都会把用户推向错误的下一步。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/api/acp-service/acp-service.api.ts` | `AcpAuthStatus` | 三值结果：已认证 / 未认证 / 未知 |
| `src/api/acp-service/acp-service.api.ts` | `AcpAuthProbe` | 一个提供商的探测定义：命令 + 分类器 |
| `src/api/acp-service/acp-service.api.ts` | `PROBE_TIMEOUT_SECONDS` | 单条命令的硬上限（10 秒，无网络下载） |
| `src/api/acp-service/acp-service.api.ts` | `ACP_AUTH_PROBES` | 提供商 → 探测定义的表；缺项即 `unknown` |
| `src/api/acp-service/acp-service.api.ts` | `classifyClaude` | 解析 `{"loggedIn": bool}`，字段类型不符即 `unknown` |
| `src/api/acp-service/acp-service.api.ts` | `classifyCodex` | 先否定后肯定的文本匹配，读 stdout + stderr |
| `src/api/acp-service/acp-service.api.ts` | `AcpService.getAuthStatus` | 查表、跑 bash、分类；无表项直接 `unknown` |
| `src/api/acp-service/acp-service.api.ts` | `streams` | stdout + stderr 合并，适配 CJI 流不一致 |

## 参考

- [OpenHands 源码：acp-service/acp-service.api.ts](https://github.com/OpenHands/OpenHands/blob/main/src/api/acp-service/acp-service.api.ts)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [Claude Code 文档：CLI 与认证](https://docs.claude.com/en/docs/claude-code/cli-reference)
- [OpenAI Codex CLI 仓库](https://github.com/openai/codex)

## 与主核 / AI 的关系

「通过被探测对象自己的状态命令来判断它」与主核里「通过存储引擎自己的健康检查来判断复制链路」是同一种取向：不去复制一份可能漂移的事实，而是每次都在需要时问一次源头。主核里这一点尤其重要，因为复制状态由外部系统维护，本地缓存必然滞后。

对 AI 工程的实际意义有三条。第一，**退出码不是状态**。`claude auth status` 在未登录时同样非零退出，于是「非零」既可能是「未登录」也可能是「命令挂了」。凡是外部 CLI 提供机器可读输出（JSON 字段、固定词元）时，都应当用它而非进程状态——因为进程状态把「业务结论」与「执行故障」编进了同一个位。

第二，**「未知」必须是一等状态，且在 UI 上有明确的下一步**。这里的三值设计的价值不在分类器本身，而在「未知 → 显示 API-key 输入框」这条映射：它把不确定性翻译成一个用户能行动的选择，而不是一条让用户猜的错误结论。

第三，**门控放在调用方是有意的责任划分**。本模块只说「命令在 agent-server 上跑」，不说「什么后端才算合适」。这让模块保持简单（不依赖后端注册表），代价是对误用不设防。识别这类「有意的无防御」需要读调用方——只看本文件会以为漏了检查。
