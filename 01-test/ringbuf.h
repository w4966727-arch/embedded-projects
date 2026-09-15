#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdint.h>
#include <stddef.h>

#define RINGBUF_SIZE    256

typedef struct
{
    uint8_t buf[RINGBUF_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
}ringbuf_t;

//全局环形缓冲区
extern ringbuf_t g_rb;

void ringbuf_init(ringbuf_t *rb);
int ringbuf_is_empty(const ringbuf_t *rb);
int ringbuf_is_full(const ringbuf_t *rb);
uint16_t ringbuf_count(const ringbuf_t *rb);
int ringbuf_push(ringbuf_t *rb, uint8_t b);
int ringbuf_pop(ringbuf_t *rb, uint8_t *b);
uint16_t ringbuf_push_bulk(ringbuf_t *rb, volatile const uint8_t *src, uint16_t n);
uint16_t ringbuf_pop_bulk(ringbuf_t *rb, uint8_t *dst, uint16_t n);

#endif