"""iFlytek (讯飞) streaming ASR over WebSocket -- the PC-side link check.

Same discipline as sensenova_chat.py: prove the protocol on the PC before a
single line of board code gets written.  Push-to-Talk on the Waveshare board
means "record a WAV, hand it to this script" for now; this script is what that
will eventually call.

    python xfyun_asr.py --selftest                 # sign + frame + dry-run, no network
    python xfyun_asr.py --wav test.wav             # transcribe a 16k mono WAV
    python xfyun_asr.py --mic 5                    # record 5 s from the PC mic, then transcribe
    python xfyun_asr.py --sweep                    # which services can this APPID open?
    python xfyun_asr.py --list-endpoints           # endpoint / business matrix

Credentials come from --appid/--apikey/--apisecret, the XFYUN_* environment
variables, or xfyun_key.json (written by --save-*).

★ THE PROTOCOL IS NOT WHAT THE OLD DOCS SHOW ★
----------------------------------------------
The 大模型识别 family (中文语音大模型 / 听写多语种) uses a three-layer envelope:

    {"header": {...}, "parameter": {...}, "payload": {...}}

NOT the legacy 语音听写 envelope {"common": ..., "business": ..., "data": ...}
that every 2019-era blog post and demo shows.  Sending the legacy shape to
iat.cn-huabei-1.xf-yun.com gets you a clean WebSocket upgrade, then a silent
close on the first frame -- no error code, because the parser never got far
enough to have one.  That is exactly what happened on 2026-10-08 and it cost
an hour; don't repeat it.

  legacy (ws-api.xfyun.cn/v2/iat)      LLM (iat.cn-huabei-1.xf-yun.com/v1)
  --------------------------------     ------------------------------------
  common.app_id                        header.app_id
  business.language                    parameter.iat.language
  business.domain                      parameter.iat.domain
  business.accent                      parameter.iat.accent
  business.vad_eos                     parameter.iat.eos
  business.dwa                         parameter.iat.dwa
  data.status                          header.status
  data.format = "audio/L16;rate=..."   payload.audio.sample_rate / bit_depth / channels
  data.encoding = raw                  payload.audio.encoding = raw
  data.audio                           payload.audio.audio   (+ .seq, + .status)

Other facts that trip people up:

  * `ptt` in the business block means 标点预测 (punctuation), NOT Push-to-Talk.
    There is no PTT flag on the wire; PTT is purely a client-side concept
    ("send status=2 the moment the button is released").

  * The result payload moves around.  Legacy puts words at
    `data.result.ws[].cw[].w`; the LLM service puts them at
    `payload.result.text` (base64 of a JSON doc) -> `.ws[].cw[].w`.  This
    script accepts both.

  * Audio is raw 16 kHz / 16-bit / mono PCM, exactly 1280 bytes per frame
    (40 ms).  10-15 s of silence and the server hangs up; 60 s is the hard cap.

  * Auth is HMAC-SHA256 over a three-line preimage, base64'd into the query
    string -- not a Bearer token.  The date is RFC1123 in *GMT* and must be
    within 300 s of the server clock.

★ WHICH PRODUCT LINE AN APPID IS LICENSED FOR ★
------------------------------------------------
`--sweep` sends the same audio down every route the account might have, so you
find out in one run which one your APPID actually pays for. Read the answers
like this:

    code=0      this route works
    11201       licensed, but the daily quota is gone
    11200       not licensed at all for this route
    HTTP 403    wrong path -- the gateway answered, the route does not exist
                (this is what a mistyped endpoint looks like, and it is easy
                to misread as "not subscribed")

11201 is "daily quota exhausted", NOT "not subscribed".  The tell is that a
silent handshake still returns code=0 while real audio returns 11201: the
quota is only debited once the engine actually recognises something.

Route names, and the console service each belongs to:

    zh_cn       + domain spark_asr_v2   语音听写大模型（中英）V2.0
    mul_cn      大模型多语种语音识别 (console.xfyun.cn/services/bmm)
    zh_en_v2    中英识别大模型 V2.0
    iat_legacy  the classic /v2/iat route, if the account still has it
    rtasr       real-time route; needs a different signature

Re-run --sweep after topping up or after swapping credentials, and put the
language that answers code=0 into CONFIG_MUSE_CN_XFYUN_LANGUAGE.
"""

from __future__ import annotations

import argparse
import base64
import datetime
import hashlib
import hmac
import json
import os
import ssl
import struct
import sys
import time
import wave
from pathlib import Path
from urllib.parse import urlencode

CRED_FILE = Path(__file__).with_name("xfyun_key.json")

# ---------------------------------------------------------------------------
# Endpoint / business matrix.
#
# Two protocol generations live here and they are NOT interchangeable.  See the
# module docstring: `proto` decides which envelope frame_payload() builds.
# ---------------------------------------------------------------------------
ENDPOINTS = {
    # --- 大模型识别 family: header/parameter/payload envelope, path /v1 ---
    #
    # ★ 主机名与产品名的对应关系容易记反（2026-10-10 核对官方文档后订正）：
    #   语音听写大模型（中英）V2.0  -> wss://iat.cn-huabei-1.xf-yun.com/v1  domain=spark_asr_v2
    #   中英识别大模型（202 方言）  -> ws[s]://iat.xf-yun.com/v1            domain=slm
    #   大模型多语种识别            -> wss://iat.cn-huabei-1.xf-yun.com/v1  language=mul_cn
    # V2.0 与 slm 两个 domain 在同一台主机上都答 code=0，所以 domain 只决定
    # 「扣哪个产品的额度」——买错产品时握手照样成功，只有第一帧会回 11201。
    "zh_v2": {
        "proto": "llm",
        "host": "iat.cn-huabei-1.xf-yun.com",
        "path": "/v1",
        "section": "iat",
        "domain": "spark_asr_v2",
        "language": "zh_cn",
        "accent": "mandarin",
        "note": "★ 语音听写大模型（中英）V2.0（当前实测可用）",
    },
    "zh_cn": {
        "proto": "llm",
        "host": "iat.cn-huabei-1.xf-yun.com",
        "path": "/v1",
        "section": "iat",
        "domain": "slm",
        "language": "zh_cn",
        "accent": "mandarin",
        "note": "中英识别大模型 slm 域（202 方言免切，当前实测可用）",
    },
    "mul_cn": {
        "proto": "llm",
        "host": "iat.cn-huabei-1.xf-yun.com",
        "path": "/v1",
        "section": "iat",
        "domain": "slm",
        "language": "mul_cn",
        "accent": "mandarin",
        "note": "大模型多语种识别（32 语种；本 APPID 已于 10-10 失效，回 11201）",
    },
    # 中英识别大模型的官方主机；V2.0 是 cn-huabei-1，别搞混
    "zh_en_v2": {
        "proto": "llm",
        "host": "iat.xf-yun.com",
        "path": "/v1",
        "section": "iat",
        "domain": "slm",
        "language": "zh_cn",
        "accent": "mandarin",
        "note": "iat.xf-yun.com 主机（中英识别大模型官方地址）",
    },
    # --- 老版语音听写: common/business/data envelope, path /v2/iat ---
    "iat_legacy": {
        "proto": "legacy",
        "host": "ws-api.xfyun.cn",
        "path": "/v2/iat",
        "section": "iat",
        "domain": "iat",
        "language": "zh_cn",
        "accent": "mandarin",
        "note": "经典语音听写（流式版），老 common/business/data 结构",
    },
    "iat_api": {
        "proto": "legacy",
        "host": "iat-api.xfyun.cn",
        "path": "/v2/iat",
        "section": "iat",
        "domain": "iat",
        "language": "zh_cn",
        "accent": "mandarin",
        "note": "经典语音听写（流式版）官方推荐主机 iat-api",
    },
    # --- 实时语音转写（标准版）: 另一套签名 signa=base64(HmacSHA1(MD5(appid+ts), apiKey)) ---
    "rtasr": {
        "proto": "rtasr",
        "host": "rtasr.xfyun.cn",
        "path": "/v1/ws",
        "section": "iat",
        "domain": "ast",
        "language": "cn",
        "accent": "mandarin",
        "note": "实时语音转写（标准版，rtasr.xfyun.cn，独立商品）",
    },
}

CHUNK = 1280          # bytes per frame == 40 ms @ 16k/16bit/mono
SAMPLE_RATE = 16000
MAX_SESSION_S = 55    # doc says 60 s hard cap; stop a little early


def die(msg: str, code: int = 1):
    print(f"[x] {msg}", file=sys.stderr)
    raise SystemExit(code)


def log(tag: str, msg: str):
    print(f"[{tag}] {msg}")


# ---------------------------------------------------------------------------
# credentials
# ---------------------------------------------------------------------------

def load_creds(args) -> dict:
    if args.appid and args.apikey and args.apisecret:
        return {"appid": args.appid, "apikey": args.apikey, "apisecret": args.apisecret}
    env = {k: os.environ.get(f"XFYUN_{k.upper()}", "").strip()
           for k in ("appid", "apikey", "apisecret")}
    if all(env.values()):
        return env
    if CRED_FILE.exists():
        d = json.loads(CRED_FILE.read_text(encoding="utf-8"))
        if all(d.get(k) for k in ("appid", "apikey", "apisecret")):
            return d
    die("缺凭据。用 --appid/--apikey/--apisecret，或设 XFYUN_APPID/XFYUN_APIKEY/XFYUN_APISECRET，"
        "或先跑 --save-appid ... --save-apikey ... --save-apisecret ...")


def mask(s: str) -> str:
    return f"{s[:4]}...{s[-4:]}(len {len(s)})" if len(s) > 10 else f"len {len(s)}"


# ---------------------------------------------------------------------------
# auth -- HMAC-SHA256 signed URL
# ---------------------------------------------------------------------------

def signed_url(host: str, path: str, apikey: str, apisecret: str) -> tuple[str, str]:
    """Return (wss_url_with_query, date_string).

    The preimage is literally:
        host: iat.cn-huabei-1.xf-yun.com
        date: Mon, 08 Oct 2026 12:00:00 GMT
        GET /v1 HTTP/1.1
    -- three lines, \\n separated, trailing newline NOT included.  Getting the
    date format wrong is the single most common 403.
    """
    date = datetime.datetime.now(datetime.timezone.utc).strftime("%a, %d %b %Y %H:%M:%S GMT")
    preimage = f"host: {host}\ndate: {date}\nGET {path} HTTP/1.1"
    sig = hmac.new(apisecret.encode("utf-8"), preimage.encode("utf-8"),
                   hashlib.sha256).digest()
    sig_b64 = base64.b64encode(sig).decode("utf-8")

    origin = (f'api_key="{apikey}", algorithm="hmac-sha256", '
              f'headers="host date request-line", signature="{sig_b64}"')
    auth = base64.b64encode(origin.encode("utf-8")).decode("utf-8")

    params = {"authorization": auth, "date": date, "host": host}
    return f"wss://{host}{path}?{urlencode(params)}", date


def rtasr_url(appid: str, apikey: str, lang: str = "cn") -> tuple[str, str]:
    """RTASR 标准版 signs differently:  signa = base64(HmacSHA1(MD5(appid + ts), apiKey))

    Note it is keyed on the **APIKey**, not the APISecret, and the ts is plain
    unix seconds -- no RFC1123 date, no `authorization` blob.  Different product,
    different auth.  Returns (url, ts).
    """
    ts = str(int(time.time()))
    base = hashlib.md5((appid + ts).encode("utf-8")).hexdigest()
    signa = base64.b64encode(
        hmac.new(apikey.encode("utf-8"), base.encode("utf-8"), hashlib.sha1).digest()
    ).decode("utf-8")
    q = urlencode({"appid": appid, "ts": ts, "signa": signa, "lang": lang})
    return f"wss://rtasr.xfyun.cn/v1/ws?{q}", ts


# ---------------------------------------------------------------------------
# audio
# ---------------------------------------------------------------------------

def wav_to_pcm16k(path: Path) -> bytes:
    """Read a WAV and hand back mono 16-bit PCM at 16 kHz.

    Resampling is done with a plain linear interpolation -- good enough for a
    speech link check, and it avoids dragging numpy/scipy into the toolchain.
    For real use record straight at 16 kHz and this is a no-op.
    """
    with wave.open(str(path), "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    log("i", f"WAV: {rate} Hz, {ch} ch, {width * 8}-bit, {n} frames, {len(raw)} B")

    if width != 2:
        die(f"只支持 16-bit PCM WAV，这一个是 {width * 8}-bit。用 ffmpeg 转一下：\n"
            f"    ffmpeg -i in.wav -ar 16000 -ac 1 -c:a pcm_s16le out.wav")

    samples = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    if ch > 1:
        samples = [samples[i] for i in range(0, len(samples), ch)]   # take left channel

    if rate != SAMPLE_RATE:
        ratio = SAMPLE_RATE / rate
        out_len = int(len(samples) * ratio)
        res = []
        for i in range(out_len):
            pos = i / ratio
            i0 = int(pos)
            i1 = min(i0 + 1, len(samples) - 1)
            frac = pos - i0
            res.append(int(samples[i0] * (1 - frac) + samples[i1] * frac))
        samples = res
        log("i", f"重采样 {rate} -> {SAMPLE_RATE} Hz，{len(samples)} 个样点")

    return struct.pack(f"<{len(samples)}h", *samples)


def record_mic(seconds: int, path: Path | None = None) -> bytes:
    """Record from the PC mic as 16k/16-bit/mono PCM."""
    try:
        import sounddevice as sd          # noqa: F401
    except ImportError:
        die("录音需要 sounddevice：pip install sounddevice\n"
            "    （或者自己录一个 16k 单声道 WAV，用 --wav 传进来）")
    import sounddevice as sd
    log("i", f"录音 {seconds} 秒（对着麦克风说话）...")
    rec = sd.rec(int(seconds * SAMPLE_RATE), samplerate=SAMPLE_RATE,
                 channels=1, dtype="int16")
    sd.wait()
    pcm = rec.tobytes()
    log("i", f"录到 {len(pcm)} B ({len(pcm) / (SAMPLE_RATE * 2):.1f} s)")
    if path:
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(SAMPLE_RATE)
            w.writeframes(pcm)
        log("i", f"存到 {path}")
    return pcm


# ---------------------------------------------------------------------------
# the actual session
# ---------------------------------------------------------------------------

def frame_payload(appid: str, ep: dict, status: int, audio: bytes, seq: int = 1,
                  eos: int = 6000, dwa: str | None = "wpgs",
                  vinfo: int | None = None, extra: dict | None = None) -> str:
    """Build one request frame in whichever envelope this endpoint speaks.

    `status`: 0 first, 1 middle, 2 last.  It appears in the header on the LLM
    protocol and inside the data block on the legacy one -- and the LLM
    protocol *repeats* it under payload.audio, which looks redundant but is
    what the doc shows for both first and middle frames.
    """
    if ep["proto"] == "llm":
        parameter = {
            ep["section"]: {
                "domain": ep["domain"],
                "language": ep["language"],
                "accent": ep["accent"],
                "eos": eos,
                "result": {"encoding": "utf8", "compress": "raw", "format": "json"},
            }
        }
        if dwa:
            parameter[ep["section"]]["dwa"] = dwa
        if vinfo is not None:
            parameter[ep["section"]]["vinfo"] = vinfo
        if extra:
            parameter[ep["section"]].update(extra)

        payload = {"audio": {
            "encoding": "raw",
            "sample_rate": SAMPLE_RATE,
            "channels": 1,
            "bit_depth": 16,
            "seq": seq,
            "status": status,
            "audio": base64.b64encode(audio).decode("utf-8"),
        }}
        # Only the first frame carries parameter; the rest are header+payload.
        body = {"header": {"app_id": appid, "status": status}, "payload": payload}
        if status == 0:
            body["parameter"] = parameter
        return json.dumps(body, ensure_ascii=False)

    # --- legacy 语音听写 ---
    business = {
        "language": ep["language"],
        "domain": ep["domain"],
        "accent": ep["accent"],
        "vad_eos": eos,
        "nunum": 1,
    }
    if dwa:
        business["dwa"] = dwa
    if extra:
        business.update(extra)
    data = {"status": status, "format": f"audio/L16;rate={SAMPLE_RATE}",
            "encoding": "raw", "audio": base64.b64encode(audio).decode("utf-8")}
    body = {"data": data}
    if status == 0:
        body["common"] = {"app_id": appid}
        body["business"] = business
    return json.dumps(body, ensure_ascii=False)


def _as_json(raw) -> dict | None:
    """Decode one WebSocket frame to a dict, or None if it isn't JSON."""
    try:
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8", "replace")
        return json.loads(raw)
    except Exception:
        return None


def collect_result(msg: dict, state: dict, ep: dict) -> None:
    """Fold one server frame into state, printing partials as they arrive.

    Handles both result shapes: legacy `data.result.ws[].cw[].w` and the LLM
    service's base64-wrapped `payload.result.text` -> `.ws[].cw[].w`.
    """
    # The error code lives in `header.code` on the LLM protocol and at the top
    # level (`code`) on the legacy one.
    hdr = msg.get("header") or {}
    code = hdr.get("code", msg.get("code", 0))
    sid = hdr.get("sid", msg.get("sid", ""))
    if code != 0:
        state["error"] = f'code={code} {hdr.get("message", msg.get("message", ""))} sid={sid}'
        return
    if sid:
        state["sid"] = sid

    # Locate the text blob.
    text_b64 = ""
    if msg.get("payload"):
        pl = msg["payload"]
        text_b64 = ((pl.get("result") or {}).get("text")) or ""
        state["result_status"] = (pl.get("result") or {}).get("status")
    if not text_b64 and msg.get("data"):
        dt = msg["data"]
        text_b64 = ((dt.get("result") or {}).get("text")) or ""
        state["result_status"] = dt.get("status")
        if not text_b64:
            # Some legacy responses inline the parsed object.
            res = dt.get("result") or {}
            if res.get("ws"):
                _absorb(res, state)
                return
    if not text_b64:
        return

    try:
        raw = base64.b64decode(text_b64).decode("utf-8")
        res = json.loads(raw)
    except Exception:
        return
    _absorb(res, state)


def _absorb(res: dict, state: dict) -> None:
    """Merge a decoded result document into the word accumulator."""
    pgs = res.get("pgs")          # "apd" append / "rpl" replace
    rg = res.get("rg")            # [from, to] word indices to replace
    words = []
    for item in res.get("ws", []):
        for cand in item.get("cw", []):
            w = cand.get("w", "")
            if w:
                words.append(w)
    if not words:
        return

    if pgs == "rpl" and rg and len(rg) == 2:
        # Replace the previously-emitted words in the inclusive range, then
        # append this chunk.  Confirmed shape per the doc's 动态修正 section.
        state["words"] = state["words"][: rg[0] - 1] + words
    else:
        state["words"].extend(words)

    now = "".join(state["words"])
    if now and now != state["last_shown"]:
        state["last_shown"] = now
        log("…", f"部分结果: {now}")


# ---------------------------------------------------------------------------
# RTASR 标准版 (实时语音转写) -- a different product with a different protocol
# ---------------------------------------------------------------------------

def _absorb_rtasr(data_str: str, state: dict) -> None:
    """Fold one `action=result` payload into per-segment accumulators.

    Shape: data = json string of {"cn": {"st": {"rt": [{"ws": [{"cw":[{"w":..}]}]}]}}}
    plus a top-level seg_id.  `type` "1" is an intermediate (replace in place),
    "0" is the segment's final text.  Results are keyed by seg_id because the
    same segment is re-sent as it grows.
    """
    try:
        doc = json.loads(data_str)
    except Exception:
        return
    cn = doc.get("cn") or {}
    st = cn.get("st") or {}
    seg = doc.get("seg_id", 0)
    words = []
    for rt in st.get("rt", []):
        for item in rt.get("ws", []):
            for cand in item.get("cw", []):
                w = cand.get("w")
                if w:
                    words.append(w)
    if not words:
        return
    state.setdefault("segs", {})[seg] = "".join(words)
    now = "".join(state["segs"][k] for k in sorted(state["segs"]))
    if now and now != state.get("last_shown"):
        state["last_shown"] = now
        log("…", f"部分结果: {now}")


def transcribe_rtasr(creds: dict, pcm: bytes, ep: dict, *, lang: str = "cn",
                     raw_frames: bool = False) -> str | None:
    """实时语音转写（标准版）: raw binary audio frames, `{"end": true}` to finish."""
    import websocket

    url, ts = rtasr_url(creds["appid"], creds["apikey"], lang=lang)
    log("i", f"端点 {ep['host']}{ep['path']}  ·  协议=rtasr  ·  lang={lang}（ts={ts}）")

    ws = websocket.create_connection(
        url, timeout=15,
        sslopt={"cert_reqs": ssl.CERT_NONE},
    )
    log("+", "WebSocket 已连接")

    def recv_json(where: str) -> dict | None:
        try:
            raw = ws.recv()
        except Exception as e:
            log("!", f"{where}: 读帧失败 {type(e).__name__}: {e}")
            return None
        if raw is None or raw == "" or raw == b"":
            log("!", f"{where}: 收到空帧 / 连接已被服务端关闭")
            return None
        try:
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", "replace")
            return json.loads(raw)
        except Exception:
            log("!", f"{where}: 帧不是 JSON：{raw[:400]!r}")
            return None

    # First thing the server sends is action=started (or action=error).
    hello = recv_json("握手回应")
    if hello is None:
        die("RTASR 建会话失败：服务端没回 started。")
    log("←", f"action={hello.get('action')} code={hello.get('code')} "
             f"desc={hello.get('desc')} sid={hello.get('sid')}")
    if hello.get("action") == "error" or str(hello.get("code")) != "0":
        code = str(hello.get("code"))
        hint = {
            "10105": "没有权限（illegal access）—— 检查 appid/apiKey/ts 是否匹配。",
            "10110": ("★ no license —— 这枚 APPID **没有开通「实时语音转写」**（≠额度用尽）。"
                      "去控制台服务页确认是否添加了这个服务。"),
            "10106": "无效参数 —— 检查 signa/lang 的格式。",
            "10700": "引擎错误 —— 稍后重试。",
        }.get(code)
        die(f"RTASR 拒绝：{hello.get('desc')}" + (f"\n    {hint}" if hint else ""))

    state: dict = {"segs": {}, "last_shown": "", "sid": hello.get("sid", "")}
    chunks = [pcm[i:i + CHUNK] for i in range(0, len(pcm), CHUNK)] or [b""]
    log("i", f"音频 {len(pcm)} B -> {len(chunks)} 帧（每帧 {CHUNK} B / 40 ms，binary）")

    t0 = time.time()
    try:
        for i, c in enumerate(chunks):
            try:
                ws.send_binary(c)
            except Exception as e:
                log("!", f"帧 {i} 发不出去：{type(e).__name__}")
                break
            if raw_frames:
                log("→", f"帧 {i} {len(c)} B")
            msg = recv_json(f"帧 {i} 回应")
            if msg:
                if msg.get("action") == "error":
                    die(f"RTASR 中断：{msg.get('desc')} (code={msg.get('code')})")
                if msg.get("data"):
                    _absorb_rtasr(msg["data"], state)
            if time.time() - t0 < 1.0:
                time.sleep(0.04)
            if time.time() - t0 > MAX_SESSION_S:
                log("!", f"已 {MAX_SESSION_S}s，主动收尾")
                break

        ws.send_binary(b'{"end": true}')
        log("→", '结束标识 {"end": true}')

        ws.settimeout(10)
        while True:
            try:
                raw = ws.recv()
            except Exception:
                break
            if raw is None or raw == "" or raw == b"":
                break
            try:
                msg = json.loads(raw.decode("utf-8", "replace")
                                 if isinstance(raw, bytes) else raw)
            except Exception:
                continue
            if msg.get("action") == "error":
                log("!", f"收尾报错：{msg.get('desc')} (code={msg.get('code')})")
                break
            if msg.get("data"):
                _absorb_rtasr(msg["data"], state)
    finally:
        try:
            ws.close()
        except Exception:
            pass

    text = "".join(state["segs"][k] for k in sorted(state["segs"])).strip()
    log("i", f"耗时 {time.time() - t0:.2f}s，"
             + (f"sid={state['sid']}" if state["sid"] else ""))
    return text or None


def transcribe(creds: dict, pcm: bytes, ep_key: str = "zh_cn", *,
               dwa: str | None = "wpgs", eos: int = 6000, vinfo: int | None = None,
               raw_frames: bool = False, extra: dict | None = None) -> str | None:
    """Open a WebSocket session and return the final transcript (or None).

    Note there is no `ptt` argument: Push-to-Talk is not a protocol flag.  The
    client implements it by sending the status=2 frame the instant the button
    is released, which is what this function does as soon as `pcm` runs out.
    """
    try:
        import websocket      # websocket-client
    except ImportError:
        die("需要 websocket-client：pip install websocket-client")

    ep = ENDPOINTS[ep_key]
    if ep["proto"] == "rtasr":
        return transcribe_rtasr(creds, pcm, ep, lang=ep["language"], raw_frames=raw_frames)
    host, path = ep["host"], ep["path"]
    url, date = signed_url(host, path, creds["apikey"], creds["apisecret"])
    log("i", f"端点 {host}{path}  ·  协议={ep['proto']}  ·  "
             f"{ep['section']}.domain={ep['domain']}  language={ep['language']}")
    log("i", f"签名日期 {date}（服务器与本机时钟偏差 >300s 会 403）")

    ws = websocket.create_connection(
        url, timeout=15,
        sslopt={"cert_reqs": ssl.CERT_NONE},   # some corporate proxies MITM this host
    )
    log("+", "WebSocket 已连接")

    def recv_json(where: str) -> dict | None:
        """Read one frame, tolerating both a text JSON and a raw close.

        iFlytek answers a bad `common`/`business` by sending a small JSON
        object and then closing.  websocket-client surfaces that second step as
        an empty frame, which would otherwise look like a network fault.
        """
        try:
            raw = ws.recv()
        except Exception as e:
            log("!", f"{where}: 读帧失败 {type(e).__name__}: {e}")
            return None
        if raw is None or raw == "" or raw == b"":
            log("!", f"{where}: 收到空帧 —— 服务端在正常回应之后主动关闭了连接")
            return None
        try:
            if isinstance(raw, bytes):
                raw = raw.decode("utf-8", "replace")
            return json.loads(raw)
        except Exception:
            log("!", f"{where}: 帧不是 JSON：{raw[:400]!r}")
            return None

    state = {"words": [], "last_shown": "", "error": None, "result_status": None,
             "sid": ""}
    total = len(pcm)
    sent = 0
    idx = 0
    chunks = [pcm[i:i + CHUNK] for i in range(0, total, CHUNK)] or [b""]
    log("i", f"音频 {total} B -> {len(chunks)} 帧（每帧 {CHUNK} B / 40 ms）")

    t0 = time.time()
    try:
        # --- first frame (status 0): carries header.app_id + parameter ---
        first = frame_payload(creds["appid"], ep, 0, chunks[0], seq=idx + 1,
                              eos=eos, dwa=dwa, vinfo=vinfo, extra=extra)
        if raw_frames:
            log("→", "首帧 " + first)
        ws.send(first)
        sent += len(chunks[0])
        log("→", f"帧 {idx} status=0  {len(chunks[0])} B")
        msg = recv_json("首帧回应")
        if msg is None:
            raise SystemExit(
                "[x] 服务端在建会话后立刻关闭。\n"
                "    签名是对的（否则握手就 403 了），问题在 header/parameter 这一层。\n"
                "    按经验顺序排查：\n"
                "      · 这枚 APPID 没开通这个服务（大模型识别/听写/转写是三个独立商品）\n"
                "      · language/domain/accent 与所购服务不匹配\n"
                "      · 帧结构用错了协议代（老 common/business/data vs 新 header/parameter/payload）\n"
                "    跑 --sweep 把每个端点都试一遍。"
            )
        log("←", f"code={msg.get('header', {}).get('code', msg.get('code'))} "
                 f"{msg.get('header', {}).get('message', msg.get('message', ''))}")
        collect_result(msg, state, ep)
        if state["error"]:
            code = state["error"].split()[0].split("=")[-1]
            hint = {
                "11200": ("该 APPID 没有这个功能的授权。去控制台服务页确认已添加对应服务，"
                          "或先领免费额度。"),
                "11201": ("★ 每日交互次数已用尽（免费额度）。静音握手不扣额度所以能过，"
                          "一送真音频就失败 —— 这就是免费额度耗尽的特征曲线。\n"
                          "        明天额度会刷新；要长期用就买套餐。"),
                "11202": "秒级并发超限：请求太密，降一点频率。",
                "10313": "appid 与 apikey 不匹配 —— 检查是不是把别的应用的 Key 配到这儿了。",
                "10163": "参数不合法 —— 多半是 language/domain/accent 与所购服务不匹配。",
                "10165": "无效句柄 —— 首帧必须带 status=0 且带完整 header.app_id。",
            }.get(code)
            die(f"服务端拒绝：{state['error']}" + (f"\n    {hint}" if hint else ""))
        idx += 1

        # --- middle frames ---
        #
        # ★ Do NOT block waiting for a reply per frame. ★  The server only
        # pushes a message when it actually has a result, so a blocking recv()
        # here can sit on its 15 s timeout, time the socket out, and kill the
        # session after three frames -- which is exactly what the V2.0 endpoint
        # did on 2026-10-10 while answering `code=0 success` to every frame.
        # Poll instead: send at the 40 ms cadence and drain whatever is pending.
        ws.settimeout(0.01)
        dead = False
        for i in range(1, len(chunks)):
            try:
                ws.send(frame_payload(creds["appid"], ep, 1, chunks[i], seq=idx + 1,
                                      eos=eos, dwa=dwa, vinfo=vinfo, extra=extra))
            except Exception as e:
                log("!", f"帧 {idx} 发不出去：{type(e).__name__} —— 服务端已经关闭了连接")
                dead = True
                break
            sent += len(chunks[i])
            if raw_frames:
                log("→", f"帧 {idx} status=1  {len(chunks[i])} B")
            # Drain anything the server has queued, without waiting.
            while True:
                try:
                    raw = ws.recv()
                except Exception as e:
                    if type(e).__name__ in ("WebSocketTimeoutException", "TimeoutError"):
                        break                      # nothing pending, keep streaming
                    log("!", f"帧 {idx} 读帧失败 {type(e).__name__}: {e}")
                    dead = True
                    break
                if raw is None or raw == "" or raw == b"":
                    dead = True
                    break
                msg = _as_json(raw)
                if msg is not None:
                    collect_result(msg, state, ep)
                    if state["error"]:
                        die(f"服务端报错：{state['error']}")
            if dead:
                break
            # 40 ms cadence for the first second keeps the server's VAD happy on
            # short clips; after that we go as fast as we can read, which is
            # near-realtime for a 16 kHz stream on a LAN.
            if time.time() - t0 < 1.0:
                time.sleep(0.04)
            idx += 1
            if time.time() - t0 > MAX_SESSION_S:
                log("!", f"已 {MAX_SESSION_S}s，主动收尾（服务端硬上限 60s）")
                break

        # --- last frame (status 2): this is our Push-to-Talk release ---
        last = frame_payload(creds["appid"], ep, 2, b"", seq=idx + 1,
                             eos=eos, dwa=dwa, vinfo=vinfo, extra=extra)
        if raw_frames:
            log("→", "末帧 " + last)
        try:
            ws.send(last)
            log("→", "末帧 status=2  (0 B)")
        except Exception as e:
            log("!", f"末帧发不出去：{type(e).__name__}")

        ws.settimeout(10)
        while True:
            msg = recv_json("收尾")
            if msg is None:
                break
            collect_result(msg, state, ep)
            if state["error"]:
                die(f"服务端报错：{state['error']}")
            # LLM protocol reports end on header.status; legacy on data.status.
            hdr_st = (msg.get("header") or {}).get("status")
            if hdr_st == 2 or state["result_status"] == 2:
                log("+", "服务端已结束本次会话 (status=2)")
                break
    finally:
        try:
            ws.close()
        except Exception:
            pass

    text = "".join(state["words"]).strip()
    log("i", f"耗时 {time.time() - t0:.2f}s，发送 {sent}/{total} B"
             + (f"，sid={state['sid']}" if state["sid"] else ""))
    return text or None


# ---------------------------------------------------------------------------
# commands
# ---------------------------------------------------------------------------

def cmd_selftest(creds: dict, args) -> int:
    """Everything except the socket: sign, frame, size-check.  No network."""
    ep = ENDPOINTS["zh_cn"]
    url, date = signed_url(ep["host"], ep["path"], creds["apikey"], creds["apisecret"])
    log("i", f"APPID {creds['appid']}  APIKey {mask(creds['apikey'])}  "
             f"APISecret {mask(creds['apisecret'])}")
    log("i", f"date = {date}")
    log("i", f"授权串长度 {len(url.split('authorization=')[1].split('&')[0])} (base64)")
    print(f"\n    {url[:150]}...\n")

    # 1 s of silence == exactly 25 frames of 1280 B
    pcm = b"\x00\x00" * SAMPLE_RATE
    for st in (0, 1, 2):
        audio = pcm[:CHUNK] if st != 2 else b""
        js = frame_payload(creds["appid"], ep, st, audio, seq=st + 1, eos=6000)
        d = json.loads(js)
        assert d["header"]["status"] == st, "header.status 不对"
        assert d["header"]["app_id"] == creds["appid"], "header.app_id 不对"
        if st == 0:
            assert "parameter" in d and "iat" in d["parameter"], "首帧缺 parameter.iat"
        else:
            assert "parameter" not in d, f"status={st} 不该带 parameter（只在首帧发）"
        if st != 2:
            got = len(base64.b64decode(d["payload"]["audio"]["audio"]))
            assert got == CHUNK, f"帧长 {got} != {CHUNK}"
        a = d["payload"]["audio"]
        for k in ("encoding", "sample_rate", "channels", "bit_depth", "seq", "status"):
            assert k in a, f"payload.audio 缺 {k}"
        if st == 0:
            log("i", "首帧 parameter = "
                     + json.dumps(d["parameter"], ensure_ascii=False))
            log("i", "首帧 payload.audio 元数据 = "
                     + json.dumps({k: v for k, v in a.items() if k != "audio"},
                                  ensure_ascii=False))
    log("+", "签名 / header+parameter+payload 组帧 / 1280B 校验全部通过（未联网）")

    # Show the legacy envelope too, so the difference is visible side by side.
    lj = json.loads(frame_payload(creds["appid"], ENDPOINTS["iat_legacy"], 0,
                                  pcm[:CHUNK], seq=1))
    log("i", "对比 · 老版协议首帧顶层键 = " + ", ".join(lj.keys()))
    log("i", "下一步：python xfyun_asr.py --wav <16k 单声道 wav> 或 --mic 5")
    return 0


def cmd_endpoints() -> int:
    print(f"{'key':11} {'proto':7} {'host/path':40} {'language':11} note")
    print("-" * 118)
    for k, v in ENDPOINTS.items():
        print(f"{k:11} {v['proto']:7} {(v['host'] + v['path']):40} "
              f"{v['language']:11} {v['note']}")
    return 0


def cmd_run(creds: dict, args) -> int:
    if args.wav:
        pcm = wav_to_pcm16k(Path(args.wav))
    elif args.mic:
        pcm = record_mic(int(args.mic))
    else:
        # No audio source: synthesise 1 s of silence so we can still exercise
        # the handshake and learn *which* error the account produces.
        log("i", "没给音频，用 1 秒静音做握手测试（只看能不能建会话）")
        pcm = b"\x00\x00" * SAMPLE_RATE

    text = transcribe(creds, pcm, args.endpoint,
                      dwa=None if args.no_dwa else args.dwa, eos=args.eos,
                      vinfo=args.vinfo, raw_frames=args.raw, extra=args.extra or None)
    if text is None:
        print("\n[!] 没拿到识别结果（看上面是哪个环节断的）")
        return 1
    print(f"\n[识别结果] {text}")
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        log("i", f"已写入 {args.out}")
    return 0


def cmd_sweep(creds: dict, args) -> int:
    """Try every endpoint and report which ones this APPID can actually open.

    There is no API to ask iFlytek "what did I buy", so this is the answer.
    Run it whenever credentials change.
    """
    pcm = b"\x00\x00" * (SAMPLE_RATE // 2)   # 0.5 s
    results = {}
    for key in ENDPOINTS:
        ep = ENDPOINTS[key]
        log("i", f"--- 试 {key} ({ep['host']}) ---")
        try:
            text = transcribe(creds, pcm, key, dwa=None)
            results[key] = (f"★ 可用，返回 {text!r}" if text
                            else "★ 会话建立并正常收尾（静音无文本属正常）")
        except SystemExit as e:
            results[key] = f"FAIL (exit {e.code})"
        except Exception as e:
            results[key] = f"FAIL {type(e).__name__}: {str(e)[:120]}"
    print("\n=== 端点可用性 ===")
    for k, v in results.items():
        print(f"  {k:11} {v}")
    print("""
判读方法（2026-10-08 实测）：
  · code=0 / 正常收尾      -> 这个服务已开通且还有额度，可以用
  · code=11200             -> 没开通这个服务
  · code=11201             -> 开通了但当日额度用尽；静音握手仍能过，真音频必挂
  · HTTP 403 handshake     -> 这个 APPID 连这条路由都没有（服务不存在）
""")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="讯飞流式语音识别 WebSocket 测试",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Protocol notes")[0],
    )
    ap.add_argument("--appid")
    ap.add_argument("--apikey")
    ap.add_argument("--apisecret")
    ap.add_argument("--save-appid", metavar="V")
    ap.add_argument("--save-apikey", metavar="V")
    ap.add_argument("--save-apisecret", metavar="V")
    ap.add_argument("--endpoint", default="mul_cn", choices=list(ENDPOINTS),
                    help="默认 mul_cn（这枚 APPID 实测开通的是多语种大模型）")
    ap.add_argument("--dwa", default="wpgs", help="动态修正，wpgs=流式返回（默认）")
    ap.add_argument("--no-dwa", action="store_true", help="关掉流式部分结果，只要最终结果")
    ap.add_argument("--eos", type=int, default=6000,
                    help="尾静音截断 ms（默认 6000；说几句会中间停顿就调大）")
    ap.add_argument("--vinfo", type=int, choices=[0, 1], help="句子级帧对齐，返回 vad 边界")
    ap.add_argument("--extra", help='塞进 parameter.iat 的额外 JSON，例如 \'{"ltc":1}\'')
    ap.add_argument("--wav", help="16k 单声道 16-bit WAV 文件")
    ap.add_argument("--mic", metavar="SEC", help="录 N 秒 PC 麦克风再识别")
    ap.add_argument("--out", help="把识别文本写到文件")
    ap.add_argument("--raw", action="store_true", help="打印每一帧的收发 JSON")
    ap.add_argument("--selftest", action="store_true", help="只做签名/组帧自检，不联网")
    ap.add_argument("--list-endpoints", action="store_true")
    ap.add_argument("--sweep", action="store_true", help="逐个端点试，看这枚 APPID 能用哪些")
    args = ap.parse_args()
    if args.extra:
        try:
            args.extra = json.loads(args.extra)
        except json.JSONDecodeError:
            die(f"--extra 不是合法 JSON：{args.extra}")

    saved = {k: v for k, v in (("appid", args.save_appid), ("apikey", args.save_apikey),
                               ("apisecret", args.save_apisecret)) if v}
    if saved:
        cur = json.loads(CRED_FILE.read_text(encoding="utf-8")) if CRED_FILE.exists() else {}
        cur.update(saved)
        CRED_FILE.write_text(json.dumps(cur, ensure_ascii=False, indent=2), encoding="utf-8")
        log("+", f"已保存到 {CRED_FILE}: {', '.join(saved)}")
        if len(saved) < 3 and not (args.selftest or args.wav or args.mic or args.sweep):
            return 0

    if args.list_endpoints:
        return cmd_endpoints()

    creds = load_creds(args)
    log("i", f"APPID {creds['appid']}  APIKey {mask(creds['apikey'])}  "
             f"APISecret {mask(creds['apisecret'])}")

    if args.selftest:
        return cmd_selftest(creds, args)
    if args.sweep:
        return cmd_sweep(creds, args)
    return cmd_run(creds, args)


if __name__ == "__main__":
    raise SystemExit(main())
