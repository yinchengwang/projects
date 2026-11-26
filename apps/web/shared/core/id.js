/**
 * id.js —— 轻量唯一 ID
 *
 * 时间戳（36 进制）+ 4 位随机后缀。单机本地数据足够，无需 UUID。
 */

/** 生成一个短 ID，例如 "m1x2y3z4ab7k" */
function genId () {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 6)
}

module.exports = { genId: genId }
