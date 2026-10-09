#include "uart_link.h"

#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#define UART_PORT UART_NUM_1    //与stm32f411通信端口为UART1
#define TX_GPIO 17    //tx引脚
#define RX_GPIO 18    //rx引脚
#define BAUD_RATE 115200    //比特率
#define RX_BUF_SIZE 1024    //rx缓冲区大小
#define TX_BUF_SIZE 1024    //tx缓冲区大小

#define LINK_TIMEOUT_MS 3000    //3s没消息算超时

static const char *TAG = "uart_link";

/* 协议解析器实例 */
static frame_parser_t s_parser;

/* 收到完整帧的回调（上层注册） */
static uart_link_cb_t s_cb = NULL;

/* 链路监控 */
static int64_t s_last_rx_ms = 0;
static bool    s_link_lost  = false;

/* 读超时时间 = 判定"总线空闲/帧间隔"的阈值（毫秒）
 * 大于一个字节的传输时间即可；115200 下 1 字节 ≈ 0.09ms，取 20ms  */
#define FRAME_GAP_MS  20

//接收任务
static void uart_rx_task(void *arg)
{
    uint8_t buf[128];
    frame_t f;

    for(;;)
    {
        //获取收到的字节数n,从UART_PORT串口获取,获取的数据放入buf,大小为buf大小,50ms没数据就走
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(FRAME_GAP_MS));
        if(n>0)     //有数据时
        {
            s_last_rx_ms = esp_timer_get_time() / 1000;
            for(int i = 0;i < n; i++)
            {
                if (frame_parser_feed(&s_parser, buf[i], &f) == FRAME_OK)
                {
                    if (s_cb) s_cb(f.cmd, f.payload, f.len);
                }
            }
        }
        else
        {
            /* ★ 读超时 = 一段时间没数据 = 总线空闲
             *   等价于 STM32 侧的 USART IDLE 中断：丢弃被截断的半帧，
             *   避免下一帧被错位吞掉 */
            frame_parser_resync(&s_parser);
        }

        //链路断线检测
        int64_t now = esp_timer_get_time() / 1000;
        bool lost = (s_last_rx_ms > 0) && ((now - s_last_rx_ms) >LINK_TIMEOUT_MS);
        if(lost != s_link_lost)
        {
            s_link_lost = lost;
            if(lost)
            {
                ESP_LOGW(TAG, "与主机断开连接:未收到数据%d毫秒", LINK_TIMEOUT_MS);
            }
            else{
                ESP_LOGW(TAG, "与主机连接");
            }
        }
    }

}

//UART硬件初始化
void uart_link_init(void)
{
    frame_parser_init(&s_parser);
    
    uart_config_t cfg =
    {
        .baud_rate = BAUD_RATE,                     //115200
        .data_bits = UART_DATA_8_BITS,              //8位数据
        .parity = UART_PARITY_DISABLE,              //无校验位
        .stop_bits = UART_STOP_BITS_1,              //1位停止位
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,      //关闭硬件流控
        .source_clk = UART_SCLK_DEFAULT,            //默认时钟源
    };

    //安装UART驱动
    //UART_PORT为串口,接收缓冲区为RX_BUF*2,接收缓冲区为TX_BUF*2,事件队列大小为20,不获取事件队列句柄,默认内存分配
    uart_driver_install(UART_PORT, RX_BUF_SIZE * 2, TX_BUF_SIZE * 2, 20, NULL, 0);
    //将硬件参数配置好
    uart_param_config(UART_PORT, &cfg);
    //引脚映射,不分配RTS(请求发送)和CTS(清除发送)引脚,因为流控关闭了
    uart_set_pin(UART_PORT, TX_GPIO, RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "初始化:TX=GPIO%d RX=GPIO%d baud=%d", TX_GPIO, RX_GPIO, BAUD_RATE);
}

//分配接收任务优先级
void uart_link_start(void)
{
    xTaskCreatePinnedToCore(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL, 0);
}

//发送帧组装
void uart_link_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    //组帧逻辑复用三端共用的 frame_build()，不再手工拼字节
    uint8_t frame[FRAME_MAX_PAYLOAD + 5];
    int n = frame_build(cmd, payload, len, frame, sizeof(frame));

    if (n < 0) return;      //参数非法（负载超长或缓冲不足）

    //把组装好的帧写入发送缓冲区
    uart_write_bytes(UART_PORT, frame, n);
    ESP_LOGD(TAG, "TX:cmd=0x%02x len=%d", cmd, len);

}

void uart_link_set_callback(uart_link_cb_t cb)
{
    s_cb = cb;
}