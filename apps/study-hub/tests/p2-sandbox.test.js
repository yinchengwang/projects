"use strict"
/**
 * p2-sandbox.test.js —— 沙箱增补：拷进沙箱 / 产物读取 / 超时硬上限
 *
 * 这一层守住两件事：
 *   1. 判题的一切执行都在临时目录里跑，learning 仓工作树里一个文件都不留；
 *   2. 产物文件不许被读出沙箱目录（不能 ../ 逃逸，也不能读绝对路径）。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const sandbox = require("../judge/sandbox.js")

function tmpDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix))
}

test("MAX_TIMEOUT_MS 是 60000", function () {
  assert.strictEqual(sandbox.MAX_TIMEOUT_MS, 60000)
})

test("clampTimeoutMs：正常值原样返回", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(1500), 1500)
})

test("clampTimeoutMs：超上限夹到 60000", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(999999), 60000)
})

test("clampTimeoutMs：非法值退化为默认超时", function () {
  assert.strictEqual(sandbox.clampTimeoutMs(0), sandbox.DEFAULT_TIMEOUT_MS)
  assert.strictEqual(sandbox.clampTimeoutMs(-5), sandbox.DEFAULT_TIMEOUT_MS)
  assert.strictEqual(sandbox.clampTimeoutMs("abc"), sandbox.DEFAULT_TIMEOUT_MS)
})

test("isSafeRelPath：正常相对路径合法", function () {
  assert.strictEqual(sandbox.isSafeRelPath("main.py"), true)
  assert.strictEqual(sandbox.isSafeRelPath("out/result.txt"), true)
})

test("isSafeRelPath：越界路径一律非法", function () {
  assert.strictEqual(sandbox.isSafeRelPath(""), false)
  assert.strictEqual(sandbox.isSafeRelPath("../etc/passwd"), false)
  assert.strictEqual(sandbox.isSafeRelPath("a/../../b"), false)
  assert.strictEqual(sandbox.isSafeRelPath("/etc/passwd"), false)
  assert.strictEqual(sandbox.isSafeRelPath("C:\\Windows\\x"), false)
})

test("copyIntoSandbox：复制文件并返回绝对路径", function () {
  const src = tmpDir("p2-src-")
  const from = path.join(src, "main.py")
  fs.writeFileSync(from, "print('hi')\n", "utf8")
  const box = sandbox.createSandbox()
  try {
    const target = sandbox.copyIntoSandbox(box, from, "demo/main.py")
    assert.strictEqual(fs.readFileSync(target, "utf8"), "print('hi')\n")
    assert.ok(target.startsWith(box.dir), "目标必须落在沙箱目录内")
  } finally {
    box.cleanup()
    fs.rmSync(src, { recursive: true, force: true })
  }
})

test("copyIntoSandbox：父目录不存在时自动建", function () {
  const src = tmpDir("p2-src-")
  const from = path.join(src, "a.txt")
  fs.writeFileSync(from, "x", "utf8")
  const box = sandbox.createSandbox()
  try {
    const target = sandbox.copyIntoSandbox(box, from, "deep/nest/a.txt")
    assert.strictEqual(fs.readFileSync(target, "utf8"), "x")
  } finally {
    box.cleanup()
    fs.rmSync(src, { recursive: true, force: true })
  }
})

test("copyIntoSandbox：越界相对路径抛错", function () {
  const src = tmpDir("p2-src-")
  const from = path.join(src, "a.txt")
  fs.writeFileSync(from, "x", "utf8")
  const box = sandbox.createSandbox()
  try {
    assert.throws(
      function () { sandbox.copyIntoSandbox(box, from, "../escape.txt") },
      /沙箱相对路径非法/
    )
    assert.throws(
      function () { sandbox.copyIntoSandbox(box, from, "/abs.txt") },
      /沙箱相对路径非法/
    )
  } finally {
    box.cleanup()
    fs.rmSync(src, { recursive: true, force: true })
  }
})

test("copyDirIntoSandbox：递归复制整个目录", function () {
  const src = tmpDir("p2-src-")
  fs.writeFileSync(path.join(src, "main.py"), "print(1)\n", "utf8")
  fs.mkdirSync(path.join(src, "sub"))
  fs.writeFileSync(path.join(src, "sub", "schema.sql"), "CREATE TABLE t(id INT);\n", "utf8")
  const box = sandbox.createSandbox()
  try {
    const target = sandbox.copyDirIntoSandbox(box, src, "demo")
    assert.strictEqual(fs.readFileSync(path.join(target, "main.py"), "utf8"), "print(1)\n")
    assert.strictEqual(
      fs.readFileSync(path.join(target, "sub", "schema.sql"), "utf8"),
      "CREATE TABLE t(id INT);\n"
    )
  } finally {
    box.cleanup()
    fs.rmSync(src, { recursive: true, force: true })
  }
})

test("readArtifact：读得到沙箱里的文件", function () {
  const box = sandbox.createSandbox()
  try {
    fs.writeFileSync(path.join(box.dir, "out.txt"), "hello\n", "utf8")
    assert.strictEqual(sandbox.readArtifact(box, "out.txt"), "hello\n")
  } finally {
    box.cleanup()
  }
})

test("readArtifact：文件不存在返回 null", function () {
  const box = sandbox.createSandbox()
  try {
    assert.strictEqual(sandbox.readArtifact(box, "missing.txt"), null)
  } finally {
    box.cleanup()
  }
})

test("readArtifact：越界路径抛错", function () {
  const box = sandbox.createSandbox()
  try {
    assert.throws(
      function () { sandbox.readArtifact(box, "../secret") },
      /产物路径越界/
    )
    assert.throws(
      function () { sandbox.readArtifact(box, "/etc/passwd") },
      /产物路径越界/
    )
  } finally {
    box.cleanup()
  }
})

test("readArtifact：目录返回 null", function () {
  const box = sandbox.createSandbox()
  try {
    fs.mkdirSync(path.join(box.dir, "adir"))
    assert.strictEqual(sandbox.readArtifact(box, "adir"), null)
  } finally {
    box.cleanup()
  }
})

test("collectArtifacts：只收 file 型断言，缺文件与越界都跳过", function () {
  const box = sandbox.createSandbox()
  try {
    fs.writeFileSync(path.join(box.dir, "out.txt"), "hello", "utf8")
    const got = sandbox.collectArtifacts(box, [
      { type: "file", path: "out.txt", expect: "hello" },
      { type: "file", path: "missing.txt", expect: "x" },
      { type: "file", path: "../escape.txt", expect: "x" },
      { type: "exit", expect: 0 },
      { type: "contains", text: "x" },
    ])
    assert.deepStrictEqual(got, { "out.txt": "hello" })
  } finally {
    box.cleanup()
  }
})

test("collectArtifacts：越界路径**就算沙箱外真有这个文件**也不许读到", function () {
  // 上一条用例只传了 `../escape.txt`，但那个文件**并不存在** ——
  // 所以「跳过越界」与「文件不存在返回 null」两条路径的**结果一样**，
  // 把越界检查整个删掉测试仍然全绿（干跑实测：该变异体存活）。
  // 这条在沙箱外**真的放一个文件**，越界检查一失效就会读到它。
  const box = sandbox.createSandbox()
  const outside = path.join(box.dir, "..", "escape-" + Date.now() + ".txt")
  try {
    fs.writeFileSync(outside, "SHOULD-NOT-READ", "utf8")
    const got = sandbox.collectArtifacts(box, [
      { type: "file", path: "../" + path.basename(outside), expect: "x" },
    ])
    assert.deepStrictEqual(got, {}, "沙箱外的文件不许被收集：" + JSON.stringify(got))
  } finally {
    try {
      fs.unlinkSync(outside)
    } catch (err) {
      // 文件没建出来也没关系
    }
    box.cleanup()
  }
})

test("createSandbox 的目录建在系统临时目录下，cleanup 之后消失", function () {
  const box = sandbox.createSandbox()
  assert.ok(box.dir.startsWith(os.tmpdir()), box.dir)
  const dir = box.dir
  box.cleanup()
  assert.strictEqual(fs.existsSync(dir), false)
})
