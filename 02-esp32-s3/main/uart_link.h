#ifndef UART_LINK_H
#define UART_LINK_H

#include <stdint.h>
#include <stddef.h>

#define FRAME_HEAD1         0xAA
#define FRAME_HEAD2         0x55
#define FRAME_MAX_PAYLOAD   64

//命令字典
#define CMD_TEMP_HUM        0x01
#define CMD_HEARTBEAT       0x02
#define CMD_SERVO           0x10
#define CMD_LED             0x11
#define CMD_OLED_TEXT       0x20

//对外接口函数
void uart_link_init(void);
void uart_link_start(void);
void uart_link_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len);

//回调函数注册
typedef void(*uart_link_cb_t)(uint8_t cmd, const uint8_t *payload, uint8_t len);
void uart_link_set_callback(uart_link_cb_t cb);

#endif