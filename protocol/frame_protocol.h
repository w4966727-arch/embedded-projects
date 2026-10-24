#ifndef FRAME_PROTOCOL_H
#define FRAME_PROTOCOL_H

#include <stdint.h>

/*帧协议: 0xAA 0x55 | LEN | CMD | PATLOAD[0...LEN-1] | SUM(XOR)  */
#define FRAME_HEAD1         0xAAu
#define FRAME_HEAD2         0x55u
#define FRAME_MAX_PAYLOAD   64

/*命令码*/
#define CMD_TEMP_HUM        0x01
#define CMD_HEARTBEAT       0x02
#define CMD_LED_STATE       0x03
#define CMD_SERVO           0x10
#define CMD_LED             0x11
#define CMD_OLED_TEXT        0x20

/*解析结果*/
typedef enum{
    FRAME_NONE = 0,         //未凑齐一帧
    FRAME_OK,               //解析出来一帧
    FRAME_ERR_CHECKSUM,     //校验失败
    FRAME_ERR_LEN           //长度字段非法
}frame_result_t;

/*完整帧数据*/
typedef struct{
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[FRAME_MAX_PAYLOAD];
}frame_t;

/*解析器上下文*/
typedef struct{
    int      state;          //状态机当前状态
    uint8_t  payload[FRAME_MAX_PAYLOAD];
    uint8_t  len, cmd, sum, idx;

    uint32_t st_bytes;      //累计喂入字节数
    uint32_t st_frames;     //成功解析帧数
    uint32_t st_err_sum;    //校验错误次数
    uint32_t st_err_len;    //长度非法次数
    uint32_t st_dropped;    //因超时/空闲丢弃的半帧数
}frame_parser_t;

/*复位解析器*/
void frame_parser_init(frame_parser_t *p);

/*向解析器p喂字节检查，通过则把帧放入输出缓冲区out*/
frame_result_t frame_parser_feed(frame_parser_t *p, uint8_t byte, frame_t *out);

/*复位未完成的半帧（上层检测到帧间隔/总线空闲时调用）
 *
 * 为什么需要它：字节流协议无法自行区分"帧被截断"和"帧还没发完"，
 * 必须由上层提供时间信息：
 *   STM32 → USART IDLE 中断；ESP32 → uart 读超时；PC → 直接调用。
 * 不调用它时，被截断的半帧会错位吞掉紧随其后的一帧。*/
void frame_parser_resync(frame_parser_t *p);

/*组帧, 返回帧总长度, out空间不足返回-1*/
int frame_build(uint8_t cmd, const uint8_t *payload, uint8_t len,
                uint8_t *out, int out_size);

#endif