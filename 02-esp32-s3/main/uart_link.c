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

//帧解析状态机，用来记录接收程序读取到哪一帧
typedef enum{
    S_HEAD1,    //正在等待帧的第1个头字节 
    S_HEAD2,    //正在等待帧的第2个头字节
    S_LEN,      //正在读取数据长度
    S_CMD,      //正在读取命令码
    S_DATA,     //正在读取数据负载
    S_SUM       //正在读取校验和
}rx_state_t;

//每一帧的状态
static rx_state_t s_state = S_HEAD1;            //告诉程序收到的下一个字节应该做什么(帧头，长度，命令，数据)
static uint8_t s_payload[FRAME_MAX_PAYLOAD];    //数据负载缓冲区
//s_len:存放刚刚解析出的数据负载长度;
//s_cmd:存放刚刚解析出的命令码;
//s_sum:运行中的异或校验和;
//s_idx:负载数组的写入索引
static uint8_t s_len, s_cmd, s_sum, s_idx;


static uart_link_cb_t s_cb = NULL;
static const char *TAG = "uart_link";

//链路状态监控
static int64_t s_last_rx_ms = 0;        //上次收到数据的打卡时间
static bool s_link_lost = false;        //当前链路的通断状态

//一个字节一个字节的收取并判断，满足一帧要求则返回true
static bool uart_feed_byte(uint8_t b)
{
    switch(s_state)
    {
        case S_HEAD1:       //判断是否第一个头
            if(b == FRAME_HEAD1)
            {
                s_state = S_HEAD2;      //进入第二头
            }
            break;
        
        case S_HEAD2:       //等待第二个头
            s_state = (b == FRAME_HEAD2) ? S_LEN : S_HEAD1;     //若为第二个头则进入读数据长度，不是则舍弃第一个头
            break;

        case S_LEN:         //等待数据长度
            s_len = b;
            //若发送的数据长度超过了负载则直接拒收
            if(s_len > FRAME_MAX_PAYLOAD)
            {
                s_state = S_HEAD1;
                break;
            }
            s_sum = b;      //检验和的第一个异或为数据长度
            s_idx = 0;
            s_state = S_CMD;
            break;

        case S_CMD:
            s_cmd = b;
            s_sum ^= b;     //第二给校验位命令位异或进去
            s_state = (s_len == 0) ? S_SUM : S_DATA;    //如果数据长度为0，则跳过数据区，进入检验位
            break;

        case S_DATA:
            s_payload[s_idx++] = b;     //把数据存入负载缓冲区
            s_sum ^= b;                 //异或数据位
            if(s_idx >= s_len)          //数据收集满了，则进入校验位
            {
                s_state = S_SUM;
            }
            break;

        case S_SUM:
            s_state = S_HEAD1;      //准备下一帧
            if(b == s_sum)          //比较校验和
            {
                if(s_cb)        //确定存在操作函数
                {
                    //对数据进行处理
                    s_cb(s_cmd, s_payload, s_len);
                }
                return true;
            }
            ESP_LOGW(TAG, "和校验错误:得到:0x%02x,想要:0x%02x", b, s_sum);
            break;
    }
    return false;
}

//接收任务
static void uart_rx_task(void *arg)
{
    uint8_t buf[128];

    for(;;)
    {
        //获取收到的字节数n,从UART_PORT串口获取,获取的数据放入buf,大小为buf大小,50ms没数据就走
        int n = uart_read_bytes(UART_PORT, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if(n>0)     //有数据时
        {
            s_last_rx_ms = esp_timer_get_time() / 1000;
            for(int i = 0;i < n; i++)
            {
                //由于与发送方定义的是：数据是一个字节一个字节的发，所以可以一个字节一个字节的校验
                uart_feed_byte(buf[i]);
            }
        }

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
    //帧头2个,LEN,CMD,SUM
    uint8_t frame[FRAME_MAX_PAYLOAD + 5];
    uint8_t sum = len ^ cmd;
    uint8_t i = 0;

    //截断保护
    if(len > FRAME_MAX_PAYLOAD)
    {
        len = FRAME_MAX_PAYLOAD;
    }

    frame[i++] = FRAME_HEAD1;
    frame[i++] = FRAME_HEAD2;
    frame[i++] = len;
    frame[i++] = cmd;

    for(uint8_t j = 0; j < len; j++)
    {
        frame[i] = payload[j];
        sum ^= payload[j];
        i++;
    }
    frame[i++] = sum;

    //把组装好的帧frame写入发送缓冲区
    uart_write_bytes(UART_PORT, frame, i);
    ESP_LOGD(TAG, "TX:cmd=0x%02x len=%d", cmd, len);

}

void uart_link_set_callback(uart_link_cb_t cb)
{
    s_cb = cb;
}