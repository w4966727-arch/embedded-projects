#include "include.h"
#include "ringbuf.h"
#include <string.h>
#include <stdio.h>
#include "task.h"
#include "queue.h"
#include "semphr.h"


GPIO_TypeDef *led_port = GPIOC;       //小灯在c寄存器
uint16_t led_pin = GPIO_PIN_13;       //小灯在c寄存器的13号引脚

GPIO_TypeDef *dht22_port = GPIOA;     //DHT22在A寄存器
uint16_t dht22_pin = GPIO_PIN_1;      //DHT22在A寄存器的1号引脚
dht22_samp_t samples[100] = {0};      // 存储采样数据的数组
uint8_t dht22_data[5] = {0};     // [0][1]=湿度高/低字节  [2][3]=温度高/低字节  [4]=校验和

GPIO_TypeDef *oled_port = GPIOB;    //oled屏1在B端口
uint16_t oled_scl_pin = GPIO_PIN_6;   //oled屏1时钟引脚在PB6
uint16_t oled_sda_pin = GPIO_PIN_7;   //oled屏1数据引脚在PB7
//SSD1306在i2c上的屏幕地址为固定7位加1位读写，固定七位为：011110+SA0电平(0/1，默认为0)
//前7位地址0111100(0x3c)左移1位+写标志位(0) = 0x78,读地址为0x79
uint8_t oled_addr = 0x78;

//uart帧状态机定义
typedef enum 
{
    S_HEAD1,
    S_HEAD2,
    S_LEN,
    S_CMD,
    S_DATA,
    S_SUM
}rx_state_t;
static rx_state_t s_state = S_HEAD1;
//帧解析缓冲区
static uint8_t s_payload[FRAME_MAX_PAYLOAD];
static uint8_t s_len, s_cmd, s_sum, s_idx;

//记录解析丢包次数
uint16_t frame_drop_cnt = 0;

//uart1的dma搬运目标
static volatile uint8_t rx_buf[RX_BUF_SIZE];
//水位线：cpu已经处理到的地方
static volatile uint16_t rx_watermark = 0;

//全局环形缓冲区
ringbuf_t g_rb;

//记录空闲中断的时刻
TickType_t last_fulldma_tick = 0;




//uart初始化
void usart1_init(void)
{
    //把GPIOA时钟打开,驱动相关寄存器并将数字信号转换为物理电平
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    //开启UART1时钟,设置波特率，并自动执行串并转换，移位，启停位插入和采样
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
    //设置位复位模式
    GPIOA->MODER &= ~(0x3u << (tx_pin)*2);
    GPIOA->MODER &= ~(0x3u << (rx_pin)*2);
    GPIOA->MODER |= (0x2u << (tx_pin)*2);
    GPIOA->MODER |= (0x2u << (rx_pin)*2);
    //AFRH控制高8位引脚(PA8~PA15),每个引脚4位控制
    GPIOA->AFR[1] &= ~(0xFu << (tx_pin - 8)*4);
    GPIOA->AFR[1] &= ~(0xFu << (rx_pin - 8)*4);
    //将AF设置为0x7u(0111)将启用usart1通道
    GPIOA->AFR[1] |= (0x7u << (tx_pin - 8)*4);
    GPIOA->AFR[1] |= (0x7u << (rx_pin - 8)*4);
    //波特率为115200=16MB/(16*(008+11/16)),其中0x8B高12位为008,低4位为B(11)
    USART1->BRR = 0x8B;
    //使能,UE为总使能，TE为发送使能，RE为接收使能。UE必须先使能才能使能TE和RE
    USART1->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}
//uart1的dma初始化
void uart1_dma_init(void)
{
    //uart1的rx分配在dma2上，开启dma2的时钟
    RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    //禁流后才能修改配置
    DMA2_Stream2->CR = 0;
    //等待CR寄存器的流使能关闭
    while(DMA2_Stream2->CR & DMA_SxCR_EN);
    //开启相应寄存器
    //CIRC:循环模式，开启后DMA传输完一轮数据会重新开始
    //  dma有一个递减计数器NDTR，记录剩余传输的数据量(一般设置为接收缓冲区的大小)，当NDTR减为0时，硬件会自动重载达到无限循环
    //MINC:内存地址自增，每次传输完成，DMA的内存地址会自动移到下一位置；rx_buf[i]->rx_buf[i+1]
    //HTIE:半传输中断使能，当DMA传输完预设数据数量的一半时会触发一次中断
    //TCIE:传输完成中断使能，当DMA传输完全部数据后会触发一次中断
    //  开启了uart的dma中断后，dma会自动响应RXNE并通过硬件电路直接搬运到rx_buf中，填满一半或全部后dma中断响应
    //  2个中断都是走向dma2中断服务函数，需要判断标志位
    DMA2_Stream2->CR |= DMA_SxCR_CIRC | DMA_SxCR_MINC | DMA_SxCR_HTIE | DMA_SxCR_TCIE;
    //DMA_SxCR_CHSEL_Pos值一般为25，设置通道时一般选择chsel[2:0]即25,26,27这三位，100(4)为uart的rx通道
    DMA2_Stream2->CR |= (4u << DMA_SxCR_CHSEL_Pos);
    //设置源地址和目标地址
    //DR有数据时，即RXNE为1时，向dma控制器发送一个通道4的请求信号
    //数据流2收到这个信号后根据配置好源地址PAR和目标地址M0AR.
    //  数据从外设总线APB出发，经过总线矩阵AHB，被DMA2的数据流2读取，
    //  通过AHB总线写入SRAM的目标缓冲区rx_buf，开启MINC后，目标地址自动加1
    DMA2_Stream2->PAR = (uint32_t)&USART1->DR;
    DMA2_Stream2->M0AR = (uint32_t)rx_buf;
    //配置NDTR递减计数器
    DMA2_Stream2->NDTR = RX_BUF_SIZE;
    //使能dma流
    DMA2_Stream2->CR |= DMA_SxCR_EN;
    //开启dma控制器，这样dma就可以检测RXNE并启动中断
    USART1->CR3 |= USART_CR3_DMAR;
    //将RXNE的interrupt enable位关闭，这样RXNE就不会通知usart1中断，不会和dma抢数据
    USART1->CR1 &= ~USART_CR1_RXNEIE;
    //开启CR1寄存器中的IDLE interrupt enable位，即总线空闲中断，当rx线上超过1个字节的时间没有数据
    //则认为这一帧数据发完了，触发IDLE中断，cpu接入去计算搬了多少数据然后进行处理
    USART1->CR1 |= USART_CR1_IDLEIE;
    //告诉cpu响应dma2_stream2中断函数
    NVIC_EnableIRQ(DMA2_Stream2_IRQn);
    NVIC_SetPriority(DMA2_Stream2_IRQn, 5);
    //响应IDLE空闲中断函数，该中断在USART1上
    NVIC_EnableIRQ(USART1_IRQn);
    NVIC_SetPriority(USART1_IRQn, 5);

}
//发送
void usart1_send_byte(uint8_t c)
{
    //等待发送寄存器空的标志位TXE1，代表数据寄存器DR中的数据被拿走了，现在为空
    while(!(USART1->SR & USART_SR_TXE));
    //只要UE,TE使能，向DR写入数据就会发送
    USART1->DR = c;
    //当数据丢入DR后：
    //第1步（搬运）：硬件逻辑自动将DR寄存器里的并行数据，瞬间搬运到内部的 “发送移位寄存器”（TDR） 中。
    //搬运完成后，硬件会把TXE标志位重新置1（表示DR空了，你可以发下一字节了），这就是为什么while循环会通过。

    //第2步（组帧）：移位寄存器拿到数据（如 01000001）后，硬件控制逻辑自动在它前面加上起始位（0）
    //在后面加上停止位（1）（以及可能的校验位），组装成一串完整的帧。

    //第3步（并转串 - 核心动作）：波特率发生器（就是BRR寄存器控制的那个定时器）产生精准的移位时钟脉冲。
    //每来一个脉冲，移位寄存器就物理性地向右“挪”一位，把最低位（LSB）先推到TX引脚上。

    //第4步（电气输出）：TX引脚上的电平（0V或3.3V）随着移位寄存器的节奏高低变化，这就是串行信号。
}
//看接收标志位RXNE(rx not empty),如果为1则代表数据传过来了，放在了DR中
uint8_t usart1_rx_ready(void)
{
    return (USART1->SR & USART_SR_RXNE) ? 1:0;
}
//取出接收数据寄存器DR中的值
uint8_t usart1_recv_byte(void)
{
    while(!(USART1->SR & USART_SR_RXNE));
    return USART1->DR;
}
//中断单字节收集一帧
void uart_feed_byte(uint8_t b)
{
    switch (s_state)
    {
    case S_HEAD1:
        if (b == FRAME_HEAD1)
        {
            s_state = S_HEAD2;
        }
        break;
    case S_HEAD2:
        s_state = (b == FRAME_HEAD2) ? S_LEN : S_HEAD1;
        break;
    case S_LEN:
        s_len = b;
        if (s_len > FRAME_MAX_PAYLOAD)
        { 
            s_state = S_HEAD1;
            break; 
        }
        s_sum = b;
        s_idx = 0;
        s_state = S_CMD;
        break;
    case S_CMD:
        s_cmd = b;
        s_sum ^= b;
        s_state = (s_len == 0) ? S_SUM : S_DATA;
        break;
    case S_DATA:
        s_payload[s_idx++] = b;
        s_sum ^= b;
        if (s_idx >= s_len)
        {
            s_state = S_SUM;
        }
        break;
    case S_SUM:
        s_state = S_HEAD1;
        if (b == s_sum)
        {
            //校验通过
            uart_frame_t f;                 //声明临时栈堆
            f.cmd = s_cmd;                  //把命令拷贝进临时堆
            f.len = s_len;                  //数据长度拷贝进临时堆
            memcpy(f.payload, s_payload, s_len);    //深度拷贝，确保操作的数据是想操作的
            //把数据放入消息队列
            if(xQueueSend(xRxQueue, &f, 0) != pdPASS)
            {
                //记录丢包次数
                frame_drop_cnt++;
            }
        }
        break;
    }
}
//dma把数据放入rx_buf缓冲区中，把它取出放入g_rb中
static void rx_process(uint16_t start, uint16_t end)
{
    //数据批量处理
    ringbuf_push_bulk(&g_rb, &rx_buf[start], (uint16_t)(end - start));
}
//uart发送帧
void usart1_send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    uint8_t sum = len ^ cmd;

    usart1_send_byte(FRAME_HEAD1);
    usart1_send_byte(FRAME_HEAD2);
    usart1_send_byte(len);
    usart1_send_byte(cmd);
    for (uint8_t i = 0; i < len; i++)
    {
        usart1_send_byte(payload[i]);
        sum ^= payload[i];
    }
    usart1_send_byte(sum);
}
//dma2中断入口函数
void DMA2_Stream2_IRQHandler(void)
{
    //跳转到高优先级任务
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    //TCIF2为传输完成中断
    if(DMA2->LISR & DMA_LISR_TCIF2)
    {
        //清除TCIF2标志位
        DMA2->LIFCR |= DMA_LIFCR_CTCIF2;
        //处理后半区
        rx_process(rx_watermark, RX_BUF_SIZE);
        //下次写前半区
        rx_watermark = 0;

        //释放计数信号量
        xSemaphoreGiveFromISR(xDataSem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
    //HTIF2为半传输中断标志
    if(DMA2->LISR & DMA_LISR_HTIF2)
    {
        //清除HTIF2标志位
        DMA2->LIFCR |= DMA_LIFCR_CHTIF2;
        //处理前半区
        rx_process(rx_watermark, RX_BUF_SIZE / 2);
        //下次写后半区
        rx_watermark = RX_BUF_SIZE / 2;

        //释放计数信号量
        xSemaphoreGiveFromISR(xDataSem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }

}

//usart1中断函数入口,!!!函数名必须与中断向量表一致
void USART1_IRQHandler(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    //空闲中断
    if(USART1->SR & USART_SR_IDLE)
    {
        //清除IDLE标志位，先读SR,再读DR
        volatile uint32_t tmp = USART1->SR;
        (void)tmp;
        tmp = USART1->DR;
        (void)tmp;

        //数据没传满半个缓冲区就停了则在空闲中断里把这些数据搬了
        uint16_t pos = RX_BUF_SIZE - DMA2_Stream2->NDTR;
        if(pos > rx_watermark)
        {
            //搬数据
            rx_process(rx_watermark, pos);
            rx_watermark = pos;
            //数据传完了直接释放信号量
            xSemaphoreGiveFromISR(xDataSem, &xHigherPriorityTaskWoken);
        }
    
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

//SPI初始化
void spi1_init(void)
{
    //开启时钟，spi挂载在APB2上，GPIOA挂载在AHB1上
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;

    //PA5,PA6,PA7输出模式设置为复用
    GPIOA->MODER = (GPIOA->MODER & ~(0x3Fu << 10)) | (0x2Au << 10);
    //给输出速度寄存器OSPEEDR设置为最高速
    GPIOA->OSPEEDR |= (0x3Fu << 10);
    //设置复用功能寄存器AFR接通spi1模块,每4位控制一个引脚
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~(0xFFFu << 20));
    GPIOA->AFR[0] |= (0x5u << 20) | (0x5u << 24) | (0x5u << 28);

    //配置SPI1:
    //mstr:设置为主机
    //ssi：让主机允许一直发送
    //ssm：软件片选，少用一个片选引脚
    //BR写入011会选择：总线主频/8（16MB/8=2MBHZ）
    //SPE：spi模块使能（在最后使能）
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSI | SPI_CR1_SSM
                | (0x3u << SPI_CR1_BR_Pos) | SPI_CR1_SPE;
}
//spi发送一个字节并接收一个字节
uint8_t spi1_transfer(uint8_t tx)
{
    //等待发送寄存器空
    while(!(SPI1->SR & SPI_SR_TXE));
    //写数据，硬件开始产生时钟
    SPI1->DR = tx;
    //等待接收寄存器空
    while(!(SPI1->SR & SPI_SR_RXNE));
    //读数据
    return (uint8_t)SPI1->DR;
}
//spi回环测试
int spi1_loopback_test(void)
{
    for(uint16_t i = 0; i < 256; i++)
    {
        if(spi1_transfer((uint8_t)i) != (uint8_t)i)
        {
            return 0;
        }
    }
    return 1;
}


//看门狗初始化
void iwdg_init(void)
{
    //开启看门狗低速时钟
    RCC->CSR |= RCC_CSR_LSION;
    //等待时钟晶振稳定
    while(!(RCC->CSR & RCC_CSR_LSIRDY));
    //启动看门狗
    IWDG->KR = 0xCCCC;
    //在调试期间切断LSI时钟和IWDG寄存器之间的时钟使能，使看门狗暂停
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
    //IWDG->KR寄存器不储存数值，而是通过写入特定数据来执行操作
    //0x5555：解锁 IWDG_PR 和 IWDG_RLR 寄存器
    //0xCCCC：启动看门狗
    //0xAAAA：重载计数器（喂狗）
    IWDG->KR = 0x5555;
    //设置分频系数，PR为三位寄存器，110（6）的分配系数最大，为256
    //32kHZ/256 = 125HZ,得到每计时一次8ms
    IWDG->PR = 0x06;
    //确保写操作真正生效
    while (IWDG->SR & IWDG_SR_PVU);
    //重载寄存器，250次，即250*8ms=2s喂一次狗
    IWDG->RLR = 250;
    //等待状态寄存器写入
    while (IWDG->SR & IWDG_SR_RVU);
    
}
//看门狗喂养
void iwdg_feed(void)
{
    IWDG->KR = 0xAAAA;
}


//舵机初始化
void init_servo(void){
    // 1. PB0 时钟 + 复用模式(AF2=TIM3_CH3)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;      // 开TIM3时钟
    GPIOB->MODER  &= ~(0x3 << (0*2));
    //复用功能是让该引脚脱离CPU的控制，让定时器来定时发送命令
    GPIOB->MODER  |=  (0x2 << (0*2));        // PB0=复用功能10
    //AFR寄存器的低4位设置为0x2来告诉引脚把数据线连接到TIM3的通道3(CH3)输出端
    GPIOB->AFR[0] |=  (0x2 << (0*4));        // AF2: TIM3_CH3

    // 2. 定时器配置：1MHz计数(1us一格)，ARR=20000 => 20ms周期
    //要让时钟每1MB输出则需要让PSC每在16MB情况下数16次，
    //而PSC通过0,1...一直开始计数，所以需要PSC的值为15，这样才能保证1us时钟频率正常
    TIM3->PSC = (SystemCoreClock / 1000000u) - 1;  // 16MHz下=15
    //ARR和PSC同样是从0开始计数，所以需要减一。
    //ARR的计数单位为1个PSC时钟应答，20ms满足舵机50HZ的要求
    TIM3->ARR = 20000 - 1;
    //向上计数，设置ARR里的CNT计数器值<CCR3时输出高电平
    TIM3->CCMR2 |= (0x6 << 4);               // OC3: PWM模式1(110)
    TIM3->CCER  |= 0x1 << 8;                 // 使能CC3输出(CC3E)
    TIM3->CCR3  = 1500;                      // 1.5ms=90°(中间位置)
    TIM3->CR1   |= 0x1;                      // 启动定时器(CEN)
}
// 转角度: 0~180
void servo_set_angle(uint16_t angle){
    if(angle > 180) angle = 180;
    // 0°=500us, 180°=2500us
    TIM3->CCR3 = 500 + angle * 2000 / 180;
}

//i2c1初始化(PB6/PB7)
//注：i2c外部上拉，如果i2c总线上 连接 自带上拉电阻的外设 并联过多导致阻值减小，会有电流过大的风险
void i2c1_init(void)
{
    //i2c1挂载APB1,GPIOB挂AHB1
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    //PB6/7复用开漏
    GPIOB->MODER = ((GPIOB->MODER & ~(0xFu << 12)) | (0xAu << 12));
    GPIOB->OTYPER |= (0x3u << 6);        //1为开漏
    GPIOB->PUPDR &= ~(0xFu << 12);       //关闭内部上下拉,保证i2c总线上的硬件自带上拉电阻.2位
    //选择复用外设为i2c的scl(PB6)和sda(PB7),AFR寄存器4位控制一个引脚。写入4来选择i2c1
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~(0xFu << 24) & ~(0xFu << 28));
    GPIOB->AFR[0] = (GPIOB->AFR[0] | (0x4u <<24) | (0x4u << 28));

    //设置外设时钟：f_SCL = f_PCLK1 / (2 × CCR)
    //APB1总线频率f_PCLK1 = 16MHz, i2c总线频率f_SCL = 100kHz, 求出CCR = 80
    I2C1->CR2 = 16;             //告诉外设主设备的时钟频率为16MHz
    I2C1->CCR = 80;             //i2c总线通信频率
    I2C1->TRISE = 17;           //最大上升时间为1000ns，设置为17个(1/16MHZ = 62.5ns)即1062.5ns
    I2C1->CR1 |= I2C_CR1_PE;    //使能i2c外设
}
//i2c1发起始位和地址
static void i2c1_start_write(uint8_t addr)
{
    while(I2C1->SR2 & I2C_SR2_BUSY);        //等待总线空闲
    I2C1->CR1 |= I2C_CR1_START;             //发送起始标志
    while(!(I2C1->SR1 & I2C_SR1_SB));       //等待起始位接收完成
    I2C1->DR = addr;                        //发送地址
    while(!(I2C1->SR1 & I2C_SR1_ADDR));     //等待从机应答ack
    (void)I2C1->SR2;                        //再读SR2清除ADDR标志位。必须先读SR1再读SR2才能清除addr标志位
}
//i2c1写一个字节
static void i2c1_write_byte(uint8_t data)
{
    while(!(I2C1->SR1 & I2C_SR1_TXE));       //等待数据寄存器空
    I2C1->DR = data;                         //写入数据
    while(!(I2C1->SR1 & I2C_SR1_BTF));       //等待数据收到后的应答
}
//i2c1停止位
static void i2c1_stop(void)
{
    I2C1->CR1 |= I2C_CR1_STOP;               //发送停止位
}

//通过i2c1向oled发送命令
static void oled_write_cmd(uint8_t cmd)
{
    i2c1_start_write(oled_addr);
    i2c1_write_byte(0x00);    //设置为命令模式
    i2c1_write_byte(cmd);
    i2c1_stop();
}
//通过i2c1向oled发送数据
static void oled_write_data(uint8_t data)
{
    i2c1_start_write(oled_addr);
    i2c1_write_byte(0x40);    //设置为数据模式
    i2c1_write_byte(data);
    i2c1_stop();
}
//oled采用多总线模式(i2c)传输数据
//每次数据传输需要开始标志和结束标志
//标志通过scl和sda的状态改变来实现
//开始标志为scl=1，sda拉低(状态改变动作)
/*i2c软件模拟
//oled时钟引脚
static void PB6_set_0(void){
    GPIOB -> BSRR = (1 << 6) << 16;
}
static void PB6_set_1(void){
    GPIOB -> BSRR = 1 << 6;
}
//oled1数据引脚
static void PB7_set_0(void){
    GPIOB -> BSRR = (1 << 7) << 16;
}
static void PB7_set_1(void){
    GPIOB -> BSRR = 1 << 7;
}
void i2c_start(void)
{
    //初始化为高电平(空闲状态),方便后续翻转
    PB6_set_1();
    PB7_set_1();
    //若oled自带上拉电阻4.7(kv)，则2us完全满足，若只依靠内部上拉电阻则可能时间过短，建议2~3us
    delay_us(2);         
    
    //将sda拉低，开始标志
    PB7_set_0();
    delay_us(2);

    //将scl拉低，不然数据不会被写入
    PB6_set_0();
    delay_us(2);
}
//停止标志为scl=1，sda拉高
static void i2c_stop(void)
{
    //拉低sda
    PB7_set_0();
    delay_us(2);
    //拉高scl
    PB6_set_1();
    delay_us(2);
    //拉高sda，结束标志
    PB7_set_1();
    delay_us(2);
}
//读取oled应答数据ack
static uint8_t oled_sda_read(void)
{
    return ((oled_port->IDR & oled_sda_pin)? 1 : 0);
}
//向oled发送数据
//其中规则为：数据1个字节，
static uint8_t i2c_write_byte(uint8_t dat)
{
    //发送8个bit数据，消耗8个时钟
    for(int i = 0;i<8;i++)
    {
        if(dat & 0x80)      //把最高位通过sda引脚传输
        {
            PB7_set_1();
        }
        else{
            PB7_set_0();
        }
        dat <<= 1;   //数据发送完左移1位接着传输下一位
        //立即拉高scl,此时oled发现scl的上升沿会立刻锁存sda上的数据(1/0),达到读取数据的效果
        PB6_set_1();
        delay_us(2);
        //拉低scl准备下一个数据
        PB6_set_0();
    }
    //经过8bit数据发送后，在第9个时钟周期oled会发送应答ack
    //释放总线
    PB7_set_1();    //必需在第9次时钟上升前释放sda引脚，确保ack值正常
    delay_us(2);
    PB6_set_1();    //进行第9次时钟上升,此时oled发现上升沿会立刻发出应答数据ack到sda引脚
    delay_us(2);
    //读取ack
    uint8_t ack = oled_sda_read();
    //安全的结束这一次发送
    PB6_set_0();
    delay_us(2);
    return ack;
}
//控制命令:0x00=命令，0x40显示数据
static void oled_write_cmd(uint8_t cmd)
{
    //开始标志
    i2c_start();
    //写入oled地址
    i2c_write_byte(oled_addr);
    //设置为命令,后面一个字节放入指令寄存器
    i2c_write_byte(0x00);
    //发送具体命令
    i2c_write_byte(cmd);
    //结束标志
    i2c_stop();
}
//数据命令
static void oled_write_data(uint8_t dat)
{
    i2c_start();
    i2c_write_byte(oled_addr);
    i2c_write_byte(0x40);    //指令放入显存中
    i2c_write_byte(dat);
    i2c_stop();
}
//初始化oled屏1
void pin_init_oled(GPIO_TypeDef *port, uint16_t scl_pin, uint16_t sda_pin)
{
    //掩码转换为int
    scl_pin = (uint16_t)__builtin_ctz(scl_pin);
    sda_pin = (uint16_t)__builtin_ctz(sda_pin);
    //接通端口时钟
    if(port == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if(port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if(port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if(port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;

    //scl引脚设置输出
    port -> MODER &= ~(0x3 << (scl_pin * 2));    // 清空模式位
    port -> MODER |=  (0x1 << (scl_pin * 2));    // 设置输出模式01
    //设置为开漏输出
    port -> OTYPER |= (0x1 << scl_pin);          // 设置为开漏输出
    port->PUPDR  &= ~(0x3 << (scl_pin * 2));  //重置pupdr寄存器
    port->PUPDR |= (0x1 << (scl_pin * 2));  // 内部上拉，防止总线悬空
    port->ODR   |= (0x1 << scl_pin);        // 初始释放总线，空闲为高
    port -> OSPEEDR &= ~(0x3 << (scl_pin * 2));  //低速

    //sda引脚设置输出
    port -> MODER &= ~(0x3 << (sda_pin * 2));    // 清空模式位
    port -> MODER |=  (0x1 << (sda_pin * 2));    // 设置输出模式01
    //设置为开漏输出
    port -> OTYPER |= (0x1 << sda_pin);          // 设置为开漏输出
    port->PUPDR  &= ~(0x3 << (sda_pin * 2));  //重置pupdr寄存器
    port->PUPDR |= (0x1 << (sda_pin * 2));  // 内部上拉，防止总线悬空
    port->ODR   |= (0x1 << sda_pin);        // 初始释放总线，空闲为高
    port -> OSPEEDR &= ~(0x3 << (sda_pin * 2));  //低速
}
    */
//初始化序列
void oled_init(void)
{
    i2c1_init();            //硬件初始化
    //pin_init_oled(oled_port,oled_scl_pin,oled_sda_pin);   //软件初始化
    delay_us(50000);    //阻塞等待配置好
    static const uint8_t cmds[] = {
        0xAE,          // 关显示
        0xD5, 0x80,    // 设置时钟频率，0x80为默认值
        0xA8, 0x3F,    // 复用比63
        0xD3, 0x00,    // 显示偏移0
        0x40,          // 起始行0
        0x8D, 0x14,    // 开电荷泵(必须),OLED屏需要7~10v才能点亮，需要这个命令启动内部升压电路
        0x20, 0x00,    // 水平寻址模式,写满一行换一行
        0xA1,          // 段重映射(左右正常),左右镜像修正
        0xC8,          // COM扫描方向,配合左右修正
        0xDA, 0x12,    // COM引脚配置(适配64行的屏幕)
        0x81, 0xCF,    // 对比度(亮度)
        0xD9, 0xF1,    // 预充电(影响点亮速度)
        0xDB, 0x40,    // VCOM检测(稳定内部电压)
        0xA4,          // 恢复RAM显示(让屏幕显示显存里已有的内容，而不是全部点亮或熄灭)
        0xA6,          // 正常(非反色)
        0x2E,          // 关滚动(防止上电时意外触发滚动特效)
        0xAF           // 开显示
    };
    for(int i = 0; i < sizeof(cmds);i++)
    {
        oled_write_cmd(cmds[i]);
    }
}
//光标定位 + 清屏（8页 × 128列）
static void oled_set_cursor(uint8_t x,uint8_t page)
{
    oled_write_cmd(0xB0 | page);      //选页
    oled_write_cmd(0x00 | (x & 0x0F));    //列地址低4位
    oled_write_cmd(0x10 | ((x >> 4) & 0x0F));    //列地址高4位
}
void oled_clear(void){
    for(uint8_t page = 0; page < 8; page++){
        oled_set_cursor(0, page);
        i2c1_start_write(oled_addr);
        i2c1_write_byte(0x40);
        for(int i = 0; i < 128; i++) i2c1_write_byte(0x00);
        i2c1_stop();
    }
}
//显示字符（6×8 小字体，含常用符号；一个字符=8字节，每字节一列，LSB在最上）
static const char charset[] = "0123456789.-%C: TEMPHUIF";
static const uint8_t font_6x8[][8] = {
    {0x3E,0x51,0x49,0x45,0x3E,0,0,0},  // 0
    {0x00,0x42,0x7F,0x40,0x00,0,0,0},  // 1
    {0x42,0x61,0x51,0x49,0x46,0,0,0},  // 2
    {0x21,0x41,0x45,0x4B,0x31,0,0,0},  // 3
    {0x18,0x14,0x12,0x7F,0x10,0,0,0},  // 4
    {0x27,0x45,0x45,0x45,0x39,0,0,0},  // 5
    {0x3C,0x4A,0x49,0x49,0x30,0,0,0},  // 6
    {0x01,0x71,0x09,0x05,0x03,0,0,0},  // 7
    {0x36,0x49,0x49,0x49,0x36,0,0,0},  // 8
    {0x06,0x49,0x49,0x29,0x1E,0,0,0},  // 9
    {0x00,0x00,0x60,0x60,0x00,0,0,0},  // .
    {0x08,0x08,0x08,0x08,0x08,0,0,0},  // -
    {0x23,0x13,0x08,0x64,0x62,0,0,0},  // %
    {0x3C,0x42,0x42,0x42,0x24,0,0,0},  // C
    {0x00,0x36,0x36,0x00,0x00,0,0,0},  // :
    {0x00,0x00,0x00,0x00,0x00,0,0,0},  // 空格
    {0x0F,0x08,0x08,0x08,0x08,0,0,0},  // T
    {0x7F,0x49,0x49,0x49,0x41,0,0,0},  // E
    {0x7F,0x04,0x08,0x04,0x7F,0,0,0},  // M
    {0x7F,0x09,0x09,0x09,0x06,0,0,0},  // P
    {0x7F,0x08,0x08,0x08,0x7F,0,0,0},  // H
    {0x3E,0x41,0x41,0x41,0x3E,0,0,0},  // U
    {0x00,0x41,0x7F,0x41,0x00,0,0,0},  // I
    {0x7F,0x09,0x09,0x09,0x01,0,0,0},  // F
};
//显示字符
void oled_show_char(uint8_t x, uint8_t page, char ch){
    int idx = -1;
    for(int i = 0; charset[i]; i++){ if(charset[i] == ch){ idx = i; break; } }
    if(idx < 0) return;
    oled_set_cursor(x, page);
    i2c1_start_write(oled_addr);
    i2c1_write_byte(0x40);
    for(int i = 0; i < 8; i++) i2c1_write_byte(font_6x8[idx][i]);
    i2c1_stop();
}
void oled_show_string(uint8_t x, uint8_t page, const char *str){
    while(*str){
        oled_show_char(x, page, *str++);
        x += 6;                    // 每字符占6列
        if(x > 122){ x = 0; page++; }   // 换行
    }
}


//初始化小灯，输入端口和引脚
void init_light(GPIO_TypeDef *port, uint16_t pin){
    // 把小灯引脚号的掩码(0x2000)转成引脚编号(13)
    pin = (uint16_t)__builtin_ctz(pin);  //GCC内置函数__builtin_ctz需传入一个整数中末尾0的个数

    //给开启对应端口时钟.所有的GPIO都挂载在AHB1总线上，对于的时钟控制寄存器是RCC->AHB1ENR，
    //GPIOC是第二位,给它写1就能接通时钟。
    if(port == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if(port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if(port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if(port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;

    //GPIO 的模式配置存在 MODER 寄存器里，每个引脚占 2 位，第 13 号引脚对应 bit 26~27
    //输出模式的编码是 01，所以我们先清空这两位，再写入 01
    port->MODER &= ~(0x3 << (pin * 2));    // 左移26位，将26，27清空设为00
    port->MODER |=  (0x1 << (pin * 2));    // 设置输出模式01
    printf("端口%p设置的引脚%d设置为输出模式\n", port, pin);

    // 3. 输出类型：推挽输出（bit 置 0）
    port->OTYPER &= ~(0x1 << pin);
    

    // 4. 输出速度：高速（10）
    port->OSPEEDR &= ~(0x3 << (pin * 2));
    port->OSPEEDR |=  (0x2 << (pin * 2));
    printf("端口%p设置的引脚%d设置为高速输出\n", port, pin);

    // 5. 无上下拉（00）
    port->PUPDR &= ~(0x3 << (pin * 2));

}
//点亮小灯,!低电平点亮
void light_on(void){
    //BSRR 寄存器是32位，低16位用于置位,高16位用于复位,期望掩码
    //要点亮需要在BSRR的高16位中对应引脚编号的那一位写1
    led_port ->BSRR = (uint32_t)led_pin << 16;
    printf("点亮小灯成功\n");
}
//熄灭小灯
void light_off(void){
    //将BSRR的低16位中对应引脚编号的那一位写1
    led_port->BSRR = (uint32_t)led_pin;
    printf("熄灭小灯成功\n");
}
//小灯翻转
void light_toggle(void)
{
    led_port->ODR ^= led_pin;    /* ODR 异或 = 翻转 */
}


//DH22温湿度传感器初始化
void init_dht22(GPIO_TypeDef *port, uint16_t pin)
{
    // 把DHT22引脚号的掩码(0x0002)转成引脚编号(1)
    pin = (uint16_t)__builtin_ctz(pin);
    //接通时钟
    if(port == GPIOA) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    else if(port == GPIOB) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    else if(port == GPIOC) RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    else if(port == GPIOD) RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN;
    //设置输出
    port -> MODER &= ~(0x3 << (pin * 2));    // 清空模式位
    port -> MODER |=  (0x1 << (pin * 2));    // 设置输出模式01
    //设置为开漏输出
    port -> OTYPER |= (0x1 << pin);          // 设置为开漏输出
    port->PUPDR |= (0x1 << (pin * 2));  // 内部上拉，防止总线悬空
    port->ODR   |= (0x1 << pin);        // 初始释放总线，空闲为高
    //输出速度：低速（00）
    port -> OSPEEDR &= ~(0x3 << (pin * 2));
}


//dht22温湿度引脚
static void PA1_set_0(void){
    GPIOA -> BSRR = (1 << 1) << 16;
}
static void PA1_set_1(void){
    GPIOA -> BSRR = 1 << 1;
}
static uint8_t pin_read(void){
    //IDR为input data register（输入数据寄存器）的缩写
    return (dht22_port -> IDR & dht22_pin)? 1:0;
}
static uint8_t wait_level(uint8_t lv,uint32_t timeout_us)
{
    uint32_t t0 = micros();
    while(pin_read() != lv)
    {
        if(micros() - t0 > timeout_us)
        {
            printf("未发现电平\n");
            return 0;
        }
    }
    return 1;
}
//获取温湿度
uint8_t get_tem_hum(void){
    //下拉电平向dht22请求数据
    PA1_set_0();
    //等2ms
    delay_ms(2);

    //释放总线
    PA1_set_1();
    //等20微秒
    delay_us(20);

    //开始查找下降沿,等待dht22应答，低电平持续80us，我们等待100us判断超时
    if(!wait_level(0,100))
    {
        return 0;
    }
    //开始等待上升沿，等待dht22准备数据，80us后开始发数据，等待200us判断超时
    if(!wait_level(1,200))
    {
        return 0;
    }
    //等待下降沿，dht22开始传输数据的标志，等待200us判断超时
    if(!wait_level(0,200))
    {
        return 0;
    }
    //开始接收dht22传输过来的40位数据
    //每一位都以50us低电平开始，根据电平持续时间判断是高电平还是低电平
    //高电平持续26~28us，这一位数据为0
    //高电平持续70us，这一位数据为1
    //40位的发送顺序为：湿度整数 8位，湿度小数 8位，温度整数 8位，温度小数 8位，校验和 8位
    for(int i = 0;i < 40; i++)
    {
        //开始等待高电平
        if(!wait_level(1,200))
        {
            return 0;
        }
        //记录初始时间用来判断高电平时间
        uint32_t t1 = micros();
        //开始等待高电平结束
        if(!wait_level(0,200))
        {
            return 0;
        }
        //判断数据是1还是0
        uint8_t bit = (micros() - t1) > 40;  //若>40us则认为是1
        //存储数据
        //dht22先发数据最高位，如0100 0101，发送顺序为0->1->0->0->0->1->0->1
        //数据接收时先放入当前位，然后左移，丢掉做最高位，最低位补0，将数据向左推实现数据的匹配
        //初始为0000 0000 ，接收0后变为0000 0000，接收1后变为0000 0001，最终得到0100 0101
        dht22_data[i/8] = (dht22_data[i/8] << 1) | bit;
    }
    //开始校验数据
    //校验和是通过将data[0]+data[1]+data[2]+data[3]相加之和并取低8位放入data[4]中得到的
    //其中data[4]就是校验和
    //判断时只需将接收到的前4个字节的数据相加并取低8位与data[4]判断是否相等即可
    //取低8位时使用uint8_t
    if((uint8_t)(dht22_data[0]+dht22_data[1]+dht22_data[2]+dht22_data[3]) != dht22_data[4])
    {
        printf("数据错误\n");
        return 0;
    }
    //进行数据的换算
    for(int i = 99;i > 0;i--)
    {
        samples[i] = samples[i-1];
    }
    samples[0].humidity_x10 = (uint16_t)(dht22_data[0] << 8) | dht22_data[1];
    //dht22温度数据最高位为是温度符号位，需要转换为原码，
    int16_t t2 = (int16_t)(((uint16_t)dht22_data[2] << 8) | dht22_data[3]);
    //如果温度的最高位为1，那将符号提出来，并把最高位置为0
    if(t2 & 0x8000)
    {
        t2 = -(t2 & 0x7fff);
    }
    samples[0].temperature_x10 = t2;
    return 1;
}




void time_init(void){
    //开启DWT调试时钟
    CoreDebug -> DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT -> CYCCNT = 0;   //计数器清0
    DWT -> CTRL |= DWT_CTRL_CYCCNTENA_Msk;  //使能周期计数器
}
//自动适配时钟频率
uint32_t micros(void){
    return DWT -> CYCCNT / (SystemCoreClock / 1000000u);
}
//延时us
void delay_us(uint32_t us){
    uint32_t start = micros();
    while (micros() - start < us);  //等待us
}
void delay_ms(uint32_t ms)
{
    // 1. 计算循环次数
    // 系统时钟为 16MHz，1s = 16000 个时钟周期
    // 每次循环大约需要 4 ~ 6个时钟周期
    volatile uint32_t count = (SystemCoreClock / 5000) * ms;

    // 2. 循环延时
    while(count--)
    {
        __NOP(); // 空操作，防止优化掉循环体
    }
}





