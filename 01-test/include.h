#ifndef INCLUDE_H
#define INCLUDE_H

#include "stm32f4xx_hal.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

#define tx_pin 9
#define rx_pin 10

#define RX_BUF_SIZE     256


//帧定义
#define FRAME_HEAD1        0xAA
#define FRAME_HEAD2        0x55
#define FRAME_MAX_PAYLOAD  64

#define CMD_TEMP_HUM   0x01
#define CMD_HEARTBEAT  0x02
#define CMD_SERVO      0x10
#define CMD_LED        0x11
#define CMD_OLED_TEXT  0x20
//一帧完整结构
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[FRAME_MAX_PAYLOAD];
} uart_frame_t;
//发送一帧
void usart1_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len);



extern GPIO_TypeDef *led_port;
extern uint16_t led_pin;
extern GPIO_TypeDef *dht22_port;
extern uint16_t dht22_pin;

typedef struct {
    int16_t temperature_x10;         // 温度值x10，单位为摄氏度     
    uint16_t humidity_x10;           // 湿度值x10，单位为百分比
}dht22_samp_t;
extern dht22_samp_t samples[100];

//小灯初始化函数
void init_light(GPIO_TypeDef *port, uint16_t pin);
//点亮小灯函数
void light_on(void);
//熄灭小灯函数
void light_off(void);
//小灯翻转
void light_toggle(void);
//dht22初始化
void init_dht22(GPIO_TypeDef *port, uint16_t pin);
//获取dht22发来的温度和湿度
uint8_t get_tem_hum(void);
//SPI初始化
void spi1_init(void);
//spi发送一个字节并接收一个字节
uint8_t spi1_transfer(uint8_t tx);
//spi回环测试
int spi1_loopback_test(void);
//i2c1初始化
void i2c1_init(void);
//oled屏初始化
void oled_init(void);
//oled屏全部擦除
void oled_clear(void);
//oled屏显示字符
void oled_show_char(uint8_t x, uint8_t page, char ch);
//oled屏显示字符串
void oled_show_string(uint8_t x, uint8_t page, const char *str);
//舵机初始化
void init_servo(void);
//舵机调换角度
void servo_set_angle(uint16_t angle);

//看门狗初始化
void iwdg_init(void);
//喂狗
void iwdg_feed(void);


//记录解析丢包次数
extern uint16_t frame_drop_cnt;

//二值信号量,判断数据是否过来
extern SemaphoreHandle_t xDataSem;
extern TickType_t last_fulldma_tick;
//声明消息队列
extern QueueHandle_t xRxQueue;

//usart1初始化
void usart1_init(void);
//取出数据
uint8_t usart1_recv_byte(void);
//判断是否接收到数据
uint8_t usart1_rx_ready(void);
//发送数据
void usart1_send_byte(uint8_t c);
//中断单字节收集一帧
void uart_feed_byte(uint8_t b);

//dma2初始化
void uart1_dma_init(void);




void delay_ms(uint32_t ms);
void time_init(void);    //DWT时钟初始化
uint32_t micros(void);    //微秒检测
void delay_us(uint32_t us);



#endif