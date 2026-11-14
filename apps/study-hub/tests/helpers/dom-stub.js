"use strict"
/**
 * dom-stub.js —— 极简 DOM 桩
 *
 * web/app.js 是零依赖的原生 JS，本仓不引 jsdom（零依赖铁律）。
 * 这个桩只实现 app.js 真正用到的那几个动作：
 * createElement / createTextNode / getElementById / addEventListener，
 * 以及节点的 className / textContent / value / disabled / setAttribute / appendChild。
 */

/**
 * 造一个节点。
 * @param {string} tag
 * @returns {object}
 */
function createNode(tag) {
  const node = {
    tagName: tag,
    className: "",
    attributes: {},
    children: [],
    listeners: {},
    value: "",
    disabled: false,
    _text: "",
    setAttribute: function (key, val) {
      this.attributes[key] = String(val)
    },
    getAttribute: function (key) {
      return this.attributes[key]
    },
    appendChild: function (child) {
      this.children.push(child)
      return child
    },
    addEventListener: function (type, fn) {
      if (!this.listeners[type]) this.listeners[type] = []
      this.listeners[type].push(fn)
    },
    /** 触发事件（测试里模拟点击、输入、切换） */
    dispatch: function (type, evt) {
      const list = this.listeners[type] || []
      list.forEach(function (fn) {
        fn(evt || {})
      })
    },
  }

  Object.defineProperty(node, "textContent", {
    get: function () {
      if (this.children.length > 0) {
        return this.children
          .map(function (c) {
            return c.textContent
          })
          .join("")
      }
      return this._text
    },
    set: function (value) {
      this._text = String(value)
      this.children = []
    },
  })

  return node
}

/**
 * 造一个 document（含 id 注册表）。
 * @returns {{document: object, registry: object}}
 */
function createDocument() {
  const registry = {}

  const document = {
    createElement: function (tag) {
      const node = createNode(tag)
      const rawSetAttribute = node.setAttribute
      node.setAttribute = function (key, val) {
        rawSetAttribute.call(node, key, val)
        if (key === "id") registry[val] = node
      }
      return node
    },
    createTextNode: function (text) {
      const node = createNode("#text")
      node.textContent = text
      return node
    },
    getElementById: function (id) {
      return registry[id] || null
    },
    addEventListener: function () {},
  }

  return { document: document, registry: registry }
}

/**
 * 在树里按 id 找节点。
 * @param {object} node
 * @param {string} id
 * @returns {object|null}
 */
function findById(node, id) {
  if (!node) return null
  if (node.attributes && node.attributes.id === id) return node
  const children = node.children || []
  for (const child of children) {
    const hit = findById(child, id)
    if (hit) return hit
  }
  return null
}

/**
 * 数一棵树里有几个 id 等于给定值的节点。
 * @param {object} node
 * @param {string} id
 * @returns {number}
 */
function countById(node, id) {
  let count = 0
  if (!node) return count
  if (node.attributes && node.attributes.id === id) count += 1
  const children = node.children || []
  for (const child of children) count += countById(child, id)
  return count
}

module.exports = {
  createNode: createNode,
  createDocument: createDocument,
  findById: findById,
  countById: countById,
}
