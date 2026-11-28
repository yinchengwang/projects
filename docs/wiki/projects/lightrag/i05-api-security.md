---
project: lightrag
feature: api-security
title: API 鉴权与准入控制
page_type: feature
schema_version: 1
refs:
  - path: lightrag/api/auth.py
    symbols: [AuthHandler, TokenPayload]
  - path: lightrag/api/login_rate_limit.py
    symbols: [LoginRateLimiter, _Entry]
  - path: lightrag/api/admission.py
    symbols: [AdmissionTicket, publish_admission_ticket]
  - path: lightrag/api/body_limit_middleware.py
    symbols: [BodyLimitMiddleware, resolve_body_limits]
---

# API 鉴权与准入控制

## 一句话职责

把「谁能进」与「进多少个」四件事分层处理：JWT 校验（`AuthHandler`）、登录爆破限速（`LoginRateLimiter`）、请求体上限（`BodyLimitMiddleware`）、以及**跨三个所有者传递的一次容量预留**（`AdmissionTicket`）——而四层各自面对的失败模式完全不同，所以四个模块的代码形态也完全不同。

## 机制是什么

四层的位置与职责：

| 层 | 模块 | 防什么 | 失败方向 |
|---|---|---|---|
| JWT 校验 | `auth.py` | 伪造/过期令牌、claim 滥用 | 拒绝（401） |
| 登录限速 | `login_rate_limit.py` | 密码爆破、并发绕过 | 限速（429） |
| 请求体上限 | `body_limit_middleware.py` | 超大体积耗尽资源 | 截断（413） |
| 容量准入 | `admission.py` | 排队任务挤占内存 | 拒绝（429/503） |

而这一页最有意思的技术点不是任何单层，而是**它们的相对顺序**。`body_limit_middleware` 的模块 docstring 明确写了两句：

> 位置要紧。这个中间件装在准入中间件的**外层**，所以一个超大请求体会在它能消耗一个容量槽之前就被拒绝；而在 CORS 的**内侧**，好让一个 413 带上浏览器读取状态所需的头。

所以一个 413 的三层嵌套关系是：CORS → BodyLimit → Admission。三层的位置各有硬理由：

- **BodyLimit 在 Admission 外层**：因为「拒绝一个超大体」比「占一个槽再拒绝」便宜，且否则攻击者可以用超大体把容量槽占满。
- **BodyLimit 在 CORS 内侧**：因为 413 是浏览器要读的状态码，而没有 CORS 头的话浏览器只会显示一个网络错误。

`body_limit_middleware` docstring 的第一段还记录了这个模块**为何被拆出来**：

> `MAX_REQUEST_BODY_BYTES` 过去在准入中间件里执行，因此恰好只覆盖准入关心的那三个摄取路由。**这让那个名字成了一个谎言**：运维可以设置它、看着 `/documents/text` 毫秒级拒绝一个 8 MiB 的请求体，却让 `/api/chat`——在发布的默认白名单下不需要任何凭据——接受同一个请求体并把整个进程卡在分词上（GHSA-r8jh-295g-vv42）。

这段值得完整引用，因为它描述的是一种很常见的错觉：**一个配置项被设置了，但它实际生效的范围远小于名字暗示的范围**。而检测方法是「对每个路由问一次『这个上限对它生效吗』」，而不是「确认这个变量被读到了」。

## 怎么实现的

**`AuthHandler.__init__` 把三类启动期配置错误都变成构造异常，而三类的判据不同。**

```python
if not self.secret:
    if auth_accounts:
        raise ValueError("TOKEN_SECRET must be explicitly set to a non-default value when AUTH_ACCOUNTS is configured.")
    self.secret = DEFAULT_TOKEN_SECRET
    logger.warning("TOKEN_SECRET not set and AUTH_ACCOUNTS is not configured. Falling back to the default guest-mode JWT secret.")
```

第一类：**配了账号但没配密钥 → 拒绝启动**。理由显然：账号意味着「有东西要保护」，而用公开的默认密钥签的令牌谁都造得出，等于没有鉴权。而没配账号时退化到默认密钥是安全的——因为那时系统本来就是访客模式，令牌不保护任何东西。

```python
algorithm = global_args.jwt_algorithm
if not algorithm or algorithm.lower() == "none":
    raise ValueError("JWT_ALGORITHM must be set to a secure algorithm (e.g. HS256). The 'none' algorithm is not permitted.")
```

第二类：**`none` 算法直接拒**。这是 JWT 的经典漏洞——`alg: none` 意味着「不签名」，而一个不校验算法的实现会接受任何令牌。这里在**配置期**就拒，而不是在解码时拒。

而 `validate_token` 里还有第二道：

```python
allowed_algorithms = [self.algorithm]
if "none" in (a.lower() for a in allowed_algorithms):
    raise HTTPException(status_code=500, detail="Insecure JWT algorithm configuration")
payload = jwt.decode(token, self.secret, algorithms=allowed_algorithms)
```

两道是因为 `self.algorithm` 是实例属性，可能在构造之后被改（测试、热更新）。所以解码前再查一次——而这次报 500 不是 401，因为它是**服务端配置错误**而不是「你的令牌不对」。这个区分很重要：401 会让客户端去换令牌，而问题在服务端。

**第三类是用户名长度，而它的处理方式揭示了「一个常量必须两端共用」这个原则。**

```python
if len(username) > MAX_TOKEN_SUBJECT_LENGTH:
    # Rejected at configuration time, not at login. create_token signs the username into the "sub"
    # claim and validate_token caps that claim at the same bound, so accepting the account here would
    # let it authenticate at /login and then fail every subsequent request with 401 -- an unusable
    # account and a baffling symptom. Both ends of the claim share one constant.
    oversized_username_lengths.append(len(username))
    continue
```

这是一个「半个校验」的具体症状描述：若只在 `validate_token` 里限长，那一个长用户名**能登录成功**（`create_token` 不看长度），然后**此后每个请求都 401**。症状是「登录成功但什么都做不了」，而日志里没有任何错误。

所以修法是「在配置期就拒」，并且 `MAX_TOKEN_SUBJECT_LENGTH` 这一个常量同时被三处使用：配置期校验、`create_token` 的隐含保证、`validate_token` 的上限。docstring 明说「claim 的两端共用一个常量」。

而它的日志只记长度不记内容：

```python
# Only the lengths are logged: the offending entry carries a password.
logger.error(f"AUTH_ACCOUNTS contains {len(oversized_username_lengths)} username(s) longer than {MAX_TOKEN_SUBJECT_LENGTH} characters (lengths: {oversized_username_lengths})")
```

「只记长度，因为那条有问题的条目带着一个密码」——这是日志脱敏的一个精确判断：错误消息要能帮人定位，但定位信息不能包含凭据。

**`verify_password` 的每一条分支都恰好跑一次 bcrypt，而三条分支的写法各不相同。**

```python
stored_password = self.accounts.get(username)
if stored_password is None:
    verify_password(plain_password, _DUMMY_VERIFY_SPEC)   # 未知用户：跑假验证
    return False
if not stored_password.startswith(BCRYPT_PASSWORD_PREFIX):
    password_matches = verify_password(plain_password, stored_password)   # 真结果
    verify_password(plain_password, _DUMMY_VERIFY_SPEC)                   # 再跑假验证
    return password_matches
return verify_password(plain_password, stored_password)
```

三条分支对应三种存储形态：未知用户、明文账号、bcrypt 账号。三次的 bcrypt 次数：1、2、1。

第三条（bcrypt 账号）本来就跑一次真 bcrypt，所以不用补。第二条（明文账号）的真实比较只有微秒级，所以**补一次假 bcrypt** 让总耗时匹配。第一条（未知用户）直接跑假验证。

而 `_DUMMY_VERIFY_SPEC` 的注释把成本因子的问题也说清了：

> 成本因子是 12，与 `hash_password()` / `bcrypt.gensalt()` 的默认一致。**用手选的不同 bcrypt 成本存的账号仍会有一些差异**；完全关闭那个差距需要强制统一成本（或干脆放弃明文支持）。见 GHSA-c759-cx9p-mrwq。

所以这是一个「减轻」而不是「消除」的措施，而 docstring 承认了残留。这种诚实很重要，因为它告诉读者「这里还有一个可利用的侧信道」。

这条防护针对的是 CWE-208（可观测的时间差异），而它的实际价值在于：**用户枚举**需要区分「用户存在」与「不存在」，若前者快后者慢（或反之），一个自动化的探测就能枚举出所有账号。

而 `LoginRateLimiter` 用的是 `confirmed + pending` 计数——这一点与 `verify_password` 的时序防护是同一个问题的两个面：时序侧信道与并发绕过。而 `login_rate_limit` 的 docstring 把两者的区别写得很清楚：

> **限速决策**用 `confirmed + pending`，好让许多同时到达的请求不能在 bcrypt 还在飞的时候全部通过检查（TOCTOU 并发绕过）。**锁定告警**只用 `confirmed`，所以一个正确的密码（或一批正确密码）与一个错误密码同时解析时，永远不会产生假的「账号已锁定」告警。

所以同一个数据结构被两个用途读，而两个用途需要的视图不同：

- **决策**要保守（算上未决的），否则并发能绕过。
- **告警**要精确（只算确认的），否则会误报。

而这个不对称是从一个具体场景推出来的：一次正确的登录尝试与一次错误的登录尝试同时被 `verify_password` 处理，两者的结果几乎同时返回。若告警用 `confirmed+pending`，那正确密码那次也会被算进「失败次数」，于是第 N 次尝试时误报「账号已锁定」。

**`_Entry` 的两个字段是两类状态，而模块 docstring 明说「不能混淆」。**

```python
class _Entry:
    __slots__ = ("failures", "pending")
    def __init__(self) -> None:
        self.failures: deque = deque()   # 已确认的错误密码时刻，最旧在前
        self.pending: int = 0            # 结果未知的预约
```

`failures` 是 `deque` 因为要做滑动窗口（`_prune` 从左侧弹出过期项）。`pending` 是计数而不是时间戳，因为它只关心「有多少在飞」。

**`retry_after` 的返回值分两种情况，而区分它们的判据是「阻塞的原因」。**

```python
confirmed = len(entry.failures)
if confirmed + entry.pending < self.max_attempts:
    return None
if confirmed >= self.max_attempts:
    return max(0.0, self.window_seconds - (now - entry.failures[0]))
return _PENDING_RETRY_AFTER_SECONDS
```

三条出口：

1. **没超限** → `None`（允许）。
2. **确认失败已达上限** → 等到最老的确认失败滑出窗口。
3. **仅因在飞预约而超限** → 返回 `1.0` 秒。

第三条的理由写在注释里：「那些在一秒之内就解析完（bcrypt），而且可能全部成功——从而清掉失败计数——所以建议短暂重试而不是整个窗口」。

这个区分对用户体验影响很大：若第二种情况也返回整个窗口（300 秒），那一个正确的用户在一波并发请求之后就被告知「等 5 分钟」，而实际上 1 秒后就能进。

**`release` 的删空逻辑与 `_make_room` 的正确性直接相关，而注释点明了这个依赖。**

```python
if entry.pending > 0: entry.pending -= 1
if entry.pending == 0 and not entry.failures:
    del self._entries[slot]
```

docstring：「一旦完全空闲就移除条目（没有预约也没有确认失败），好让成功的登录不留下死条目占容量。**让 map 里没有空闲条目也是让 `_make_room` 可以相信『一个活的队首意味着整张表都是活的』的东西**」。

而 `_make_room` 确实依赖这一点：

```python
while len(self._entries) >= self.max_tracked_keys:
    slot, entry = next(iter(self._entries.items()))  # 最久未活动
    self._prune(entry, now)
    if entry.failures or entry.pending > 0:
        # 容量被活记录耗尽：可能是洪泛。
        self._alert(now, "table_full", ...)
        return False
    del self._entries[slot]  # 死了 -> 回收
```

注释说「键按最久未活动在前排序，所以一旦队首是活的，剩下的每一个也都是活的，什么都不能再驱逐了」。这个推理依赖 `OrderedDict` 的 `move_to_end` 在每次活动时被调用——而 `reserve_attempt` 与 `commit_failure` 都调了。

所以「一次驱逐尝试就够」这个判断是对的：不需要遍历整张表找可驱逐项，因为「队首活着」意味着全部活着。

**「活记录永不驱逐」这条规则针对一个具体的攻击。**

模块 docstring：

> 被跟踪的键数上限为 `max_tracked_keys`。在满容量时，**只驱逐既不在窗口内也不在飞的键；一个活记录（活跃锁定或未决请求）永不被驱逐**——否则攻击者可以通过灌入唯一键来清掉一个锁定。如果每个被跟踪键都是活的，一个新键就干脆不被跟踪直到有槽空出来。

第二条是最关键的：如果满了就随便驱逐一个，那攻击者只要用 N 个不同的用户名/IP 组合去打，就能把针对他真实目标的那条锁定记录挤掉。所以「满了就不跟踪新键」这个看似更差的策略反而是对的——它的代价是「新键暂时不受限速」，而那个代价被 `_alert` 里的 `table_full` 告警显式报告出来了：

```python
f"Login rate-limit table is full ({self.max_tracked_keys} active keys); new client/username pairs are temporarily not rate-limited -- possible flood, consider an upstream reverse proxy / WAF rate limit"
```

所以这个退化不是静默的，而是明确说明「现在哪些请求不受保护」并给出下一步（上游 WAF）。

**键存成固定长度的 SHA-256 摘要，而理由是「内存上限按字节算」。**

```python
@staticmethod
def _slot(key: str) -> bytes:
    """Fixed-size map index for ``key``, whatever the key's length."""
    return hashlib.sha256(key.encode("utf-8", "surrogatepass")).digest()
```

模块 docstring：

> 键存成固定长度的 SHA-256 摘要，**从不存提交的原文**。用户名是攻击者控制的，且只有请求体上限约束它，所以逐字存储会让每次失败登录保留最多一整个请求体的字节，而键数量上限在字节上就什么也约束不了。

这个论证很实在：`max_tracked_keys = 10_000` 看起来是一个内存上限，但如果每个键可以是 1 MiB 的字符串，那上限就是 10 GiB。所以「数量上限」只有在「每项大小有界」时才是真的内存上限。

而 `encode("utf-8", "surrogatepass")` 的理由是容错——用户名可能含孤立代理项（来自畸形输入），而默认的 `encode` 会抛。用 `surrogatepass` 让「任何字符串都能算出摘要」。

而 `safe_log_value(key)` 在告警里传入可读键——所以模块 docstring 说「调用方继续传可读的键；只有锁定告警看到它，且被截断」。

**`_alert` 的节流是「每个类别每区间一条 + 被抑制数汇总」。**

```python
last = self._alert_last.get(category)
if last is not None and now - last < self.alert_interval_seconds:
    self._alert_suppressed[category] = self._alert_suppressed.get(category, 0) + 1
    return
suppressed = self._alert_suppressed.pop(category, 0)
self._alert_last[category] = now
if suppressed:
    message += f" ({suppressed} more since last alert)"
logger.warning(message)
```

两个设计点：

1. **按类别分别节流** ——`lockout` 与 `table_full` 各自独立计数，所以一个类别刷屏不会掩盖另一个。
2. **被抑制的数量在下一条里报告** ——所以「抑制」不是「丢弃」，信息在下一个窗口里补上。

第 2 点让节流不损失信息量：攻击持续 10 分钟，日志里有 10 条（每分一条）且每条带「上次以来又有 N 次」。若直接丢弃，运维会以为只发生了 10 次。

**`_live_entry` 的「读时清理」让空闲键不积累。**

```python
entry = self._entries.get(slot)
if entry is None: return None
self._prune(entry, now)
if not entry.failures and entry.pending == 0:
    del self._entries[slot]
    return None
return entry
```

所以「清理」发生在**读时**而不是定时任务里。这是一个常见但需要说明的选择：没有后台任务意味着没有定时器的生命周期问题，代价是「永不访问的键会一直留着」——而 `_make_room` 会处理那些（因为它会尝试驱逐队首）。

**`BodyLimitMiddleware` 的三档上限对应「路由的合法体积差几个数量级」。**

```python
def _limit_for(self, scope):
    route = get_route_path(scope, self._api_prefix)
    if route == UPLOAD_PATH: return self._upload_limit
    if route in INGEST_TEXT_PATHS: return self._ingest_limit
    return self._default_limit
```

三档的理由在 docstring 里各有一句：

- `/documents/upload` 的上限**从 `MAX_UPLOAD_SIZE` 推导**，加上 multipart 封装的余量。
- `/documents/text` 与 `/documents/texts` 拿到**宽松的摄取上限**，因为「一段粘贴的文档或一次批量插入不是一个聊天回合，而批量在上传路由上没有对等物」。
- **其他一切走默认值，「那才是真正约束未鉴权表面的东西」**。

第三句是这套分层的目的：**默认值必须小**（因为未鉴权路由用它），而需要大的路由必须被显式列出。这与「一个全局上限」相反——后者的值必然被最大的那个用例撑大，于是未鉴权路由也拿到那个大值。

**`__call__` 里有两道检查，而第二道才是真正的执行。**

第一道是「诚实的 Content-Length」：

```python
declared = header_value(scope, b"content-length")
if declared and declared.isdigit() and int(declared) > limit:
    await send_json(send, 413, self._detail(limit))
    return
```

注释：「一个诚实的客户端让拒绝发生而**完全不碰 `receive()`**，所以请求体从不离开客户端，而 uvicorn 从不发那个会邀请它的 『100 Continue』」——「下面的计数包装才是真正的执行」。

所以第一道是**优化而不是保护**：它只在客户端诚实时报错。而它对 `Expect: 100-continue` 的效果是具体的——如果不提前拒，服务器会先发 100 Continue，客户端才开始传 8 MiB，然后才被拒。

第二道用 `limited_receive(receive, limit)` 包装——这是真正的执行，因为它累计实际收到的字节。

**`BodyLimitMiddleware` 对方法的选择有一个反直觉的决定：不限 POST。**

```python
# Applied to every method rather than POST only: a body on GET or DELETE is unusual but legal,
# and "unusual" is exactly where an unbounded read would otherwise survive. Methods that carry
# no body make this a no-op.
```

这条很短但很对。「只检查 POST」是一个很自然的实现，而它的漏洞是「DELETE 带 body」这种少见但合法的情况——攻击者只需要找到一种不被检查的方法即可。

**`response_started` 的跟踪解决了「413 已经太晚」的问题。**

```python
async def _tracking_send(message):
    nonlocal response_started
    if message.get("type") == "http.response.start":
        response_started = True
    await send(message)
...
except BodyLimitExceeded as exceeded:
    logger.warning(...)
    if not response_started:
        await send_json(send, 413, self._detail(limit))
```

注释：「否则应用已经承诺了一个状态行；连接以一个短 body 结束，而那是协议在那时允许的全部。**没有东西被静默吞掉——上面的警告就是记录**」。

所以三种结局：没开始发送 → 发 413；已开始 → 只能断连，但**留下 WARNING 日志**；无论哪种都有日志。

这条是流式响应的必然结果：一旦第一个字节发出去，状态行就固定了。而正确做法不是「装作能改」，而是「明确记录发生了什么」。

**`resolve_body_limits` 里最重要的判据是「值是否被显式配置，绝不靠与默认值比较来推断」。**

```python
configured = bool(getattr(args, "max_request_body_bytes_explicit", True))
ingest_limit = default_limit if configured else DEFAULT_MAX_INGEST_BODY_BYTES
```

docstring：

> 一个值是否被配置是从 `args.max_request_body_bytes_explicit` 读的，**故意不通过与默认值比较来推断**：一个故意把值设成默认值的运维，与一个什么都没设的运维将无法区分，而摄取路由会**静默地拿到 50 MiB 档而不是所要求的上限——放宽一条已配置的安全边界**。

这个论证很精确：`max_request_body_bytes == DEFAULT` 有两种归因（显式设成默认 / 没设），而两者想要的摄取上限不同。所以必须有一个额外的标志来区分。

而「猜测的默认方向」也有理由：

> 一个手工构造 `args` 并省略这个标志的调用方被当作**已配置**。往那个方向猜错代价是一次大摄取上的 413，那是可见且可恢复的；**往另一个方向猜错会静默地放宽一个上限，而那正是这个参数存在要防止的失败。**

所以默认值是 `True`（当作已配置），即选「更严」的那一侧。这与 `_PairingGate` 里「新状态只会抑制拒绝」是同一类推理：**默认值应该落在安全侧**。

**四种配置语义被逐条列出，而不是留给读者推导。**

```
* MAX_REQUEST_BODY_BYTES = 0 ——显式弃用，所有上限都关，包括推导出的上传上限。
* 未配置 ——普通路由拿默认，摄取路由拿内置的摄取上限。
* 配置成任何正值 ——运维的意图在每一个非上传路由上胜出，摄取也算。
* MAX_UPLOAD_SIZE 为 0/None 意味着「无限制上传」，一个有文档的设置；那就没有尺寸可以推导，所以上传路由不设上限。
```

四条覆盖了所有组合。而第一条特别值得注意：`0` 是「全关」而不是「用默认」——所以「显式写 0」与「不写」行为不同。这与 `configured` 标志一起构成了「三态」：未设 / 设成 0 / 设成正数。

**`AdmissionTicket` 是这一页最有结构美感的部分，因为它要跨三个所有者传递一次预留。**

模块 docstring 先说清为什么需要它：

> 容量决策必须发生在第一次 `receive()` 之前——一个会被拒绝的请求绝不能读过它的请求体——但它取的预留必须一直持有到文档在 `doc_status` 里可见，而那发生在响应发出很久之后的背景任务里。**票就是那次预留如何在三个所有者之间传递的方式**：
>
> ASGI 中间件 → 端点采纳 → 托管后台任务 → 释放

而「任何时刻恰好一个所有者负责」这条被写成了三个互斥的情形：

```
* 未被采纳（路由之前就拒了，或路由根本没跑）——中间件在它的 finally 里释放它；
* 已采纳但未移交（校验拒绝、流式失败）——端点在它的 finally 里释放它；
* 已移交——后台任务的 finally / 兜底释放它。
```

所以释放责任是**按状态转移分配**的，而不是「谁申请谁释放」。这是必须的，因为预留的生命期**超过**了申请它的那一层：中间件的 `__call__` 在响应发出时就返回了，而预留要活到文档可见。

而 `adopted` 这个布尔就是这个责任指针：

```python
@dataclass
class AdmissionTicket:
    token: str
    weight: int = 1
    adopted: bool = False
```

`adopt_admission_ticket` 把它翻成 `True`：

```python
ticket.adopted = True
return ticket
```

所以中间件的 `finally` 读这个字段决定是否释放——「标记为已采纳，好让中间件的 `finally` 不管它」。

而「返回 HTTP 响应绝不能释放」这条被单独强调：

> 返回 HTTP 响应**绝不能**释放：客户端已经被告知文档被接受了，所以预留一直计费到它落地为止。

这是一条容易违反的规则，因为「请求结束」与「工作结束」在这里是分离的。若在响应返回时释放，那容纳量就不再等于在飞的文档数——于是可以无限接单。

**`adopt_admission_ticket` 的 `None` 返回是一条重要的降级路径。**

```python
state = getattr(request, "state", None)
ticket = getattr(state, ADMISSION_STATE_KEY, None) if state is not None else None
if not isinstance(ticket, AdmissionTicket):
    return None
ticket.adopted = True
return ticket
```

docstring：「返回票（现在标记为已采纳）或 `None` 当这个请求没有中间件跑过时——**路由随后自己取一次预留，正如之前那样，而这正是『中间件未安装（容量关闭）』或『路由被测试装置直接调用』时准入仍然正确的原因**」。

所以「没有票」不等于「没有容量控制」，而是「这一层不做决定，下一层会做」。`isinstance` 检查而不是「非 None」检查也重要：一个畸形值不会被当成票。

而模块的 `__init__` 也只 import 了 `dataclass` 与 `typing`：

> 这个模块是**故意无依赖的**，好让中间件与文档路由都能 import 它而不产生环。

这是「共享小类型放在一个无依赖模块」的常见手法，而它在这里的必要性很具体：中间件与路由互相不能 import，而票要穿过两者。

## 关键取舍

**源码事实：** `TOKEN_SECRET` 缺失时：配了 `AUTH_ACCOUNTS` → **拒绝启动**；没配 → 退到默认 guest 密钥并 WARNING。

**源码事实：** `JWT_ALGORITHM` 为 `none` 在**配置期**就拒；`validate_token` 解码前再检查一次并报 500 而不是 401，因为那是服务端配置错误。

**源码事实：** `MAX_TOKEN_SUBJECT_LENGTH = 256` 同时约束配置期、签发与校验三处；只在 `validate_token` 限长会让一个超长用户名「能登录但此后每个请求都 401」——「一个不可用的账号和一个令人困惑的症状」。

**源码事实：** 配置错误的日志只记用户名**长度**不记内容，因为「那条有问题的条目带着一个密码」。

**源码事实：** `verify_password` 的三条分支各跑一次 bcrypt（未知用户 1 次假、明文账号 1 真 + 1 假、bcrypt 账号 1 次真），为抹平用户枚举的时序侧信道（CWE-208）；docstring 承认「手选不同 bcrypt 成本的账号仍会有一些差异」。

**源码事实：** `LoginRateLimiter` 的**决策**用 `confirmed + pending`（防并发绕过），**告警**只用 `confirmed`（防正确密码与错误密码并存时误报「锁定」）。

**源码事实：** `retry_after` 三档：未超限 → `None`；确认失败已达上限 → 等最老失败滑出窗口；仅因在飞预约超限 → `1.0` 秒（「那些在一秒内解析完，且可能全部成功从而清掉失败计数」）。

**源码事实：** 容量满时**只驱逐死记录**，活记录永不驱逐——「否则攻击者可以通过灌入唯一键来清掉一个锁定」；新键干脆不被跟踪，并发出 `table_full` 告警说明「这些请求暂不受限速，考虑上游 WAF」。

**源码事实：** 键存成定长 SHA-256 摘要而非原文，因为「用户名是攻击者控制的，逐字存储会让每次失败保留最多一整个请求体的字节，而键数量上限在字节上什么也约束不了」。

**源码事实：** `_alert` 按类别分别节流，被抑制的数量在**下一条**里以「(N more since last alert)」补出——所以节流不丢信息量。

**源码事实：** `_make_room` 只需看队首：「键按最久未活动在前排序，所以一旦队首是活的，剩下的每一个也都是活的」——这个推理依赖每次活动都 `move_to_end`。

**源码事实：** `BodyLimitMiddleware` 装在**准入中间件外层**（超大体积在消耗容量槽之前被拒）而**在 CORS 内侧**（413 要带浏览器能读的响应头）。

**源码事实：** 上限分三档：上传从 `MAX_UPLOAD_SIZE` + multipart 余量推导、摄取路由宽松、**其余走默认——「那才是真正约束未鉴权表面的东西」**。

**源码事实：** 中间件对**每个方法**都限而不是只 POST，因为「GET 或 DELETE 上的 body 少见但合法，而『少见』正是无界读会存活的地方」。

**源码事实：** `Content-Length` 预检只是优化（「一个诚实的客户端让拒绝发生而完全不碰 `receive()`，uvicorn 从不发那个会邀请它的 100 Continue」），真正的执行是 `limited_receive` 的计数包装。

**源码事实：** `response_started` 为真时无法再发 413，「连接以一个短 body 结束，而那是协议在那时允许的全部」，但**留下 WARNING 日志**——「没有东西被静默吞掉」。

**源码事实：** `resolve_body_limits` 从 `max_request_body_bytes_explicit` 读「是否配置过」，**绝不用与默认值比较来推断**——否则「故意设成默认值」与「没设」无法区分，摄取路由会静默拿到 50 MiB 档从而「放宽一条已配置的安全边界」；手工构造且省略标志的调用方被当作**已配置**（往那个方向猜错的代价是一次可见的 413，反向猜错是静默放宽）。

**源码事实：** `MAX_REQUEST_BODY_BYTES = 0` 是**显式弃用**（全关，包括推导出的上传上限），与「未配置」行为不同；`MAX_UPLOAD_SIZE` 为 0/None 是「无限制上传」，上传路由不设上限。

**源码事实：** `AdmissionTicket` 的释放责任按**状态转移**分配（未被采纳 / 已采纳未移交 / 已移交），由 `adopted` 布尔充当责任指针；「返回 HTTP 响应绝不能释放——客户端已被告知文档被接受，所以预留一直计费到它落地」。

**源码事实：** `adopt_admission_ticket` 返回 `None` 时路由自己取预留——这保证「中间件未安装」或「路由被测试直接调用」时准入仍然正确；票的模块**故意无依赖**以便中间件与路由都能 import 而不成环。

**架构推论：** 四层的顺序是一个**成本递增的漏斗**：先拒签名（微秒）、再拒体积（毫秒，不读 body）、再拒容量（不占槽）、最后才排队。每一层的判据都比下一层便宜，所以顺序不是随意的——**把贵的检查放在便宜的后面，就能让攻击者用最少的资源被挡住**。这也解释了为什么 `Content-Length` 预检值得写：它让一次 8 MiB 的攻击只花服务器几个字节。

**架构推论：** 「决策用保守视图、告警用精确视图」这个不对称可以推广到任何「有未决状态的限流器」。决策要防绕过，所以必须把未决算进去；告警要防误报，所以必须只看确定事实。**两个用途从同一个数据结构读，但读的投影不同**——混淆它们会产生两种相反的错误：要么可被并发绕过，要么在正常并发下误报警。

**架构推论：** 这一层反复出现的模式是「**把不变量的两端放在一个常量里**」：`MAX_TOKEN_SUBJECT_LENGTH` 同时约束配置、签发、校验三处。三处独立实现是这类 bug 的温床——每处看起来都合理，而组合起来产生「能登录但永远 401」这种最难查的症状。判据是：**这个界限是否在两个地方的边界上？** 是的话，两处必须引用同一个符号。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lightrag/api/auth.py` | `AuthHandler` | 启动期配置校验 + 登录校验 + 令牌签发/校验 |
| `lightrag/api/auth.py` | `TokenPayload` | `sub` / `exp` / `role` / `metadata` 四字段模型 |
| `lightrag/api/auth.py` | `_DUMMY_VERIFY_SPEC` | 只用于抹平时序的假 bcrypt 规格，非机密 |
| `lightrag/api/auth.py` | `MAX_TOKEN_SUBJECT_LENGTH` | 一个常量约束配置/签发/校验三处 |
| `lightrag/api/login_rate_limit.py` | `LoginRateLimiter` | 滑动窗口 + 在飞预约的登录限速 |
| `lightrag/api/login_rate_limit.py` | `_Entry` | `failures` 队列与 `pending` 计数，两类状态不混 |
| `lightrag/api/login_rate_limit.py` | `retry_after` | 三档等待，区分「确认锁定」与「在飞阻塞」 |
| `lightrag/api/login_rate_limit.py` | `_make_room` | 只驱逐死记录，队首活即全活 |
| `lightrag/api/login_rate_limit.py` | `_slot` | 定长 SHA-256 摘要作索键 |
| `lightrag/api/login_rate_limit.py` | `_alert` | 分类节流 + 抑制数汇总 |
| `lightrag/api/login_rate_limit.py` | `reserve_attempt` | bcrypt 之前取预约，计数入限 |
| `lightrag/api/admission.py` | `AdmissionTicket` | 跨三层传递的容量预留与责任指针 |
| `lightrag/api/admission.py` | `publish_admission_ticket` | 写进 `scope["state"]`，路由经 `request.state` 读 |
| `lightrag/api/admission.py` | `adopt_admission_ticket` | 认领并置位，`None` 让路由自己取 |
| `lightrag/api/body_limit_middleware.py` | `BodyLimitMiddleware` | 三档上限的 ASGI 中间件 |
| `lightrag/api/body_limit_middleware.py` | `resolve_body_limits` | 从配置推导三档，靠显式标志判「是否配置」 |
| `lightrag/api/body_limit_middleware.py` | `INGEST_TEXT_PATHS` | 需要宽松上限的摄取路由白名单 |
| `lightrag/api/body_limit_middleware.py` | `_limit_for` | 按路由选档 |

## 参考

- [lightrag 源码：api/auth.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/api/auth.py)
- [lightrag 源码：api/login_rate_limit.py](https://github.com/HKUDS/LightRAG/blob/main/lightrag/api/login_rate_limit.py)
- [CWE-307：登录尝试限制不当](https://cwe.mitre.org/data/definitions/307.html)
- [CWE-208：可观测的时间差异](https://cwe.mitre.org/data/definitions/208.html)

## 与主核 / AI 的关系

主核里最接近的是**连接认证 + 每用户连接数限制 + 每查询资源配额**这三层。数据库的 `pg_hba.conf` 决定谁能连，`max_connections` 决定多少个，`statement_timeout` / `work_mem` 决定单个查询能花多少——而三层的顺序也是成本递增：认证最便宜（一次哈希），连接槽次之（一个内存结构），资源配额最后（实际执行时才用）。

**差别在于「预留的生命期」。** 数据库里一个连接的生命期与一次查询基本重合，所以「谁申请谁释放」是对的。而这里的 `AdmissionTicket` 要跨越「请求返回」这个边界——文档被接受了，但实际写入还没完成。所以释放责任必须按状态转移分配，而不是按调用栈。

这与 g01 的 `JsonDocStatusStorage` 那条「upsert 同步刷盘因为它是恢复锚点」是同一种「生命期错配」问题的另一面：那边是「写入必须比调用活得久」，这边是「预留必须比响应活得久」。两者都在处理「用户看到的完成」与「系统实际的完成」之间的差。

与 i03（存储锚点）的关系是「锁」的两种用法：那边是文件锁（跨进程），这边是内存计数器（进程内）。而 `LoginRateLimiter` 的 docstring 也明确了这个边界：「状态是内存内且每进程的……在 gunicorn N worker 下有效上限是 N × `max_attempts`，那仍然能挡住字典爆破。要严格的跨 worker 强制就用共享存储（例如 Redis）」。

对 AI 工程最实用的一条是 **`resolve_body_limits` 那个「显式标志」**。任何「配置项有默认值」的系统都会遇到同一个问题：值等于默认值时无法区分「设过」与「没设」。而多数实现靠比较值来猜，代价是「用户显式设成默认值」时行为与预期不同。正确做法是让配置解析器记录「这个键出现过」这个事实——一个布尔，成本极低，而它能防止一整类「静默放宽安全边界」的问题。

另一条是 `verify_password` 那个时序抹平。任何「用户名/令牌存在与否」会改变响应耗时的认证实现都有用户枚举风险，而修法很便宜（固定跑一次哈希）。而它的**不彻底**也值得学：docstring 明说手选成本的账号仍有差异——**承认残留比假装解决了更有用**，因为它告诉后续维护者「这里还有一个可利用的通道」。

最后一条是 `_make_room` 与 `release` 之间那条隐含契约。`_make_room` 的正确性依赖「`release` 会立刻删除空条目」，而这个依赖只写在 `release` 的 docstring 里。这类「一个函数的正确性依赖另一个函数的行为」的耦合是维护中的隐患——若有人给 `release` 加一句「保留条目以免频繁重建」，`_make_room` 就会在满表时错误地认为「全活」而停止驱逐。这种耦合值得写进测试而不是只写进注释。
