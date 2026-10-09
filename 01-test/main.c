#include <stdio.h>
#include "include.h"
#include "ringbuf.h"
#include "FreeRTOS.h"    /*钩子需要*/
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
#include "timers.h"

/* 协议解析器实例（状态从 include.c 的 static 变量搬到这里，
 * 由解析任务独占访问，无竞态） */
static frame_parser_t s_parser;

//同步对象
QueueHandle_t       xRxQueue;                   //存储消息队列的句柄
SemaphoreHandle_t   xOledMutex;                 //oled外设互斥锁
SemaphoreHandle_t   xUartTxMutex;               //uart发送互斥锁，防串帧
SemaphoreHandle_t   xDataSem;                   //dma搬运计数信号量
EventGroupHandle_t  xSysEvents;                 //事件组
TimerHandle_t       xHeartbeatTimer;            //心跳定时器

//事件位定义
#define EVT_TEMP_READY  (1 << 0)        //温度已更新
#define EVT_NEW_FRAME   (1 << 1)        //收到新帧

//任务句柄：任务通知的目标
static TaskHandle_t xHeartbeatTaskHandle = NULL;
static TaskHandle_t xUartTaskHandle = NULL;

//共享计数
static volatile uint32_t s_frame_cnt = 0;


//任务1:协议解析
static void vTaskParse(void *pvParameters)
{
    (void)pvParameters;
    uint8_t b;
    frame_t f;

    for (;;)
    {
        /* 阻塞等"DMA 搬了一批数据"的计数信号量 */
        if (xSemaphoreTake(xDataSem, portMAX_DELAY) == pdTRUE)
        {
            /* 把 ring buffer 里积压的字节全部消费完 */
            while (ringbuf_pop(&g_rb, &b))
            {
                if (frame_parser_feed(&s_parser, b, &f) == FRAME_OK)
                {
                    xQueueSend(xRxQueue, &f, 0);
                }
            }

            /* 期间总线空闲过 → 丢弃可能残留的半帧
             * （对应 STM32 的 IDLE 中断 / ESP32 的读超时） */
            if (uart_take_idle_flag())
            {
                frame_parser_resync(&s_parser);
            }
        }
    }
}
//任务2:业务处理
static void vTaskUart(void *pvParameters)
{
    (void)pvParameters;
    frame_t f;
    
    //保存句柄，供心跳定时器做任务通知
    xUartTaskHandle = xTaskGetCurrentTaskHandle();

    for(;;)
    {
        //无限期等待帧
        if (xQueueReceive(xRxQueue, &f, portMAX_DELAY) == pdPASS)
        {
            switch (f.cmd)
            {
            case CMD_SERVO:
                if (f.len == 1)
                {
                    servo_set_angle(f.payload[0]);
                }
                break;
            case CMD_LED:
                if (f.len == 1)
                {
                    (f.payload[0] ? light_on() : light_off());
                }
                break;
            case CMD_OLED_TEXT:
                //留一个字符给结束符
                if (f.len > 0 && f.len < FRAME_MAX_PAYLOAD)
                {
                    f.payload[f.len] = '\0';    /* 补结束符 */
                    xSemaphoreTake(xOledMutex, portMAX_DELAY);
                    oled_show_string(0, 2, (const char *)f.payload);
                    xSemaphoreGive(xOledMutex);
                }
                break;
            default:
                break;
            }

            //关总中断，进入临界区操作帧数据
            taskENTER_CRITICAL();
            s_frame_cnt++;
            taskEXIT_CRITICAL();
        }

    }
}

//传感器采集
static void vTaskSensor(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last = xTaskGetTickCount();
    char buf[16];

    for(;;)
    {
        //执行这一段代码，任务会主动放弃cpu使用权并从运行态切换为阻塞态，时间到后唤醒为就绪态
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000));

        get_tem_hum();
        int16_t t = samples[0].temperature_x10;
        uint16_t h = samples[0].humidity_x10;
        uint8_t th[4] = 
        {
            (uint8_t)(t >> 8), (uint8_t)t,
            (uint8_t)(h >> 8), (uint8_t)h
        };

        //串口发送温度和湿度
        xSemaphoreTake(xUartTxMutex, portMAX_DELAY);
        usart1_send_frame(CMD_TEMP_HUM, th, 4);
        xSemaphoreGive(xUartTxMutex);

        //OLED屏显示温湿度（便于直接观察采集与上报是否正常）
        xSemaphoreTake(xOledMutex, portMAX_DELAY);
        sprintf(buf, "T:%d.%d", t / 10, (t < 0 ? -t : t) % 10);
        oled_show_string(0, 4, buf);
        sprintf(buf, "H:%d.%d", h / 10, h % 10);
        oled_show_string(0, 6, buf);
        xSemaphoreGive(xOledMutex);

        //事件组更新，告诉显示任务：温湿度更新了
        xEventGroupSetBits(xSysEvents, EVT_TEMP_READY);

        //内存监控
        printf("heap free:%lu\r\n", (unsigned long)xPortGetFreeHeapSize());
    }
}
//任务4:显示
static void vTaskShow(void *pvParameters)
{
    (void)pvParameters;
    char buf[16];
    uint32_t n = 0;

    for(;;)
    {
        //事件响应
        xEventGroupWaitBits(xSysEvents,             
                            EVT_TEMP_READY | EVT_NEW_FRAME, //关注这2个事件,没触发则挂起任务
                            pdTRUE,                         //进入后先清除2者         
                            pdFALSE,                        //2个事件为或
                            portMAX_DELAY
                            );
                        
        //记录次数
        xSemaphoreTake(xOledMutex, portMAX_DELAY);
        sprintf(buf, "N:%lu", (unsigned long)(n++ % 1000));
        oled_show_string(0, 0, buf);
        xSemaphoreGive(xOledMutex);
    }
}
//任务5:心跳通知
static void vTaskHeartbeat(void *pvParameters)
{
    (void)pvParameters;
    xHeartbeatTaskHandle = xTaskGetCurrentTaskHandle();

    for(;;)
    {
        //若有通知，则消耗所有通知次数，没有则挂起
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        //小灯翻转
        GPIOC->ODR ^= GPIO_PIN_13;
    }
}
//软件定时器回调：1s心跳
static void heartbeat_cb(TimerHandle_t xTimer)
{
    (void)xTimer;

    //发送心跳通知
    xTaskNotifyGive(xHeartbeatTaskHandle);
}
//空闲钩子，喂狗
void vApplicationIdleHook(void)
{
    iwdg_feed();
}

int main(void)
{
    init_light(led_port, led_pin);
    light_on();
    time_init();
    oled_init();
    oled_clear();
    usart1_init();
    init_dht22(dht22_port, dht22_pin);



    //创建消息队列
    xRxQueue = xQueueCreate(4, sizeof(frame_t));
    //oled屏幕外设互斥锁
    xOledMutex = xSemaphoreCreateMutex();
    //uarttx互斥锁
    xUartTxMutex = xSemaphoreCreateMutex();
    //计数信号量，上限10，初始0
    xDataSem = xSemaphoreCreateCounting(10, 0);
    //事件组
    xSysEvents = xEventGroupCreate();
    //软件计时器
    xHeartbeatTimer = xTimerCreate("hbeat",                 //定时器名称
                                    pdMS_TO_TICKS(1000),    //周期为1s
                                    pdTRUE,                 //设置为自动重载模式
                                    NULL,                   //定时器id
                                    heartbeat_cb            //回调函数指针
                                    );

    
    //若内存不够，则会直接赋值NULL;进入空循环方便发现
    if (xRxQueue == NULL || xOledMutex == NULL || xDataSem == NULL ||
        xSysEvents == NULL || xHeartbeatTimer == NULL) { for (;;); }

    /*
    //中断触发顺序：NVIC中断的37号(USART1_IRQn)触发后去中断向量表找37号，
    //中断向量表37号为函数USART1_IRQHandler的入口
    USART1->CR1 |= USART_CR1_RXNEIE;    //触发器使能，监听RXNE标志位来触发中断
    NVIC_SetPriority(USART1_IRQn, 5);    //USART1_IRQn为USART1中断标识符
    NVIC_EnableIRQ(USART1_IRQn);    //使能UART1中断号,与RXNEIE同时开启才能完整传输中断信号
    */

    //定时器是独立任务，启动后还要手动启动
    xTimerStart(xHeartbeatTimer, 0);

    //dma2中断初始化
    uart1_dma_init();
    /* 初始化协议解析器 */
    frame_parser_init(&s_parser);

    //创建任务并分配优先级
    //参数：任务函数，任务昵称(自定)，堆栈大小(单位为字:4个字节)，传参，优先级(越大优先级越高)，任务句柄
    xTaskCreate(vTaskParse,"parse",256,NULL,3,NULL);
    xTaskCreate(vTaskUart,"UART",256,NULL,2,NULL);
    xTaskCreate(vTaskSensor,"SENSOR",256,NULL,1,NULL);
    xTaskCreate(vTaskShow,"SHOW",256,NULL,1,NULL);
    xTaskCreate(vTaskHeartbeat,"HEART",256,NULL,1,NULL);

    //开启FreeROTS调度
    vTaskStartScheduler();

    //一般无法到这里
    for(;;);

 
}

//freertos钩子
void vApplicationMallocFailedHook(void) { for (;;); }
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask; (void)pcTaskName;
    for (;;);
}