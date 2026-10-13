/*
 * Chinese voice assistant backend: iFlytek ASR + SenseNova LLM.
 *
 * Drop-in replacement for muse_chat_session.cpp (Meta's VM session) and
 * muse_chat_link.c (Home Link relay).  All three implement the muse_chat.h
 * contract that muse_voice.c drives, so the push-to-talk state machine, the
 * button handling, the pre-roll, the caption paging and the UI all stay
 * exactly as they are -- only the far end changes.
 *
 *   press    -> 16 kHz PCM accumulates in a PSRAM buffer
 *   release  -> a task opens the iFlytek WebSocket, uploads the note, ends the
 *               session with status=2 and waits for the final transcript
 *   then     -> the transcript goes to SenseNova /v1/chat/completions over HTTPS
 *   reply    -> text, published as MUSE_HATCH_EV_REPLY
 *   then     -> a second iFlytek session (在线合成 v2) turns that text into
 *               16 kHz PCM, which muse_hatch_turn_read() hands to the speaker
 *   last     -> MUSE_HATCH_EV_DONE, once the last sample has been played
 *
 * Everything after the release happens on a task of its own (see cn_turn_task)
 * because it blocks on the network for seconds: the voice task that calls into
 * here has only 6 KiB of stack and must stay responsive to a barge-in press.
 *
 * The two sessions never overlap.  iFlytek keeps a session open for ~15 s
 * after the last frame, and one esp_websocket_client is an 8 KB task stack
 * plus two 4 KB buffers out of internal RAM -- of which this board has about
 * 13 KB in one piece when idle.  So close_ws() stops the ASR client outright
 * (rather than leaving it to iFlytek's idle close) before the TTS client is
 * created.  See the comment on close_ws() for how that is kept safe.
 *
 * DONE is published after the audio, not with the text: muse_voice.c's
 * hatch_reply() leaves its loop on it, so pushing it early would cut the
 * speech off the moment it started.
 *
 * ---------------------------------------------------------------------------
 * Three things about the iFlytek protocol that will bite you if you forget:
 *
 *  1. The 大模型识别 family does NOT use the legacy common/business/data
 *     envelope.  It uses header/parameter/payload.  Sending the legacy shape
 *     gets a clean WebSocket upgrade and then a silent close on frame one --
 *     no error code at all, because the parser never got far enough to have
 *     one.  Verified the hard way on 2026-10-08.
 *
 *  2. The FIRST frame must carry header.status = 0, and it is the only one that
 *     carries parameter/iat.  Sending audio straight away with status 1 gets
 *     the session closed just as silently.
 *
 *  3. `ptt` in the business block means 标点预测 (punctuation), not
 *     Push-to-Talk.  Push-to-talk is not on the wire at all; it is just
 *     "send status=2 once the note has been uploaded".
 *
 * The credentials baked in below were verified working from the PC side
 * (tools/xfyun_asr.py --sweep).  Note the APPID is subscribed to 大模型多语种
 * 语音识别 (mul_cn), which auto-detects the language, so Mandarin works fine.
 * ---------------------------------------------------------------------------
 */

#include "muse_chat.h"
#include "muse_chat_priv.h"   /* muse_hatch_caption_at(): the reply's page */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>   /* the autotest synthesises a tone; also M_PI */
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_attr.h"   /* EXT_RAM_BSS_ATTR: the big buffers live in PSRAM */
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"   /* iFlytek needs a real clock; see ensure_clock() */
#include "esp_system.h"   /* esp_reset_reason(): the autotest reports it */
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "sdkconfig.h"

#include "muse_audio.h"   /* MUSE_AUDIO_RATE / MUSE_AUDIO_CHUNK */
#include "muse_mem.h"
#include "muse_state.h"
#include "muse_wifi.h"

static const char *TAG = "muse_cn";

/* ------------------------------------------------------------------ tuning */

/* The reply is one chat completion, so one page of caption is all we get.
 * muse_state caps a caption at MUSE_CAPTION_MAX (400) and the screen pages it. */
#define REPLY_MAX 768
#define HEARD_MAX 192
/* Room for a 512-token Chinese reply plus the JSON envelope around it. */
#define RESP_MAX 8192

/* iFlytek wants exactly 1280 B (40 ms) per frame of 16 kHz mono PCM. */
#define ASR_FRAME_BYTES 1280
/* Our capture chunk is 20 ms, so it takes two reads to fill one frame. */
#define ASR_BUFFERED_FRAMES (ASR_FRAME_BYTES / 2)

/* A session is capped at 60 s by the service; stop well inside it. */
#define ASR_MAX_SECONDS 55

#define HTTP_TIMEOUT_MS 15000
#define ASR_CONNECT_TIMEOUT_MS 8000
/* Waiting for the server's end-of-session frame after status=2. */
#define ASR_RESULT_TIMEOUT_MS 10000
/* Backstop on the whole turn -- connect, upload, transcript, completion.  If
 * nothing has been published by then, muse_hatch_turn_event() fails the turn
 * so hatch_reply()'s poll loop cannot spin forever. */
#define THINKING_TIMEOUT_MS 45000

/* ★★ Room for one WebSocket client, in internal RAM ★★
 *
 * esp_websocket_client_start() calls xTaskCreate() with our 8192-byte
 * task_stack plus two 4 KiB socket buffers, and xTaskCreate() needs that stack
 * as ONE contiguous internal block.  The client is only ever freed by its own
 * socket task (see close_ws()), and that task does not leave until the poll
 * (ping_interval_sec, 10 s) and then the ping/pong timeout (10 s) expire -- so
 * for ten-odd seconds after a session ends, ~20 KB of internal RAM is still
 * held by the previous client.
 *
 * A press that lands in that window used to get as far as start() and die
 * there with "WebSocket 连接失败" -- a caption that blames the network for a
 * purely local shortage.  Measured on this board: a healthy turn starts with
 * the largest internal block at 13312 B and start() succeeds; the failing turn
 * had 4608 B and start() returned ESP_FAIL.  Gate on this instead. */
#define CN_WS_MIN_INTERNAL (10 * 1024)
/* How long a released turn will wait for that room before giving up. */
#define CN_WS_ROOM_WAIT_MS 8000

/* ------------------------------------------------------------------ state */

typedef enum {
    CN_IDLE = 0,
    CN_LISTENING,     /* press held: the microphone is filling the PSRAM buffer */
    CN_THINKING,      /* released: upload, transcript, completion in flight */
    CN_DONE,
    CN_FAILED,
} cn_phase_t;

static cn_phase_t s_phase = CN_IDLE;
static char s_heard[HEARD_MAX];

/* The reply, the transcript and the history add up to ~10 KB, and internal RAM
 * is tight here: after the UI, BLE, Wi-Fi and the codecs are up this board has
 * ~22 KB free with only ~8 KB in the largest block -- and one turn needs an
 * 8 KB task stack plus two 4 KB socket buffers out of that same pool.  So the
 * big buffers live in PSRAM (.ext_ram.bss) instead.  Needs
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY; ESP-IDF zeroes that section
 * during PSRAM init, and none of this is touched before then. */
#define CN_BIG_BSS EXT_RAM_BSS_ATTR

static char s_reply[REPLY_MAX] CN_BIG_BSS;
/* Why iFlytek refused, when it did.  Kept apart from s_reply so turn_fail()
 * does not overwrite the one message the reader actually needs: the server's
 * reason beats "the upload stopped". */
static char s_asr_err[64];
static volatile bool s_reply_ready;
/* Set on the WebSocket's own task and read by the turn task.
 *
 * ★ The turn task must NEVER poll the handle for any of this ★
 *
 * On a failed connect -- and when the server closes the session -- the socket
 * task frees the client itself (see close_ws()), so a getter such as
 * esp_websocket_client_is_connected() would be reading freed memory.  These
 * flags are the only safe way to observe the socket from outside its task. */
static volatile bool s_ws_connected;   /* handshake finished */
static volatile bool s_ws_closed;      /* the socket task is on its way out */
static volatile bool s_asr_final;      /* the transcript is complete */
/* The HTTP status of a failed upgrade, for the caption and the log.  iFlytek
 * answers 403 when the signature's date is stale, which is a clock problem and
 * says nothing useful in the generic "cannot connect" message. */
static volatile int s_ws_http;
/* Set from the voice task, polled by the turn task: abandon this turn. */
static volatile bool s_cancel;
/* True while a turn task exists.  Only that task touches s_ws, so nothing else
 * may start (or close) a session until this clears. */
static volatile bool s_turn_busy;
static int64_t s_think_t0;

/* The open ASR session.  Non-NULL exactly while the socket task is alive and
 * the turn task owns it.  The event handler gets it back as its `arg`, so it
 * can tell this socket's events from a previous turn's late arrivals -- and it
 * clears this the moment the socket goes down, which is also the last moment
 * the handle can safely be touched. */
static esp_websocket_client_handle_t s_ws;

/* The chat history.  SenseNova wants multi-turn context and a voice assistant
 * without it is useless ("那德国呢?").  Kept as a small ring of role/text. */
#define HISTORY_TURNS 4
static char s_hist_role[HISTORY_TURNS][12];
static char s_hist_text[HISTORY_TURNS][REPLY_MAX] CN_BIG_BSS;
static int s_hist_head;   /* next slot to write */
static int s_hist_count;

/* Pending events handed to muse_voice.c through muse_hatch_turn_event(). */
typedef struct {
    muse_hatch_ev_t ev;
    char text[REPLY_MAX];
} cn_event_t;

static cn_event_t s_events[6] CN_BIG_BSS;
static int s_ev_r, s_ev_w;

/* Transcript comes from the WebSocket's own task, so it needs the lock. */
static SemaphoreHandle_t s_lock;

/* ---------------------------------------------------------------- mailbox */

static void push_event(muse_hatch_ev_t ev, const char *text)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    int next = (s_ev_w + 1) % (int)(sizeof(s_events) / sizeof(s_events[0]));
    if (next != s_ev_r) {
        s_events[s_ev_w].ev = ev;
        strlcpy(s_events[s_ev_w].text, text ? text : "", sizeof(s_events[s_ev_w].text));
        s_ev_w = next;
    } else {
        ESP_LOGW(TAG, "event queue full, dropped %d", (int)ev);
    }
    xSemaphoreGive(s_lock);
}

/* ------------------------------------------------------------------- ASR */

/* iFlytek answers with the transcript spread over many frames.  Each frame's
 * `payload.result.text` is base64 of a JSON doc whose ws[].cw[].w are words;
 * with dwa=wpgs a frame may instead say "replace the range rg". */
static char s_asr_words[REPLY_MAX] CN_BIG_BSS;
static size_t s_asr_len;
/* Byte offset where each word-group starts, so a wpgs "rpl" frame can cut the
 * tail back to the group it supersedes.  iFlytek's rg is a 1-based inclusive
 * word-group range within the *current* result sequence, so the offsets have
 * to be re-based every time we cut. */
#define ASR_MAX_GROUPS 384
static uint16_t s_asr_off[ASR_MAX_GROUPS] CN_BIG_BSS;
static int s_asr_groups;

static void asr_clear_text(void)
{
    s_asr_len = 0;
    s_asr_words[0] = 0;
    s_asr_groups = 0;
}

static void asr_append_word(const char *w)
{
    size_t wl = strlen(w);
    if (s_asr_len + wl + 1 >= sizeof(s_asr_words)) {
        return;
    }
    if (s_asr_groups < ASR_MAX_GROUPS) {
        s_asr_off[s_asr_groups] = (uint16_t)s_asr_len;
    }
    memcpy(s_asr_words + s_asr_len, w, wl);
    s_asr_len += wl;
    s_asr_words[s_asr_len] = 0;
    s_asr_groups++;
}

static void asr_absorb_words(const char *pgs, int rg_a, int rg_b, cJSON *ws)
{
    (void)rg_b;
    if (pgs && strcmp(pgs, "rpl") == 0) {
        if (rg_a >= 1 && rg_a <= s_asr_groups && rg_a <= ASR_MAX_GROUPS) {
            /* Drop the tail from the start of group rg_a; the corrected groups
             * arrive in this same frame and are appended below. */
            s_asr_len = s_asr_off[rg_a - 1];
            s_asr_words[s_asr_len] = 0;
            s_asr_groups = rg_a - 1;
        } else {
            /* Index we never tracked: the only safe reading is a full redo. */
            asr_clear_text();
        }
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, ws) {
        cJSON *cw = cJSON_GetObjectItem(item, "cw");
        cJSON *w = cw ? cJSON_GetObjectItem(cJSON_GetArrayItem(cw, 0), "w") : NULL;
        if (cJSON_IsString(w) && w->valuestring[0]) {
            asr_append_word(w->valuestring);
        }
    }
}

static void asr_handle_frame(const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        return;
    }
    cJSON *hdr = cJSON_GetObjectItem(root, "header");
    int code = 0;
    if (cJSON_IsObject(hdr)) {
        cJSON *c = cJSON_GetObjectItem(hdr, "code");
        if (cJSON_IsNumber(c)) {
            code = c->valueint;
        }
        /* status 2 on the header is the server's "this session is complete".
         * Waiting for the socket to drop instead would stall the turn for an
         * extra second every time. */
        cJSON *st = cJSON_GetObjectItem(hdr, "status");
        if (cJSON_IsNumber(st) && st->valueint == 2) {
            s_asr_final = true;
        }
    }
    if (code != 0) {
        char why[96];
        cJSON *m = cJSON_GetObjectItem(hdr, "message");
        /* The raw text goes to the log; the caption gets something the person
         * holding the board can act on.  Logging language+domain alongside the
         * code is what makes a 11201 diagnosable: the handshake succeeds for an
         * unlicensed product too, so the code alone does not say which product
         * this board asked for. */
        ESP_LOGE(TAG, "ASR %d %s (language=%s domain=%s)", code,
                 cJSON_IsString(m) ? m->valuestring : "",
                 CONFIG_MUSE_CN_XFYUN_LANGUAGE, CONFIG_MUSE_CN_XFYUN_DOMAIN);
        if (code == 11201) {
            snprintf(why, sizeof(why), "讯飞额度已用完 (%d)", code);
        } else if (code == 11200) {
            snprintf(why, sizeof(why), "讯飞未授权 (%d)", code);
        } else {
            snprintf(why, sizeof(why), "讯飞错误 %d", code);
        }
        strlcpy(s_asr_err, why, sizeof(s_asr_err));
        strlcpy(s_reply, why, sizeof(s_reply));
        s_phase = CN_FAILED;
        s_asr_final = true;      /* let the voice task stop waiting */
        push_event(MUSE_HATCH_EV_ERROR, why);
        cJSON_Delete(root);
        return;
    }

    cJSON *pl = cJSON_GetObjectItem(root, "payload");
    cJSON *res = pl ? cJSON_GetObjectItem(pl, "result") : NULL;
    if (cJSON_IsObject(res)) {
        cJSON *text = cJSON_GetObjectItem(res, "text");
        if (cJSON_IsString(text) && text->valuestring[0]) {
            size_t b64 = strlen(text->valuestring);
            unsigned char *dec = malloc(b64 + 4);
            size_t dec_len = 0;
            if (dec && mbedtls_base64_decode(dec, b64 + 4, &dec_len,
                                             (const unsigned char *)text->valuestring,
                                             b64) == 0) {
                dec[dec_len] = 0;
                cJSON *doc = cJSON_Parse((char *)dec);
                if (doc) {
                    cJSON *ws = cJSON_GetObjectItem(doc, "ws");
                    cJSON *pgs = cJSON_GetObjectItem(doc, "pgs");
                    cJSON *rg = cJSON_GetObjectItem(doc, "rg");
                    int rg_a = 0, rg_b = 0;
                    if (cJSON_IsArray(rg)) {
                        cJSON *e = cJSON_GetArrayItem(rg, 0);
                        if (cJSON_IsNumber(e)) {
                            rg_a = e->valueint;
                        }
                        e = cJSON_GetArrayItem(rg, 1);
                        if (cJSON_IsNumber(e)) {
                            rg_b = e->valueint;
                        }
                    }
                    if (cJSON_IsArray(ws)) {
                        asr_absorb_words(cJSON_IsString(pgs) ? pgs->valuestring : NULL,
                                         rg_a, rg_b, ws);
                    }
                    cJSON_Delete(doc);
                }
                if (s_asr_len) {
                    strlcpy(s_heard, s_asr_words, sizeof(s_heard));
                    push_event(MUSE_HATCH_EV_HEARD, s_heard);
                }
            }
            free(dec);
        }
    }
    cJSON_Delete(root);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    /* `arg` is the handle this handler was registered for.  A turn that was
     * cancelled can still deliver events as its socket drains, and without
     * this check they would be folded into the *next* turn's transcript. */
    if (arg != (void *)s_ws) {
        return;
    }
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "iFlytek WebSocket connected");
        s_ws_connected = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "iFlytek WebSocket disconnected");
        /* Mark it down but KEEP the handle: this callback runs on the socket
         * task, a few instructions before it leaves, and without
         * destroy_on_exit it does not free the client.  Dropping the handle
         * here would leak it; close_ws() parks it for the next turn instead. */
        s_ws_closed = true;
        break;
    case WEBSOCKET_EVENT_CLOSED:
        s_ws_closed = true;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 || d->op_code == 0x2) {
            asr_handle_frame((const char *)d->data_ptr, d->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (d) {
            s_ws_http = d->error_handle.esp_ws_handshake_status_code;
        }
        ESP_LOGE(TAG, "iFlytek WebSocket error (HTTP %d)", s_ws_http);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------- the wall clock */

/*
 * iFlytek signs its handshake with an RFC1123 GMT date and refuses anything
 * more than five minutes off, so the ASR needs a real clock.  This board has no
 * RTC and nothing in the image synced time, which left time() at 1970-01-01
 * and every handshake answered with HTTP 403 -- while SenseNova kept working,
 * because CONFIG_MBEDTLS_HAVE_TIME_DATE is off and so no certificate date is
 * ever checked.  The missing clock was invisible everywhere but here.
 */
static bool s_sntp_started;
static volatile bool s_clock_ok;

/* Anything past this counts as "the clock has been set": 2023-11-14. */
#define CN_CLOCK_MIN 1700000000

static bool ensure_clock(int wait_ms)
{
    if (s_clock_ok || time(NULL) > CN_CLOCK_MIN) {
        s_clock_ok = true;
        return true;
    }
    if (!s_sntp_started) {
        /* One server only: CONFIG_LWIP_SNTP_MAX_SERVERS is 1. */
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        cfg.start = true;
        if (esp_netif_sntp_init(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "SNTP 启动失败");
            return false;
        }
        s_sntp_started = true;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(wait_ms)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 还没对上时间");
        return false;
    }
    s_clock_ok = true;
    time_t t = time(NULL);
    struct tm g;
    gmtime_r(&t, &g);
    ESP_LOGI(TAG, "SNTP: clock is %04d-%02d-%02d %02d:%02d:%02d UTC",
             g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec);
    return true;
}

/* Starts SNTP as soon as the netif is up so the first press does not have to
 * wait for the clock.  A press still waits inside ensure_clock() if this has
 * not got there yet. */
static void clock_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 900 && !muse_wifi_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (muse_wifi_connected()) {
        ensure_clock(15000);
    }
    vTaskDelete(NULL);
}

/* --------------------------------------------------- iFlytek URL signing */

/* authorization = base64('api_key="K", algorithm="hmac-sha256",
 *                        headers="host date request-line", signature="S"')
 * where S = base64(hmac_sha256(apisecret,
 *               "host: H\ndate: D\nGET /v1 HTTP/1.1"))
 * and D is RFC1123 in GMT within 300 s of the server clock. */

static void base64_enc(const unsigned char *in, size_t in_len, char *out, size_t out_cap)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_cap, &olen, in, in_len) != 0) {
        out[0] = 0;
    }
}

static void url_encode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

static bool xfyun_signed_url_for(const char *host, const char *path, char *out, size_t cap)
{
    const char *apikey = CONFIG_MUSE_CN_XFYUN_APIKEY;
    const char *apisecret = CONFIG_MUSE_CN_XFYUN_APISECRET;

    /* RFC1123 in GMT.  strftime needs the day/month names in C locale; the
     * default locale is "C" in a fresh ESP-IDF image so this is safe. */
    char date[64];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tmv);

    char preimage[256];
    snprintf(preimage, sizeof(preimage), "host: %s\ndate: %s\nGET %s HTTP/1.1",
             host, date, path);

    unsigned char mac[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, (const unsigned char *)apisecret, strlen(apisecret),
                               (const unsigned char *)preimage, strlen(preimage), mac) != 0) {
        ESP_LOGE(TAG, "hmac failed");
        return false;
    }
    char sig_b64[64];
    base64_enc(mac, sizeof(mac), sig_b64, sizeof(sig_b64));
    if (!sig_b64[0]) {
        return false;
    }

    char origin[320];
    snprintf(origin, sizeof(origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", "
             "headers=\"host date request-line\", signature=\"%s\"",
             apikey, sig_b64);

    char auth_b64[512];
    base64_enc((const unsigned char *)origin, strlen(origin), auth_b64, sizeof(auth_b64));
    if (!auth_b64[0]) {
        return false;
    }

    char auth_esc[1024], date_esc[128], host_esc[128];
    url_encode(auth_b64, auth_esc, sizeof(auth_esc));
    url_encode(date, date_esc, sizeof(date_esc));
    url_encode(host, host_esc, sizeof(host_esc));
    snprintf(out, cap, "wss://%s%s?authorization=%s&date=%s&host=%s",
             host, path, auth_esc, date_esc, host_esc);
    return true;
}

static bool xfyun_signed_url(char *out, size_t cap)
{
    return xfyun_signed_url_for(CONFIG_MUSE_CN_XFYUN_HOST, "/v1", out, cap);
}

/* --------------------------------------------------------------- ASR run */

/* Builds one request frame in the header/parameter/payload envelope. */
static char *asr_frame(int status, const uint8_t *audio, size_t audio_len, int seq)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddObjectToObject(root, "header");
    cJSON_AddStringToObject(hdr, "app_id", CONFIG_MUSE_CN_XFYUN_APPID);
    cJSON_AddNumberToObject(hdr, "status", status);

    if (status == 0) {
        cJSON *par = cJSON_AddObjectToObject(root, "parameter");
        cJSON *iat = cJSON_AddObjectToObject(par, "iat");
        cJSON_AddStringToObject(iat, "domain", CONFIG_MUSE_CN_XFYUN_DOMAIN);
        cJSON_AddStringToObject(iat, "language", CONFIG_MUSE_CN_XFYUN_LANGUAGE);
        cJSON_AddStringToObject(iat, "accent", "mandarin");
        cJSON_AddNumberToObject(iat, "eos", 3000);
        cJSON_AddStringToObject(iat, "dwa", "wpgs");
        cJSON *result = cJSON_AddObjectToObject(iat, "result");
        cJSON_AddStringToObject(result, "encoding", "utf8");
        cJSON_AddStringToObject(result, "compress", "raw");
        cJSON_AddStringToObject(result, "format", "json");
    }

    cJSON *pl = cJSON_AddObjectToObject(root, "payload");
    cJSON *a = cJSON_AddObjectToObject(pl, "audio");
    cJSON_AddStringToObject(a, "encoding", "raw");
    cJSON_AddNumberToObject(a, "sample_rate", 16000);
    cJSON_AddNumberToObject(a, "channels", 1);
    cJSON_AddNumberToObject(a, "bit_depth", 16);
    cJSON_AddNumberToObject(a, "seq", seq);
    cJSON_AddNumberToObject(a, "status", status);

    if (audio_len) {
        size_t b64_cap = ((audio_len + 2) / 3) * 4 + 1;
        char *b64 = malloc(b64_cap);
        size_t olen = 0;
        if (b64 && mbedtls_base64_encode((unsigned char *)b64, b64_cap, &olen,
                                         audio, audio_len) == 0) {
            cJSON_AddStringToObject(a, "audio", b64);
        }
        free(b64);
    } else {
        cJSON_AddStringToObject(a, "audio", "");
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* Sends one request frame.  Caller is the turn task, the only owner of s_ws. */
static bool asr_send_frame(int status, const uint8_t *pcm, size_t n, int seq)
{
    /* Not esp_websocket_client_is_connected(): as soon as the socket task
     * decides to leave it frees the client, and that getter would read the
     * freed handle.  s_ws_closed is set on that task before it does. */
    if (!s_ws || s_ws_closed) {
        return false;
    }
    char *js = asr_frame(status, pcm, n, seq);
    if (!js) {
        return false;
    }
    int r = esp_websocket_client_send_text(s_ws, js, strlen(js), pdMS_TO_TICKS(3000));
    free(js);
    return r >= 0;
}

/* Releases the ASR session.
 *
 * ★★ Never free the client from here ★★
 *
 * This library runs its own event loop *inside the socket task*
 * (esp_event_loop_run() at the tail of esp_websocket_client_dispatch_event),
 * so ws_event_handler() executes on the socket task.  That task clears
 * `client->run` BEFORE it dispatches CLOSED and only sets STOPPED_BIT
 * afterwards.  So by the time we see the event, `run` is already false:
 *
 *   stop()    sees !run  -> logs "Client was not started", returns ESP_FAIL
 *                            WITHOUT waiting for the task to finish
 *   destroy() sees !run  -> skips its stop() and goes straight to
 *                            destroy_and_free_resources(), which deletes the
 *                            very event loop the socket task is running in,
 *                            then its lock, its buffers, and the client
 *
 * The socket task then returns through freed memory.  Symptom: the chip
 * resets ~1 s after the ASR session ends, with no panic text and no Guru
 * Meditation -- it looks exactly like a brownout.  That was this backend's
 * bug.
 *
 * The old fix was esp_websocket_client_destroy_on_exit(): the socket task freed
 * the client itself, and this side only dropped the handle.  That is safe, but
 * it leaves the client alive until the socket task leaves -- and iFlytek holds
 * the session open for ~15 s after the last frame (measured from the PC side:
 * the close frame arrives at t+15.5 s).  Fifteen seconds of an 8 KB task stack
 * and two 4 KB buffers, on the board with the least internal RAM to spare.
 *
 * With a TTS session to open straight after the ASR one that is not a waste
 * but a wall: the second client cannot get its contiguous 8 KB while the first
 * one is still holding it.
 *
 * So the teardown is ownerless no longer.  destroy_on_exit is NOT armed, and
 * the turn task ends the session itself:
 *
 *   - stop() with run still true sets run = false and *waits* for STOPPED_BIT,
 *     which the socket task sets on its way out.  Only then do we destroy, so
 *     the free lands after the task's last touch of the client.  The 50 ms is
 *     for the two statements between that bit and vTaskDelete().
 *
 *   - If the session went down on its own, run is already false, stop()
 *     returns ESP_FAIL without waiting, and there is NO safe moment to free:
 *     the task is on its way out but nothing tells us when it is done.  Such a
 *     client is parked in s_ws_reap and destroyed at the start of the next
 *     turn, by which time the task is certainly gone.
 */
static esp_websocket_client_handle_t s_ws_reap;   /* socket task long gone; free next turn */
/* The same parking spot for the synthesis client, declared here because
 * reap_ws() below clears both. */
static esp_websocket_client_handle_t s_tts_reap;

static void ws_release(esp_websocket_client_handle_t *ph,
                       esp_websocket_client_handle_t *park, bool already_closed)
{
    esp_websocket_client_handle_t h = *ph;
    *ph = NULL;
    if (!h) {
        return;
    }
    if (!already_closed && esp_websocket_client_stop(h) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_websocket_client_destroy(h);
        return;
    }
    *park = h;
    ESP_LOGW(TAG, "socket left on its own; freeing it on the next turn");
}

static void close_ws(void)
{
    /* ws_event_handler() keys on the handle, so this also stops any late frame
     * from a session we are done with reaching the next turn's transcript. */
    ws_release(&s_ws, &s_ws_reap, s_ws_closed);
}

/* A client parked by ws_release() has been stopped for a whole turn by now, so
 * its socket task is long gone and its buffers can go back to the heap. */
static void reap_ws(void)
{
    if (s_ws_reap) {
        esp_websocket_client_destroy(s_ws_reap);
        s_ws_reap = NULL;
    }
    if (s_tts_reap) {
        esp_websocket_client_destroy(s_tts_reap);
        s_tts_reap = NULL;
    }
}

/* ------------------------------------------------- the turn's PCM staging */

/*
 * The whole utterance is buffered here and streamed after the release, rather
 * than streamed live from the microphone.
 *
 * Live streaming looks tempting -- muse_voice.c hands us 20 ms chunks as it
 * reads them -- but it fails three ways on this board:
 *
 *   - turn_begin() would have to open the WebSocket, and the TLS handshake
 *     takes ~500 ms.  The mic's DMA ring is only 40 ms deep, so nothing may
 *     block waiting for it, and every chunk captured in the meantime has
 *     nowhere to go.
 *   - The WebSocket's task and the voice task would then both touch the send
 *     state and the transcript.
 *   - A press shorter than the handshake would release with nothing sent at
 *     all, and the status=2 frame that ends the session can only follow a
 *     status=0 one.
 *
 * Buffering the lot removes all three.  muse_voice.c caps a recording at
 * MAX_SECS = 15 s (plus the pre-roll), so 16 s is the whole requirement.
 */
#define STAGE_MAX_FRAMES (MUSE_AUDIO_RATE * 16)
static int16_t *s_stage;      /* PSRAM, allocated once on the first turn */
static size_t s_stage_n;      /* frames captured this turn */

/* ------------------------------------------------------------- SenseNova */

static esp_err_t http_post_json(const char *url, const char *auth_header,
                                const char *body, char *out, size_t out_cap,
                                int *status_out)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_header(cli, "Authorization", auth_header);
    esp_http_client_set_post_field(cli, body, strlen(body));

    esp_err_t err = esp_http_client_open(cli, strlen(body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http open: %s", esp_err_to_name(err));
        esp_http_client_cleanup(cli);
        return err;
    }
    esp_http_client_write(cli, body, strlen(body));
    esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    size_t total = 0;
    /* Read the body whatever the status.  Do NOT gate on the content-length
     * that fetch_headers() returned: with chunked transfer it is -1 and we
     * would silently return an empty body.  And do not gate on 200 either --
     * a 429 carries its reason in the body ("rate limit ..."), which is the
     * one thing that tells throttling apart from a dead link. */
    while (total + 1 < out_cap) {
        int r = esp_http_client_read(cli, out + total, out_cap - total - 1);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    out[total] = 0;
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (status_out) {
        *status_out = status;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "SenseNova HTTP %d: %.200s", status, out);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Pulls one JSON string value out of `json` by key name and decodes its escapes
 * (\n \r \t \b \f \/ \" \\ and \uXXXX, surrogate pairs included) into `out`.
 *
 * Hand-rolled on purpose.  cJSON builds one heap node per element, so parsing
 * an 8 KiB completion needs roughly 20 KiB of internal RAM -- and this board
 * has ~22 KiB left once the display and the audio DMA rings are up.  Reading
 * the single field we want costs no extra heap at all. */
static bool json_find_string(const char *json, const char *key,
                             char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    /* Anchor on "choices" when present: it keeps a same-named field elsewhere
     * in the envelope from being picked up instead. */
    const char *p = strstr(json, "\"choices\"");
    p = strstr(p ? p : json, pat);
    if (!p || cap == 0) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p++ != ':') {
        return false;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    if (*p++ != '"') {
        return false;   /* null, a number, or an object */
    }

    size_t o = 0;
    while (*p && *p != '"') {
        unsigned int cp;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n':  cp = '\n'; p++; break;
            case 'r':  cp = '\r'; p++; break;
            case 't':  cp = '\t'; p++; break;
            case 'b':  cp = '\b'; p++; break;
            case 'f':  cp = '\f'; p++; break;
            case '/':  cp = '/';  p++; break;
            case '"':  cp = '"';  p++; break;
            case '\\': cp = '\\'; p++; break;
            case 'u': {
                unsigned v = 0;
                p++;
                for (int k = 0; k < 4 && isxdigit((unsigned char)*p); k++, p++) {
                    v = v * 16u + (unsigned)(isdigit((unsigned char)*p)
                                             ? *p - '0'
                                             : tolower((unsigned char)*p) - 'a' + 10);
                }
                cp = v;
                if (v >= 0xD800 && v <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    const char *q = p + 2;
                    unsigned lo = 0;
                    for (int k = 0; k < 4 && isxdigit((unsigned char)*q); k++, q++) {
                        lo = lo * 16u + (unsigned)(isdigit((unsigned char)*q)
                                                   ? *q - '0'
                                                   : tolower((unsigned char)*q) - 'a' + 10);
                    }
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((v - 0xD800u) << 10) + (lo - 0xDC00u);
                        p = q;
                    }
                }
                break;
            }
            default:
                cp = (unsigned char)*p;
                p++;
                break;
            }
        } else {
            unsigned char b = (unsigned char)*p++;
            if (b >= 0x80u) {
                /* A byte above ASCII is already part of a UTF-8 sequence that
                 * is sitting in the JSON verbatim.  SenseNova returns raw
                 * UTF-8 while iFlytek escapes everything as \uXXXX, so this
                 * is the normal case here: copy it through untouched.  Feeding
                 * it to the encoder below would re-encode it (你 -> ä½). */
                if (o + 1 >= cap) {
                    break;
                }
                out[o++] = (char)b;
                continue;
            }
            cp = b;
        }

        /* Emit UTF-8. */
        if (cp < 0x80u) {
            if (o + 1 >= cap) break;
            out[o++] = (char)cp;
        } else if (cp < 0x800u) {
            if (o + 2 >= cap) break;
            out[o++] = (char)(0xC0u | (cp >> 6));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            if (o + 3 >= cap) break;
            out[o++] = (char)(0xE0u | (cp >> 12));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        } else {
            if (o + 4 >= cap) break;
            out[o++] = (char)(0xF0u | (cp >> 18));
            out[o++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[o++] = (char)(0x80u | (cp & 0x3Fu));
        }
    }
    out[o] = 0;
    if (o == 0) {
        return false;
    }
    /* A one-char reply is legitimate; a JSON error envelope is not. */
    return true;
}

static void history_push(const char *role, const char *text)
{
    strlcpy(s_hist_role[s_hist_head], role, sizeof(s_hist_role[0]));
    strlcpy(s_hist_text[s_hist_head], text, sizeof(s_hist_text[0]));
    s_hist_head = (s_hist_head + 1) % HISTORY_TURNS;
    if (s_hist_count < HISTORY_TURNS) {
        s_hist_count++;
    }
}

/* HTTP status of the last SenseNova call, kept so the caller can tell a
 * throttle (429) apart from an unreachable host.  0 means the request never
 * got a status at all. */
static int s_llm_status;

/* Asks SenseNova.  Returns true and fills `out` on success. */
static bool sensenova_ask(const char *user_text, char *out, size_t out_cap)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", CONFIG_MUSE_CN_SENSENOVA_MODEL);
    /* Without this the model burns ~100x the tokens on a scratchpad before
     * answering; measured on the PC side (836 tokens vs 7 for the same reply). */
    cJSON_AddStringToObject(root, "reasoning_effort", "none");
    cJSON_AddNumberToObject(root, "max_tokens", 512);
    cJSON_AddBoolToObject(root, "stream", 0);

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
                            "你是一个桌面小屏幕上的语音助手。回答要简短、口语化，"
                            "直接给结论，不要客套话，控制在50字以内。");
    cJSON_AddItemToArray(msgs, sys);

    for (int i = 0; i < s_hist_count; i++) {
        int idx = (s_hist_head - s_hist_count + i + HISTORY_TURNS * 2) % HISTORY_TURNS;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", s_hist_role[idx]);
        cJSON_AddStringToObject(m, "content", s_hist_text[idx]);
        cJSON_AddItemToArray(msgs, m);
    }
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", user_text);
    cJSON_AddItemToArray(msgs, um);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        return false;
    }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_MUSE_CN_SENSENOVA_KEY);

    /* PSRAM: parsing the completion with cJSON would need ~20 KiB of the
     * ~22 KiB internal RAM left after the display and the audio rings. */
    char *resp = heap_caps_malloc(RESP_MAX, MUSE_BIG_CAPS);
    if (!resp) {
        free(body);
        return false;
    }
    /* SenseNova throttles bursts: measured from the PC side, roughly one
     * single request in three comes back 429 while the very next one sails
     * through.  Two retries with a short backoff is what turns "no answer at
     * all" into "answers on the second try" -- the PC-side tool does exactly
     * this.  Worst case this adds 4.5 s to the turn. */
    esp_err_t err = ESP_FAIL;
    int status = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(1500 * attempt));
        }
        err = http_post_json(CONFIG_MUSE_CN_SENSENOVA_URL, auth, body, resp,
                             RESP_MAX, &status);
        if (err == ESP_OK || status != 429) {
            break;
        }
        ESP_LOGW(TAG, "SenseNova 429 (try %d/3), backing off", attempt + 1);
    }
    s_llm_status = (err == ESP_OK) ? 200 : status;
    free(body);
    if (err != ESP_OK) {
        free(resp);
        return false;
    }

    /* Walk the reply by hand rather than with cJSON (see above). */
    bool ok = json_find_string(resp, "content", out, out_cap);
    if (!ok) {
        ESP_LOGE(TAG, "no content in reply: %.200s", resp);
    }
    free(resp);
    return ok;
}

/* ------------------------------------------------------------------- TTS */

/*
 * iFlytek 在线合成 v2 -- ★ a DIFFERENT protocol generation from the ASR ★
 *
 *   ASR (大模型识别): wss://iat.cn-huabei-1.xf-yun.com/v1   header/parameter/payload
 *   TTS (在线合成):   wss://tts-api.xfyun.cn/v2/tts        common/business/data
 *
 * The older envelope, and the failure code sits at the top level (code == 0 is
 * success) rather than under header.  Verified from the PC side with
 * tools/xfyun_tts.py: four voices answer code=0 on this account.
 *
 * aue=raw + auf=audio/L16;rate=16000 makes the service return plain PCM at
 * exactly the rate the microphone runs at, so the reply goes to
 * muse_audio_write() with no decoder and nothing to resample.
 *
 * Synthesis is quick -- 3.5 s of speech came back in 0.17 s from the PC -- so
 * the whole reply is collected before a single sample is played.  That keeps
 * the player a plain buffer read instead of a producer/consumer ring.
 */
#define TTS_HOST "tts-api.xfyun.cn"
#define TTS_PATH "/v2/tts"
/* xiaoyan 小燕/女 · xiaofeng 小峰/男 · x4_yezi 叶子/女 · aisjiuxu 许久.
 * A #define and not a Kconfig entry on purpose: a new symbol changes
 * sdkconfig.h and costs a full rebuild just to try the next voice. */
#define TTS_VOICE "xiaoyan"
#define TTS_TIMEOUT_MS 8000

/* How much of the reply is spoken.  REPLY_MAX is 768 bytes of UTF-8 -- around
 * 256 Chinese characters, and at a walking pace that is well over three
 * minutes of speech, far more than anyone wants read aloud.  Spoken answers
 * stop at TTS_MAX_CHARS, at a sentence end when there is one near it. */
#define TTS_MAX_CHARS 140
#define TTS_MAX_SECONDS 45
#define TTS_MAX_BYTES (MUSE_AUDIO_RATE * 2 * TTS_MAX_SECONDS)

static int16_t *s_tts;              /* PSRAM: the synthesised reply */
static volatile size_t s_tts_len;   /* bytes of it that are valid (socket task writes it) */
static size_t s_tts_rd;             /* bytes already handed to the player */
static volatile bool s_tts_have;    /* this turn produced audio to play */
static volatile bool s_tts_settled; /* tts_speak() has returned: no more is coming */
static volatile bool s_tts_done_sent;   /* MUSE_HATCH_EV_DONE has been published */
static volatile bool s_tts_up;      /* the TTS handshake finished */
static volatile bool s_tts_over;    /* the TTS socket is finished, one way or another */
static volatile int s_tts_err;      /* the service's code, when it refused */
static esp_websocket_client_handle_t s_tts_ws;

/* One TTS message, reassembled.  iFlytek sends the audio in a WEBSOCKET frame
 * per message and those frames are big: measured 13-45 KB each, five of them
 * for 3.4 s of speech.  The client's rx buffer is 4 KB, so every frame arrives
 * as a run of WEBSOCKET_EVENT_DATA events and a slice on its own is not JSON.
 * Both buffers are in PSRAM: cJSON would need ~50 KB of the internal heap for
 * a message this size, and this board has ~14 KB free with both sockets up. */
#define TTS_MSG_MAX (64 * 1024)
static char s_tts_msg[TTS_MSG_MAX] CN_BIG_BSS;
static char s_tts_b64[TTS_MSG_MAX] CN_BIG_BSS;
static size_t s_tts_msg_len;
static int s_tts_msgs;              /* messages seen this turn, for the log */

/* Where in the reply the caption is.  While the voice plays it follows the
 * audio; once that stops -- and when there is no voice at all -- it carries on
 * at reading pace, so the pages past the spoken part are shown rather than
 * dropped.  iFlytek's frame size does not matter to any of this. */
#define READ_CHARS_PER_S 10
#define READ_BYTES_PER_S (READ_CHARS_PER_S * 3)   /* ~three bytes to a Chinese character */
#define READ_HOLD_MS 2500                         /* the last page stays put this long */
#define READ_MAX_MS 30000                         /* and the whole scroll is bounded */
static size_t s_spoken_bytes;       /* of the reply, how much tts_speak() said */
static size_t s_read_base;          /* where the reading scroll starts */
static int64_t s_read_t0;           /* and when it started */
static bool s_reading;              /* the scroll is running */

static bool sentence_end(const char *p, size_t left)
{
    static const char *const ends[] = {"。", "！", "？", "；", "…", ".", "!", "?", ";", "\n"};
    for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); i++) {
        size_t l = strlen(ends[i]);
        if (left >= l && memcmp(p, ends[i], l) == 0) {
            return true;
        }
    }
    return false;
}

/* What to say, cut to TTS_MAX_CHARS characters and back to the last sentence
 * end that is not too close to the start.  Returns the byte length. */
static size_t tts_script(const char *reply, char *out, size_t cap)
{
    size_t n = strlen(reply);
    size_t chars = 0, i = 0, last_end = 0, last_end_chars = 0;
    while (i < n && chars < TTS_MAX_CHARS) {
        unsigned char c = (unsigned char)reply[i];
        size_t step = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
        if (i + step > n) {
            step = 1;   /* malformed tail: step over the byte, do not run past it */
        }
        chars++;
        i += step;
        if (sentence_end(reply + i - step, n - (i - step))) {
            last_end = i;
            last_end_chars = chars;
        }
    }
    size_t cut = i >= n ? n : (last_end_chars * 3 >= TTS_MAX_CHARS ? last_end : i);
    if (cut >= cap) {
        cut = cap - 1;
    }
    memcpy(out, reply, cut);
    out[cut] = 0;
    return cut;
}

/* One-shot request: status 2 with the whole text is the entire protocol. */
static char *tts_frame(const char *text)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *common = cJSON_AddObjectToObject(root, "common");
    cJSON_AddStringToObject(common, "app_id", CONFIG_MUSE_CN_XFYUN_APPID);
    cJSON *business = cJSON_AddObjectToObject(root, "business");
    cJSON_AddStringToObject(business, "aue", "raw");
    cJSON_AddStringToObject(business, "auf", "audio/L16;rate=16000");
    cJSON_AddStringToObject(business, "vcn", TTS_VOICE);
    cJSON_AddStringToObject(business, "tte", "UTF8");
    cJSON_AddNumberToObject(business, "sfl", 1);
    cJSON_AddNumberToObject(business, "speed", 50);
    cJSON_AddNumberToObject(business, "volume", 50);
    cJSON_AddNumberToObject(business, "pitch", 50);
    cJSON *data = cJSON_AddObjectToObject(root, "data");
    cJSON_AddNumberToObject(data, "status", 2);

    size_t tl = strlen(text);
    char *b64 = malloc(tl * 2 + 8);
    if (!b64) {
        cJSON_Delete(root);
        return NULL;
    }
    base64_enc((const unsigned char *)text, tl, b64, tl * 2 + 8);
    cJSON_AddStringToObject(data, "text", b64);
    free(b64);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

/* The integer value of `"key": N`, or `dflt` when the key is not there. */
static int json_find_int(const char *json, const char *key, int dflt)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) {
        return dflt;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':') {
        p++;
    }
    if (!isdigit((unsigned char)*p) && *p != '-') {
        return dflt;
    }
    return (int)strtol(p, NULL, 10);
}

/* One TTS message, whole: either the service refusing, or a slice of audio.
 * The audio is decoded straight out of s_tts_b64 into the PCM buffer -- no
 * JSON tree is built, so a 45 KB message costs no internal RAM at all. */
static void tts_handle_message(void)
{
    s_tts_msgs++;
    if (s_tts_msgs == 1) {
        /* The envelope, on the first one: a refusal names itself in the head. */
        ESP_LOGI(TAG, "TTS msg 1: %u bytes: %.110s", (unsigned)s_tts_msg_len, s_tts_msg);
    }
    int code = json_find_int(s_tts_msg, "code", 0);
    if (code != 0) {
        s_tts_err = code;
        ESP_LOGE(TAG, "TTS refused: code %d", code);
        s_tts_over = true;
        return;
    }
    if (s_tts && json_find_string(s_tts_msg, "audio", s_tts_b64, sizeof(s_tts_b64))) {
        size_t slen = strlen(s_tts_b64);
        size_t have = TTS_MAX_BYTES - s_tts_len;
        size_t got = 0;
        if (slen / 4 * 3 > have) {
            ESP_LOGW(TAG, "spoken reply hit the %d s cap", TTS_MAX_SECONDS);
            s_tts_over = true;
        } else if (mbedtls_base64_decode((unsigned char *)s_tts + s_tts_len, have, &got,
                                         (const unsigned char *)s_tts_b64, slen) == 0) {
            s_tts_len += got;
        } else {
            ESP_LOGW(TAG, "TTS: %u chars of audio that is not base64", (unsigned)slen);
        }
    }
    if (json_find_int(s_tts_msg, "status", 0) == 2) {
        s_tts_over = true;   /* the last frame; the socket stays up regardless */
    }
}

/* Puts one slice of a WEBSOCKET_EVENT_DATA frame back into the message buffer,
 * and parses it once the last slice of that frame has arrived.  A TTS frame is
 * far bigger than the client's 4 KB rx buffer, so this is the normal path, not
 * an edge case: a frame arrives as one event per buffer-full. */
static void tts_take_slice(const char *p, int len, int off, int total)
{
    if (off == 0) {
        s_tts_msg_len = 0;
    }
    if (len <= 0 || (size_t)off + (size_t)len >= TTS_MSG_MAX) {
        ESP_LOGW(TAG, "TTS frame of %d bytes at %d does not fit", len, off);
        s_tts_over = true;
        return;
    }
    memcpy(s_tts_msg + off, p, len);
    s_tts_msg_len = (size_t)off + (size_t)len;
    s_tts_msg[s_tts_msg_len] = 0;   /* json_find_*() walk it as a C string */
    if (off + len >= total) {
        tts_handle_message();
        s_tts_msg_len = 0;
    }
}

static void tts_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    /* Same guard as the ASR handler: a cancelled turn's socket can still be
     * delivering events, and they must not be folded into the next reply. */
    if (arg != (void *)s_tts_ws) {
        return;
    }
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "TTS WebSocket connected");
        s_tts_up = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        /* The handle stays: see the same case in ws_event_handler(). */
        s_tts_over = true;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 || d->op_code == 0x2) {
            tts_take_slice(d->data_ptr, d->data_len, d->payload_offset, d->payload_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "TTS WebSocket error");
        s_tts_over = true;
        break;
    default:
        break;
    }
}

/*
 * Says `reply`: opens the second session, collects the PCM into PSRAM and
 * leaves it for muse_hatch_turn_read() to play.  Returns true if there is
 * audio.  Runs on the turn task, after close_ws() has taken the ASR client
 * down -- two sockets cannot fit in this board's internal RAM at once.
 */
static bool tts_speak(const char *reply)
{
    if (!s_tts) {
        s_tts = heap_caps_malloc(TTS_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_tts) {
            ESP_LOGW(TAG, "no PSRAM for the spoken reply");
            return false;
        }
    }
    s_tts_len = s_tts_rd = 0;
    s_tts_up = s_tts_over = false;
    s_tts_err = 0;
    s_tts_msgs = 0;
    s_tts_msg_len = 0;
    s_spoken_bytes = 0;

    char script[TTS_MAX_CHARS * 4 + 1];
    s_spoken_bytes = tts_script(reply, script, sizeof(script));
    if (s_spoken_bytes == 0) {
        return false;
    }

    char url[1400];
    if (!xfyun_signed_url_for(TTS_HOST, TTS_PATH, url, sizeof(url))) {
        return false;
    }

    /* The ASR client is already gone (close_ws() stopped it rather than
     * waiting out iFlytek's 15 s idle close), so this is a formality -- but a
     * cheap one, and without it a leaked client turns into "连不上讯飞". */
    for (int i = 0; i < CN_WS_ROOM_WAIT_MS / 100 &&
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < CN_WS_MIN_INTERNAL; i++) {
        if (s_cancel) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < CN_WS_MIN_INTERNAL) {
        ESP_LOGW(TAG, "no room for the TTS socket: internal free %u (largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .task_stack = 8192,
        .disable_auto_reconnect = true,
        .network_timeout_ms = ASR_CONNECT_TIMEOUT_MS,
    };
    s_tts_ws = esp_websocket_client_init(&cfg);
    if (!s_tts_ws) {
        return false;
    }
    esp_websocket_register_events(s_tts_ws, WEBSOCKET_EVENT_ANY, tts_event_handler, s_tts_ws);
    if (esp_websocket_client_start(s_tts_ws) != ESP_OK) {
        esp_websocket_client_handle_t h = s_tts_ws;
        s_tts_ws = NULL;
        esp_websocket_client_destroy(h);   /* no task was created: safe here */
        ESP_LOGW(TAG, "TTS start() refused: internal free %u (largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }

    int64_t t0 = esp_timer_get_time();
    while (!s_tts_up && !s_tts_over && !s_cancel &&
           (esp_timer_get_time() - t0) / 1000 < ASR_CONNECT_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_tts_up) {
        ESP_LOGW(TAG, "TTS handshake did not finish");
        ws_release(&s_tts_ws, &s_tts_reap, s_tts_over);
        return false;
    }

    char *frame = tts_frame(script);
    if (!frame) {
        ws_release(&s_tts_ws, &s_tts_reap, s_tts_over);
        return false;
    }
    int sent = esp_websocket_client_send_text(s_tts_ws, frame, (int)strlen(frame),
                                              pdMS_TO_TICKS(5000));
    free(frame);
    if (sent < 0) {
        ESP_LOGW(TAG, "TTS request not sent (%d)", sent);
        ws_release(&s_tts_ws, &s_tts_reap, s_tts_over);
        return false;
    }
    ESP_LOGI(TAG, "TTS asked for %u of the reply's %u bytes",
             (unsigned)s_spoken_bytes, (unsigned)strlen(reply));

    t0 = esp_timer_get_time();
    while (!s_tts_over && !s_cancel &&
           (esp_timer_get_time() - t0) / 1000 < TTS_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "TTS: %u bytes in %lld ms%s", (unsigned)s_tts_len,
             (esp_timer_get_time() - t0) / 1000,
             s_cancel ? " (cancelled)" : s_tts_over ? "" : " (timed out)");
    ws_release(&s_tts_ws, &s_tts_reap, s_tts_over);
    return s_tts_len > 0 && !s_cancel;
}

/* ------------------------------------------------------- turn lifecycle */

void muse_hatch_turn_begin(void)
{
    if (!s_lock) {
        return;
    }
    /* The previous turn's task may still be closing its socket.  Its cleanup
     * is quick (it polls s_cancel every 20 ms), and letting the two overlap
     * would mean two owners of s_ws. */
    for (int i = 0; i < 15 && s_turn_busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_turn_busy) {
        ESP_LOGW(TAG, "previous turn still running; press ignored");
        push_event(MUSE_HATCH_EV_ERROR, "上一轮还没结束");
        s_phase = CN_IDLE;
        return;
    }

    /* Fast path of the room check in cn_turn_task(): the previous client may
     * still be holding its ~20 KB (see CN_WS_MIN_INTERNAL).  Turning the press
     * down here costs the user nothing -- no audio is captured yet -- whereas
     * letting it through would record, then fail seconds later with a caption
     * about the WebSocket.  Keep it short: this runs while the button is held,
     * and the mic's DMA ring is only 40 ms deep. */
    for (int i = 0; i < 5; i++) {
        if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= CN_WS_MIN_INTERNAL) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < CN_WS_MIN_INTERNAL) {
        ESP_LOGW(TAG, "no room for a socket yet (largest internal %u); press ignored",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        push_event(MUSE_HATCH_EV_ERROR, "上一轮还在收尾，稍等一下");
        s_phase = CN_IDLE;
        return;
    }

    if (!s_stage) {
        /* Allocated once and kept for the life of the device. */
        s_stage = heap_caps_malloc((size_t)STAGE_MAX_FRAMES * sizeof(int16_t),
                                   MUSE_BIG_CAPS);
        if (!s_stage) {
            ESP_LOGE(TAG, "no PSRAM for the staging buffer");
            push_event(MUSE_HATCH_EV_ERROR, "内存不足");
            s_phase = CN_FAILED;
            return;
        }
    }

    s_phase = CN_LISTENING;
    s_cancel = false;
    /* Per-turn socket state, read and written on the socket task.  The previous
     * task is gone by now (the wait above), so clearing them here is safe. */
    s_ws_connected = false;
    s_ws_closed = false;
    s_asr_final = false;
    s_ws_http = 0;
    s_asr_err[0] = 0;
    asr_clear_text();
    s_heard[0] = 0;
    s_reply[0] = 0;
    s_reply_ready = false;
    s_tts_have = false;
    s_tts_settled = false;
    s_tts_done_sent = false;
    s_tts_rd = 0;
    s_tts_len = 0;
    s_reading = false;
    s_read_base = 0;
    s_read_t0 = 0;
    s_spoken_bytes = 0;
    s_stage_n = 0;
    ESP_LOGI(TAG, "listening: capture to PSRAM, ASR follows the release");
}

void muse_hatch_turn_audio(const int16_t *pcm, size_t frames)
{
    if (s_phase != CN_LISTENING || !s_stage) {
        return;
    }
    if (s_stage_n >= (size_t)STAGE_MAX_FRAMES) {
        return;   /* muse_voice.c already stops at MAX_SECS; this is a guard */
    }
    size_t room = (size_t)STAGE_MAX_FRAMES - s_stage_n;
    if (frames > room) {
        frames = room;
    }
    memcpy(s_stage + s_stage_n, pcm, frames * sizeof(int16_t));
    s_stage_n += frames;
}

size_t muse_hatch_turn_audio_wait(const int16_t *pcm, size_t frames, int wait_ms)
{
    (void)wait_ms;
    /* Nothing blocks on the network any more, so there is always room. */
    size_t before = s_stage_n;
    muse_hatch_turn_audio(pcm, frames);
    return s_stage_n - before;
}

void muse_hatch_turn_cancel(void)
{
    /* Ask, do not do: the turn task owns the socket and the staging count, so
     * it tears down the socket and the next turn_begin() resets the count.
     * Touching either from here could race a send already in progress. */
    s_cancel = true;
    s_phase = CN_IDLE;
}

/* ================================================ the turn, start to finish */

/* Fails the turn, releases the socket and exits.  Never returns. */
static void turn_fail(const char *why)
{
    close_ws();
    if (s_asr_err[0]) {
        /* iFlytek already said why, and it is on the caption.  Sending stops
         * right after the server refuses, so this is usually where a real ASR
         * failure lands -- writing "the upload stopped" over it would hide the
         * only line worth reading. */
        ESP_LOGW(TAG, "turn failed after: %s (trigger: %s)", s_asr_err,
                 why && why[0] ? why : "cancel");
        s_phase = CN_FAILED;
    } else if (why && why[0]) {
        ESP_LOGE(TAG, "turn failed: %s", why);
        strlcpy(s_reply, why, sizeof(s_reply));
        push_event(MUSE_HATCH_EV_ERROR, why);
        s_phase = CN_FAILED;
    } else {
        s_phase = CN_IDLE;      /* cancelled by the user */
    }
    s_turn_busy = false;
    vTaskDelete(NULL);
}

/* Opens the ASR session, uploads the staged note, collects the transcript, then
 * asks SenseNova and publishes the reply.  Runs on its own task (PSRAM stack):
 * it blocks on the network for seconds, which the voice task cannot do. */
static void cn_turn_task(void *arg)
{
    (void)arg;
    s_turn_busy = true;

    /* A turn needs an 8 KB task stack and two 4 KB socket buffers, and those
     * come from internal RAM, which is the scarcest thing on this board.  Log
     * both ends of the turn so a failure to allocate is obvious in the log
     * rather than a silent no-op. */
    ESP_LOGI(TAG, "turn start: internal free %u (largest %u)", 
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    /* A client a previous turn could not free safely (it went down on its own)
     * has had a whole turn to finish dying; take its memory back before asking
     * for more. */
    reap_ws();

    /* The signature carries a GMT date, so the clock has to be right before
     * the URL is built.  SNTP normally finished during boot; this is the
     * backstop for a first press that came in before it did. */
    if (!ensure_clock(10000)) {
        turn_fail("对时失败，稍后再试");
        return;
    }

    char url[1400];
    if (!xfyun_signed_url(url, sizeof(url))) {
        turn_fail("签名失败");
        return;
    }
    /* Length only: the URL carries the signed authorization. */
    ESP_LOGI(TAG, "turn: url %d bytes, opening the WebSocket", (int)strlen(url));

    /* ★ Backstop for the check in turn_begin().  The utterance is already safe
     * in PSRAM by now, so waiting costs nothing but latency, and a client from
     * an earlier turn that the user cut short usually frees itself inside this
     * window.  Without it start() just fails to fit the 8 KiB task stack and
     * the user is told the WebSocket is broken. */
    for (int i = 0; i < CN_WS_ROOM_WAIT_MS / 100 &&
                    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < CN_WS_MIN_INTERNAL; i++) {
        if (s_cancel) {
            turn_fail(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < CN_WS_MIN_INTERNAL) {
        ESP_LOGE(TAG, "no room for the socket: internal free %u (largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        turn_fail("内存紧张，稍等一下再按住说话");
        return;
    }

    esp_websocket_client_config_t cfg = {
        .uri = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        /* TLS handshakes inside the WebSocket's own task, and the 4 KiB
         * default stack does not survive one. */
        .task_stack = 8192,
        /* One session per press: after status=2 the server closes, and a
         * reconnect would only fight the turn that is finishing. */
        .disable_auto_reconnect = true,
        .network_timeout_ms = ASR_CONNECT_TIMEOUT_MS,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        turn_fail("WebSocket 初始化失败");
        return;
    }
    /* ★ Deliberately NOT armed with destroy_on_exit: see close_ws().  The
     * socket task then only sets STOPPED_BIT on its way out, which is what
     * lets stop() below wait for it and free the client afterwards -- instead
     * of leaving it (and its 8 KB stack) alive until iFlytek's idle close. */
    ESP_LOGI(TAG, "turn: client up, internal free %u (largest %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    /* Pass the handle as the handler's arg: see ws_event_handler(). */
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, s_ws);
    if (esp_websocket_client_start(s_ws) != ESP_OK) {
        /* start() failed before creating the task, so nobody else can free
         * this one: do it here rather than leak its 8 KB of buffers. */
        esp_websocket_client_handle_t h = s_ws;
        s_ws = NULL;
        esp_websocket_client_destroy(h);
        ESP_LOGE(TAG, "start() refused: internal free %u (largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        turn_fail("WebSocket 连接失败");
        return;
    }

    /* The handshake takes a few hundred ms.  The audio is already safe in
     * PSRAM, so waiting costs nothing but time.
     *
     * Wait on the task's own flags, never on the handle: a handshake that the
     * server refuses (iFlytek answers 403 when the signature's date is stale)
     * makes the socket task free the client before we get here. */
    int64_t deadline = esp_timer_get_time() + (int64_t)ASR_CONNECT_TIMEOUT_MS * 1000;
    int64_t last_note = 0;
    while (!s_ws_connected && !s_ws_closed) {
        if (s_cancel) {
            turn_fail(NULL);
            return;
        }
        if (esp_timer_get_time() > deadline) {
            turn_fail("连不上讯飞");
            return;
        }
        /* A TLS handshake runs for seconds with nothing else in the log; a
         * heartbeat here is what tells a slow connect from a dead one. */
        if (esp_timer_get_time() - last_note > 1000000) {
            last_note = esp_timer_get_time();
            ESP_LOGI(TAG, "turn: awaiting the TLS handshake, internal free %u (largest %u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s_ws_connected) {
        char why[64];
        snprintf(why, sizeof(why), s_ws_http ? "讯飞拒绝连接 (HTTP %d)" : "连不上讯飞",
                 s_ws_http);
        turn_fail(why);
        return;
    }

    const uint8_t *pcm = (const uint8_t *)s_stage;
    size_t bytes = s_stage_n * sizeof(int16_t);
    int frames_total = (int)((bytes + ASR_FRAME_BYTES - 1) / ASR_FRAME_BYTES);
    ESP_LOGI(TAG, "ASR connected: uploading %.1f s (%d frames)",
             (double)s_stage_n / MUSE_AUDIO_RATE, frames_total);

    /* The service is built for a real-time stream -- its own demo paces one
     * 1280-byte frame every 40 ms.  Send the first second at that cadence so
     * the engine's onset detection sees a normal start, then speed up: the
     * server buffers what it has not consumed, and pacing the whole note would
     * add its full duration to the turn's latency. */
    int seq = 1;
    bool first = true;
    int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; first || off < bytes; off += ASR_FRAME_BYTES) {
        if (s_cancel) {
            turn_fail(NULL);
            return;
        }
        size_t n = off < bytes ? bytes - off : 0;
        if (n > ASR_FRAME_BYTES) {
            n = ASR_FRAME_BYTES;
        }
        /* status 0 on the very first frame: that is the one carrying
         * parameter/iat, and without it the server closes the session. */
        if (!asr_send_frame(first ? 0 : 1, pcm + off, n, seq++)) {
            turn_fail("推流失败");
            return;
        }
        first = false;
        if (off < 25 * ASR_FRAME_BYTES) {                       /* first second */
            vTaskDelay(pdMS_TO_TICKS(40));
        } else if ((off / ASR_FRAME_BYTES) % 8 == 0) {          /* then ~4x realtime */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (esp_timer_get_time() - t0 > (int64_t)ASR_MAX_SECONDS * 1000000) {
            ESP_LOGW(TAG, "upload hit the %d s session cap", ASR_MAX_SECONDS);
            break;
        }
    }

    /* status=2 with no audio ends the session and makes the server finalise. */
    if (!asr_send_frame(2, NULL, 0, seq++)) {
        turn_fail("收尾失败");
        return;
    }

    deadline = esp_timer_get_time() + (int64_t)ASR_RESULT_TIMEOUT_MS * 1000;
    while (!s_asr_final && !s_ws_closed) {
        if (s_cancel) {
            turn_fail(NULL);
            return;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "no end-of-session frame within %d ms",
                     ASR_RESULT_TIMEOUT_MS);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    close_ws();       /* the transcript is in; nothing else will arrive */

    const char *q = s_heard[0] ? s_heard : s_asr_words;
    if (!q[0]) {
        ESP_LOGW(TAG, "empty transcript");
        strlcpy(s_reply, "没听清，再说一次？", sizeof(s_reply));
        s_reply_ready = true;
        push_event(MUSE_HATCH_EV_REPLY, s_reply);
        push_event(MUSE_HATCH_EV_DONE, s_reply);
        s_phase = CN_DONE;
        s_turn_busy = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "heard: %s", q);
    push_event(MUSE_HATCH_EV_HEARD, q);

    if (s_cancel) {
        turn_fail(NULL);
        return;
    }
    char *answer = malloc(REPLY_MAX);
    if (!answer) {
        turn_fail("内存不足");
        return;
    }
    if (sensenova_ask(q, answer, REPLY_MAX)) {
        history_push("user", q);
        history_push("assistant", answer);
        strlcpy(s_reply, answer, sizeof(s_reply));
        s_reply_ready = true;
        ESP_LOGI(TAG, "reply: %s", s_reply);
        push_event(MUSE_HATCH_EV_SENT, s_reply);
        /* The text goes up first: that is the answer, and the voice is a second
         * reading of it.  DONE is deliberately withheld -- hatch_reply() leaves
         * on it, so publishing it here would cut the speech off before it
         * started.  muse_hatch_turn_read() publishes it once the audio is out. */
        push_event(MUSE_HATCH_EV_REPLY, s_reply);

        /* The second socket is about to go up; give the backstop in
         * muse_hatch_turn_event() a fresh budget for it. */
        s_think_t0 = esp_timer_get_time();
        s_phase = CN_THINKING;
        s_tts_done_sent = false;
        s_tts_have = tts_speak(s_reply);
        /* Whatever it produced is all there will be: from here the voice task
         * plays it, and then scrolls the pages the voice did not cover.  DONE
         * is withheld either way -- muse_hatch_turn_read() publishes it once
         * the whole answer has been shown. */
        s_tts_settled = true;
        s_phase = CN_DONE;
    } else {
        /* "check Wi-Fi" is wrong for a 429: the link is demonstrably fine,
         * SenseNova is just throttling us.  Say which one it is. */
        if (s_llm_status == 429) {
            strlcpy(s_reply, "商汤限流了，稍等再问一次", sizeof(s_reply));
        } else {
            strlcpy(s_reply, "连不上商汤，检查 WiFi", sizeof(s_reply));
        }
        push_event(MUSE_HATCH_EV_ERROR, s_reply);
        s_phase = CN_FAILED;
    }
    free(answer);
    ESP_LOGI(TAG, "turn done: internal free %u (largest %u), phase=%d",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (int)s_phase);
    s_turn_busy = false;
    vTaskDelete(NULL);
}

void muse_hatch_turn_end(void)
{
    if (s_phase != CN_LISTENING) {
        return;
    }
    s_phase = CN_THINKING;
    s_think_t0 = esp_timer_get_time();
    /* Logged before the task is created on purpose: if creation dies the
     * release would otherwise leave no trace at all in the log. */
    ESP_LOGI(TAG, "released: %.1f s staged, ASR follows",
             (double)s_stage_n / MUSE_AUDIO_RATE);
    /* Connect, upload and complete take seconds -- far too long for the voice
     * task, which has 6 KiB of stack and must stay able to see a barge-in
     * press.  Hand the turn to its own task. */
    if (xTaskCreateWithCaps(cn_turn_task, "cn_turn", 12288, NULL, 5, NULL,
                            MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the turn task");
        push_event(MUSE_HATCH_EV_ERROR, "内存不足");
        s_phase = CN_FAILED;
        return;
    }
}

muse_hatch_ev_t muse_hatch_turn_event(char *text, size_t cap)
{
    /* Backstop: if the turn task died without publishing anything, hatch_reply()
     * would poll forever. */
    if (s_phase == CN_THINKING &&
        (esp_timer_get_time() - s_think_t0) / 1000 > THINKING_TIMEOUT_MS) {
        ESP_LOGE(TAG, "turn timed out with no result");
        s_phase = CN_FAILED;
        push_event(MUSE_HATCH_EV_ERROR, "超时，没拿到回答");
    }

    muse_hatch_ev_t ev = MUSE_HATCH_EV_NONE;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return MUSE_HATCH_EV_NONE;
    }
    if (s_ev_r != s_ev_w) {
        ev = s_events[s_ev_r].ev;
        strlcpy(text, s_events[s_ev_r].text, cap);
        s_ev_r = (s_ev_r + 1) % (int)(sizeof(s_events) / sizeof(s_events[0]));
    }
    xSemaphoreGive(s_lock);
    return ev;
}

/* The byte of the reply the caption should be showing, given how much of the
 * voice has played. */
static size_t caption_at(size_t played)
{
    size_t len = strlen(s_reply);
    if (!len) {
        return 0;
    }
    if (!s_tts_settled) {
        return 0;   /* still being synthesised: open the reply */
    }
    if (!s_reading) {
        /* Follow the voice: everything before s_spoken_bytes is what was said,
         * so the page turns as that part goes past.  hatch_reply() counts
         * frames, s_tts_len counts bytes: two to a frame. */
        size_t said = s_tts_have && s_tts_len
                          ? (size_t)((uint64_t)s_spoken_bytes * played * 2 / s_tts_len) : 0;
        return said < len ? said : len - 1;
    }
    size_t at = s_read_base + (size_t)((esp_timer_get_time() - s_read_t0)
                                       * READ_BYTES_PER_S / 1000000);
    return at < len ? at : len - 1;
}

bool muse_hatch_turn_caption(size_t played, char *out, size_t cap)
{
    if (!s_reply_ready || !s_reply[0]) {
        return false;
    }
    /* muse_chat_text.c wraps the reply to the screen's page (muse_state_page,
     * which the UI sets) and hands back the page holding that byte, so an
     * answer too long for one screenful is read a page at a time instead of
     * being cut off at the first line. */
    return muse_hatch_caption_at(s_reply, caption_at(played), out, cap);
}

size_t muse_hatch_turn_read(int16_t *pcm, size_t frames, int wait_ms)
{
    (void)wait_ms;   /* the sleeps below are this loop's clock, not the caller's */
    if (s_tts_have && s_tts_rd < s_tts_len) {
        size_t want = frames * sizeof(int16_t);
        size_t left = s_tts_len - s_tts_rd;
        if (want > left) {
            want = left;
        }
        memcpy(pcm, (const uint8_t *)s_tts + s_tts_rd, want);
        s_tts_rd += want;
        return want / sizeof(int16_t);
    }
    if (!s_tts_settled) {
        /* The reply is still being synthesised.  No audio to hand over yet,
         * and the text must not start scrolling until it is known whether
         * there will be any -- a scroll that finished first would publish
         * DONE and hatch_reply() would leave before the voice started. */
        vTaskDelay(pdMS_TO_TICKS(20));
        return 0;
    }
    /* The voice is done, or there never was one.  The answer may still have
     * pages the screen has not shown -- only TTS_MAX_CHARS is ever spoken, and
     * cutting back to a sentence end can leave a lot behind -- so the turn is
     * held while the caption scrolls the rest at reading pace. */
    if (!s_reading) {
        s_reading = true;
        s_read_t0 = esp_timer_get_time();
        s_read_base = s_tts_have ? s_spoken_bytes : 0;
    }
    size_t len = strlen(s_reply);
    int64_t scroll = (int64_t)(len > s_read_base ? len - s_read_base : 0)
                     * 1000000 / READ_BYTES_PER_S;
    int64_t hold = (int64_t)READ_HOLD_MS * 1000;
    if (scroll + hold > (int64_t)READ_MAX_MS * 1000) {
        hold = (int64_t)READ_MAX_MS * 1000 - scroll;
        if (hold < 0) {
            hold = 0;   /* a very long answer: cut the scroll, keep the turn moving */
        }
    }
    if (!s_cancel && esp_timer_get_time() >= s_read_t0 + scroll + hold) {
        if (!s_tts_done_sent) {
            s_tts_done_sent = true;
            push_event(MUSE_HATCH_EV_DONE, s_reply);
        }
        return 0;
    }
    /* hatch_reply() polls us in a tight loop and this call is its only pacing,
     * so sleep a slice (small enough that a barge-in press is still seen
     * promptly) rather than spinning the core flat out. */
    vTaskDelay(pdMS_TO_TICKS(30));
    return 0;
}

/* ------------------------------------------------------------ hatch glue */

static muse_hatch_state_t s_state = MUSE_HATCH_NOT_SET;
static char s_detail[48];

#if CONFIG_MUSE_CN_AUTOTEST
static void muse_hatch_autotest_start(void);
#endif

void muse_hatch_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_state = MUSE_HATCH_UNTESTED;
    strlcpy(s_detail, "iFlytek + SenseNova", sizeof(s_detail));
    ESP_LOGI(TAG, "CN voice backend armed (ASR=%s, LLM=%s)",
             CONFIG_MUSE_CN_XFYUN_APPID, CONFIG_MUSE_CN_SENSENOVA_MODEL);
    /* Set the wall clock as soon as Wi-Fi is up.  The ASR handshake is signed
     * with it, and iFlytek refuses a stale date, so a turn that runs before
     * this finishes would fail for no visible reason. */
    xTaskCreateWithCaps(clock_task, "cn_clock", 5120, NULL, 3, NULL, MUSE_BIG_CAPS);
#if CONFIG_MUSE_CN_AUTOTEST
    muse_hatch_autotest_start();
#endif
}

void muse_hatch_status(muse_hatch_status_t *out)
{
    if (!out) {
        return;
    }
    out->state = s_state;
    strlcpy(out->detail, s_detail, sizeof(out->detail));
}

/* A cheap reachability check: SenseNova's model list.  Unlike the Meta path
 * there is no VM to hand-shake with, so "can we reach the internet at all"
 * is the whole question. */
void muse_hatch_test(void)
{
    if (!muse_wifi_connected()) {
        s_state = MUSE_HATCH_OFFLINE;
        strlcpy(s_detail, "no Wi-Fi", sizeof(s_detail));
        return;
    }
    s_state = MUSE_HATCH_TESTING;
    strlcpy(s_detail, "checking...", sizeof(s_detail));

    char auth[160], resp[1024];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_MUSE_CN_SENSENOVA_KEY);
    esp_err_t err = http_post_json(CONFIG_MUSE_CN_SENSENOVA_URL, auth,
                                   "{\"model\":\"" CONFIG_MUSE_CN_SENSENOVA_MODEL
                                   "\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],"
                                   "\"max_tokens\":1,\"reasoning_effort\":\"none\"}",
                                   resp, sizeof(resp), NULL);
    if (err == ESP_OK) {
        s_state = MUSE_HATCH_REACHABLE;
        strlcpy(s_detail, "SenseNova OK", sizeof(s_detail));
    } else {
        s_state = MUSE_HATCH_UNREACHABLE;
        strlcpy(s_detail, "SenseNova unreachable", sizeof(s_detail));
    }
}

void muse_hatch_config_changed(void)
{
    s_state = MUSE_HATCH_UNTESTED;
    s_hist_count = 0;
    s_hist_head = 0;
}

const char *muse_hatch_state_name(muse_hatch_state_t state)
{
    switch (state) {
    case MUSE_HATCH_NOT_SET:     return "not set";
    case MUSE_HATCH_OFFLINE:     return "offline";
    case MUSE_HATCH_UNTESTED:    return "untested";
    case MUSE_HATCH_TESTING:     return "testing";
    case MUSE_HATCH_REACHABLE:   return "reachable";
    case MUSE_HATCH_UNREACHABLE: return "unreachable";
    }
    return "?";
}

void muse_hatch_set_resting(bool resting)
{
    (void)resting;
}

bool muse_hatch_ready(void)
{
    return muse_wifi_connected();
}

/* Serial-console typed turns: run the same LLM path without any audio. */
static void text_turn_task(void *arg)
{
    char *text = arg;
    char *answer = malloc(REPLY_MAX);
    if (!answer) {
        ESP_LOGE(TAG, "@chat {\"type\":\"error\",\"msg\":\"oom\"}");
        free(text);
        vTaskDelete(NULL);
        return;
    }
    if (sensenova_ask(text, answer, REPLY_MAX)) {
        history_push("user", text);
        history_push("assistant", answer);
        strlcpy(s_reply, answer, sizeof(s_reply));
        s_reply_ready = true;
        ESP_LOGI(TAG, "@chat {\"type\":\"text\",\"msg\":\"%s\"}", answer);
    } else {
        ESP_LOGE(TAG, "@chat {\"type\":\"error\",\"msg\":\"SenseNova failed\"}");
    }
    free(answer);
    free(text);
    vTaskDelete(NULL);
}

void muse_hatch_text_turn(char *text)
{
    /* PSRAM stack: the TLS handshake alone wants ~5 KiB. */
    if (xTaskCreateWithCaps(text_turn_task, "cn_text", 10240, text, 5, NULL,
                            MUSE_BIG_CAPS) != pdPASS) {
        free(text);
    }
}

void muse_hatch_text_cancel(void)
{
}

/* muse_hatch_console() and muse_hatch_unescape() are provided by
 * muse_chat_text.c, which is built for every backend. */

size_t muse_hatch_mp3_selftest(int16_t **pcm)
{
    (void)pcm;
    return 0;   /* no bundled MP3 in this backend */
}

/* ------------------------------------------------------- the boot autotest */

#if CONFIG_MUSE_CN_AUTOTEST
/*
 * Runs one text turn and one synthetic audio turn by itself, a few seconds
 * after the network comes up.
 *
 * Why this exists: a crash inside a turn leaves nothing behind.  The reset
 * wipes the log, and this board's console is USB-Serial-JTAG, so the ROM's
 * "rst:0x..(BROWN_OUT)" banner is lost while the port re-enumerates.  The
 * only way to tell a brownout from a panic from a plain software reset is to
 * report the reset reason *after* the port is back -- which is what this does
 * first.  It also makes the whole path reproducible without a person holding
 * the button at exactly the right moment.
 *
 * s_at_boots lives in RTC memory, so it survives a reset: an autotest that
 * kills the chip cannot restart itself forever.  After three boots it stops
 * and leaves the board alone.
 */
RTC_NOINIT_ATTR static uint32_t s_at_boots;

static const char *reset_name(int why)
{
    static const char *names[] = {
        "unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt",
        "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse",
        "pwr_glitch", "cpu_lockup",
    };
    return (why >= 0 && why < (int)(sizeof(names) / sizeof(names[0])))
               ? names[why] : "?";
}

/* Drains the turn's events until the caption lands or `seconds` run out. */
static void cn_autotest_drain(const char *phase, int seconds)
{
    char text[HEARD_MAX];
    for (int i = 0; i < seconds * 10; i++) {
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            ESP_LOGI(TAG, "autotest %s: event %d '%s'", phase, (int)ev, text);
        }
        if (s_reply_ready) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "autotest %s: end (phase=%d, stack %u B free, internal %u)",
             phase, (int)s_phase,
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void cn_autotest_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 600 && !muse_wifi_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int why = (int)esp_reset_reason();
    if (why == ESP_RST_POWERON) {
        s_at_boots = 0;   /* a fresh power-on resets the budget */
    }
    ESP_LOGI(TAG, "autotest: boot %u, last reset %d (%s), wifi %s",
             (unsigned)s_at_boots, why, reset_name(why),
             muse_wifi_connected() ? "up" : "DOWN");
    s_at_boots++;
    if (s_at_boots > 3 || !muse_wifi_connected()) {
        ESP_LOGW(TAG, "autotest: stopping here (boot %u)", (unsigned)s_at_boots);
        vTaskDelete(NULL);
        return;
    }

    /* Phase 1: the LLM half, with no audio involved.  A real utterance can
     * transcribe to nothing (the free quota, a quiet room), and that path
     * returns early -- so testing the LLM on its own is what makes a failure
     * unambiguous. */
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "autotest: ---- phase 1: text turn ----");
    char *q = strdup("你好，用一句话介绍你自己");
    if (q) {
        muse_hatch_text_turn(q);   /* takes ownership of q */
    }
    vTaskDelay(pdMS_TO_TICKS(25000));   /* its "@chat {...}" lands in the log */

    /* Phase 2: the ASR half -- open the iFlytek session, upload, wait for the
     * final frame, then let the turn go on to the LLM. */
    ESP_LOGI(TAG, "autotest: ---- phase 2: audio turn ----");
    const size_t frames = MUSE_AUDIO_RATE * 3 / 2;             /* 1.5 s */
    int16_t *pcm = heap_caps_malloc(frames * sizeof(int16_t), MUSE_BIG_CAPS);
    if (!pcm) {
        ESP_LOGE(TAG, "autotest: no PSRAM for the test note");
        vTaskDelete(NULL);
        return;
    }
    /* A tone with a speech-like envelope: it gives the engine real energy to
     * chew on.  (It is not speech, so the transcript may well come back
     * empty -- the point is to exercise the socket, not to dictate.) */
    for (size_t i = 0; i < frames; i++) {
        double t = (double)i / MUSE_AUDIO_RATE;
        double env = 0.35 * (0.5 - 0.5 * cos(2.0 * M_PI * 3.0 * t));
        pcm[i] = (int16_t)(env * 12000.0 * sin(2.0 * M_PI * 220.0 * t));
    }
    muse_hatch_turn_begin();
    for (size_t off = 0; off < frames; off += MUSE_AUDIO_CHUNK) {
        size_t n = frames - off < MUSE_AUDIO_CHUNK ? frames - off : MUSE_AUDIO_CHUNK;
        muse_hatch_turn_audio(pcm + off, n);
        vTaskDelay(pdMS_TO_TICKS(20));   /* as the microphone would */
    }
    heap_caps_free(pcm);
    muse_hatch_turn_end();
    cn_autotest_drain("audio", 90);

    ESP_LOGI(TAG, "autotest: all done, internal %u (largest %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    vTaskDelete(NULL);
}

static void muse_hatch_autotest_start(void)
{
    if (xTaskCreateWithCaps(cn_autotest_task, "cn_auto", 8192, NULL, 3, NULL,
                            MUSE_BIG_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "autotest: cannot start");
    }
}
#endif  /* CONFIG_MUSE_CN_AUTOTEST */
