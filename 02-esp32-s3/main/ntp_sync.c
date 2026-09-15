#include "ntp_sync.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include "esp_log.h"

static const char *TAG = "ntp";

#define NTP_SERVER "ntp.aliyun.com" /*国内服务器*/
#define TZ_CHINA "CST-8"            /*UTC+8 -> CST-8*/
#define NTP_RETRY_MS 2000

//同步完成标志，volatile是因为lwIP会改写
static volatile bool s_synced = false;

//同步完成回调函数
static void ntp_sync_cb(struct timeval *tv)
{
    s_synced = true;
    ESP_LOGI(TAG, "时间同步完成");
}
//
esp_err_t ntp_sync_start(void)
{
    //设置时区并生效
    setenv("TZ", TZ_CHINA, 1);
    tzset();

    //重置s_synced
    s_synced = false;

    //设置工作模式POLL(定期询问服务器,自动保持同步)
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);

    //指定服务器
    esp_sntp_setservername(0, NTP_SERVER);

    //注册同步完成回调
    esp_sntp_set_time_sync_notification_cb(ntp_sync_cb);

    //启动，内部会创建socket异步去请求,等待时间同步
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP已启动,服务器:%s", NTP_SERVER);
    return ESP_OK;
}

bool ntp_sync_wait(uint32_t timeout_ms)
{
    //截至时间
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int retry = 0;

    while(!s_synced && xTaskGetTickCount() < deadline)
    {
        if(esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
        {
            s_synced = true;
            break;
        }
        ESP_LOGI(TAG, "等待同步中...(%d)", ++retry);
        vTaskDelay(pdMS_TO_TICKS(NTP_RETRY_MS));
    }
    return s_synced;
}

bool ntp_sync_is_synced(void)
{
    return s_synced;
}

void ntp_sync_print(void)
{
    time_t now = time(NULL);
    struct tm t;

    localtime_r(&now, &t);

    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);

    ESP_LOGI(TAG, "当前时间:%s(UTC+8),时间戳=%lld", buf, (long long)now);
}