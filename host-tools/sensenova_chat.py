"""SenseNova (商汤日日新) chat test — the PC-side link check.

Three modes, in the order you'd normally want them:

    python sensenova_chat.py --models
    python sensenova_chat.py --once "你好，介绍一下你自己"
    python sensenova_chat.py                          # interactive REPL

The key comes from --key, the SENSENOVA_API_KEY environment variable, or
sensenova_key.txt (written by --save-key) — so the board-side work later has
one place to read it from.

Endpoint: https://token.sensenova.cn/v1  (OpenAI-compatible, Bearer auth)

Two things measured against the live API on 2026-10-08 that shape the defaults:

  * reasoning_effort:"none" is not optional. With it off, "北京下午3点是白天
    还是晚上" answered in 7 output tokens; with it on the same question burned
    836 tokens (2077 characters of scratchpad before the six-character answer).
    A screen assistant can't afford a 100x multiplier on a 1500-calls-per-5h
    budget, so every request here sends "none" unless you override it.

  * deepseek-v4-flash is the fast one: 1.2 s round trip against sensenova-6.8-
    flash-lite's 12.2 s, same answer. That is the difference between a usable
    voice assistant and an unusable one, so it is the default model.

The account is rate limited (429 "inference exceeds tpm/rpm limit" comes back
after a burst), so requests are serialized with a small gap in front of them.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from pathlib import Path

import requests

BASE_URL = "https://token.sensenova.cn/v1"
DEFAULT_MODEL = "deepseek-v4-flash"
DEFAULT_EFFORT = "none"
KEY_FILE = Path(__file__).with_name("sensenova_key.txt")

# Models the account can reach (GET /v1/models). Kept for help text only.
KNOWN_MODELS = [
    "deepseek-v4-flash",        # fastest; plain text, no scratchpad
    "deepseek-v4.1-flash",
    "deepseek-flash",
    "sensenova-6.8-flash-lite", # SmartNova's own, multimodal, slower
    "sensenova-u1.5-lite",
    "glm-5.2",
    "kimi-k3",
]

TIMEOUT = 60

# Serialize calls and keep a floor between them so a burst doesn't trip the
# account's per-minute limit. Values are a guess at a safe pace, not a published
# figure -- the server's own 429 is the authority.
_MIN_GAP_S = 0.6
_throttle_lock = threading.Lock()
_last_call = 0.0


def _throttle():
    global _last_call
    with _throttle_lock:
        wait = _MIN_GAP_S - (time.monotonic() - _last_call)
        if wait > 0:
            time.sleep(wait)
        _last_call = time.monotonic()


def die(msg: str, code: int = 1):
    print(f"[x] {msg}", file=sys.stderr)
    raise SystemExit(code)


def load_key(cli_key: str | None) -> str:
    if cli_key:
        return cli_key.strip()
    env = os.environ.get("SENSENOVA_API_KEY", "").strip()
    if env:
        return env
    if KEY_FILE.exists():
        k = KEY_FILE.read_text(encoding="utf-8").strip()
        if k:
            return k
    die("没有 API Key。用 --key sk-xxx，或设 SENSENOVA_API_KEY，或先跑 --save-key。")


def headers(key: str) -> dict:
    return {"Authorization": f"Bearer {key}", "Content-Type": "application/json"}


def explain_http_error(r: requests.Response):
    """Turn SenseNova's status codes into something actionable."""
    hints = {
        401: "Key 不对或已失效。去 https://platform.sensenova.cn/console 重新复制一枚。",
        403: "Key 没有权限，或者服务没开通（控制台 → 资源管理 里确认已开通日日新）。",
        404: "地址或模型名不对。Base URL 必须是 https://token.sensenova.cn/v1（带 /v1）；模型名注意大小写。",
        429: "触发限流：每模型 1500 次 / 5 小时，60 RPM。等一会儿再试。",
    }
    print(f"[x] HTTP {r.status_code}", file=sys.stderr)
    if r.status_code in hints:
        print(f"    {hints[r.status_code]}", file=sys.stderr)
    body = r.text.strip()
    if body:
        print(f"    服务端返回: {body[:600]}", file=sys.stderr)


def cmd_models(key: str) -> int:
    url = f"{BASE_URL}/models"
    print(f"[i] GET {url}")
    try:
        r = requests.get(url, headers=headers(key), timeout=TIMEOUT)
    except requests.RequestException as e:
        die(f"连不上: {e}")
    if r.status_code != 200:
        explain_http_error(r)
        return 1
    data = r.json()
    ids = [m.get("id", "?") for m in data.get("data", [])]
    if not ids:
        print("[!] 返回里没有模型列表，原始内容：")
        print(json.dumps(data, ensure_ascii=False, indent=2)[:2000])
        return 1
    print(f"[+] 可用模型 {len(ids)} 个：")
    for i in ids:
        mark = "  <- 默认" if i == DEFAULT_MODEL else ""
        print(f"    {i}{mark}")
    return 0


def ask(key: str, model: str, messages: list, max_tokens: int,
        effort: str | None = DEFAULT_EFFORT, retries: int = 2) -> str | None:
    url = f"{BASE_URL}/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "stream": False,
    }
    if effort and effort != "default":
        payload["reasoning_effort"] = effort

    r = None
    for attempt in range(retries + 1):
        _throttle()
        try:
            r = requests.post(url, headers=headers(key), json=payload, timeout=TIMEOUT)
        except requests.RequestException as e:
            print(f"[x] 请求失败: {e}", file=sys.stderr)
            return None
        if r.status_code == 429 and attempt < retries:
            # Back off and try again: this is the account limit, not a bad request.
            delay = 3.0 * (attempt + 1)
            print(f"[!] 限流 (429)，{delay:.0f}s 后重试 ({attempt + 1}/{retries})...", file=sys.stderr)
            time.sleep(delay)
            continue
        break

    if r.status_code != 200:
        explain_http_error(r)
        return None
    data = r.json()
    try:
        choice = data["choices"][0]
        msg = choice["message"]
    except (KeyError, IndexError):
        print("[x] 响应结构不对：")
        print(json.dumps(data, ensure_ascii=False, indent=2)[:2000])
        return None
    content = (msg.get("content") or "").strip()
    reasoning = (msg.get("reasoning") or "").strip()
    usage = data.get("usage") or {}
    if reasoning:
        print(f"\n[思考] {reasoning[:400]}{'...' if len(reasoning) > 400 else ''}")
    print(f"\n[回答] {content}")
    if usage:
        print(
            f"\n[用量] 输入 {usage.get('prompt_tokens', '?')} / "
            f"输出 {usage.get('completion_tokens', '?')} / "
            f"合计 {usage.get('total_tokens', '?')} tokens"
            f"  finish={choice.get('finish_reason', '?')}"
        )
    return content


def cmd_once(key: str, model: str, text: str, system: str | None, max_tokens: int,
             effort: str | None) -> int:
    msgs = []
    if system:
        msgs.append({"role": "system", "content": system})
    msgs.append({"role": "user", "content": text})
    print(f"[i] 模型 {model}  ·  effort={effort or '默认'}  ·  提问: {text}")
    return 0 if ask(key, model, msgs, max_tokens, effort) is not None else 1


def cmd_repl(key: str, model: str, system: str | None, max_tokens: int,
             effort: str | None) -> int:
    msgs = []
    if system:
        msgs.append({"role": "system", "content": system})
    print(f"[i] 模型 {model}  ·  effort={effort or '默认'}  ·  多轮对话，空行 / Ctrl-C 退出")
    print("    命令: :reset 清空上下文, :model <id> 换模型, :sys <文本> 换 system")
    while True:
        try:
            line = input("\n你> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            break
        if line == ":reset":
            msgs = [m for m in msgs if m["role"] == "system"]
            print("[i] 上下文已清空")
            continue
        if line.startswith(":model "):
            model = line[7:].strip()
            print(f"[i] 切到 {model}")
            continue
        if line.startswith(":sys "):
            msgs = [m for m in msgs if m["role"] != "system"]
            msgs.insert(0, {"role": "system", "content": line[5:].strip()})
            print("[i] system 已更新")
            continue
        msgs.append({"role": "user", "content": line})
        reply = ask(key, model, msgs, max_tokens, effort)
        if reply is None:
            msgs.pop()  # don't poison the history with a turn that never happened
            continue
        msgs.append({"role": "assistant", "content": reply})
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="商汤日日新 SenseNova 对话测试",
        epilog="模型: " + ", ".join(KNOWN_MODELS),
    )
    ap.add_argument("--key", help="API Key (sk-...)")
    ap.add_argument("--save-key", metavar="KEY", help="把这枚 Key 存到 sensenova_key.txt 后退出")
    ap.add_argument("--model", default=DEFAULT_MODEL, help=f"模型 id（默认 {DEFAULT_MODEL}）")
    ap.add_argument("--effort", default=DEFAULT_EFFORT,
                    choices=["none", "low", "medium", "high", "default"],
                    help=f"reasoning_effort（默认 {DEFAULT_EFFORT}；low/medium/high 会成百倍地烧 token）")
    ap.add_argument("--models", action="store_true", help="列出可用模型后退出")
    ap.add_argument("--once", metavar="TEXT", help="问一句就退出")
    ap.add_argument("--system", help="system prompt（例如“回答不超过 30 个字”）")
    ap.add_argument("--max-tokens", type=int, default=2000)
    args = ap.parse_args()

    if args.save_key:
        KEY_FILE.write_text(args.save_key.strip(), encoding="utf-8")
        print(f"[+] 已保存到 {KEY_FILE}")
        return 0

    key = load_key(args.key)
    print(f"[i] Key {key[:7]}...{key[-4:]}（长度 {len(key)}）")

    if args.models:
        return cmd_models(key)
    if args.once:
        return cmd_once(key, args.model, args.once, args.system, args.max_tokens, args.effort)
    return cmd_repl(key, args.model, args.system, args.max_tokens, args.effort)


if __name__ == "__main__":
    raise SystemExit(main())
