#ifndef UART_LINK_H
#define UART_LINK_H

/* 三端共用的帧协议（STM32 / ESP32 / PC 同一份实现） */
#include "frame_protocol.h"

#include <stdint.h>
#include <stddef.h>


//对外接口函数
void uart_link_init(void);
void uart_link_start(void);
void uart_link_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len);

//回调函数注册
typedef void(*uart_link_cb_t)(uint8_t cmd, const uint8_t *payload, uint8_t len);
void uart_link_set_callback(uart_link_cb_t cb);

#endif