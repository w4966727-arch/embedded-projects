/**
 * ESP32-CAM 工程骨架
 * 后续在这里添加摄像头采集、SD 卡、WiFi 等功能
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    printf("Hello from ESP32-CAM!\n");

    int count = 0;
    while (1) {
        printf("cam tick %d\n", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
