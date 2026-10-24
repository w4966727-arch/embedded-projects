/**
 * ws2812.h — 板载 WS2812 可寻址 RGB LED 驱动（HW-678A: GPIO48）
 *
 * 硬件：WS2812B，单总线可寻址 RGB，24bit/颗（GRB 顺序）
 * 驱动：ESP32-S3 RMT 外设（按预设时序自动翻转 GPIO，CPU 不参与）
 */
#ifndef WS2812_H
#define WS2812_H

#include <stdint.h>
#include "esp_err.h"

/* 初始化 RMT 通道（不点亮） */
esp_err_t ws2812_init(void);

/* 设置颜色（范围 0~255，建议先从小值如 32 试起，全亮刺眼且费电） */
esp_err_t ws2812_set_rgb(uint8_t r, uint8_t g, uint8_t b);

/* 熄灭 */
esp_err_t ws2812_off(void);

/* 硬件验证序列：红 → 绿 → 蓝 → 白 → 灭 */
void ws2812_test_sequence(void);

#endif /* WS2812_H */
