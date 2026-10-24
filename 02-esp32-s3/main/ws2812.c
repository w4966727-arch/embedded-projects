/**
 * ws2812.c — WS2812 驱动实现（基于 ESP-IDF v5.x 新 RMT API）
 *
 * ══════════════ 原理说明（重点看这里）══════════════
 *
 * 【WS2812 是什么】
 *   一颗"可寻址 RGB LED"，内部集成了控制 IC + 三色 LED。
 *   只用一根数据线，靠"高低电平的持续时间"编码 0 和 1：
 *
 *     bit = 0 :  高电平 0.4µs  +  低电平 0.85µs
 *     bit = 1 :  高电平 0.8µs  +  低电平 0.45µs
 *     复位     :  低电平 > 50µs（锁存数据，准备接收下一帧）
 *
 *   ⚠️ 时序容差只有 ±150ns —— 软件 GPIO 翻转（受中断/调度影响）根本做不到，
 *      必须用硬件外设按精确时钟自动翻转。这就是 RMT 的用武之地。
 *
 * 【RMT 是什么】
 *   Remote Control Transceiver，本来是给红外遥控设计的，
 *   本质是"按预设的 (电平, 持续时长) 序列自动翻转 GPIO"的硬件。
 *   用 DMA 解放 CPU 是同一个思路：
 *     DMA → 自动搬运数据，CPU 不参与
 *     RMT → 自动翻转电平，CPU 不参与
 *
 * 【RMT 的编码单位：symbol】
 *   一个 rmt_symbol_word_t 描述"两段电平"：
 *     level0 + duration0 → 第一段（例如高电平 0.4µs）
 *     level1 + duration1 → 第二段（例如低电平 0.85µs）
 *   正好对应 WS2812 一个 bit 的"高一低"结构，一个 symbol = 一个 bit。
 */

#include "ws2812.h"

#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ws2812";

/* ── 硬件配置 ───────────────────────────────────────────── */
#define WS2812_GPIO        GPIO_NUM_48
#define WS2812_LED_NUM     1              /* 板载只有 1 颗 */

/* RMT 时钟分辨率：10MHz → 1 tick = 0.1µs（精度足够表达 0.4/0.85µs） */
#define RMT_RESOLUTION_HZ  10000000

/* ── WS2812 时序（单位 tick，1 tick = 0.1µs）───────────────
 *   数据手册典型值，留了少许余量：
 *     T0H = 0.4µs → 4 tick      T0L = 0.85µs → 9 tick
 *     T1H = 0.8µs → 8 tick      T1L = 0.45µs → 5 tick
 *   复位低电平 > 50µs → 取 600 tick（60µs）
 */
#define WS2812_T0H         4
#define WS2812_T0L         9
#define WS2812_T1H         8
#define WS2812_T1L         5
#define WS2812_RESET_TICKS 600

/* ── 驱动句柄 ───────────────────────────────────────────── */
static rmt_channel_handle_t s_tx_chan      = NULL;
static rmt_encoder_handle_t s_copy_encoder = NULL;

/**
 * 初始化 RMT 发送通道
 *
 * 这里用的是 copy 编码器：它把用户传入的 symbol 数组"原样复制"到 RMT 硬件，
 * 不做任何转换 —— 所以编码工作（把 bit 变成 symbol）由我们自己完成，
 * 这样能把 WS2812 的时序完全暴露在代码里。
 */
esp_err_t ws2812_init(void)
{
    /* ① 创建 RMT 发送通道，绑定 GPIO48 */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num            = WS2812_GPIO,
        .clk_src             = RMT_CLK_SRC_DEFAULT,
        .resolution_hz       = RMT_RESOLUTION_HZ,
        .mem_block_symbols   = 64,        /* 硬件 FIFO 容量（symbol 个数） */
        .trans_queue_depth   = 4,         /* 传输队列深度 */
    };
    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &s_tx_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 RMT 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ② 创建拷贝编码器（原样搬运 symbol） */
    rmt_copy_encoder_config_t copy_cfg = {};
    ret = rmt_new_copy_encoder(&copy_cfg, &s_copy_encoder);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建拷贝编码器失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* ③ 使能通道，可以开始发数据 */
    ret = rmt_enable(s_tx_chan);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "使能 RMT 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "初始化完成: GPIO%d, 分辨率 %d Hz (1 tick = 0.1us)",
             WS2812_GPIO, RMT_RESOLUTION_HZ);
    return ESP_OK;
}

/**
 * 设置 RGB 颜色
 *
 * 流程：把 24bit 数据编码成 24 个 symbol（+1 个复位 symbol）→ 交给 RMT 发送
 */
esp_err_t ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_tx_chan == NULL || s_copy_encoder == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* ★ 关键细节：WS2812 的数据顺序是 GRB（不是 RGB），且高位先发 */
    uint8_t grb[3] = { g, r, b };

    /* 24 个数据 symbol + 1 个复位 symbol */
    rmt_symbol_word_t symbols[24 + 1];
    int idx = 0;

    for (int byte = 0; byte < 3; byte++)
    {
        for (int bit = 7; bit >= 0; bit--)        /* MSB 先发 */
        {
            int one = (grb[byte] >> bit) & 1;   //当前bit是否为1

            /* 一个 bit = 一个 symbol（高 + 低），时长随时序表选择 */
            symbols[idx].level0    = 1;
            symbols[idx].duration0 = one ? WS2812_T1H : WS2812_T0H;
            symbols[idx].level1    = 0;
            symbols[idx].duration1 = one ? WS2812_T1L : WS2812_T0L;
            idx++;
        }
    }

    /* 复位段：拉低 >50µs，让灯珠锁存本次数据 */
    symbols[idx].level0    = 0;
    symbols[idx].duration0 = WS2812_RESET_TICKS;
    symbols[idx].level1    = 0;
    symbols[idx].duration1 = 0;
    idx++;

    /* 交给 RMT 硬件发送（异步），然后等它发完 */
    rmt_transmit_config_t tx_config = { .loop_count = 0 };
    esp_err_t ret = rmt_transmit(s_tx_chan, s_copy_encoder, symbols,
                                 idx * sizeof(rmt_symbol_word_t), &tx_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "发送失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 阻塞等待发送完成（单颗灯 25 个 symbol 约 30µs，很快） */
    return rmt_tx_wait_all_done(s_tx_chan, pdMS_TO_TICKS(100));
}

/** 熄灭（发全 0） */
esp_err_t ws2812_off(void)
{
    return ws2812_set_rgb(0, 0, 0);
}

/**
 * 硬件验证序列
 *
 * 亮度刻意用 32（而非 255）：全亮既刺眼又耗电，
 * 而且板子用 USB 供电时全白可能拉低电压导致异常。
 */
void ws2812_test_sequence(void)
{
    static const struct { uint8_t r, g, b; const char *name; } seq[] = {
        { 32,  0,  0, "红" },
        {  0, 32,  0, "绿" },
        {  0,  0, 32, "蓝" },
        { 16, 16, 16, "白" },
        {  0,  0,  0, "灭" },
    };
    const int n = sizeof(seq) / sizeof(seq[0]);

    ESP_LOGI(TAG, "═══ WS2812 验证开始（红→绿→蓝→白→灭）═══");
    for (int i = 0; i < n; i++)
    {
        ESP_LOGI(TAG, "  → %s  (r=%d g=%d b=%d)", seq[i].name, seq[i].r, seq[i].g, seq[i].b);
        ws2812_set_rgb(seq[i].r, seq[i].g, seq[i].b);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    ESP_LOGI(TAG, "═══ 验证结束：若看到灯依次变色，硬件正常 ═══");
}
