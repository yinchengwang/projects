/**
 * seed-data.js —— Knowledge Hub 的「内容引擎」种子数据
 *
 * 原 Taro 版的 `@/data/*`（quiz 题库 / learn_deep 正文 / radar-data /
 * five_year_plan_config / interview_bank / initialExcerpts）在本环境快照中缺失，
 * 这里提供一份**结构一致、规模精简**的代表性数据，足以驱动全部 17 个页面的功能：
 *   - 题库覆盖 7 个技术栈、5 种题型、多档难度
 *   - learn_deep 提供按「栈 / 象限」组织的真实 Markdown 文章
 *   - 技术雷达 / 五年计划 / 面试题库 / 摘抄 均有演示数据
 *
 * 字段形状严格对齐 design doc 与数据模型清单，原生页面可直接消费。
 */

const STACKS = ['c', 'cpp', 'db', 'ds', 'linux', 'py', 'vdb']

/* ============ 1. 题库（按 stack 分组） ============ */
/* 题型 type：选择题 / 判断题 / 填空题 / 简答题 / 编程题
 * 判分约定：选择题 answer 为选项字母，options 形如 ['A. ...','B. ...']；
 *           判断题 answer 为布尔；填空/简答为文本（精确匹配，忽略首尾空格）。 */

const QUIZ_BANK = [
  // ---- C ----
  {
    id: 'q-c-1', stack: 'c', type: '选择题', difficulty: '入门', quadrant: 'language', ring: 'basic',
    scenario: '语言基础', stem: '在 C 语言中，以下哪个关键字用于声明一个只读变量？',
    options: ['A. const', 'B. static', 'C. volatile', 'D. register'], answer: 'A',
    explanation: 'const 声明只读变量；static 控制存储期/链接，volatile 提示可能被意外修改，register 建议存寄存器。'
  },
  {
    id: 'q-c-2', stack: 'c', type: '判断题', difficulty: '初级', quadrant: 'language', ring: 'basic',
    scenario: '内存', stem: 'C 语言中，未初始化的全局变量默认值为 0。',
    options: null, answer: true,
    explanation: '全局/静态变量在 BSS 段，由系统清零；局部变量不初始化则是随机值。'
  },
  {
    id: 'q-c-3', stack: 'c', type: '填空题', difficulty: '中级', quadrant: 'language', ring: 'intermediate',
    scenario: '指针', stem: '设 int a[3]={1,2,3}; 则 *(a+1) 的值为 ____。',
    options: null, answer: '2',
    explanation: '数组名 a 退化为指向首元素的指针，a+1 指向第二个元素，解引用得 2。'
  },
  // ---- C++ ----
  {
    id: 'q-cpp-1', stack: 'cpp', type: '选择题', difficulty: '中级', quadrant: 'language', ring: 'intermediate',
    scenario: '智能指针', stem: 'C++ 中用于「独占所有权」的智能指针是？',
    options: ['A. shared_ptr', 'B. unique_ptr', 'C. weak_ptr', 'D. auto_ptr'], answer: 'B',
    explanation: 'unique_ptr 独占所有权，不可拷贝只能移动；shared_ptr 引用计数；weak_ptr 弱引用避免环。'
  },
  {
    id: 'q-cpp-2', stack: 'cpp', type: '判断题', difficulty: '初级', quadrant: 'language', ring: 'basic',
    scenario: '面向对象', stem: 'C++ 中构造函数可以声明为虚函数（virtual）。',
    options: null, answer: false,
    explanation: '构造函数不能虚：虚表指针在构造期间才建立，且虚函数靠对象调用，构造时对象尚未完全形成。'
  },
  {
    id: 'q-cpp-3', stack: 'cpp', type: '简答题', difficulty: '高级', quadrant: 'language', ring: 'advanced',
    scenario: '内存管理', stem: '简述 C++ 中浅拷贝与深拷贝的区别，并说明何时需要自定义拷贝构造。',
    options: null, answer: '浅拷贝仅复制指针成员的值（地址），深拷贝复制指针指向的内容。当类持有动态资源（堆内存、文件句柄）时必须自定义拷贝构造与赋值运算符，否则多个对象共享同一资源导致双重释放。',
    explanation: '考察 Rule of Three/Five 的基本理解。'
  },
  // ---- 数据库 ----
  {
    id: 'q-db-1', stack: 'db', type: '选择题', difficulty: '初级', quadrant: 'systems', ring: 'basic',
    scenario: '索引', stem: 'B-Tree 索引最适合哪种查询？',
    options: ['A. 仅等值查询', 'B. 仅模糊匹配', 'C. 等值与前缀/范围查询', 'D. 全文检索'], answer: 'C',
    explanation: 'B-Tree 有序，支持等值、范围与最左前缀；全文检索用倒排索引。'
  },
  {
    id: 'q-db-2', stack: 'db', type: '判断题', difficulty: '中级', quadrant: 'systems', ring: 'intermediate',
    scenario: '事务', stem: '数据库事务的隔离级别「可重复读」能完全避免幻读。',
    options: null, answer: false,
    explanation: '标准下「可重复读」不能避免幻读；MySQL InnoDB 通过 MVCC+间隙锁在可重复读级别避免了幻读，但这是实现细节而非标准保证。'
  },
  {
    id: 'q-db-3', stack: 'db', type: '填空题', difficulty: '中级', quadrant: 'systems', ring: 'intermediate',
    scenario: '范式', stem: '消除了非主属性对候选键的传递依赖，达到第 ____ 范式。',
    options: null, answer: '三',
    explanation: '1NF 原子；2NF 消除部分依赖；3NF 消除传递依赖。'
  },
  // ---- 数据结构 ----
  {
    id: 'q-ds-1', stack: 'ds', type: '选择题', difficulty: '中级', quadrant: 'algorithms', ring: 'intermediate',
    scenario: '复杂度', stem: '在平均情况下，哈希表查找的时间复杂度是？',
    options: ['A. O(1)', 'B. O(log n)', 'C. O(n)', 'D. O(n log n)'], answer: 'A',
    explanation: '理想哈希函数下查找为 O(1)，最坏（大量冲突）退化为 O(n)。'
  },
  {
    id: 'q-ds-2', stack: 'ds', type: '判断题', difficulty: '入门', quadrant: 'algorithms', ring: 'basic',
    scenario: '栈', stem: '用两个栈可以实现一个队列。',
    options: null, answer: true,
    explanation: 'stack-in 负责入队，stack-out 负责出队；出队时若 out 空则把 in 整体倒入 out。'
  },
  {
    id: 'q-ds-3', stack: 'ds', type: '编程题', difficulty: '高级', quadrant: 'algorithms', ring: 'advanced',
    scenario: '链表', stem: '反转单链表，返回新头节点。',
    options: null,
    answer: 'function reverse(head){let p=null;while(head){const n=head.next;head.next=p;p=head;head=n;}return p;}',
    explanation: '三指针迭代，时间 O(n) 空间 O(1)。'
  },
  // ---- Linux ----
  {
    id: 'q-linux-1', stack: 'linux', type: '选择题', difficulty: '初级', quadrant: 'systems', ring: 'basic',
    scenario: '权限', stem: '权限字符串 rwxr-xr-- 对应的八进制是？',
    options: ['A. 754', 'B. 751', 'C. 752', 'D. 750'], answer: 'A',
    explanation: 'rwx=7, r-x=5, r--=4 → 754。'
  },
  {
    id: 'q-linux-2', stack: 'linux', type: '判断题', difficulty: '中级', quadrant: 'systems', ring: 'intermediate',
    scenario: '进程', stem: 'fork() 之后，父子进程共享同一块虚拟地址空间。',
    options: null, answer: false,
    explanation: 'fork 后子进程获得父进程地址空间的副本（写时复制），并非共享。'
  },
  {
    id: 'q-linux-3', stack: 'linux', type: '填空题', difficulty: '初级', quadrant: 'systems', ring: 'basic',
    scenario: '命令', stem: '查看当前目录下各文件大小的命令是 ____ -h。',
    options: null, answer: 'du',
    explanation: 'du 看磁盘用量，df 看文件系统剩余；常配合 -h 人类可读。'
  },
  // ---- Python ----
  {
    id: 'q-py-1', stack: 'py', type: '选择题', difficulty: '入门', quadrant: 'language', ring: 'basic',
    scenario: '语法', stem: '以下哪个不是 Python 的内置数据类型？',
    options: ['A. list', 'B. dict', 'C. tuple', 'D. array（需 import array）'], answer: 'D',
    explanation: 'array 属于标准库模块 array，并非内置类型；list/dict/tuple/set 为内置。'
  },
  {
    id: 'q-py-2', stack: 'py', type: '判断题', difficulty: '初级', quadrant: 'language', ring: 'basic',
    scenario: '可变对象', stem: 'Python 中列表（list）是可变对象，作为函数默认参数可能导致意外共享。',
    options: null, answer: true,
    explanation: '默认参数在函数定义时求值一次，可变默认参会在多次调用间共享，应避免。'
  },
  {
    id: 'q-py-3', stack: 'py', type: '简答题', difficulty: '中级', quadrant: 'language', ring: 'intermediate',
    scenario: '装饰器', stem: '简述 Python 装饰器的作用与基本实现方式。',
    options: null,
    answer: '装饰器是高阶函数，接收函数并返回新函数，用于在不动原函数的前提下增强其行为（如日志、鉴权）。基本写法用 @wrapper 语法糖，等价于 f = wrapper(f)。',
    explanation: '考察对闭包与函数一等公民的理解。'
  },
  // ---- 向量数据库 ----
  {
    id: 'q-vdb-1', stack: 'vdb', type: '选择题', difficulty: '中级', quadrant: 'engineering', ring: 'intermediate',
    scenario: '检索', stem: '向量检索中衡量相似度最常用的是？',
    options: ['A. 余弦相似度', 'B. 编辑距离', 'C. Jaccard', 'D. 汉明距离'], answer: 'A',
    explanation: '向量语义检索通常用余弦相似度/内积；编辑距离用于字符串，Jaccard 用于集合。'
  },
  {
    id: 'q-vdb-2', stack: 'vdb', type: '判断题', difficulty: '入门', quadrant: 'engineering', ring: 'basic',
    scenario: '概念', stem: '近似最近邻（ANN）检索以少量精度换取大幅加速。',
    options: null, answer: true,
    explanation: 'HNSW/IVF 等 ANN 算法牺牲少量召回换取查询速度，适合海量向量。'
  },
  {
    id: 'q-vdb-3', stack: 'vdb', type: '填空题', difficulty: '高级', quadrant: 'engineering', ring: 'advanced',
    scenario: '索引', stem: 'HNSW 是建立在 ____ 图（Hierarchical Navigable Small World）上的索引结构。',
    options: null, answer: '小世界',
    explanation: 'HNSW 利用小世界网络的多层图结构做高效近似检索。'
  }
]

/* ============ 2. learn_deep 图解文章 ============ */
/* 字段：slug / stack / quadrant / title / md（数组 join('\n')，保留 ``` 代码块） */

const LEARN_DEEP = [
  {
    slug: 'c-pointers', stack: 'c', quadrant: 'language', title: 'C 指针三步法',
    md: [
      '# C 指针三步法',
      '',
      '指针是 C 的精髓，也是 bug 温床。记住三步即可破局：',
      '',
      '1. **定义**：`int *p;` 表示 p 是指向 int 的指针',
      '2. **取址**：`p = &a;` 让 p 指向 a',
      '3. **解引用**：`*p = 10;` 修改 p 指向的值',
      '',
      '```c',
      'int a = 5;',
      'int *p = &a;',
      '*p = 10;   // a 现在等于 10',
      '```',
      '',
      '> 口诀：指针存地址，解引用见真身。'
    ].join('\n')
  },
  {
    slug: 'cpp-smart-ptr', stack: 'cpp', quadrant: 'language', title: '智能指针三兄弟',
    md: [
      '# 智能指针三兄弟',
      '',
      '- **unique_ptr**：独占，不可拷贝，可移动',
      '- **shared_ptr**：引用计数，可共享',
      '- **weak_ptr**：弱引用，打破循环',
      '',
      '```cpp',
      'auto p = std::make_shared<Widget>();',
      'std::weak_ptr<Widget> w = p;',
      '```',
      '',
      '使用 `make_shared` 可一次性分配控制块与对象，更安全高效。'
    ].join('\n')
  },
  {
    slug: 'db-index', stack: 'db', quadrant: 'systems', title: '索引为什么快',
    md: [
      '# 索引为什么快',
      '',
      '索引本质是一棵有序的 B-Tree：',
      '',
      '- 等值查询：从根二分下钻，O(log n)',
      '- 范围查询：定位起点后顺序扫描叶子链表',
      '- 复合索引遵循**最左前缀**原则',
      '',
      '```sql',
      'CREATE INDEX idx_orders ON orders(user_id, created_at);',
      '```',
      '',
      '回表：二级索引只存主键，需再查聚簇索引取完整行。'
    ].join('\n')
  },
  {
    slug: 'ds-binary-tree', stack: 'ds', quadrant: 'algorithms', title: '二叉树遍历',
    md: [
      '# 二叉树遍历',
      '',
      '- 前序：根 → 左 → 右',
      '- 中序：左 → 根 → 右（BST 中序即有序）',
      '- 后序：左 → 右 → 根',
      '',
      '递归写法三行搞定：',
      '',
      '```js',
      'function inorder(n){ if(!n) return; inorder(n.left); visit(n); inorder(n.right); }',
      '```'
    ].join('\n')
  },
  {
    slug: 'linux-permissions', stack: 'linux', quadrant: 'systems', title: 'Linux 权限位图解',
    md: [
      '# Linux 权限位图解',
      '',
      'r=4 w=2 x=1，三段分别属 属主/属组/其它。',
      '',
      '- `rwxr-xr--` = 754',
      '- `chmod 754 file` 设定之',
      '',
      'x 对目录表示「可进入」，对文件表示「可执行」。'
    ].join('\n')
  },
  {
    slug: 'py-decorator', stack: 'py', quadrant: 'language', title: 'Python 装饰器',
    md: [
      '# Python 装饰器',
      '',
      '装饰器是高阶函数，接收函数返回新函数：',
      '',
      '```python',
      'def log(f):',
      '    def wrapper(*a, **k):',
      '        print("call", f.__name__)',
      '        return f(*a, **k)',
      '    return wrapper',
      '',
      '@log',
      'def add(a, b): return a + b',
      '```',
      '',
      '`@log` 等同于 `add = log(add)`。'
    ].join('\n')
  }
]

/* ============ 3. 技术雷达（书籍 / 知识点） ============ */
/* 字段：id / title / quadrant / ring / desc / tags */

const RADAR_ITEMS = [
  { id: 'r-1', title: 'C 指针与内存', quadrant: 'language', ring: 'advanced', desc: '指针运算、内存布局、段错误排查', tags: ['c'] },
  { id: 'r-2', title: 'C++ 模板与 RAII', quadrant: 'language', ring: 'advanced', desc: '模板元编程、资源管理习惯', tags: ['cpp'] },
  { id: 'r-3', title: 'SQL 优化', quadrant: 'systems', ring: 'intermediate', desc: '执行计划、索引、慢查询', tags: ['db'] },
  { id: 'r-4', title: '事务与隔离级别', quadrant: 'systems', ring: 'intermediate', desc: 'ACID、MVCC、锁', tags: ['db'] },
  { id: 'r-5', title: '排序与查找', quadrant: 'algorithms', ring: 'basic', desc: '快排、归并、二分', tags: ['ds'] },
  { id: 'r-6', title: '图算法', quadrant: 'algorithms', ring: 'advanced', desc: '最短路、拓扑、连通性', tags: ['ds'] },
  { id: 'r-7', title: 'Shell 脚本', quadrant: 'systems', ring: 'basic', desc: '管道、正则、自动化', tags: ['linux'] },
  { id: 'r-8', title: '进程与线程', quadrant: 'systems', ring: 'intermediate', desc: 'fork、调度、同步', tags: ['linux'] },
  { id: 'r-9', title: 'Python 数据结构', quadrant: 'language', ring: 'basic', desc: 'list/dict/set 的取舍', tags: ['py'] },
  { id: 'r-10', title: '并发模型', quadrant: 'language', ring: 'intermediate', desc: 'asyncio、GIL、线程池', tags: ['py'] },
  { id: 'r-11', title: '向量检索基础', quadrant: 'engineering', ring: 'intermediate', desc: 'Embedding、相似度', tags: ['vdb'] },
  { id: 'r-12', title: 'ANN 索引', quadrant: 'engineering', ring: 'advanced', desc: 'HNSW、IVF、量化', tags: ['vdb'] },
  { id: 'r-13', title: '系统设计入门', quadrant: 'engineering', ring: 'basic', desc: '负载均衡、缓存', tags: ['systems'] },
  { id: 'r-14', title: '分布式一致性', quadrant: 'engineering', ring: 'advanced', desc: 'Raft、Paxos、CAP', tags: ['systems'] },
  { id: 'r-15', title: '网络协议栈', quadrant: 'systems', ring: 'intermediate', desc: 'TCP/UDP、HTTP', tags: ['linux'] },
  { id: 'r-16', title: '正则引擎', quadrant: 'language', ring: 'intermediate', desc: 'NFA/DFA、回溯', tags: ['c'] },
  { id: 'r-17', title: '内存池', quadrant: 'engineering', ring: 'advanced', desc: '减少 malloc 开销', tags: ['cpp'] },
  { id: 'r-18', title: '位运算技巧', quadrant: 'algorithms', ring: 'intermediate', desc: '掩码、异或交换', tags: ['ds'] }
]

/* ============ 4. 五年计划配置 ============ */
/* 每年：theme / core / actions[] / extras[]；习惯项 getAllItems(year) 返回 */

const FIVE_YEAR_CONFIG = {
  2026: {
    year: 2026, theme: '夯实基础', core: '补齐语言与系统底子',
    actions: ['精读《CSAPP》', '刷完 300 道算法题', '手写一个 STL 容器'],
    extras: ['坚持每日学习打卡', '输出 12 篇图解笔记'],
    habits: [
      { id: 'h-algo', label: '算法', icon: '🧮' },
      { id: 'h-read', label: '阅读', icon: '📖' },
      { id: 'h-code', label: '编码', icon: '💻' },
      { id: 'h-english', label: '英语', icon: '🔤' }
    ]
  },
  2027: {
    year: 2027, theme: '深入系统', core: '操作系统与分布式',
    actions: ['实现 Mini-Raft', '读 TiKV 源码', '完成一个分布式 KV'],
    extras: ['参与开源项目', '技术分享 6 次'],
    habits: [
      { id: 'h-sys', label: '系统', icon: '⚙️' },
      { id: 'h-os', label: 'OS', icon: '🐧' },
      { id: 'h-read', label: '阅读', icon: '📖' },
      { id: 'h-write', label: '写作', icon: '✍️' }
    ]
  },
  2028: {
    year: 2028, theme: '工程能力', core: '从写功能到做系统',
    actions: ['主导一个中型系统', '建立可观测体系', '带教 1 名新人'],
    extras: ['考取云架构认证', '沉淀团队规范'],
    habits: [
      { id: 'h-design', label: '设计', icon: '📐' },
      { id: 'h-review', label: '评审', icon: '🔍' },
      { id: 'h-read', label: '阅读', icon: '📖' },
      { id: 'h-share', label: '分享', icon: '🎤' }
    ]
  },
  2029: {
    year: 2029, theme: '专精领域', core: '向量数据库 / AI 基础设施',
    actions: ['深入向量检索内核', '落地一个 AI 应用', '发表技术文章'],
    extras: ['构建个人技术品牌', '参加行业大会'],
    habits: [
      { id: 'h-ai', label: 'AI', icon: '🧠' },
      { id: 'h-vdb', label: '向量库', icon: '🗄️' },
      { id: 'h-read', label: '阅读', icon: '📖' },
      { id: 'h-build', label: '构建', icon: '🛠️' }
    ]
  },
  2030: {
    year: 2030, theme: '影响力', core: '从执行者到布道者',
    actions: ['成为领域 Go-To 专家', '开源项目 1k star', '带教团队'],
    extras: ['出版技术小册', '建立学习社群'],
    habits: [
      { id: 'h-mentor', label: '带教', icon: '🤝' },
      { id: 'h-open', label: '开源', icon: '🌟' },
      { id: 'h-read', label: '阅读', icon: '📖' },
      { id: 'h-talk', label: '演讲', icon: '🎙️' }
    ]
  }
}

const ALL_YEARS = [2026, 2027, 2028, 2029, 2030]

const CORE_PROJECTS = [
  { year: 2026, name: 'STL 容器库', desc: '手写 vector / list / map' },
  { year: 2027, name: 'Mini-Raft KV', desc: '基于 Raft 的分布式键值库' },
  { year: 2028, name: '可观测平台', desc: '指标 + 日志 + 链路追踪' },
  { year: 2029, name: '向量检索引擎', desc: '支持 ANN 的向量数据库' },
  { year: 2030, name: '学习雷达开源', desc: 'Reading Radar 社区版' }
]

function getYearConfig (year) {
  return FIVE_YEAR_CONFIG[year] || null
}

function getAllItems (year) {
  const cfg = FIVE_YEAR_CONFIG[year]
  return cfg ? (cfg.habits || []) : []
}

function getGroupedItems (year) {
  const items = getAllItems(year)
  const map = {}
  items.forEach(function (it) {
    const g = it.label.charAt(0)
    if (!map[g]) map[g] = []
    map[g].push(it)
  })
  return map
}

/* ============ 5. 面试题库 ============ */
/* algorithm: 按 category 分；interview: 按 source/分类分 */

const INTERVIEW_BANK = {
  algorithm: [
    {
      id: 'ib-a-1', category: '数组', question: '给定有序数组，用二分查找目标值下标，不存在返回 -1。',
      answer: [
        '# 思路',
        '维护左右闭区间，每次取中点比较，收缩边界直到相遇。',
        '',
        '```js',
        'function search(a, t){',
        '  let l=0, r=a.length-1;',
        '  while(l<=r){ const m=(l+r)>>1;',
        '    if(a[m]===t) return m;',
        '    if(a[m]<t) l=m+1; else r=m-1; }',
        '  return -1;',
        '}',
        '```'
      ].join('\n'),
      difficulty: '初级', tags: ['二分'], viewed: false, bookmarked: false
    },
    {
      id: 'ib-a-2', category: '动态规划', question: '求最长递增子序列（LIS）长度。',
      answer: [
        '# 思路',
        '定义 dp[i] 为以 a[i] 结尾的 LIS 长度，枚举此前更小的元素转移；',
        '可用二分优化到 O(n log n)。'
      ].join('\n'),
      difficulty: '中级', tags: ['DP'], viewed: false, bookmarked: false
    },
    {
      id: 'ib-a-3', category: '树', question: '判断二叉树是否对称。',
      answer: [
        '# 思路',
        '递归比较左子树的左与右子树的右、左子树的右与右子树的左是否相等。'
      ].join('\n'),
      difficulty: '中级', tags: ['树'], viewed: false, bookmarked: false
    },
    {
      id: 'ib-a-4', category: '字符串', question: '实现字符串翻转（原地）。',
      answer: [
        '# 思路',
        '双指针从两端向中间交换。'
      ].join('\n'),
      difficulty: '入门', tags: ['双指针'], viewed: false, bookmarked: false
    }
  ],
  interview: [
    {
      id: 'ib-i-1', category: '后端', question: 'HTTPS 的握手过程？为什么既安全又高效？',
      answer: [
        '# 要点',
        '1. 客户端发起，携带支持的加密套件',
        '2. 服务端返回证书（含公钥）',
        '3. 客户端验证证书，生成对称密钥用公钥加密回传',
        '4. 之后用对称密钥通信',
        '',
        '非对称加密交换密钥，对称加密传输数据，兼顾安全与性能。'
      ].join('\n'),
      difficulty: '中级', tags: ['网络', '安全'], viewed: false, bookmarked: false
    },
    {
      id: 'ib-i-2', category: '后端', question: '如何设计一个短链服务？',
      answer: [
        '# 要点',
        '- 发号器（雪花/自增）生成唯一 id，转 62 进制缩短',
        '- 存储映射，跳转用 302',
        '- 考虑高并发缓存与防遍历'
      ].join('\n'),
      difficulty: '高级', tags: ['系统设计'], viewed: false, bookmarked: false
    },
    {
      id: 'ib-i-3', category: '前端', question: '浏览器输入 URL 到页面展示经历了什么？',
      answer: [
        '# 要点',
        'DNS 解析 → TCP 连接 → HTTP 请求 → 解析 HTML → 构建 DOM/CSSOM → 渲染 → 绘制'
      ].join('\n'),
      difficulty: '初级', tags: ['网络', '渲染'], viewed: false, bookmarked: false
    },
    {
      id: 'ib-i-4', category: '数据库', question: 'MySQL 为什么用 B+Tree 而不是 B-Tree？',
      answer: [
        '# 要点',
        'B+Tree 非叶子节点不存数据，扇出更大、树更矮；叶子成链表，范围查询与全表扫描更高效。'
      ].join('\n'),
      difficulty: '中级', tags: ['数据库'], viewed: false, bookmarked: false
    }
  ]
}

/* ============ 6. 摘抄种子 ============ */

const INITIAL_EXCERPTS = [
  {
    id: 'ex-1', content: '本地优先的架构让体验稳定：先离线可用，再考虑同步。',
    book: '代码整洁之道', tags: ['arch'], note: '', favorite: true,
    createdAt: '2026-01-12', date: '2026', source: 'clean-code', status: 'read'
  },
  {
    id: 'ex-2', content: '把「记一下」的成本压到一步，比做十个整理功能更有用。',
    book: '卡片笔记法', tags: ['product'], note: '捕获优先于整理', favorite: false,
    createdAt: '2026-02-03', date: '2026', source: 'zettelkasten', status: 'mastered'
  },
  {
    id: 'ex-3', content: '复杂度不是写了多少代码，而是读代码的人要理解多少。',
    book: 'A Philosophy of Software Design', tags: ['design'], note: '', favorite: false,
    createdAt: '2026-03-18', date: '2026', source: 'tapestry', status: 'reading'
  },
  {
    id: 'ex-4', content: '调试的难度是写代码的两倍，所以如果你把代码写得尽可能巧妙，按定义你没能力调试它。',
    book: 'The Elements of Programming Style', tags: ['debug'], note: 'KISS 原则', favorite: true,
    createdAt: '2026-04-09', date: '2026', source: 'elements', status: 'read'
  }
]

module.exports = {
  STACKS: STACKS,
  QUIZ_BANK: QUIZ_BANK,
  LEARN_DEEP: LEARN_DEEP,
  RADAR_ITEMS: RADAR_ITEMS,
  FIVE_YEAR_CONFIG: FIVE_YEAR_CONFIG,
  ALL_YEARS: ALL_YEARS,
  CORE_PROJECTS: CORE_PROJECTS,
  getYearConfig: getYearConfig,
  getAllItems: getAllItems,
  getGroupedItems: getGroupedItems,
  INTERVIEW_BANK: INTERVIEW_BANK,
  INITIAL_EXCERPTS: INITIAL_EXCERPTS
}
