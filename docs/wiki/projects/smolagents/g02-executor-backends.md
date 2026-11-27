---
project: smolagents
feature: executor-backends
title: 远程执行后端实现
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/remote_executors.py
    symbols: [E2BExecutor, DockerExecutor, ModalExecutor]
  - path: src/smolagents/remote_executors.py
    symbols: [BlaxelExecutor, _wait_for_server]
---

# 远程执行后端实现

## 一句话职责

四个后端把「在一个沙箱里跑代码」分别落到 SDK 托管沙箱、本机 Docker 容器、云端沙箱、以及一台按需起停的虚机上，共用同一套「Jupyter 内核 + WebSocket」通道或原生 SDK 调用。

## 机制是什么

四个子类的差异集中在四件事：**沙箱怎么起**、**代码怎么送进去**、**依赖怎么装**、**怎么销毁**。

两类传输通道：

- **原生 SDK 调用**：`E2BExecutor` 用 `sandbox.run_code(code)`，直接拿到 `execution` 对象（含 `logs`、`error`、`results`）。
- **Jupyter 内核 + WebSocket**：`DockerExecutor`、`ModalExecutor`、`BlaxelExecutor` 都在远端起一个 `jupyter kernelgateway`，通过 HTTP 创建内核、再通过 WebSocket 发 `execute_request`。三者共用 `_websocket_run_code_raise_errors` 与 `_create_kernel_http` 两个模块级函数。

鉴权方式一致：都是本地生成一个 `secrets.token_urlsafe(16)` 的 token，通过环境变量 `KG_AUTH_TOKEN`（Docker）/Modal Secret/Blaxel 的 headers 传给内核网关，随后每个 HTTP 请求与 WebSocket URL 都带上 `?token=...`。

## 怎么实现的

**共享的 WebSocket 协议处理。** `_websocket_send_execute_request` 拼一个符合 Jupyter 消息协议的字典：`header` 里带 `msg_id`（新 uuid）、`username`、`session`、`msg_type="execute_request"`、`version="5.0"`；`content` 里 `code`、`silent=False`、`store_history=True`、`allow_stdin=False`。

`allow_stdin=False` 这一项值得注意：它禁止内核在代码里请求标准输入。若模型生成的代码调 `input()`，会直接失败而不是挂住——这是远程执行必须设定的一条，否则一次 `input()` 就能把整个运行卡死在等输入上。

`_websocket_run_code_raise_errors` 是消费端：循环 `ws.recv()` 收消息，先按 `parent_header.msg_id` 过滤（跳过不属于本次请求的消息），再按 `msg_type` 分派：

- `stream` → 把 `content["text"]` 追加到输出缓冲
- `execute_result` → 取 `content["data"]["text/plain"]` 作为结果
- `error` → 若 `ename` 等于 `FINAL_ANSWER_EXCEPTION` 就反序列化成最终答案并置标志；否则把 `traceback` 拼成 `AgentError` 抛出
- `status` 且 `execution_state == "idle"` → 结束循环

所以「一次执行结束」的判据是内核回到 idle，而不是收到某个特定的结果消息。这符合 Jupyter 协议的语义——结果消息可能有零个或多个，只有 idle 才是确定的终点。

**E2B 的响应解析更细。** 它不解析流，而是从 `execution` 对象里按优先级找结果：先查 `execution.error`（含 `FinalAnswerException` 分支），再遍历 `execution.results` 只取 `is_main_result` 的那个，然后依次尝试 `jpeg`/`png` 两个二进制属性（解 base64 后 `PIL.Image.open`），再依次尝试 `chart`/`data`/`html`/`javascript`/`json`/`latex`/`markdown`/`pdf`/`svg`/`text` 十个文本属性。都不命中就返回 `output=None`。

这个「按属性名逐个 `getattr`」的写法覆盖了 E2B 结果对象的多模态可能，代价是顺序敏感：若同一个结果对象同时有 `png` 与 `text`，图片优先。

**Docker 的初始化是一条长流水线。** 它做六步：连 Docker daemon（失败时提示「确保 Docker 在跑」）、按需构建镜像（默认 Dockerfile 是 `python:3.12-bullseye` 加上 `pip install jupyter_kernel_gateway jupyter_client ipykernel`）、起容器（强制映射 `8888/tcp` 与 `detach=True`，并注入随机 token）、轮询容器状态最多 5 次等 running、`_wait_for_server` 轮询 `/api/kernelspecs` 最多 10 次、最后 `_create_kernel_http` 建内核并拼出 `ws_url`。

异常路径是 `except Exception: self.cleanup(); raise RuntimeError(...)`——初始化失败时先清掉已起的容器再报错，避免留下孤儿容器。

**容器参数的合并方式。** 用户传的 `container_run_kwargs` 先整体 `update` 进来，然后**强制覆盖**两项：`ports["8888/tcp"] = (host, port)` 与 `detach = True`。注释写「确保必需的端口映射与后台运行」。所以用户可以定制镜像外的几乎一切，但改不了端口与前台/后台——因为后续所有逻辑（建内核、连 ws）都依赖这两个值。

环境变量则做了形态归一：若 `environment` 是 list（docker-py 也接受 `["K=V"]` 形式），先转成 dict 再塞 token。这是为了兼容两种用法的用户。

**Modal 的隧道。** Modal 的沙箱不在本机，访问要通过 `self.sandbox.tunnels()[port]` 拿一个隧道地址。所以它的 URL 是 `https://{tunnel.host}` 与 `wss://{tunnel.host}`——注意是加密的两种。默认 `encrypted_ports=[port]`，若用户自己传了就追加而不是覆盖。token 通过 `modal.Secret.from_dict({"KG_AUTH_TOKEN": token})` 注入，若用户已有 secrets 就追加。

`_wait_for_server` 与其他实现不同：它**只捕获 `RequestException`** 才计重试，成功（200）就 break，而返回其他状态码时不计数也不 break——会一直循环。上限 60 次，每 10 次打一条日志。这个实现有一个隐含假设：只要连得上但状态码不对，就继续等（可能是网关还没就绪）。但若状态码永久不对（例如 404），循环会跑满 60 次才报错。

**Blaxel 绕开了 `!pip install`。** 它覆盖了 `install_packages`，不用 IPython 语法，而是调沙箱的进程 API：`post_process.sync` 起一个 `pip install --root-user-action=ignore ...` 的进程，然后每 1 秒轮询 `get_process_identifier.sync` 直到 `status != "running"`，最长等 600 秒。若 `exit_code != 0` 就打错误日志并**返回空列表**（不抛异常）。

返回空列表的后果是 `installed_packages` 不会记录这些包，于是下次 `send_tools` 会再次尝试安装。这是「失败不致死但不记账」的策略：安装失败不打断运行，代价是每次工具发送都重试一次。

`--root-user-action=ignore` 是 pip 23 之后的参数，用来抑制「以 root 运行」的警告——在容器里以 root 运行 pip 是常态。

**Blaxel 的清理加了幂等标志。** `cleanup()` 开头 `if self._cleaned_up: return`，然后置真再执行删除。注释只在行内写「防止双重清理」。因为 `__del__` 也会调 `cleanup()`，没有这个标志就会在显式清理后再删一次。

三处清理入口也值得列清：`cleanup()`（显式）、`delete()`（转发到 cleanup，`DockerExecutor`/`ModalExecutor`/`BlaxelExecutor` 都有）、`__del__()`（仅 Blaxel）。

**E2B 的构造兼容两个大版本。** `if hasattr(Sandbox, "create"): Sandbox.create(**kwargs) else: Sandbox(**kwargs)`。注释说明 v2 用 `create`、v1 用构造函数。这种「探测方法存在性」的兼容手法在处理第三方 SDK 大版本升级时很常见，比读版本号更直接。

## 关键取舍

**源码事实：** Docker 与 Modal 的 `_wait_for_server` 实现不一致：Docker 用固定 10 次重试且任何失败都算一次；Modal 只对连接异常计数，其他状态码无限等待（受 60 次异常上限约束）。同一语义、两种实现。

**源码事实：** Blaxel 的 `install_packages` 失败时返回空列表而不是抛错，导致 `installed_packages` 不记录，下次会重装。这是「不打断运行」优先于「安装记账准确」。

**源码事实：** Docker 强制覆盖端口与 `detach`，用户无法通过 `container_run_kwargs` 改这两项——因为后续流水线依赖它们。

**源码事实：** WebSocket 路径用「内核回到 idle」作为执行结束判据，而不是某条特定消息。这让「零结果消息」与「多条结果消息」都能正确收尾。

**源码事实：** E2B 的多模态结果解析有优先级：图片（jpeg/png）先于文本类属性。所以同一个结果若同时含图与文本，只会返回图。

**架构推论：** 四个后端的共同点是「把远端当成一台能跑 Jupyter 的机器」，差异只在「怎么起这台机器」与「怎么连上它」。Docker 在本机、Modal 走隧道、Blaxel 走它自己的 API，但内核协议是一样的。这个统一让 90% 的代码（协议处理、错误解析、最终答案识别）可以共享，只有初始化与清理需要各写一遍。

**架构推论：** 用 Jupyter 内核作为远程执行载体，是一个很实际的选型：它天然提供「持久命名空间」（变量跨执行保留）、结构化结果（`execute_result` 的 data 字典支持多模态）、以及成熟的错误报告（traceback 列表）。代价是要维护一条 WebSocket 与内核生命周期，且必须自己处理「内核崩溃」这类故障——代码里对应的是 `status`/idle 判据与初始化的重试。

**架构推论：** Blaxel 覆盖 `install_packages` 是因为它的沙箱不跑 IPython 魔法。这暴露了基类默认实现的一个隐含假设：`!pip install` 只有在 IPython 内核里才有效。任何「不是 Jupyter 系」的后端都必须覆盖它，否则 `send_tools` 会在装依赖时静默失败（`!pip` 会被当成语法错误）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/remote_executors.py` | `E2BExecutor` | 托管沙箱后端，原生 SDK 调用与多模态结果解析 |
| `src/smolagents/remote_executors.py` | `DockerExecutor` | 本机容器后端，完整初始化流水线与强制端口映射 |
| `src/smolagents/remote_executors.py` | `ModalExecutor` | 云端沙箱后端，走隧道与 Secret 注入 |
| `src/smolagents/remote_executors.py` | `BlaxelExecutor` | 虚机后端，覆盖依赖安装与幂等清理 |
| `src/smolagents/remote_executors.py` | `_websocket_send_execute_request` | 构造 Jupyter 执行请求，禁用标准输入 |
| `src/smolagents/remote_executors.py` | `_websocket_run_code_raise_errors` | 按消息类型消费，以 idle 为结束判据 |
| `src/smolagents/remote_executors.py` | `_create_kernel_http` | 通过 HTTP 创建内核 |
| `src/smolagents/remote_executors.py` | `DockerExecutor._wait_for_server` | 固定次数的连通性轮询 |
| `src/smolagents/remote_executors.py` | `ModalExecutor._wait_for_server` | 按异常计数的隧道就绪等待 |
| `src/smolagents/remote_executors.py` | `_strip_ansi_colors` | 去除 ANSI 转义序列 |

## 参考

- [smolagents 源码：remote_executors.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/remote_executors.py)
- [Jupyter Kernel Gateway 文档](https://jupyter-kernel-gateway.readthedocs.io/)
- [Modal 文档：Sandbox](https://modal.com/docs/guide/sandbox)

## 与主核 / AI 的关系

主核里最接近的是「不同存储后端实现同一套接口」：本地文件、对象存储、远端服务各自管理自己的连接与生命周期，而读写语义一致。这里四个执行后端的关系完全同构——`run_code_raise_errors` 是那个统一语义，其余（怎么起、怎么连、怎么销毁）是后端私事。

与 CrewAI 的 MCP 客户端相比，两边的「远端」含义不同。CrewAI 连的是一个**已经存在的服务**，客户端只负责会话；smolagents 要**先创建一台机器**再连它。这多出来的「创建与销毁」正是这一层复杂度的来源，也解释了为什么清理逻辑（`cleanup`/`delete`/`__del__`）占了这么大篇幅——创建出来的资源必须被显式回收。

对 AI 工程最实用的一条是「以 idle 判结束」这个协议细节。任何基于流式协议的执行，都要确定「怎么知道这次执行完了」。用「特定结果消息」当判据会在零结果场景下挂住；用「超时」会把正常的长执行误杀。回落到协议自己定义的「状态回到空闲」，是唯一可靠的判据。

另一条是 Docker 那段「强制覆盖端口与 detach」。它说明了一个模式：**用户可定制的参数里，必须把流水线依赖的那几个挑出来锁死**。若不锁，用户传一个 `detach=False` 就会让后面的 `container.reload()` 与状态轮询全部失效。这类「看似可配实则不可配」的参数，应该在类型层面就分开（例如用单独的关键字参数），而不是靠覆盖合并后的字典。
