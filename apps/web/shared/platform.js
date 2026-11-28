"use strict"
/**
 * platform.js —— 平台适配层
 *
 * 把小程序 wx 的 9 个交互 API 用同名同签名承接，让页面代码只需把
 * `wx.` 换成 `platform.` 就能在浏览器里跑。
 *
 * 零依赖。浏览器挂到 window.AlgoPlanPlatform，Node 可 require（供单测）。
 *
 * 为什么值得单独一层：35 个页面共 193 处 wx 交互调用，只用了 9 个 API。
 * 在这里实现一次，比每个页面各写一遍便宜得多，也让「交互层」有唯一去处。
 */

;(function (root, factory) {
  if (typeof module === "object" && module.exports) {
    module.exports = factory()
  } else {
    root.AlgoPlanPlatform = factory()
  }
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict"

  /* ============================================================
   * 运行环境探测
   * ============================================================ */

  const isBrowser = typeof document !== "undefined" && typeof document.createElement === "function"

  /** 浮层宿主节点（懒建） */
  let toastHost = null

  /** 当前挂着的遮罩（loading / modal / sheet 共用一个） */
  let activeMask = null

  /** 当前弹层里用于测试的钩子 */
  const testHooks = {
    clickSheet: null,
    clickModal: null
  }

  function ensureToastHost () {
    if (!isBrowser) return null
    if (toastHost) return toastHost
    toastHost = document.createElement("div")
    toastHost.className = "pl-toast-host"
    if (document.body) document.body.appendChild(toastHost)
    return toastHost
  }

  function el (tag, cls, text) {
    const node = document.createElement(tag)
    if (cls) node.className = cls
    if (text !== undefined && text !== null) node.textContent = String(text)
    return node
  }

  /* ============================================================
   * 轻提示
   * ============================================================ */

  /**
   * 轻提示。
   * @param {{title: string, icon?: 'none'|'success'|'error', duration?: number}} opts
   */
  function showToast (opts) {
    const o = opts || {}
    const host = ensureToastHost()
    if (!host) return null
    const node = el("div", "pl-toast", o.title || "")
    host.appendChild(node)
    const duration = o.duration === undefined ? 2000 : o.duration
    setTimeout(function () {
      // 不能用 host.children.indexOf —— children 是 HTMLCollection，
      // 没有 indexOf 也没有 splice（浏览器里会抛，测试桩是数组才侥幸没暴露）。
      // 结果就是提示条永远消不掉。
      if (node.parentNode) node.parentNode.removeChild(node)
    }, duration)
    return node
  }

  /* ============================================================
   * 动作菜单
   * ============================================================ */

  /**
   * 动作菜单。
   * @param {{itemList: string[], itemColor?: string}} opts
   * @returns {Promise<{tapIndex: number}>} tapIndex 为 -1 表示取消
   */
  function showActionSheet (opts) {
    const o = opts || {}
    const items = o.itemList || []
    return new Promise(function (resolve) {
      const mask = buildMask()
      const dialog = el("div", "pl-dialog")
      const list = el("div", "pl-sheet-list")

      items.forEach(function (label, index) {
        const item = el("div", "pl-sheet-item", label)
        if (o.itemColor) item.style = { color: o.itemColor }
        item.addEventListener("click", function () {
          closeMask(mask)
          resolve({ tapIndex: index })
        })
        list.appendChild(item)
      })

      dialog.appendChild(list)
      const actions = el("div", "pl-dialog-actions")
      const cancel = el("button", "pl-btn", "取消")
      cancel.addEventListener("click", function () {
        closeMask(mask)
        resolve({ tapIndex: -1 })
      })
      actions.appendChild(cancel)
      dialog.appendChild(actions)
      mask.appendChild(dialog)

      // 测试钩子：桩环境下模拟点击
      testHooks.clickSheet = function (index) {
        closeMask(mask)
        resolve({ tapIndex: index })
      }
    })
  }

  /* ============================================================
   * 对话框
   * ============================================================ */

  /**
   * 对话框。editable=true 时兼作输入框。
   * @param {{title?: string, content?: string, editable?: boolean, confirmText?: string, cancelText?: string, confirmColor?: string}} opts
   * @returns {Promise<{confirm: boolean, content: string}>}
   */
  function showModal (opts) {
    const o = opts || {}
    return new Promise(function (resolve) {
      const mask = buildMask()
      const dialog = el("div", "pl-dialog")

      if (o.title) dialog.appendChild(el("h3", "pl-dialog-title", o.title))
      if (o.content) dialog.appendChild(el("p", "pl-dialog-content", o.content))

      let input = null
      if (o.editable) {
        input = el("input", "pl-dialog-input")
        input.setAttribute("type", "text")
        dialog.appendChild(input)
      }

      const actions = el("div", "pl-dialog-actions")
      const cancel = el("button", "pl-btn", o.cancelText || "取消")
      const confirm = el("button", "pl-btn primary", o.confirmText || "确定")
      if (o.confirmColor) confirm.setAttribute("data-color", o.confirmColor)

      cancel.addEventListener("click", function () {
        closeMask(mask)
        resolve({ confirm: false, content: "" })
      })
      confirm.addEventListener("click", function () {
        const text = input ? (input.value || "") : ""
        closeMask(mask)
        resolve({ confirm: true, content: text })
      })

      actions.appendChild(cancel)
      actions.appendChild(confirm)
      dialog.appendChild(actions)
      mask.appendChild(dialog)

      if (input) setTimeout(function () { if (input.focus) input.focus() }, 0)

      testHooks.clickModal = function (confirmed, content) {
        closeMask(mask)
        resolve({ confirm: !!confirmed, content: confirmed ? String(content || "") : "" })
      }
    })
  }

  /* ============================================================
   * 遮罩
   * ============================================================ */

  function buildMask () {
    const mask = el("div", "pl-mask")
    if (document.body) document.body.appendChild(mask)
    activeMask = mask
    return mask
  }

  function closeMask (mask) {
    // 同 showToast：children 是 HTMLCollection，不能 indexOf/splice。
    // 这里原本会抛异常，把 closeMask 的调用方（对话框的 resolve）一起带没 ——
    // 遮罩留在页面上，整个应用点不动。
    if (mask && mask.parentNode) mask.parentNode.removeChild(mask)
    if (activeMask === mask) activeMask = null
  }

  /* ============================================================
   * 路由
   * ============================================================ */

  /**
   * 压栈跳转。接受字符串或 `{url}` 对象，兼容旧调用形状。
   * 小程序风格的 `/pages/...` 会补上 `/workbench` 前缀。
   * @param {string|{url:string}} target
   */
  function navigateTo (target) {
    const url = normalizeUrl(target)
    if (!url || !isBrowser) return
    global.history.pushState({ url: url }, "", url)
  }

  /** 回退一步 */
  function navigateBack () {
    if (!isBrowser) return
    global.history.back()
  }

  /**
   * 切换主标签：替换当前记录，不压栈。
   * @param {string|{url:string}} target
   */
  function switchTab (target) {
    const url = normalizeUrl(target)
    if (!url || !isBrowser) return
    global.history.replaceState({ url: url }, "", url)
  }

  /**
   * 统一跳转地址：字符串与 `{url}` 均可，`/pages/` 开头补 `/workbench`。
   * @param {string|{url:string}} target
   * @returns {string} 规范化后的路径
   */
  function normalizeUrl (target) {
    const raw = typeof target === "string" ? target : (target && target.url) || ""
    if (!raw) return ""
    if (raw.indexOf("/pages/") === 0) return "/workbench" + raw
    return raw
  }

  /* ============================================================
   * 加载遮罩
   * ============================================================ */

  /**
   * 显示加载遮罩。
   * @param {{title?: string, mask?: boolean}} opts
   */
  function showLoading (opts) {
    const o = opts || {}
    const mask = buildMask()
    mask.appendChild(el("div", "pl-loading", o.title || "加载中…"))
    return mask
  }

  /** 隐藏加载遮罩 */
  function hideLoading () {
    if (activeMask) closeMask(activeMask)
  }

  /* ============================================================
   * 剪贴板
   * ============================================================ */

  /**
   * 写剪贴板。
   * @param {{data: string}} opts
   * @returns {Promise<void>}
   */
  function setClipboardData (opts) {
    const o = opts || {}
    const clipboard = (typeof navigator !== "undefined") ? navigator.clipboard : null
    if (!clipboard || typeof clipboard.writeText !== "function") {
      return Promise.reject(new Error("当前环境不支持剪贴板写入"))
    }
    return clipboard.writeText(String(o.data === undefined ? "" : o.data))
  }

  /**
   * 读剪贴板。
   * @returns {Promise<{data: string}>}
   */
  function getClipboardData () {
    const clipboard = (typeof navigator !== "undefined") ? navigator.clipboard : null
    if (!clipboard || typeof clipboard.readText !== "function") {
      return Promise.reject(new Error("当前环境不支持剪贴板读取"))
    }
    return clipboard.readText().then(function (text) {
      return { data: String(text === undefined ? "" : text) }
    })
  }

  /* ============================================================
   * 触感反馈
   * ============================================================ */

  /** 震动时长（毫秒），与 wx.vibrateShort 的 type 对应 */
  const VIBRATE_MS = { light: 15, medium: 40, heavy: 80 }

  /**
   * 短震动。设备不支持时静默返回（不打断交互）。
   * @param {{type?: 'light'|'medium'|'heavy'}} opts
   */
  function vibrateShort (opts) {
    const o = opts || {}
    try {
      if (typeof navigator === "undefined" || typeof navigator.vibrate !== "function") return false
      const ms = VIBRATE_MS[o.type] || VIBRATE_MS.light
      return navigator.vibrate(ms)
    } catch (e) {
      return false
    }
  }

  /**
   * 长震动。设备不支持时静默返回（不打断交互）。
   */
  function vibrateLong () {
    try {
      if (typeof navigator === "undefined" || typeof navigator.vibrate !== "function") return false
      return navigator.vibrate(400)
    } catch (e) {
      return false
    }
  }
  /* ============================================================
   * 图片
   * ============================================================ */

  /**
   * 选图。浏览器里用 <input type="file"> 触发。
   * @param {{count?: number, mediaType?: string[]}} opts
   * @returns {Promise<Array<{path: string, size: number, file: object}>>}
   */
  function chooseMedia (opts) {
    const o = opts || {}
    if (!isBrowser || typeof document.createElement !== "function") {
      return Promise.reject(new Error("当前环境不支持选择文件"))
    }
    return new Promise(function (resolve, reject) {
      const input = document.createElement("input")
      // Node 测试环境下 createElement 存在但没有 click —— 必须先探测再往下走，
      // 否则会抛英文的 "input.click is not a function"，违反「文案一律中文」。
      if (typeof input.click !== "function") {
        reject(new Error("当前环境不支持选择文件"))
        return
      }
      input.setAttribute("type", "file")
      input.setAttribute("accept", "image/*")
      if (o.count && o.count > 1) input.setAttribute("multiple", "multiple")
      input.addEventListener("change", function () {
        const files = input.files ? Array.prototype.slice.call(input.files) : []
        resolve(files.map(function (file) {
          return { path: URL.createObjectURL(file), size: file.size, file: file }
        }))
      })
      input.click()
    })
  }

  /**
   * 图片预览。
   * @param {{current: string, urls: string[]}} opts
   */
  function previewImage (opts) {
    const o = opts || {}
    if (!isBrowser) return null
    const urls = o.urls || []
    if (urls.length === 0) return null
    const box = el("div", "pl-lightbox")
    const img = document.createElement("img")
    img.setAttribute("src", o.current || urls[0])
    box.appendChild(img)
    box.addEventListener("click", function () {
      closeMask(box)
    })
    if (document.body) document.body.appendChild(box)
    activeMask = box
    return box
  }

  /** Web 无下拉刷新手势，空实现 */
  function stopPullDownRefresh () {}

  /* ============================================================
   * 测试钩子（仅供单测模拟用户操作）
   * ============================================================ */

  /**
   * 模拟点击动作菜单的第几项。
   * @param {number} index -1 表示取消
   */
  function __testClickSheet (index) {
    if (testHooks.clickSheet) testHooks.clickSheet(index)
  }

  /**
   * 模拟确认/取消对话框。
   * @param {boolean} confirmed
   * @param {string} content 输入框内容
   */
  function __testClickModal (confirmed, content) {
    if (testHooks.clickModal) testHooks.clickModal(confirmed, content)
  }

  return {
    showToast: showToast,
    showModal: showModal,
    showActionSheet: showActionSheet,
    navigateTo: navigateTo,
    navigateBack: navigateBack,
    switchTab: switchTab,
    showLoading: showLoading,
    hideLoading: hideLoading,
    setClipboardData: setClipboardData,
    getClipboardData: getClipboardData,
    vibrateShort: vibrateShort,
    vibrateLong: vibrateLong,
    chooseMedia: chooseMedia,
    previewImage: previewImage,
    stopPullDownRefresh: stopPullDownRefresh,
    __testClickSheet: __testClickSheet,
    __testClickModal: __testClickModal
  }
})
