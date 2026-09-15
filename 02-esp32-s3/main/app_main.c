#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "uart_link.h"
#include "wifi_app.h"
#include "ntp_sync.h"
#include "mqtt_app.h"

static const char *TAG = "app";

static void on_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch(cmd)
    {
        case CMD_TEMP_HUM:
        {
            if(len != 4)
            {
                break;
            }
            int16_t t = (int16_t)((payload[0] << 8) | payload[1]);
            uint16_t h = (uint16_t)((payload[2] << 8) | payload[3]);
            char buf[16];
            ESP_LOGI(TAG, "stm32 temp=%d.%d C, hum=%d.%d%%", t / 10, (t < 0 ? -t : t) % 10, h / 10, h % 10);
            //把温度和湿度发送到broker
            snprintf(buf, sizeof(buf), "%d.%d", t / 10, (t < 0 ? -t : t) % 10);
            mqtt_app_publish("stm32/temp", buf, 0, 1);
            snprintf(buf, sizeof(buf), "%d.%d", h / 10, h % 10);
            mqtt_app_publish("stm32/hum", buf, 0, 1);
            break;
        }
        case CMD_HEARTBEAT:
        {
            ESP_LOGI(TAG, "stm32 heartbeat");
            break;
        }
        default:
        {
            ESP_LOGW(TAG, "unknow cmd 0x%02x len=%d", cmd, len);
            break;
        }
    }
}

void app_main(void)
{
    //初始化uart通信
    uart_link_init();
    //确认数据无误后用回调函数操作数据
    uart_link_set_callback(on_frame);
    //通信分配优先级并启动freertos
    uart_link_start();

    //初始化wifi并连接
    ESP_ERROR_CHECK(wifi_app_init());
    if(wifi_app_wait_connected(15000))      //等15秒联网
    {
        ESP_LOGI(TAG, "网络就绪,RSSI=%d dBm", wifi_app_get_rssi());
        wifi_app_print_dns();
        //联网成功后进行校时
        ntp_sync_start();
        if(ntp_sync_wait(15000))
        {
            ntp_sync_print();
        }
        else
        {
            ESP_LOGW(TAG, "NTP同步超时");
        }
        //连MQTT
        mqtt_app_start();
        if(mqtt_app_wait_connected(10000))
        {
            ESP_LOGI(TAG, "MQTT就绪");
        }
        else
        {
            ESP_LOGW(TAG, "MQTT连接超时");
        }
    }
    else
    {
        ESP_LOGI(TAG, "联网失败, 请检查配置");
    }


    const char *msg = "hi STM32";
    int count = 0;

    while(1)
    {
        uart_link_send_frame(CMD_OLED_TEXT, (const uint8_t *)msg, strlen(msg));
        ESP_LOGI(TAG, "send OLED tex frame #%d", count++);
        //2s后再一个循环
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}