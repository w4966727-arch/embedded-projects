#include "ringbuf.h"



void ringbuf_init(ringbuf_t *rb)
{
    rb->head = 0;
    rb->tail = 0;
}
//判断缓冲区是否为空
int ringbuf_is_empty(const ringbuf_t *rb)
{
    return (rb->head == rb->tail);
}
//判断缓冲区是否充满
int ringbuf_is_full(const ringbuf_t *rb)
{
    return (((rb->tail + 1) & (RINGBUF_SIZE - 1)) == rb->head);
}
//
uint16_t ringbuf_count(const ringbuf_t *rb)
{
    return (uint16_t)((rb->tail - rb->head + RINGBUF_SIZE) & (RINGBUF_SIZE - 1));
}
//放入数据到缓冲区
int ringbuf_push(ringbuf_t *rb, uint8_t b)
{
    //缓冲区满了则直接丢弃数据
    if(ringbuf_is_full(rb))
    {
        return 0;
    }
    //没满则储存数据
    rb->buf[rb->head & (RINGBUF_SIZE - 1)] = b;
    rb->head = (rb->head + 1) & (RINGBUF_SIZE - 1);
    return 1;
}
//从缓冲区取出数据
int ringbuf_pop(ringbuf_t *rb, uint8_t *b)
{
    if(ringbuf_is_empty(rb))
    {
        return 0;
    }
    *b = rb->buf[rb->tail & (RINGBUF_SIZE - 1)];
    rb->tail = (rb->tail + 1) & (RINGBUF_SIZE - 1);
    return 1;
}
//把数组src写入rb缓冲区
uint16_t ringbuf_push_bulk(ringbuf_t *rb, volatile const uint8_t *src, uint16_t n)
{
    uint16_t i = 0;
    for(; i < n; i++)
    {
        //满了退出
        if(!ringbuf_push(rb, src[i]))
        {
            break;
        }
    }
    return i;
}
//
uint16_t ringbuf_pop_bulk(ringbuf_t *rb, uint8_t *dst, uint16_t n)
{
    uint16_t i = 0;
    for(; i < n; i++)
    {
        //空了就停
        if(!ringbuf_pop(rb, &dst[i]))
        {
            break;
        }
    }
    return i;
}