#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* 引入芯片头文件，SystemCoreClock 从这来 */
#include "stm32f4xx.h"

/*-----------------------------------------------------------
 * 基础配置
 *-----------------------------------------------------------*/
#define configUSE_PREEMPTION                     1   /* 抢占式调度 */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       ( SystemCoreClock )  /* 16MHz，自动适配 */
#define configTICK_RATE_HZ                       ( 1000 )             /* 心跳 1ms */
#define configMAX_PRIORITIES                     ( 5 )
#define configMINIMAL_STACK_SIZE                 ( 128 )              /* 单位：字(4字节) */
#define configMAX_TASK_NAME_LEN                  ( 16 )
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_EVENT_GROUPS                   1          //事件组

/*-----------------------------------------------------------
 * 内存管理（heap_4.c）
 *-----------------------------------------------------------*/
#define configSUPPORT_DYNAMIC_ALLOCATION         1          //动态创建
#define configSUPPORT_STATIC_ALLOCATION          0
#define configTOTAL_HEAP_SIZE                    ( 16 * 1024 )        /* 16KB 堆 */

/*-----------------------------------------------------------
 * 钩子函数
 *-----------------------------------------------------------*/
#define configUSE_IDLE_HOOK                      1   /* 空闲任务钩子，以后喂看门狗 */
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             1
#define configCHECK_FOR_STACK_OVERFLOW           2

/*-----------------------------------------------------------
 * 内核对象
 *-----------------------------------------------------------*/
#define configUSE_MUTEXES                        1              //互斥锁
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1              //计数信号量
#define configUSE_TASK_NOTIFICATIONS             1              //任务通知
#define configUSE_QUEUE_SETS                     0
#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          ( 0 )
#define configQUEUE_REGISTRY_SIZE                ( 8 )

/*-----------------------------------------------------------
 * 软件定时器
 *-----------------------------------------------------------*/
#define configUSE_TIMERS                         1              //软件定时器
#define configTIMER_TASK_PRIORITY                ( 2 )
#define configTIMER_QUEUE_LENGTH                 ( 10 )
#define configTIMER_TASK_STACK_DEPTH             ( 128 )

/*-----------------------------------------------------------
 * 中断优先级（Cortex-M4 优先级寄存器只有高 4 位有效）
 *-----------------------------------------------------------*/
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS                      __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS                      4
#endif
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY           15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY      5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/*-----------------------------------------------------------
 * 让 port.c 里的中断函数对上启动文件的向量表名字
 *-----------------------------------------------------------*/
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/*-----------------------------------------------------------
 * 需要用到的 API（以后用到再加）
 *-----------------------------------------------------------*/
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskDelayUntil                  1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_eTaskGetState                    1
#define INCLUDE_xTaskGetIdleTaskHandle           1
#define INCLUDE_pcTaskGetTaskName                1
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1

/*-----------------------------------------------------------
 * 断言：出问题时死循环停在这里，方便调试
 *-----------------------------------------------------------*/
#define configASSERT( x ) if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ); }

#endif /* FREERTOS_CONFIG_H */