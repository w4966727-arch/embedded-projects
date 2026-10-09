#include "frame_protocol.h"
#include <string.h>

/*状态机*/
typedef enum{
    S_HEAD1, S_HEAD2, S_LEN, S_CMD, S_DATA, S_SUM
}rx_state_t;

/*解析器初始化*/
void frame_parser_init(frame_parser_t *p)
{
    //检查空指针
    if(p == NULL)
    {
        return ;
    }

    //c语言标准库中函数，作用为：从p地址开始，将n个字节全部写成0
    memset(p, 0, sizeof(*p));
    p->state = S_HEAD1;
}

/*帧状态机解析*/
frame_result_t frame_parser_feed(frame_parser_t *p, uint8_t byte, frame_t *out)
{
    if(p == NULL)
    {
        return FRAME_NONE;
    }

    p->st_bytes++;              //喂入的字节数+1

    //开始解析
    switch(p->state)
    {
        case S_HEAD1:
        {
            //帧头无条件通过
            if(byte == FRAME_HEAD1)
            {
                //进入下一个帧状态
                p->state = S_HEAD2;
            }
            break;
        }
        case S_HEAD2:
        {
            //第二帧只认帧头
            if(byte == FRAME_HEAD2)
            {
                p->state = S_LEN;
            }
            //特殊情况,状态为帧头则认为是新帧的开始,直接从第二帧开始
            else if(byte == FRAME_HEAD1)
            {
                p->state = S_HEAD2;
            }
            //其余情况都重新组帧
            else
            {
                p->state = S_HEAD1;
            }
            break;
        }
        case S_LEN:
        {
            //记录长度
            p->len = byte;
            if(p->len > FRAME_MAX_PAYLOAD)
            {
                /*长度非法，整帧作废*/
                p->state = S_HEAD1;
                p->st_err_len++;
                return FRAME_ERR_LEN;
            }
            p->sum = byte;      //准备开始校验,从len开始累加
            p->idx = 0;
            p->state = S_CMD;
            break;
        }
        case S_CMD:
        {
            p->cmd = byte;
            p->sum ^= byte;     //将加入的cmd异或来进行校验
            p->state = (p->len == 0) ? S_SUM : S_DATA;  //心跳帧len为0
            break;
        }
        case S_DATA:
        {
            p->payload[p->idx++] = byte;
            p->sum ^= byte;
            if(p->idx >= p->len)
            {
                p->state = S_SUM;
            }
            break;
        }
        case S_SUM:
        {
            p->state = S_HEAD1;
            if(byte == p->sum)
            {
                //成功解析帧
                p->st_frames++;
                //把帧放入输出缓冲区中
                if(out != NULL)
                {
                    out->cmd = p->cmd;
                    out->len = p->len;
                    memcpy(out->payload, p->payload, p->len);   
                }
                return FRAME_OK;
            }
            //解析失败
            p->st_err_sum++;
            return FRAME_ERR_CHECKSUM;
        }
    }
    //没解析完一帧
    return FRAME_NONE;
}

/*组帧*/
int frame_build(uint8_t cmd, const uint8_t *payload, uint8_t len,
                uint8_t *out, int out_size)
{
    if(out == NULL)
    {
        return -1;
    }
    if(len > FRAME_MAX_PAYLOAD)
    {
        return -1;
    }
    //总长度为len(payload) + 1(s_head1) + 1(s_head2) + 1(s_len) + 1(s_cmd) + 1(s_sum)
    if(out_size < ((int)len + 5))
    {
        return -1;
    }

    uint8_t sum = len ^ cmd;
    int i = 0;

    out[i++] = FRAME_HEAD1;
    out[i++] = FRAME_HEAD2;
    out[i++] = len;
    out[i++] = cmd;

    for(uint8_t j = 0; j < len; j++)
    {
        out[i++] = payload[j];
        sum ^= payload[j];
    }
    out[i++] = sum;
    //返回整个帧的长度
    return i;
}


/*复位未完成的半帧（由上层在检测到帧间隔/总线空闲后调用）*/
void frame_parser_resync(frame_parser_t *p)
{
    if(p == NULL)
    {
        return;
    }
    //只有存在半帧残留时才需要复位
    if(p->state != S_HEAD1)
    {
        p->state = S_HEAD1;
        p->idx = 0;
        p->st_dropped++;    //半帧丢弃计数
    }
}