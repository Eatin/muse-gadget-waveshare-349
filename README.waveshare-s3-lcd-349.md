# Waveshare ESP32-S3-Touch-LCD-3.49 V2 上的 Muse 语音助手（中文链路）

> **English summary.** A port of [Meta Muse Gadgets](https://github.com/facebookincubator/muse-gadget-sdk)
> to the **Waveshare ESP32-S3-Touch-LCD-3.49 V2**, plus a Chinese voice backend
> (`MUSE_CN_VOICE`) built on **iFlytek ASR + SenseNova LLM + iFlytek TTS**. It runs with
> **no Meta account, no SDK token and no Muse app**: press the BOOT button or the
> on-screen button, speak, and the answer is shown as paged captions *and* spoken out
> loud. Apache-2.0, forked at upstream `86cf33f`; see [`NOTICE`](NOTICE).

一块 172×640 的小屏幕、一个麦克风、一个喇叭，按住说话，屏幕上出字幕、喇叭里出声音。
不接 Meta 账号，不配对，不装 App。

```
按住 BOOT(GPIO0) 或点屏幕右侧圆钮
      │
      ├─ 16 kHz PCM 攒进 PSRAM
      │
      ├─ 讯飞 ASR（WebSocket，大模型识别）      → 文字
      ├─ 商汤 SenseNova（HTTPS，deepseek-v4-flash）→ 回答
      │
      ├─ 屏幕：中文字幕，一屏 40 字，长回答自动翻页
      └─ 喇叭：讯飞 TTS（raw 16 kHz PCM）念出来
```

---

## 一、这个移植做了什么

| 模块 | 内容 |
|---|---|
| 板级驱动 | `esp32/components/muse/boards/board_waveshare_s3_lcd_349.c` —— AXS15231B QSPI 面板、触摸、ES8311 喇叭 + ES7210 麦克风、TCA9554 扩展器、电池、背光、横屏整帧旋转 |
| 中文语音后端 | `esp32/components/muse/muse_chat_cn.c` —— 讯飞 ASR / 商汤 LLM / 讯飞 TTS，实现与官方 `muse_chat.h` 相同的接口，`MUSE_CN_VOICE` 开关 |
| UI | 屏幕右缘一个触摸录音钮（点一下开始、再点一下结束），录音中变红；字幕带跟读翻页 |
| 板级默认配置 | `esp32/devices/sdkconfig.waveshare-s3-lcd-349`（**不含密钥**） |

**上游的东西一行没删。** 官方 Meta VM 会话（`MUSE_HATCH`）仍在原处，只是和 `MUSE_CN_VOICE` 互斥；
不选 `MUSE_CN_VOICE` 就是原汁原味的 Muse Gadget。改了哪些文件、为什么改，逐条列在 [`NOTICE`](NOTICE) 里。

---

## 二、硬件

| 项 | 规格 |
|---|---|
| 主控 | ESP32-S3R8，16 MB flash（W25Q128JVSI，QIO）、8 MB octal PSRAM |
| 屏幕 | 3.49"，**AXS15231B**，QSPI，原生 172×640（本移植按 **640×172 横屏**显示） |
| 触摸 | 面板控制器自带，I2C |
| 音频 | ES8311（喇叭 DAC）+ ES7210（麦克风 ADC），I2S 双工 |
| 扩展器 | TCA9554（面板复位 EXIO5、背光使能 EXIO1、电源锁存） |
| 按键 | BOOT = GPIO0，PWR = GPIO16（长按进 deep sleep） |
| 其他 | 背光 PWM = GPIO42（低有效）、电池 ADC = GPIO4、原生 USB Serial/JTAG |

引脚以微雪官方 ESP-IDF 示例（`07_BATT_PWR_Test` / `08_Audio_Test` / `09_LVGL_V8_Test`）源码为准。

---

## 三、跑起来

### 1. 环境

需要 **ESP-IDF v6.0.1**（`>=5.5` 应该也行，但本移植只在 v6.0.1 上验证过）。

### 2. 填密钥

板级默认配置里的凭据字段**故意留空**，直接编译出来的固件连不上任何服务。真实凭据放在一个被
git 忽略的副本里（`tools/board.sh` 会在最后加载它，优先级最高）：

```bash
cd esp32
cp devices/sdkconfig.waveshare-s3-lcd-349 \
   devices/sdkconfig.muse-waveshare-s3-lcd-349
$EDITOR devices/sdkconfig.muse-waveshare-s3-lcd-349
```

要填四个字段：

| 配置项 | 去哪拿 |
|---|---|
| `CONFIG_MUSE_CN_XFYUN_APPID` / `_APIKEY` / `_APISECRET` | [讯飞开放平台](https://console.xfyun.cn/) 建应用后可见 |
| `CONFIG_MUSE_CN_XFYUN_LANGUAGE` / `_DOMAIN` | 必须匹配你的 APPID 买的产品线，见下 |
| `CONFIG_MUSE_CN_SENSENOVA_KEY` | [商汤 SenseNova](https://platform.sensenova.cn/) 的 API Key，`sk-` 开头 |
| `CONFIG_HOMEHUB_WIFI_SSID` / `_PASSWORD` | 你家 WiFi。填了这个就**跳过 BLE 配网**，开机直连 |

**产品线这一项最容易翻车。** 讯飞同一枚 APPID 底下可能开过好几条线，用错的表现是
**握手成功、第一帧音频才回 `11201`**，看起来完全像服务挂了。本移植验证可用的是：

```c
CONFIG_MUSE_CN_XFYUN_LANGUAGE="zh_cn"
CONFIG_MUSE_CN_XFYUN_DOMAIN="spark_asr_v2"   // 语音听写大模型（中英）V2.0
```

不确定自己该用哪条？先跑 [`host-tools/xfyun_asr.py --sweep`](host-tools/README.md)，它会一条条试给你看。

### 3. 编译、烧录

```bash
cd esp32
./tools/board.sh waveshare-s3-lcd-349 build
./tools/board.sh waveshare-s3-lcd-349 flash-monitor /dev/ttyACM0   # Windows 上是 COMx
```

`tools/board.sh` 会把构建产物放进 `build-waveshare-s3-lcd-349/`。

> **Windows 提醒**：本移植主要在 Windows + Git Bash 上开发。这台机器上 `idf.py` 的 wrapper 和
> CMake 路径都会卡住，直接调 `ninja` 反而稳，所以开发时用的是自己的 bat 包装（不在本仓库内）。
> 正常机器上 `tools/board.sh` 就够用。

### 4. 开机应该看到什么

```
I (540)  link.main: Muse Gadget starting
I (668)  muse_ui: UI up: 640x172, 114 px Muse
I (…)    muse_cn: CN voice backend armed (ASR=<你的 APPID>, LLM=deepseek-v4-flash)
I (…)    muse_cn: reply page: 80 x 2, CJK 40 x 1
I (…)    link.heartbeat: hb t=5s setup=advertising wifi=up ws=down …
```

判在线**只看心跳里的 `wifi=up`**。`wifi:connected` 和 `got IP` 都会出现然后被踢掉
（原因见下面「Wi-Fi 那条坑」），`ping` 也没用（省电模式不回 ICMP）。

### 5. 怎么用

- **BOOT 键**：按住说话，松手发送。
- **屏幕圆钮**：点一下开始录（按钮变红），说完再点一下结束。
- 松手到出字约 1.5–2 秒（TLS 握手 + 推流）。
- 回复先出字幕，再出声；长回答会自动翻页，语音放完后继续把没念到的部分翻完。

---

## 四、密钥与隐私

- 真实凭据只在 `esp32/devices/sdkconfig.muse-waveshare-s3-lcd-349` 和
  `host-tools/xfyun_key.json`、`host-tools/sensenova_key.txt`，**三个都在 `.gitignore` 里**。
- 仓库里那份 `sdkconfig.waveshare-s3-lcd-349` 字段是空的，就是给人当模板用的。
- 固件产物（`build-*/`）会把 WiFi 密码编进二进制，所以整个 `build-*/` 都不入库。
- 建议：**开源前把用过的 key 轮换一次**。

---

## 五、这版验证到哪一步了

**真机验证通过**（串口日志为证）：

| 环节 | 结果 |
|---|---|
| 触摸录音钮 | `PTT down/up (touch button)`，录到 4.72 s 真实语音（rms −43.7 dBFS） |
| 讯飞 ASR | `heard: 你好，你是谁？你能干什么？` |
| 商汤 LLM | 撞到 429 后自动退避重试成功，回 `我是你的桌面小管家…` |
| 讯飞 TTS | `TTS: 278130 bytes in 2563 ms` → `muse reply: 8.69s of audio` |
| 启动 | `UI up: 640x172` / `wifi=up` / 无 panic |

**还没验证**：长回答（超过一屏 40 字）的自动翻页只验证到「不会出错」，真正翻页的观感没有人眼确认过。

---

## 六、踩过的坑（改代码前建议先看）

这些都是真机上量出来的，不是猜的。

### 屏与触摸

- **QSPI 忽略行地址**，LVGL 必须 direct mode 且**整帧从第 0 行开始刷**。用
  `lv_display_set_rotation()` 会产生局部窗口更新，除首块外**被静默丢弃**。本移植自己在
  `flush()` 里按带旋转（`gather_band()`，一带来 32 行）。
- **触摸 IC 的 X/Y 与面板是转置的**：`panel_col = raw_Y`、`panel_row = 639 − raw_X`。
  别照抄 SDK 里另一块 AXS15231B 板（排线朝向不同）。写错的代价是双重的：轴反 + 钳位把 0..639
  砍成 0..171，表现是**四分之三屏幕点不到**。
- `CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT` **必须 = 1**。每个 draw unit 会起一个 8 KB 内部 RAM 线程，
  本板给不出来，LVGL 会打 `lv_thread_init: xTaskCreate failed!`。
- 所有 `MALLOC_CAP_DMA|INTERNAL` 的缓冲要**一起算总账**。帧带宽设成 64 行会吃光 DMA 池，
  I2S 连 320 B 都分不出来；本移植用 32 行。

### 内存（本板最稀缺，只有约 28 KB 内部 RAM）

- 一轮对话要吃掉约 **9.6 KB（8 KB WebSocket 栈 + 两个 4 KB 缓冲）+ 约 10 KB TLS**。
  大缓冲（回复文本、历史、事件、ASR 词表）全部标 `EXT_RAM_BSS_ATTR` 落 PSRAM。
- **「WebSocket 连接失败」几乎总是内部 RAM 见底，不是网络问题。** `esp_websocket_client_start()`
  内部是 `xTaskCreate(stack=8192)`，需要**一块连续 8 KB 内部 RAM**。日志里认这一行：
  `turn start: internal free / largest`。
- WebSocket 客户端的**释放**是这套代码里最讲究的地方：不能从别的任务调 `stop()/destroy()`
  （会和 socket 任务自拆竞态 → **静默复位，没有 panic 输出**）。本移植的做法是
  `ws_release()`：会话还活着就 `stop()`+50 ms+`destroy()`；已经自己掉线的就停进一个等待槽，
  下一轮再释放。改那段前先读它上面的注释。

### 讯飞

- 签名用的是 **HMAC-SHA256 的 RFC1123 GMT 日期**，板子没有 RTC，所以必须先 SNTP 对时，
  否则一律 403。**`CONFIG_MBEDTLS_HAVE_TIME_DATE` 默认没开 ⇒ mbedtls 不校验证书日期 ⇒
  「商汤 HTTPS 跑通」不能证明时钟是对的** —— 这个骗过我们一次。
- **TTS 的音帧是 13–45 KB**，而客户端接收缓冲只有 4 KB，一帧会被切成 4–12 段回调。
  把每一段当完整 JSON 解析必然全部失败，表现是 **`TTS WebSocket connected` 紧跟
  `TTS: 0 bytes in 8 s (timed out)`**。必须按 `payload_offset` 重组（缓冲落 PSRAM），
  而且**解析要用手写函数，不能用 cJSON**——45 KB 的 JSON 要吃掉约 50 KB 内部 RAM。
- 讯飞合成完还会挂约 15 秒才关连接（见 `host-tools/README.md`）。
- **`CONFIG_MUSE_CN_AUTOTEST` 必须关**。开着时开机就自占一整轮（约 2 分钟），期间所有按键都被拒，
  表现就是「按了没反应」，而且每次上电都烧一次额度。

### Wi-Fi

- 填了 `CONFIG_HOMEHUB_WIFI_SSID` 就跳过 BLE 配网。但**只连上不够**：拿完 DHCP 后约 0.2 秒会被
  link keeper 踢掉，因为「已知网络」列表只由配网流程填充，override 的 SSID 会被当成「用户已忘记的
  网络」。验收方式很有欺骗性——`wifi:connected` 和 `got IP` 都会出现，**只有心跳的 `wifi=up` 才算过**。
  修法是连上后调 `wifi_known_remember()` 把它注册进去（已落在 `main/app.c`）。

### 串口（如果串口表现诡异，先看这条）

- **一开一关这个串口就会把板子复位** —— `close()` 释放 DTR/RTS 正好命中 USB-Serial-JTAG 的复位
  条件。所以「跑一个读串口的脚本」等于按了一次复位键，表现极像「固件闪退」。诊断时**只开一个
  串口、全程不关**。别碰 DTR/RTS：`serial.Serial(port, 115200, timeout=.3, dsrdtr=False, rtscts=False)`。
- 长按 PWR 会进 deep sleep，**USB 直接掉线**，看起来和崩溃一模一样；而且掉线后不回来
  （监视器重试 0 次就是它，不是崩溃）。下次开机日志里的 `Last reset: <原因>` 会告诉你真相。

---

## 七、主机侧工具

[`host-tools/`](host-tools/README.md)：在烧板子之前，先把讯飞 / 商汤的账号验一遍
（`xfyun_asr.py --sweep` 判断产品线、`xfyun_tts.py --probe` 判断有无在线合成、
`sensenova_chat.py --models` 看模型）。全部纯标准库，`sensenova_chat.py` 需要 `requests`。

---

## 八、许可与署名

- 本仓库是 [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)
  的 fork，取于 commit `86cf33f`，沿用上游的 **Apache License 2.0**（见 [`LICENSE`](LICENSE)）。
- 改动清单（Apache-2.0 §4(b) 要求的「声明修改」）见 [`NOTICE`](NOTICE)。
- 本 fork **与 Meta 无关，未获其背书或支持**；「Meta」「Muse」等名称仅用于指明上游来源。
  本 fork 的问题请提到本仓库，不要提到上游。
- 讯飞（讯飞开放平台）与商汤（SenseNova）是第三方服务，各自适用其服务条款；
  使用它们产生的一切责任由使用者承担。
