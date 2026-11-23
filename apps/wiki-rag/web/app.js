"use strict"

const $ = (selector) => document.querySelector(selector)
const escapeHtml = (value) => String(value ?? "").replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[ch])
let selectedProject = null

async function api(path, options = {}) {
  const response = await fetch(path, { headers: { "content-type": "application/json" }, ...options })
  const data = await response.json()
  if (!response.ok) throw new Error(data.detail || `请求失败：${response.status}`)
  return data
}

async function loadLibraries() {
  try {
    const [health, data] = await Promise.all([api("/api/health"), api("/api/projects")])
    $("#service-status").textContent = health.index_ready ? "索引就绪" : "索引未就绪"
    $(".top-status").classList.toggle("ready", health.index_ready)
    $("#embed-backend").textContent = health.embedding_backend === "sentence-transformers" ? `语义模型 · ${health.embedding_model}` : "演示降级 · 哈希嵌入"
    $("#embed-backend").title = health.embedding_warning || ""
    $("#embed-backend").classList.toggle("degraded", health.embedding_backend !== "sentence-transformers")
    const root = $("#project-list")
    $("#library-count").textContent = data.projects.length
    if (!data.projects.length) {
      root.innerHTML = '<div class="error-state">尚无索引，请先运行 tools/ingest.py。</div>'
      return
    }
    root.innerHTML = data.projects.map((item) => `<button class="project-card ${selectedProject === item.name ? "active" : ""}" data-project="${escapeHtml(item.name)}"><div class="project-name">${escapeHtml(item.name)}</div><div class="project-meta">${item.page_count} 页 · ${item.block_count} 块</div><span class="gate ${item.validation === "FAIL" ? "fail" : ""}"><i></i>${item.validation} · lint ${item.gates.wiki_lint ?? "?"} / refs ${item.gates.wiki_refs ?? "?"}</span></button>`).join("")
    root.querySelectorAll("[data-project]").forEach((button) => button.addEventListener("click", () => {
      selectedProject = selectedProject === button.dataset.project ? null : button.dataset.project
      root.querySelectorAll(".project-card").forEach((card) => card.classList.toggle("active", card.dataset.project === selectedProject))
    }))
  } catch (error) {
    $("#service-status").textContent = "服务不可用"
    $("#project-list").innerHTML = `<div class="error-state">${escapeHtml(error.message)}</div>`
  }
}

function renderSources(citations) {
  $("#source-count").textContent = citations.length
  const root = $("#sources")
  if (!citations.length) {
    root.innerHTML = '<div class="source-empty"><div>↘</div><strong>没有找到可回源证据</strong><p>请调整问题关键词，或检查当前库是否已建索引。</p></div>'
    return
  }
  root.innerHTML = citations.map((item) => `<details class="source-card"><summary><span class="source-tag">${escapeHtml(item.project)}</span><div class="source-title">${escapeHtml(item.title)}</div><div class="source-meta">${escapeHtml(item.page_file)} · ${escapeHtml(item.section)}</div><div class="source-preview">${escapeHtml(item.preview)}</div></summary><div class="source-full">${escapeHtml(item.text)}<hr>块 ID：${escapeHtml(item.block_id)}<br>源码 refs：${escapeHtml((item.refs || []).map((ref) => `${ref.path} [${(ref.symbols || []).join(", ")}]`).join("；") || "无")}</div></details>`).join("")
}

function renderMetrics(retrieval) {
  $("#retrieval-summary").textContent = `${retrieval.vector_recall_count} 向量 · ${retrieval.bm25_hit_count} BM25 · ${retrieval.elapsed_ms} ms`
  $("#retrieval-details").innerHTML = `<div class="retrieval-grid"><div class="metric"><strong>${retrieval.vector_recall_count}</strong><span>向量召回（跨库合计）</span></div><div class="metric"><strong>${retrieval.bm25_hit_count}</strong><span>BM25 命中</span></div><div class="metric"><strong>${retrieval.fused_count}</strong><span>融合结果</span></div><div class="metric"><strong>${retrieval.elapsed_ms} ms</strong><span>检索耗时</span></div></div>`
}

async function submitQuestion(question) {
  const button = $("#ask-button")
  button.disabled = true
  button.textContent = "正在检索…"
  $("#answer").className = "answer"
  $("#answer").textContent = "正在执行 BM25 + 向量检索并融合…"
  try {
    const data = await api("/api/ask", { method: "POST", body: JSON.stringify({ query: question, projects: selectedProject ? [selectedProject] : null, limit: 12, answer_limit: 4 }) })
    $("#answer").className = data.abstained ? "answer abstain" : "answer"
    $("#answer").textContent = data.answer
    $("#result-count").textContent = `${data.results.length} 个候选块`
    $("#retrieval-drawer").open = true
    renderMetrics(data.retrieval)
    renderSources(data.citations)
  } catch (error) {
    $("#answer").className = "answer error-state"
    $("#answer").textContent = `检索失败：${error.message}`
    renderSources([])
  } finally {
    button.disabled = false
    button.innerHTML = '检索并回答 <b>↗</b>'
  }
}

$("#ask-form").addEventListener("submit", (event) => {
  event.preventDefault()
  const question = $("#question").value.trim()
  if (question) submitQuestion(question)
})
$("#question").addEventListener("keydown", (event) => {
  if (event.key === "Enter" && !event.shiftKey) {
    event.preventDefault()
    $("#ask-form").requestSubmit()
  }
})
document.querySelectorAll("[data-question]").forEach((button) => button.addEventListener("click", () => {
  $("#question").value = button.dataset.question
  submitQuestion(button.dataset.question)
}))
loadLibraries()
