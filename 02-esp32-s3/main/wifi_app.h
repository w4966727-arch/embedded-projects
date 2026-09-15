#ifndef WIFI_APP_H
#define WIFI_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

//初始化并启动连接(非阻塞)
esp_err_t wifi_app_init(void);
//阻塞等待联网成功
bool wifi_app_wait_connected(uint32_t timeout_ms);
//判断联网状态
bool wifi_app_is_connected(void);
//信号强度dBm，调试用
int8_t wifi_app_get_rssi(void);
//打印当前生效的DNS
void wifi_app_print_dns(void);

#endif