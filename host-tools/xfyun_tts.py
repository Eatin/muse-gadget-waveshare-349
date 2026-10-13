#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""讯飞语音合成（TTS）探针 —— 纯标准库，自带最小 WebSocket 客户端。

为什么自己写 WS：本机没有 websocket-client（也不要下载），
而 TTS v2 只有 WebSocket 一条路。RFC6455 的客户端一侧只需
握手 + 掩码文本帧 + 收帧，几十行就够。

产品线（★ 与 ASR 完全不同的协议代，别混）：
    在线合成 v2 : wss://tts-api.xfyun.cn/v2/tts
                  信封 common/business/data（老结构）
                  business.vcn 选发音人、aue=raw 出 PCM
    超拟人合成  : wss://cbm01.cn-huabei-1.xf-yun.com/v1/private/mcd9m97e6
                  header/parameter/payload（大模型结构），vcn 如 x4_lingxiaoxuan_oral

鉴权与 ASR 相同：HMAC-SHA256 签 host+date+request-line。

用法：
    python xfyun_tts.py "你好，这是一段测试。"            # 默认在线合成 xiaoyan
    python xfyun_tts.py "你好" --vcn xiaoyan --out a.wav
    python xfyun_tts.py "你好" --endpoint cbm           # 超拟人
    python xfyun_tts.py --probe                          # 只跑一句，打印 code/message
"""

from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import hmac
import json
import os
import socket
import ssl
import struct
import sys
import time
import wave
from pathlib import Path
from urllib.parse import urlencode, urlparse

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

CRED = Path(__file__).with_name("xfyun_key.json")
DEFAULT_TEXT = "你好，我是微雪，现在可以说话了。"

# ---------------------------------------------------------------- minimal ws


class WS:
    """够用的 RFC6455 客户端：wss 握手、发掩码文本帧、收完整消息。"""

    def __init__(self, url: str, timeout: float = 15.0):
        self.timeout = timeout
        u = urlparse(url)
        self.host = u.hostname
        port = u.port or 443
        raw = socket.create_connection((self.host, port), timeout=timeout)
        ctx = ssl.create_default_context()
        self.sock = ctx.wrap_socket(raw, server_hostname=self.host)
        self.sock.settimeout(timeout)

        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            f"GET {u.path}?{u.query} HTTP/1.1\r\n"
            f"Host: {self.host}\r\n"
            f"Upgrade: websocket\r\n"
            f"Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            f"Sec-WebSocket-Version: 13\r\n"
            f"\r\n"
        )
        self.sock.sendall(req.encode())
        self.buf = b""
        head = self._read_until(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0].decode("latin1")
        if "101" not in status:
            raise RuntimeError(f"握手失败: {status} | {head[-200:]!r}")
        self._leftover = head.split(b"\r\n\r\n", 1)[1]

    # -- raw plumbing
    def _read_until(self, marker: bytes) -> bytes:
        out = b""
        while marker not in out:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise RuntimeError("握手期间连接被关闭")
            out += chunk
        head, _, rest = out.partition(marker)
        self._leftover = rest
        return head + marker

    def _recv_exact(self, n: int) -> bytes:
        out = b""
        if self._leftover:
            take = min(n, len(self._leftover))
            out, self._leftover = self._leftover[:take], self._leftover[take:]
        while len(out) < n:
            chunk = self.sock.recv(n - len(out))
            if not chunk:
                raise RuntimeError("连接被对端关闭")
            out += chunk
        return out

    def send_text(self, payload: str | bytes) -> None:
        if isinstance(payload, str):
            payload = payload.encode()
        header = bytearray([0x81])           # FIN + opcode 1 (text)
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask                       # 客户端必须掩码
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def recv_message(self) -> tuple[int, bytes]:
        """收一条完整消息 -> (opcode, payload)。"""
        frames = b""
        op = 0
        while True:
            b0, b1 = self._recv_exact(2)
            fin = b0 & 0x80
            op = b0 & 0x0F
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._recv_exact(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._recv_exact(8))[0]
            if b1 & 0x80:                    # 服务端不该掩码；保险起见处理
                mk = self._recv_exact(4)
                data = self._recv_exact(ln)
                data = bytes(b ^ mk[i & 3] for i, b in enumerate(data))
            else:
                data = self._recv_exact(ln)
            if op == 0x9:                    # ping -> pong
                self.sock.sendall(b"\x8a\x80" + os.urandom(4))
                continue
            if op == 0x8:
                raise RuntimeError("对端发了 close")
            frames += data
            if fin:
                return op, frames

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:
            pass


# ---------------------------------------------------------------- auth


def signed_url(host: str, path: str, key: str, secret: str) -> str:
    date = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%a, %d %b %Y %H:%M:%S GMT")
    pre = f"host: {host}\ndate: {date}\nGET {path} HTTP/1.1"
    sig = base64.b64encode(
        hmac.new(secret.encode(), pre.encode(), hashlib.sha256).digest()).decode()
    auth = (f'api_key="{key}", algorithm="hmac-sha256", '
            f'headers="host date request-line", signature="{sig}"')
    q = urlencode({"authorization": base64.b64encode(auth.encode()).decode(),
                   "date": date, "host": host})
    return f"wss://{host}{path}?{q}"


# ---------------------------------------------------------------- tts


def online_tts(cred: dict, text: str, vcn: str, out: Path,
               aue: str = "raw", rate: int = 16000, timeout: float = 15.0) -> dict:
    """在线合成 v2。返回 {code, message, bytes, secs}。"""
    host, path = "tts-api.xfyun.cn", "/v2/tts"
    url = signed_url(host, path, cred["apikey"], cred["apisecret"])
    auf = f"audio/L16;rate={rate}" if aue == "raw" else f"audio/mpeg;rate={rate}"
    req = {
        "common": {"app_id": cred["appid"]},
        "business": {"aue": aue, "auf": auf, "vcn": vcn, "tte": "UTF8",
                     "sfl": 1, "speed": 50, "volume": 50, "pitch": 50},
        "data": {"status": 2, "text": base64.b64encode(text.encode()).decode()},
    }
    ws = WS(url, timeout=timeout)
    t0 = time.time()
    ws.send_text(json.dumps(req))
    pcm = bytearray()
    code, msg, sid = None, None, None
    try:
        while True:
            op, raw = ws.recv_message()
            if op not in (1, 2):
                continue
            res = json.loads(raw.decode())
            code = res.get("code")
            msg = res.get("message")
            sid = res.get("sid")
            data = res.get("data") or {}
            if data.get("audio"):
                pcm += base64.b64decode(data["audio"])
            if data.get("status") == 2 or code:
                break
    except Exception as e:                      # 超时 / 对端关闭都当作结束
        if code is None:
            msg = f"{e.__class__.__name__}: {e}"
    finally:
        ws.close()
    secs = time.time() - t0
    if pcm and aue == "raw":
        with wave.open(str(out), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(bytes(pcm))
    return {"code": code, "message": msg, "sid": sid,
            "bytes": len(pcm), "secs": secs, "out": str(out)}


def cbm_tts(cred: dict, text: str, vcn: str, out: Path,
            timeout: float = 15.0) -> dict:
    """超拟人合成（大模型结构）。payload.audio.audio 为 base64 的 mp3。"""
    host = "cbm01.cn-huabei-1.xf-yun.com"
    path = "/v1/private/mcd9m97e6"
    url = signed_url(host, path, cred["apikey"], cred["apisecret"])
    req = {
        "header": {"app_id": cred["appid"], "status": 2},
        "parameter": {"tts": {
            "vcn": vcn, "tte": "UTF8", "audio": {
                "encoding": "lame", "sample_rate": 24000, "channels": 1,
                "bit_depth": 16, "frame_size": 0},
            "speed": 50, "volume": 50, "pitch": 50,
        }},
        "payload": {"text": {
            "encoding": "utf8", "status": 2,
            "seq": 0, "text": base64.b64encode(text.encode()).decode()}},
    }
    ws = WS(url, timeout=timeout)
    t0 = time.time()
    ws.send_text(json.dumps(req))
    audio = bytearray()
    code, msg = None, None
    try:
        while True:
            op, raw = ws.recv_message()
            if op not in (1, 2):
                continue
            res = json.loads(raw.decode())
            hdr = res.get("header") or {}
            code = hdr.get("code", code)
            msg = hdr.get("message", msg)
            chunk = ((res.get("payload") or {}).get("audio") or {}).get("audio")
            if chunk:
                audio += base64.b64decode(chunk)
            if hdr.get("status") == 2 or (code not in (None, 0)):
                break
    except Exception as e:
        if code is None:
            msg = f"{e.__class__.__name__}: {e}"
    finally:
        ws.close()
    secs = time.time() - t0
    if audio:
        out.write_bytes(bytes(audio))
    return {"code": code, "message": msg, "sid": None,
            "bytes": len(audio), "secs": secs, "out": str(out)}


# ---------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("text", nargs="?", default=DEFAULT_TEXT)
    ap.add_argument("--vcn", default="xiaoyan", help="发音人（在线合成默认 xiaoyan）")
    ap.add_argument("--endpoint", default="online", choices=["online", "cbm"])
    ap.add_argument("--out", default="tts_out.wav")
    ap.add_argument("--probe", action="store_true", help="跑三种发音人/端点各一次")
    ap.add_argument("--mp3", action="store_true", help="在线合成改要 mp3（aue=lame）")
    args = ap.parse_args()

    cred = json.loads(CRED.read_text(encoding="utf-8"))
    out = Path(args.out)

    if args.probe:
        print("=== 在线合成 v2 (tts-api.xfyun.cn/v2/tts) —— 逐个试发音人 ===")
        for vcn in ["xiaoyan", "x4_yezi", "aisjiuxu", "xiaofeng"]:
            r = online_tts(cred, args.text, vcn, out,
                           aue="lame" if args.mp3 else "raw")
            print(f"  vcn={vcn:14s} code={r['code']} msg={r['message']!r:30s} "
                  f"{r['bytes']:>7d} B  {r['secs']:.2f}s")
        print("=== 超拟人合成 (cbm01) ===")
        for vcn in ["x4_lingxiaoxuan_oral", "x5_lingfeiyi_oral"]:
            r = cbm_tts(cred, args.text, vcn, out)
            print(f"  vcn={vcn:22s} code={r['code']} msg={r['message']!r:30s} "
                  f"{r['bytes']:>7d} B  {r['secs']:.2f}s")
        return 0

    if args.endpoint == "cbm":
        r = cbm_tts(cred, args.text, args.vcn, out)
    else:
        r = online_tts(cred, args.text, args.vcn, out,
                       aue="lame" if args.mp3 else "raw")
    print(f"endpoint={args.endpoint} vcn={args.vcn} code={r['code']} "
          f"message={r['message']!r} bytes={r['bytes']} {r['secs']:.2f}s -> {r['out']}")
    return 0 if r["code"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
