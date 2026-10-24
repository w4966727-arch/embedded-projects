#include "mqtt_app.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"
#include "uart_link.h"
#include "ws2812.h"
#include "esp_log.h"

#define MQTT_CONNECTED_BIT BIT0

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client = NULL;
static EventGroupHandle_t s_mqtt_events = NULL;
static volatile bool s_connected = false;

/* 发布本地 WS2812 状态（QoS0 + retain：新订阅者立即可见） */
static void publish_led_state(uint8_t r, uint8_t g, uint8_t b)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d,%d,%d", r, g, b);
    mqtt_app_publish("esp32/led", buf, 0, 1);
}

/**
 * 收到命令：按 topic 区分目标设备
 *
 *   esp32/cmd  → 本地 WS2812（color R G B / color off）
 *   stm32/cmd  → 转发给 STM32（led on / led off），走 UART 帧协议
 */
static void mqtt_handle_command(const char *topic, int topic_len,
                                const char *data, int data_len)
{
    /* ① MQTT payload 不是以 '\0' 结尾的，先拷贝成 C 字符串 */
    char cmd[64];
    int n = (data_len < (int)sizeof(cmd) - 1) ? data_len : (int)sizeof(cmd) - 1;
    memcpy(cmd, data, n);
    cmd[n] = '\0';

    ESP_LOGI(TAG, "命令 topic=%.*s cmd=\"%s\"", topic_len, topic, cmd);

    int r, g, b;

    /* ② 按主题分派 */
    if (topic_len == 9 && strncmp(topic, "esp32/cmd", 9) == 0)
    {
        /* ── 本地 WS2812 ── */
        if (strcmp(cmd, "color off") == 0)
        {
            ws2812_off();
            publish_led_state(0, 0, 0);
        }
        else if (sscanf(cmd, "color %d %d %d", &r, &g, &b) == 3)
        {
            /* 参数钳位，防止越界值 */
            r = (r < 0) ? 0 : (r > 255 ? 255 : r);
            g = (g < 0) ? 0 : (g > 255 ? 255 : g);
            b = (b < 0) ? 0 : (b > 255 ? 255 : b);
            ws2812_set_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
            publish_led_state((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        else
        {
            ESP_LOGW(TAG, "未知本地命令: %s", cmd);
        }
    }
    else if (topic_len == 9 && strncmp(topic, "stm32/cmd", 9) == 0)
    {
        /* ── 转发给 STM32（走 UART 帧协议，CMD_LED 0x11）── */
        uint8_t v;
        if (strcmp(cmd, "led on") == 0)
        {
            v = 1;
            uart_link_send_frame(CMD_LED, &v, 1);
            ESP_LOGI(TAG, "已转发: LED ON → STM32");
        }
        else if (strcmp(cmd, "led off") == 0)
        {
            v = 0;
            uart_link_send_frame(CMD_LED, &v, 1);
            ESP_LOGI(TAG, "已转发: LED OFF → STM32");
        }
        else
        {
            ESP_LOGW(TAG, "未知 STM32 命令: %s", cmd);
        }
    }
}
//时间处理
static void mqtt_event_handler(void *args, esp_event_base_t base,
                                int32_t id, void *event_data)
{
    //将数据转成mqtt结构体
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch((esp_mqtt_event_id_t)id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "已连接broker");
            s_connected = true;

            //上线通告
            //向主题为"esp32/status"发送具体消息为"online"
            //0所在参数为len,调用0代表自动用strlen(data)
            //qos为1,表示broker收到publish后会向esp32发送ack,esp32没收到会重发
            //retain为1表示broker会保存这条消息，之后任何新增的订阅者都会收到这条消息
            esp_mqtt_client_publish(s_client, "esp32/status", "online", 0, 1, 1);

            //向broker订阅"esp32/cmd"这个主题,之后有设备往这个主题发消息都会发送过来
            //qos等级为1
            esp_mqtt_client_subscribe(s_client, "esp32/cmd", 1);
            esp_mqtt_client_subscribe(s_client, "stm32/cmd", 1);

            //广播
            xEventGroupSetBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "与broker断开连接");
            s_connected = false;
            xEventGroupClearBits(s_mqtt_events, MQTT_CONNECTED_BIT);
            break;
        case MQTT_EVENT_DATA:
            mqtt_handle_command(event->topic, event->topic_len,
                                event->data, event->data_len);
            break;
        case MQTT_EVENT_ERROR:
            if(event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
            {
                ESP_LOGE(TAG, "传输层错误:sock_errno=%d",
                        event->error_handle->esp_transport_sock_errno);
            }
            else
            {
                ESP_LOGI(TAG, "MQTT 错误类型=%d", event->error_handle->error_type);
            }
            break;
        default:
            break;
    }
}
//启动
esp_err_t mqtt_app_start(void)
{
    if(s_mqtt_events == NULL)
    {
        s_mqtt_events = xEventGroupCreate();
    }
    if(s_mqtt_events == NULL)
        return ESP_ERR_NO_MEM;
    
    esp_mqtt_client_config_t cfg = 
    {
        .broker.address.uri = CONFIG_MQTT_BROKER_URI,
        .credentials.client_id = CONFIG_MQTT_CLIENT_ID,
        .session.keepalive = 30,
        .session.last_will =
            {
                .topic = "esp32/status",
                .msg = "offline",
                .msg_len = 7,
                .qos = 1,
                .retain = 1,
            },
        .network.reconnect_timeout_ms = 3000,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if(s_client == NULL)
    {
        ESP_LOGE(TAG, "客户端初始化失败");
        vEventGroupDelete(s_mqtt_events);       //释放之前创建好的事件组，防止内存泄漏
        s_mqtt_events = NULL;                   //指针悬空
        return ESP_FAIL;
    }

    //注册回调函数
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT启动,broker=%s", CONFIG_MQTT_BROKER_URI);
    return ret;
}
//对外接口
bool mqtt_app_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits( s_mqtt_events,
                                            MQTT_CONNECTED_BIT,
                                            pdFALSE,
                                            pdFALSE,
                                            pdMS_TO_TICKS(timeout_ms));
    return (bits & MQTT_CONNECTED_BIT) != 0;
}
//随时查询mqtt连接状态
bool mqtt_app_is_connected(void)
{
    return s_connected;
}

//封装push函数
int mqtt_app_publish(const char *topic, const char *data, int qos, int retain)
{
    //没连上直接失败
    if(!s_connected || s_client == NULL)
    {
        return -1;
    }
    return esp_mqtt_client_publish(s_client, topic, data, 0, qos, retain);
}