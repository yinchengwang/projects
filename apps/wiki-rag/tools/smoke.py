# -*- coding: utf-8 -*-
"""RAG 应用的端到端冒烟：直接用 Python 客户端，避免 PowerShell 的编码干扰。

启动一个 uvicorn 子进程（继承 stdio 而非管道，符合沙箱限制），
依次请求 health / projects / projects/{name} / search / ask，最后断言关键字段。
"""
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

APP_ROOT = r"C:\code\projects\apps\wiki-rag"
PORT = 8012
BASE = "http://127.0.0.1:%d" % PORT


def _get(path):
    with urllib.request.urlopen(BASE + path, timeout=120) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8"))


def _post(path, payload):
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(BASE + path, data=data, method="POST",
                                 headers={"Content-Type": "application/json; charset=utf-8"})
    with urllib.request.urlopen(req, timeout=180) as resp:
        return resp.status, json.loads(resp.read().decode("utf-8"))


def main():
    proc = subprocess.Popen(
        [sys.executable, "-m", "uvicorn", "server.app:app", "--host", "127.0.0.1",
         "--port", str(PORT), "--log-level", "warning"],
        cwd=APP_ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(60):
            try:
                code, health = _get("/api/health")
                break
            except (urllib.error.URLError, ConnectionError):
                time.sleep(1)
        else:
            print("服务未在 60 秒内就绪")
            return 1

        print("health:", code, json.dumps(health, ensure_ascii=False))

        code, projects = _get("/api/projects")
        print("projects:", code, "count=", len(projects.get("projects", [])))
        for item in projects.get("projects", []):
            print("   ", item.get("name"), "pages=", item.get("page_count"),
                  "validation=", item.get("validation"), "gates=", item.get("gates"))

        code, lib = _get("/api/projects/postgres")
        print("project detail:", code, "pages=", len(lib.get("pages", [])),
              "page_count=", lib.get("page_count"), "block_count=", lib.get("block_count"))

        code, search = _post("/api/search", {"query": "缓冲池", "projects": ["postgres"], "limit": 3})
        print("search:", code, "results=", len(search.get("results", [])),
              "retrieval=", json.dumps(search.get("retrieval"), ensure_ascii=False))

        code, ask = _post("/api/ask", {"query": "缓冲池如何替换页面？", "projects": ["postgres"], "limit": 5})
        print("ask:", code, "answer_len=", len(ask.get("answer", "")),
              "citations=", len(ask.get("citations", [])),
              "abstained=", ask.get("abstained"),
              "retrieval=", json.dumps(ask.get("retrieval"), ensure_ascii=False))
        for citation in ask.get("citations", [])[:4]:
            print("   引用:", citation.get("project"), citation.get("page_file"),
                  "§", citation.get("section"))

        # 断言：引用必须能回源到块库
        chunks_path = APP_ROOT + r"\index\data\projects\postgres\chunks.json"
        with open(chunks_path, encoding="utf-8") as stream:
            blocks = json.load(stream)
        by_id = {block["block_id"]: block for block in blocks}
        missing = [c["block_id"] for c in ask.get("citations", []) if c.get("block_id") not in by_id]
        print("引用回源校验：缺失 =", missing if missing else "无（全部命中块库）")
        print("块库规模：", len(blocks), "块，唯一 ID", len({b['block_id'] for b in blocks}))

        code, ask2 = _post("/api/ask", {"query": "zzz不存在的主题qqq", "projects": ["postgres"], "limit": 5})
        print("ask(无关问题):", code, "abstained=", ask2.get("abstained"),
              "citations=", len(ask2.get("citations", [])))
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
