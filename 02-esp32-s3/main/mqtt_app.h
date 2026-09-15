#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

//启动mqtt服务
esp_err_t mqtt_app_start(void);
bool mqtt_app_wait_connected(uint32_t timeout_ms);
bool mqtt_app_is_connected(void);
int mqtt_app_publish(const char *topic, const char *data, int qos, int retain);

#endif