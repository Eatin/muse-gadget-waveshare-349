# host-tools — 在烧板子之前，先确认你的账号能跑通

板子上的语音链路依赖两家云服务：**讯飞开放平台**（语音识别 ASR + 语音合成 TTS）和
**商汤 SenseNova**（大模型）。这两家的开通状态、额度、产品线对不上时，板子上只会看到一句含糊的
错误，很难判断是固件问题还是账号问题。这三个脚本就是在 PC 上把账号先验一遍。

Host-side checkers for the two cloud services the firmware talks to (iFlytek and
SenseNova). Run them before flashing — a wrong product line or an exhausted quota
otherwise looks exactly like a broken board.

全部只用标准库，**除了 `sensenova_chat.py` 需要 `requests`**：

```bash
pip install requests
```

---

## 1. 先放凭据

真实凭据**不进版本库**。复制示例文件再填：

```bash
cd host-tools
cp xfyun_key.example.json xfyun_key.json        # 讯飞 APPID / APIKey / APISecret
cp sensenova_key.example.txt sensenova_key.txt  # 商汤 key，形如 sk-xxxxxxxx
```

两个文件都已在 `.gitignore` 里。脚本也支持环境变量（`XFYUN_APPID` / `XFYUN_APIKEY` /
`XFYUN_APISECRET` / `SENSENOVA_API_KEY`）和命令行参数。

填写完之后建议：**开源前轮换一次 key**。密钥一旦在本地明文存放过，轮换比事后追查便宜得多。

---

## 2. `xfyun_asr.py` —— 语音识别，顺带判断「这个 APPID 能跑哪条产品线」

这是最该先跑的一个。讯飞同一枚 APPID 底下可能开了好几条产品线，用错那条的表现是
**握手成功、第一帧音频才回 `11201`**，非常容易被误判成「服务挂了」。

```bash
# 一次跑遍所有已知端点，逐个告诉你结果
python xfyun_asr.py --sweep

# 用自带示例音频试一条线（sample_speech.wav 是已知内容的合成语音）
python xfyun_asr.py --wav sample_speech.wav --endpoint zh_v2

# 用 PC 麦克风录 5 秒再识别
python xfyun_asr.py --mic 5

# 只看签名/组帧对不对，不联网
python xfyun_asr.py --selftest
```

结果判读：

| 返回 | 含义 |
|---|---|
| `code=0` | 这条线能跑 —— 把它填进 `CONFIG_MUSE_CN_XFYUN_LANGUAGE` |
| `11201` | **开通了，但当日额度用尽**（判据：静音握手仍 `code=0`，送真实音频才报 11201） |
| `11200` | 这个 APPID **没开通**这条线 |
| `HTTP 403` | 地址写错了 —— 网关回了，但那条路由不存在（别当成「没开通」） |

已确认可用的组合是 `zh_cn` + `spark_asr_v2`（语音听写大模型（中英）V2.0）。若你买的是别的产品，
用 `--sweep` 找出自己该用哪条。

---

## 3. `xfyun_tts.py` —— 语音合成，确认账号有没有开通在线合成

板子的回话是**讯飞在线合成**出声的，返回 `aue=raw` 的 16 kHz PCM，直接喂喇叭、不需要解码器。

```bash
# 三种发音人/端点各跑一次，看哪些返回 code=0
python xfyun_tts.py --probe --out probe.wav

# 正常合成一句
python xfyun_tts.py "你好，我是微雪。" --vcn xiaoyan --out hello.wav
```

发音人：`xiaoyan`（小燕/女，默认）、`xiaofeng`（小峰/男）、`x4_yezi`（叶子/女）、
`aisjiuxu`（许久/男）。要换板子上的默认发音人，改 `esp32/components/muse/muse_chat_cn.c`
里的 `TTS_VOICE`。

> 顺带一个和板子行为有关的实测结论：讯飞合成完之后还会**把连接挂约 15 秒才关**。
> 板子上一个 WebSocket 客户端要吃掉 8 KB **连续**内部 RAM，本板空闲时最大块只有约 13 KB，
> 所以 `esp32/components/muse/muse_chat_cn.c` 里的收尾必须由回合任务自己 `stop()` 再
> `destroy()`，不能等对端关。改那段代码前先看它上面的注释。

---

## 4. `sensenova_chat.py` —— 大模型，确认 key 与模型 id

```bash
python sensenova_chat.py --models              # 列出这个 key 能用的模型
python sensenova_chat.py --once "你好"          # 问一句
python sensenova_chat.py --once "讲个笑话" --system "回答不超过 30 个字"
```

板端默认 `deepseek-v4-flash` + `reasoning_effort="none"`。这不是随手选的：
带 reasoning 的模型实测 12 秒，`none` 只要 1.2 秒，而板子每一轮都要等这一次调用。

**429 是限流，不是断网。** PC 上实测约 1/3 概率撞到，重试一两次就过；固件里已做了三次退避重试，
并把文案分流成「商汤限流了，稍等再问一次」，而不是原先那句误导人的「连不上商汤，检查 WiFi」。

---

## 结果对照

| 脚本 | 对应板端配置 |
|---|---|
| `xfyun_asr.py --sweep` | `CONFIG_MUSE_CN_XFYUN_LANGUAGE` / `..._DOMAIN` |
| `xfyun_tts.py --probe` | 有无在线合成额度；发音人在 `muse_chat_cn.c` 的 `TTS_VOICE` |
| `sensenova_chat.py --models` | `CONFIG_MUSE_CN_SENSENOVA_MODEL` |
