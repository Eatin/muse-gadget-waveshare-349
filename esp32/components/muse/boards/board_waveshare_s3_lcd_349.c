/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Waveshare ESP32-S3-Touch-LCD-3.49, revision V2: ESP32-S3R8 with 16 MB flash
 * and 8 MB octal PSRAM, a 3.49" 172x640 IPS panel on an AXS15231B over QSPI
 * with the controller's own capacitive touch on I2C, an ES8311 speaker DAC and
 * an ES7210 microphone ADC, a TCA9554 IO expander that owns the panel reset,
 * the backlight enable, the speaker amplifier's mode and the battery power
 * latch, and two buttons: BOOT and PWR.
 *
 * Pins and the expander's bit map come from Waveshare's own ESP-IDF demos
 * (09_LVGL_V8_Test for the panel, 08_Audio_Test for the codecs, 07_BATT_PWR_Test
 * for the battery and the latch). V2 differs from V1 in where the panel reset
 * and the backlight live: V1 drove LCD_RST (GPIO21) and the backlight (GPIO8)
 * straight from the chip; V2 puts the reset on the expander (EXIO5) and moves
 * the backlight to GPIO42, active low, gated by EXIO1.
 *
 * In QSPI mode the AXS15231B ignores the row address: a write lands at the top
 * of the screen (RAMWR, which esp_lcd_axs15231b sends when y is 0) or straight
 * after the previous one (RAMWRC). So LVGL renders in direct mode into one
 * full-screen PSRAM buffer and every frame goes out whole, top to bottom in
 * bands copied through internal RAM.
 *
 * The panel is native portrait (172x640) but the gadget reads better held
 * landscape, so LVGL works on a rotated 640x172 canvas and flush() turns the
 * frame back into the panel's own order on the way out. The rotation is done
 * here rather than with lv_display_set_rotation() because QSPI can only be fed
 * a whole frame from row 0 down: LVGL's software rotation emits partial,
 * windowed updates, and the panel would silently drop all but the first. The
 * frame buffer stays 640x172x2 = 220160 B either way, so nothing moves in RAM.
 * The touch panel is turned the same way, and the direction is LAND_ROT_DIR.
 *
 * Muse's UI needs no help with this: it has a short-landscape layout (it also
 * drives 320x240 dev kits), so it picks it from width > height with height
 * under 320.
 */
#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_audio.h"
#include "muse_board.h"
#include "muse_mem.h"

static const char *TAG = "board";

/* ------------------------------------------------------------------ pins */

/* QSPI panel: the AXS15231B takes commands and pixels on the same four lines. */
#define LCD_HOST SPI3_HOST
#define LCD_CS GPIO_NUM_9
#define LCD_SCLK GPIO_NUM_10
#define LCD_D0 GPIO_NUM_11
#define LCD_D1 GPIO_NUM_12
#define LCD_D2 GPIO_NUM_13
#define LCD_D3 GPIO_NUM_14

/* The panel's own size, as the QSPI stream wants it. */
#define LCD_W 172
#define LCD_H 640

/* Rows per QSPI transfer. The two chunk buffers live in DMA-capable internal
 * RAM (the SPI DMA cannot read PSRAM fast enough while both cores are
 * drawing), and that pool is the same one the I2S DMA rings come out of.
 * At 64 rows the pair costs 2 x 21504 B and empties the pool: audio_init()
 * then fails with "i2s_alloc_dma_desc: allocate DMA buffer failed" and the
 * voice pipeline never starts. 32 rows halves it to 2 x 11008 B and still
 * needs only 20 transfers per frame, well inside the 40 ms frame budget. */
#define CHUNK_ROWS 32
#define CHUNK_BYTES (LCD_W * CHUNK_ROWS * 2)

_Static_assert(LCD_H % CHUNK_ROWS == 0, "a frame is sent as whole bands");

/* What LVGL is told. The canvas is the panel turned a quarter turn, because
 * the reader holds the board landscape. Set to 0 to go back to the panel's own
 * portrait shape, which is what Muse's tall layout wants. */
#define LANDSCAPE 1

/* Which of the two quarter turns, differing by half a turn -- that is, by
 * whether the board's top edge ends up on the reader's left or on the right.
 * Both are upright; only one matches the hand the reader uses.
 *
 * 0 is the turn the lyrics player settled on (lvgl_port.c, s_rot_dir = 0, the
 * one its own /api/rot calls the default), so the two firmwares are read the
 * same way round. Flip it to 1 if the picture comes up upside down.
 *
 *   turn 0   panel column = 171 - ly,  panel row = lx
 *   turn 1   panel column = ly,        panel row = 639 - lx
 *
 * gather_band() and touch_read() are the two halves of this one mapping, so
 * they have to be changed together. */
#define LAND_ROT_DIR 0

#if LANDSCAPE
#  define VIEW_W LCD_H
#  define VIEW_H LCD_W
#else
#  define VIEW_W LCD_W
#  define VIEW_H LCD_H
#endif

_Static_assert((size_t)VIEW_W * VIEW_H == (size_t)LCD_W * LCD_H,
               "the rotation must not change the frame buffer's size");

/* Touch shares the AXS15231B silicon, on its own I2C bus. */
#define TP_SDA GPIO_NUM_17
#define TP_SCL GPIO_NUM_18
#define TP_ADDR 0x3B

/* The expander and both codecs share the system I2C bus. */
#define SYS_SDA GPIO_NUM_47
#define SYS_SCL GPIO_NUM_48
#define TCA9554_ADDR 0x20

/* TCA9554 P0..P7. Under 0 the bit is an output; the config register's bits
 * read 1 for inputs. */
#define EXIO_TOUCH_INT (1u << 0)
#define EXIO_BL_EN (1u << 1)
#define EXIO_IMU_INT1 (1u << 2)
#define EXIO_IMU_INT2 (1u << 3)
#define EXIO_RTC_INT (1u << 4)
#define EXIO_LCD_RST (1u << 5)
#define EXIO_SYS_EN (1u << 6)
#define EXIO_NS_MODE (1u << 7)

/* Backlight: low is bright, and EXIO_BL_EN must be high for the LED string's
 * boost to run at all. */
#define LCD_BL GPIO_NUM_42
#define BL_PWM_FREQ_HZ 20000
#define BL_PWM_BITS LEDC_TIMER_8_BIT

/* Audio: one duplex I2S bus, ES8311 out and ES7210 in, both on the system bus. */
#define I2S_MCLK GPIO_NUM_7
#define I2S_BCLK GPIO_NUM_15
#define I2S_WS GPIO_NUM_46
#define I2S_DIN GPIO_NUM_6
#define I2S_DOUT GPIO_NUM_45
/* I2S DMA rings: 4 x 160 frames = 40 ms a direction, two of Muse's 20 ms
 * chunks (MUSE_AUDIO_CHUNK) with room to spare. The driver default of 6 x 240
 * costs ~92 KB for the duplex pair; this is 10 KB. See audio_init(). */
#define I2S_DMA_DESC 4
#define I2S_DMA_FRAMES 160
#define ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
#define ES7210_ADDR ES7210_CODEC_DEFAULT_ADDR

/* Buttons: BOOT talks, PWR wakes and switches the board off, and doubles as the
 * battery-present pin (SYS_OUT) while it is up. */
#define TALK_GPIO GPIO_NUM_0
#define AUX_GPIO GPIO_NUM_16
#define AUX_LONG_PRESS_MS 1000

/* Battery voltage on GPIO4 through a 1:3 divider (ADC1 channel 3). */
#define BATT_ADC ADC_CHANNEL_3

/* ----------------------------------------------------------------- state */

static i2c_master_bus_handle_t s_i2c;       /* expander + codecs */
static i2c_master_dev_handle_t s_expander;
static i2c_master_dev_handle_t s_tp;        /* touch, on its own bus */

static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static uint8_t *s_fb;                       /* the whole screen, in PSRAM */
static uint8_t *s_chunk[2];                 /* DMA-capable copies on their way out */
static SemaphoreHandle_t s_chunk_free;
static SemaphoreHandle_t s_lv_lock;
static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;

static muse_gpio_button_t s_talk, s_aux;
static adc_oneshot_unit_handle_t s_adc;
static i2s_chan_handle_t s_tx, s_rx;

static bool s_bl_on;

/* ------------------------------------------------------- TCA9554 expander */

/* Register 0 reads the pins, 1 holds the output latch, 3 the directions. The
 * chip is small enough that a handful of writes over the IDF's new i2c_master
 * covers everything Muse needs — no esp_io_expander component. */
static esp_err_t exio_write(uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_expander, buf, sizeof(buf), 50);
}

static esp_err_t exio_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_expander, &reg, 1, val, 1, 50);
}

static esp_err_t exio_set(uint32_t mask, bool level)
{
    uint8_t out;
    ESP_RETURN_ON_ERROR(exio_read(0x01, &out), TAG, "exio read back");
    out = level ? (uint8_t)(out | mask) : (uint8_t)(out & ~mask);
    return exio_write(0x01, out);
}

static esp_err_t exio_start(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c, &cfg, &s_expander), TAG, "exio device");
    /* Outputs by default (config bit 0), except the interrupt inputs. Kept in
     * one write so no pin is ever left floating while the board comes up. */
    const uint8_t outputs = (uint8_t)~(EXIO_TOUCH_INT | EXIO_IMU_INT1 | EXIO_IMU_INT2 | EXIO_RTC_INT);
    ESP_RETURN_ON_ERROR(exio_write(0x03, (uint8_t)~outputs), TAG, "exio direction");
    /* The latch's initial state: reset released, backlight boost on but the
     * LED string dark (the PWM pin is what lights it), amplifier out of its
     * power-down mode, power latch closed. */
    ESP_RETURN_ON_ERROR(exio_write(0x01, (uint8_t)EXIO_LCD_RST | EXIO_BL_EN | EXIO_NS_MODE | EXIO_SYS_EN),
                        TAG, "exio output");
    return ESP_OK;
}

/* Opens the battery latch. Until this runs the board is powered only while PWR
 * is held, so it is the first thing init() does. */
static esp_err_t power_hold(void)
{
    ESP_RETURN_ON_ERROR(exio_set(EXIO_SYS_EN, true), TAG, "power latch");
    uint8_t lv;
    ESP_RETURN_ON_ERROR(exio_read(0x01, &lv), TAG, "power latch read");
    ESP_RETURN_ON_ERROR(lv & EXIO_SYS_EN ? ESP_OK : ESP_FAIL, TAG, "power latch stuck low");
    return ESP_OK;
}

/* ------------------------------------------------------------- ADC (battery) */

static esp_err_t battery_mv(int *out_mv)
{
    /* The divider takes the cell down by three and the 12 dB attenuation tops
     * the ADC out at about 3.1 V, so full scale is roughly 4.6 V at the cell.
     * One point per 5% of a single-cell Li-ion's discharge curve, in mV. */
    static const struct {
        int mv, pct;
    } levels[] = {
        { 3300, 0 }, { 3550, 10 }, { 3680, 20 }, { 3730, 30 }, { 3760, 40 },
        { 3790, 50 }, { 3860, 60 }, { 3950, 70 }, { 4040, 80 }, { 4150, 95 }, { 4250, 100 },
    };
    enum { N = sizeof(levels) / sizeof(levels[0]) };

    int sum = 0;
    for (int i = 0; i < 16; i++) {
        int raw;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc, BATT_ADC, &raw), TAG, "adc read");
        sum += raw;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    int raw = sum / 16;
    /* 12 dB attenuation, 12 bits, with the chip's published curve approximated
     * linearly between the pins that matter here. */
    int pin_mv = raw * 3100 / 4095;
    int cell_mv = pin_mv * 3;
    *out_mv = cell_mv;
    return ESP_OK;
}

static int battery_pct(int mv)
{
    static const int pct[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 95, 100 };
    static const int mv_at[] = { 3300, 3550, 3680, 3730, 3760, 3790, 3860, 3950, 4040, 4150, 4250 };
    enum { N = sizeof(pct) / sizeof(pct[0]) };
    if (mv <= mv_at[0]) {
        return 0;
    }
    for (int i = 0; i + 1 < N; i++) {
        if (mv >= mv_at[i] && mv < mv_at[i + 1]) {
            return pct[i] + (mv - mv_at[i]) * (pct[i + 1] - pct[i]) / (mv_at[i + 1] - mv_at[i]);
        }
    }
    return 100;
}

/* -------------------------------------------------------------------- init */

static esp_err_t init(void)
{
    /* I2C0 carries the expander and both codecs. The pins have external
     * pull-ups on this board, so the internal ones are only a safety net. */
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = SYS_SDA,
        .scl_io_num = SYS_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &s_i2c), TAG, "i2c bus");
    ESP_RETURN_ON_ERROR(exio_start(), TAG, "expander");
    ESP_RETURN_ON_ERROR(power_hold(), TAG, "power hold");

    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_talk, TALK_GPIO), TAG, "talk button");
    ESP_RETURN_ON_ERROR(muse_gpio_button_init(&s_aux, AUX_GPIO), TAG, "aux button");
    /* PWR is held at reset to get here from the bootloader, and is low whenever
     * the battery is out; don't count either as a press. */
    s_aux.pressed = gpio_get_level(AUX_GPIO) == 0;

    const adc_oneshot_unit_init_cfg_t adc_cfg = { .unit_id = ADC_UNIT_1 };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&adc_cfg, &s_adc), TAG, "adc");
    const adc_oneshot_chan_cfg_t ch_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BATT_ADC, &ch_cfg), TAG, "adc channel");
    return ESP_OK;
}

/* --------------------------------------------------------------- display */

static bool IRAM_ATTR on_chunk_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_chunk_free, &woken);
    return woken == pdTRUE;
}

#if LANDSCAPE
/*
 * Turn one band of the landscape canvas back into the panel's own order.
 *
 * The loop walks the canvas by rows so the PSRAM side stays contiguous: the
 * CHUNK_ROWS pixels one canvas row contributes to a band are neighbours, while
 * the destination hops a whole panel row (LCD_W pixels) per entry. That is
 * 640 x 172 pixel moves a frame, or a few ms, which fits the 40 ms budget next
 * to the SPI transfer it overlaps.
 *
 * The canvas column that lands on a given panel row is what the direction
 * decides, and with it which end of the band is read first:
 *
 *   turn 0   panel row = canvas column, so the band starting at panel_y0 is the
 *            canvas columns panel_y0..panel_y0+31, read forwards
 *   turn 1   panel row = 639 - canvas column, so it is the canvas columns
 *            639-panel_y0 downwards, read backwards
 *
 * Either way it is one straight run of PSRAM per canvas row; only the walk
 * direction and the destination column differ.
 */
static void gather_band(uint8_t *dst, int panel_y0)
{
    uint16_t *d = (uint16_t *)dst;
    for (int ly = 0; ly < VIEW_H; ly++) {
#if LAND_ROT_DIR == 0
        const uint16_t *src = (const uint16_t *)(s_fb + (size_t)ly * VIEW_W * 2) + panel_y0;
        uint16_t *out = d + (VIEW_H - 1 - ly);
        for (int j = 0; j < CHUNK_ROWS; j++) {
            out[(size_t)j * LCD_W] = src[j];
        }
#else
        const uint16_t *src = (const uint16_t *)(s_fb + (size_t)ly * VIEW_W * 2) + (VIEW_W - 1 - panel_y0);
        uint16_t *out = d + ly;
        for (int j = 0; j < CHUNK_ROWS; j++) {
            out[(size_t)j * LCD_W] = *src--;
        }
#endif
    }
}
#endif

/*
 * Direct mode: LVGL redraws only what changed, in place in s_fb, and calls this
 * once per area. After the last one the whole frame goes out from row 0, since
 * the panel can't be sent a window lower down. Each band is copied into
 * internal RAM while the one before is on the wire (the SPI DMA can't keep up
 * reading PSRAM while both cores draw).
 */
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    if (lv_display_flush_is_last(disp)) {
        int k = 0;
        for (int y = 0; y < LCD_H; y += CHUNK_ROWS) {
            xSemaphoreTake(s_chunk_free, portMAX_DELAY);
#if LANDSCAPE
            gather_band(s_chunk[k], y);
#else
            memcpy(s_chunk[k], s_fb + (size_t)y * LCD_W * 2, CHUNK_BYTES);
#endif
            if (esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + CHUNK_ROWS, s_chunk[k]) != ESP_OK) {
                xSemaphoreGive(s_chunk_free);
            }
            k ^= 1;
        }
        /* Both pieces back: the frame is on the panel and s_fb is LVGL's again. */
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        xSemaphoreTake(s_chunk_free, portMAX_DELAY);
        xSemaphoreGive(s_chunk_free);
        xSemaphoreGive(s_chunk_free);
    }
    lv_display_flush_ready(disp);
}

/*
 * The AXS15231B's touch answers an 11-byte read command with a small reply:
 * a gesture byte, the point count, then X and Y in 12 bits each.
 *
 * ★ The two axes it reports are transposed with respect to the glass. Its X
 * runs along the panel's long side (0..639, counted the other way up) and its Y
 * along the short one (0..171), so the pixel under the finger is
 *
 *     panel column = raw Y        panel row = 639 - raw X
 *
 * This is the mapping the official example uses (ref/ex/09_LVGL_V8_Test, its
 * portrait default: point.x = pointY, point.y = 640 - pointX) and the one the
 * lyrics player then confirmed against real touches on this same panel --
 * recorded raw frames have Y at 46..71 and X as high as 437, which only fits
 * Y on the short side. Reading the two bytes as panel X and Y instead puts
 * every touch in the wrong place, and clips X to the first 172 columns while
 * it is at it, since that is all the panel is wide.
 */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static const uint8_t cmd[11] = { 0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08 };
    uint8_t buf[8];
    data->state = LV_INDEV_STATE_RELEASED;
    if (i2c_master_transmit(s_tp, cmd, sizeof(cmd), 20) != ESP_OK ||
        i2c_master_receive(s_tp, buf, sizeof(buf), 20) != ESP_OK || buf[0] != 0 || buf[1] == 0) {
        return;
    }
    /* Clamp where the ranges are known: the chip's own frame, not the panel's. */
    int tx = (buf[2] & 0x0F) << 8 | buf[3];
    int ty = (buf[4] & 0x0F) << 8 | buf[5];
    if (tx >= LCD_H) {
        tx = LCD_H - 1;
    }
    if (ty >= LCD_W) {
        ty = LCD_W - 1;
    }
    const int panel_col = ty;
    const int panel_row = (LCD_H - 1) - tx;
    /* Back the way gather_band() came. */
#if !LANDSCAPE
    data->point.x = panel_col;
    data->point.y = panel_row;
#elif LAND_ROT_DIR == 0
    data->point.x = panel_row;
    data->point.y = (VIEW_H - 1) - panel_col;
#else
    data->point.x = (LCD_H - 1) - panel_row;
    data->point.y = panel_col;
#endif
    data->state = LV_INDEV_STATE_PRESSED;
}

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Touch is optional: if it doesn't answer, the board still comes up without
 * an input device and the settings screen is reached over BLE instead. */
static esp_err_t touch_start(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = TP_SDA,
        .scl_io_num = TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "touch i2c");
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_tp), TAG, "touch device");
    return i2c_master_probe(bus, TP_ADDR, 50);
}

/* The panel reset is the expander's, so it is pulsed by hand before init(). */
static esp_err_t lcd_reset(void)
{
    ESP_RETURN_ON_ERROR(exio_set(EXIO_LCD_RST, true), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(exio_set(EXIO_LCD_RST, false), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(250));
    return exio_set(EXIO_LCD_RST, true);
}

/* The panel's startup state. Waveshare's demo only sends sleep-out and
 * display-on: esp_lcd_axs15231b's own table does the rest, and the 172x640
 * part is the same controller family as the Guition board's. */
static const axs15231b_lcd_init_cmd_t s_lcd_init[] = {
    { 0xBB, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5 }, 8, 0 },
    { 0xA0, (uint8_t[]){ 0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00 }, 17, 0 },
    { 0xA2, (uint8_t[]){ 0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10, 0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A }, 31, 0 },
    { 0xD0, (uint8_t[]){ 0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0xC2, 0x42, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12 }, 30, 0 },
    { 0xA3, (uint8_t[]){ 0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55 }, 22, 0 },
    { 0xC1, (uint8_t[]){ 0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40 }, 30, 0 },
    { 0xC3, (uint8_t[]){ 0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01 }, 11, 0 },
    { 0xC4, (uint8_t[]){ 0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50 }, 29, 0 },
    { 0xC5, (uint8_t[]){ 0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00 }, 23, 0 },
    { 0xC6, (uint8_t[]){ 0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22 }, 20, 0 },
    { 0xC7, (uint8_t[]){ 0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF, 0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F }, 20, 0 },
    { 0xC9, (uint8_t[]){ 0x33, 0x44, 0x44, 0x01 }, 4, 0 },
    { 0xCF, (uint8_t[]){ 0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08 }, 27, 0 },
    { 0xD5, (uint8_t[]){ 0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00 }, 30, 0 },
    { 0xD6, (uint8_t[]){ 0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00 }, 30, 0 },
    { 0xD7, (uint8_t[]){ 0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F }, 19, 0 },
    { 0xD8, (uint8_t[]){ 0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19 }, 12, 0 },
    { 0xD9, (uint8_t[]){ 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }, 12, 0 },
    { 0xDD, (uint8_t[]){ 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F }, 12, 0 },
    { 0xDF, (uint8_t[]){ 0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90 }, 8, 0 },
    { 0xE0, (uint8_t[]){ 0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D }, 17, 0 },
    { 0xE1, (uint8_t[]){ 0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F }, 17, 0 },
    { 0xE2, (uint8_t[]){ 0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D }, 17, 0 },
    { 0xE3, (uint8_t[]){ 0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F }, 17, 0 },
    { 0xE4, (uint8_t[]){ 0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D }, 17, 0 },
    { 0xE5, (uint8_t[]){ 0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F }, 17, 0 },
    { 0xA4, (uint8_t[]){ 0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30 }, 16, 0 },
    { 0xA4, (uint8_t[]){ 0x85, 0x85, 0x95, 0x85 }, 4, 0 },
    { 0xBB, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 8, 0 },
    { 0x11, (uint8_t[]){ 0x00 }, 0, 120 },
    { 0x29, (uint8_t[]){ 0x00 }, 0, 100 },
    { 0x2C, (uint8_t[]){ 0x00, 0x00, 0x00, 0x00 }, 4, 0 },
};

/* Runs on the LVGL task, so the panel's SPI interrupt lands on its core. */
static esp_err_t lcd_start(void)
{
    const ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_PWM_BITS,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t bl_ch = {
        .gpio_num = LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_3,
        .duty = (1u << BL_PWM_BITS) - 1,   /* active low: full duty is dark */
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "backlight timer");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_ch), TAG, "backlight");

    s_fb = heap_caps_malloc((size_t)VIEW_W * VIEW_H * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_chunk[0] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk[1] = heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_chunk_free = xSemaphoreCreateCounting(2, 2);
    ESP_RETURN_ON_FALSE(s_fb && s_chunk[0] && s_chunk[1] && s_chunk_free, ESP_ERR_NO_MEM, TAG, "frame buffers");
    memset(s_fb, 0, (size_t)VIEW_W * VIEW_H * 2);

    const spi_bus_config_t bus = AXS15231B_PANEL_BUS_QSPI_CONFIG(LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3, CHUNK_BYTES);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "lcd bus");
    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_CS, on_chunk_sent, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &io_cfg, &io), TAG, "lcd io");
    axs15231b_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io, &panel_cfg, &s_panel), TAG, "lcd panel");
    ESP_RETURN_ON_ERROR(lcd_reset(), TAG, "lcd reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "lcd init");

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(VIEW_W, VIEW_H);
    ESP_RETURN_ON_FALSE(s_disp, ESP_ERR_NO_MEM, TAG, "lv display");
    /* The panel takes RGB565 big-endian over SPI. */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(s_disp, s_fb, NULL, (size_t)VIEW_W * VIEW_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush);

    if (touch_start() == ESP_OK) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read);
        lv_indev_set_display(s_indev, s_disp);
    } else {
        ESP_LOGW(TAG, "touch controller not answering at 0x%02x", TP_ADDR);
    }
    return ESP_OK;
}

static void lvgl_task(void *arg)
{
    (void)arg;
    s_start_err = lcd_start();
    xSemaphoreGive(s_started);
    if (s_start_err != ESP_OK) {
        vTaskDelete(NULL);
    }
    for (;;) {
        xSemaphoreTakeRecursive(s_lv_lock, portMAX_DELAY);
        uint32_t ms = lv_timer_handler();
        xSemaphoreGiveRecursive(s_lv_lock);
        ms = ms < 5 ? 5 : ms > 50 ? 50 : ms;
        vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    s_lv_lock = xSemaphoreCreateRecursiveMutex();
    s_started = xSemaphoreCreateBinary();
    if (!s_lv_lock || !s_started ||
        xTaskCreatePinnedToCoreWithCaps(lvgl_task, "lvgl", 8192, NULL, MUSE_UI_PRIORITY, NULL, MUSE_UI_CORE,
                                        MUSE_BIG_CAPS) != pdPASS) {
        return NULL;
    }
    xSemaphoreTake(s_started, portMAX_DELAY);
    if (s_start_err != ESP_OK) {
        return NULL;
    }
    *touch = s_indev;
    return s_disp;
}

static bool display_lock(int timeout_ms)
{
    return xSemaphoreTakeRecursive(s_lv_lock, timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lv_lock);
}

/*
 * Active low: the PWM pin sinks the LED string's current, so the duty is
 * written inverted. The boost is enabled separately, on the expander, and stays
 * on for the board's life; 0 leaves the string dark without touching it.
 */
static void set_brightness(int pct)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    uint32_t full = (1u << BL_PWM_BITS) - 1;
    uint32_t duty = full - (uint32_t)pct * full / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    s_bl_on = pct > 0;
}

/* The AXS15231B keeps its own RAM: SLPIN/out only stop the scan, so waking is
 * instant and the last frame is still there. */
static void panel_sleep(bool sleep)
{
    esp_lcd_panel_disp_sleep(s_panel, sleep);
}

/* ---------------------------------------------------------------- audio */

/* One duplex bus at Muse's 16 kHz; the ES8311 takes its MCLK from the board's
 * 12.288 MHz-capped clock, which this board wires, so use_mclk is on. The amp
 * has no enable pin of its own (NS_MODE on the expander only picks its mode),
 * so pa_pin is unset and the codec's own power-down does the muting. */
static esp_err_t audio_init(esp_codec_dev_handle_t *spk, esp_codec_dev_handle_t *mic)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    /* Keep the DMA rings small. The stock 6 x 240-frame ring costs ~92 KB of
     * internal DMA memory for the duplex pair; by the time the UI and the
     * BLE/Wi-Fi stacks are up this board has ~13 KB of DMA-capable memory left,
     * and i2s_std_set_slot() fails with "allocate memory for dma descriptor
     * failed". 4 descriptors x 160 frames is 40 ms a direction -- still two of
     * Muse's 20 ms chunks (MUSE_AUDIO_CHUNK), which is all the jitter buffer
     * needs, and it fits. Same numbers as board_ai_passport.c uses. */
    chan_cfg.dma_desc_num = I2S_DMA_DESC;
    chan_cfg.dma_frame_num = I2S_DMA_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MUSE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK,
            .bclk = I2S_BCLK,
            .ws = I2S_WS,
            .dout = I2S_DOUT,
            .din = I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "i2s tx on");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s rx on");

    audio_codec_i2s_cfg_t i2s_cfg = { .port = I2S_NUM_0, .rx_handle = s_rx, .tx_handle = s_tx };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t out_i2c = { .port = I2C_NUM_0, .addr = ES8311_ADDR, .bus_handle = s_i2c };
    audio_codec_i2c_cfg_t in_i2c = { .port = I2C_NUM_0, .addr = ES7210_ADDR, .bus_handle = s_i2c };
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&out_i2c);
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&in_i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && out_ctrl && in_ctrl && gpio_if, ESP_ERR_NO_MEM, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = out_ctrl,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,
        .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *out_codec = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(out_codec, ESP_FAIL, TAG, "ES8311 not responding");

    es7210_codec_cfg_t es_in = {
        .ctrl_if = in_ctrl,
        .master_mode = false,
        /* The board has two mics on the one ES7210; Muse mixes them down. */
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,
        .mclk_src = ES7210_MCLK_FROM_PAD,
        .mclk_div = 256,
    };
    const audio_codec_if_t *in_codec = es7210_codec_new(&es_in);
    ESP_RETURN_ON_FALSE(in_codec, ESP_FAIL, TAG, "ES7210 not responding");

    esp_codec_dev_cfg_t out_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if };
    esp_codec_dev_cfg_t in_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = in_codec, .data_if = data_if };
    *spk = esp_codec_dev_new(&out_cfg);
    *mic = esp_codec_dev_new(&in_cfg);
    return *spk && *mic ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------ buttons */

static unsigned poll_buttons(void)
{
    return muse_gpio_button_poll(&s_talk) | muse_gpio_button_poll(&s_aux) << 2;
}

static void wait_buttons(int timeout_ms)
{
    muse_gpio_buttons_wait((muse_gpio_button_t *const[]){ &s_talk, &s_aux }, 2, timeout_ms);
}

/* --------------------------------------------------------------- power */

/*
 * GPIO16 is both the PWR key and the battery-present flag (SYS_OUT): high when
 * the cell is in, low with it out, and low while the key is down. So the cell
 * is only reported present when the key is up and the pin reads high, and the
 * charger is never seen from here — USB power shows up as a board that stays
 * alive after power_off() cuts the latch.
 */
static esp_err_t read_power(muse_power_t *out)
{
    int mv = 0;
    battery_mv(&mv);
    bool key_down = gpio_get_level(AUX_GPIO) == 0;
    bool present = !key_down && mv > 2500;

    out->battery_mv = present ? mv : 0;
    out->battery_pct = present ? battery_pct(mv) : -1;
    out->charging = false;
    out->usb = false;
    return ESP_OK;
}

/*
 * Opening the latch cuts the battery the moment the key comes up (the key is
 * a bypass across the rail while it is held, so the board keeps running until
 * release). USB 5 V reaches the system rail directly, past the latch, so on
 * USB this only darkens the screen and the board keeps drawing: deep sleep is
 * the fallback, and the PWR key wakes it.
 */
static esp_err_t power_off(void)
{
    set_brightness(0);
    esp_lcd_panel_disp_on_off(s_panel, false);
    exio_set(EXIO_SYS_EN, false);
    /* Wait for the key to come up: that is when the latch takes effect. */
    for (int i = 0; i < 100 && gpio_get_level(AUX_GPIO) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_sleep_enable_ext0_wakeup(AUX_GPIO, 0);
    esp_deep_sleep_start();
    return ESP_FAIL;   /* only on USB power */
}

static const muse_board_t s_board = {
    .name = "Waveshare ESP32-S3-Touch-LCD-3.49",
    .width = VIEW_W,
    .height = VIEW_H,
    .round = false,
    .touch = true,
    .diagonal_in = 3.49f,
    /* BOOT talks, on the left edge; PWR is on the right and wakes the chip. */
    .talk_button = "boot",
    .aux_button = "pwr",
    .talk_hint = { LV_ALIGN_BOTTOM_LEFT, 8, -8 },
    .aux_hint = { LV_ALIGN_BOTTOM_RIGHT, -8, -8 },
    .frame_ms = 40,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = display_unlock,
    .set_brightness = set_brightness,
    .panel_sleep = panel_sleep,
    .audio_init = audio_init,
    .mic_slot = 0,              /* ES7210's first mic, on the left slot */
    .poll_buttons = poll_buttons,
    .wait_buttons = wait_buttons,
    .read_power = read_power,
    .power_off = power_off,
};

/* Home Link's app_main starts Muse with this board (main/main.c). */
const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
