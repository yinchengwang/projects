"use strict"
/**
 * legacy.test.js —— 死脚本内联题目清单的静态解析
 *
 * 本套件不碰磁盘：只把真实源码片段当字符串喂给纯函数。
 */

const test = require("node:test")
const assert = require("node:assert")

const legacy = require("../core/bank/legacy.js")

// 真实片段：projects/scripts/gen-practice-data.js（第 6–23 行的工厂函数 + 数组分类）
// 逐字抄自真实文件，只截取一部分；`offer22` 那条是真实文件里就有的坏数据。
const GEN_EXCERPT = `
function lc(title, id, diff, tags) {
  return {
    title,
    platform: "leetcode",
    problemId: id,
    difficulty: diff,
    tags: Array.isArray(tags) ? tags : [tags],
  };
}
function jz(title, id, diff, tags) {
  return {
    title: "剑指 Offer " + id + ". " + title,
    platform: "nowcoder",
    problemId: "" + id,
    difficulty: diff,
    tags: Array.isArray(tags) ? tags : [tags],
  };
}

var arrayItems = [
  lc("两数之和", 1, "easy", ["哈希表"]),
  lc("移除元素", 27, "easy", ["双指针"]),
  lc("搜索插入位置", 35, "easy", ["二分查找"]),
  jz("数组中重复的数字", "JZ03", "easy", ["哈希表"]),
  lc("链表中倒数第k个节点", offer22, "easy", ["快慢指针"]),
];

var categories = [
  cat(
    "array",
    "数组",
    1,
    1,
    "📗",
    "数组是最基本的数据结构，连续内存存储、随机访问 O(1)。",
    arrayItems,
  ),
];
`

// 真实片段：projects/scripts/expand-practice.js 的 addProblems() 调用形状
const EXP_EXCERPT = `
function addProblems(catId, newItems) {
  var cat = cats.filter(function (c) {
    return c.id === catId;
  })[0];
}

addProblems("two-pointers", [
  lc("三数之和", 15, "medium", ["排序", "双指针"]),
  lc("两数之和 II", 167, "easy", ["对撞指针"]),
  jz("合并两个排序的链表", "JZ25", "easy", ["链表"]),
]);
`

// 真实片段：projects/scripts/split-practice-data.js —— 它没有内联清单
const SPLIT_EXCERPT = `
var SRC = path.resolve(
  __dirname,
  "..",
  "project",
  "reading-radar",
  "data",
  "app",
  "practice-data.js",
);
var src = fs.readFileSync(SRC, "utf-8");
`

test("cat() 形状：解析出分类与题目，跳过未定义标识符的题号", function () {
  const parsed = legacy.parseLegacyCategories(GEN_EXCERPT)
  assert.strictEqual(parsed.categories.length, 1)
  assert.strictEqual(parsed.categories[0].slug, "array")
  assert.strictEqual(parsed.categories[0].title, "数组")
  // 5 条里 1 条题号是未定义标识符（offer22），必须跳过
  assert.strictEqual(parsed.categories[0].items.length, 4)
  assert.strictEqual(parsed.warnings.length, 1)
  assert.ok(parsed.warnings[0].indexOf("offer22") !== -1, parsed.warnings[0])
})

test("题目条目字段与契约一致：platform/problemId/title/difficulty/tags/repeat", function () {
  const parsed = legacy.parseLegacyCategories(GEN_EXCERPT)
  const first = parsed.categories[0].items[0]
  assert.deepStrictEqual(Object.keys(first).sort(), [
    "difficulty",
    "platform",
    "problemId",
    "repeat",
    "tags",
    "title",
  ])
  assert.strictEqual(first.platform, "leetcode")
  assert.strictEqual(first.problemId, 1)
  assert.strictEqual(first.title, "两数之和")
  assert.strictEqual(first.difficulty, "easy")
  assert.deepStrictEqual(first.tags, ["哈希表"])
  assert.strictEqual(first.repeat, false)
})

test("jz() 的题号取末尾数字，标题按运行时的样子还原前缀", function () {
  const parsed = legacy.parseLegacyCategories(GEN_EXCERPT)
  const jzItem = parsed.categories[0].items.filter(function (i) {
    return i.platform === "nowcoder"
  })[0]
  assert.strictEqual(jzItem.problemId, 3)
  assert.strictEqual(jzItem.title, "剑指 Offer JZ03. 数组中重复的数字")
})

test("addProblems() 形状：解析出分类与题目", function () {
  const parsed = legacy.parseLegacyCategories(EXP_EXCERPT)
  assert.strictEqual(parsed.categories.length, 1)
  assert.strictEqual(parsed.categories[0].slug, "two-pointers")
  assert.strictEqual(parsed.categories[0].items.length, 3)
  assert.strictEqual(parsed.categories[0].items[0].problemId, 15)
})

test("没有内联清单的脚本：0 个分类 + 1 条告警，不算错误", function () {
  const parsed = legacy.parseLegacyCategories(SPLIT_EXCERPT)
  assert.strictEqual(parsed.categories.length, 0)
  assert.strictEqual(parsed.warnings.length, 1)
  assert.ok(parsed.warnings[0].indexOf("没有找到内联题目清单") !== -1, parsed.warnings[0])
})

test("parseLegacyPracticeData 返回扁平题目清单（契约形状）", function () {
  const parsed = legacy.parseLegacyPracticeData(GEN_EXCERPT)
  assert.strictEqual(parsed.items.length, 4)
  assert.strictEqual(parsed.items[0].platform, "leetcode")
  assert.ok(Array.isArray(parsed.items[0].tags))
})

test("legacyIdOf 用 M1 的 makeId 生成稳定 ID", function () {
  assert.strictEqual(
    legacy.legacyIdOf({ platform: "leetcode", problemId: 1 }),
    "lc-00001"
  )
  assert.strictEqual(
    legacy.legacyIdOf({ platform: "leetcode", problemId: 1455 }),
    "lc-01455"
  )
  assert.strictEqual(
    legacy.legacyIdOf({ platform: "nowcoder", problemId: 3 }),
    "nc-00003"
  )
})

test("mapCategoryToTrack 覆盖 25 个 slug，未知 slug 落到 interview", function () {
  assert.strictEqual(Object.keys(legacy.CATEGORY_TRACK_MAP).length, 25)
  assert.strictEqual(legacy.mapCategoryToTrack("array"), "fundamentals")
  assert.strictEqual(legacy.mapCategoryToTrack("dp-advanced"), "advanced")
  assert.strictEqual(legacy.mapCategoryToTrack("interview-classic"), "interview")
  assert.strictEqual(legacy.mapCategoryToTrack("根本没有这个分类"), "interview")
  assert.strictEqual(legacy.FALLBACK_TRACK, "interview")
})

test("标签归一化：别名表生效，没命中的原样保留", function () {
  assert.strictEqual(legacy.normalizeLegacyTopic("哈希"), "哈希表")
  assert.strictEqual(legacy.normalizeLegacyTopic("DP"), "动态规划")
  assert.strictEqual(legacy.normalizeLegacyTopic("二分"), "二分查找")
  assert.strictEqual(legacy.normalizeLegacyTopic("  双指针 "), "双指针")
  assert.deepStrictEqual(legacy.normalizeLegacyTopics(["哈希", "哈希表", "双指针"]), ["哈希表", "双指针"])
})
