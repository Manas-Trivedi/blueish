#include "buffer.h"
#include <stdio.h>

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    abort();
}

size_t buf_size(Buffer *buf) {
    return buf->data_end - buf->data_begin;
}

void buf_init(struct Buffer *buf, size_t capacity) {
    uint8_t *mem = (uint8_t *)malloc(capacity);
    if (!mem) die("malloc");
    buf->buffer_begin = mem;
    buf->buffer_end = mem + capacity;
    buf->data_begin = mem;
    buf->data_end = mem;
}

void buf_append(struct Buffer *buf, const uint8_t *data, size_t len) {
    // check if enough space lies between data_end and buffer_end
    if(buf->buffer_end - buf->data_end < len) {
        //compact the data to buffer_begin if not
        size_t data_size = buf->data_end - buf->data_begin;
        memmove(buf->buffer_begin, buf->data_begin, data_size);
        buf->data_begin = buf->buffer_begin;
        buf->data_end = buf->buffer_begin + data_size;
        // check again if still not then reallocate
        if(buf->buffer_end - buf->data_end < len) {
            size_t capacity = buf->buffer_end - buf->buffer_begin;
            size_t new_capacity = capacity * 2;
            while (new_capacity < data_size + len) {
                new_capacity *= 2;
            }
            uint8_t *new_buf = (uint8_t *)malloc(new_capacity);
            if (!new_buf) die("malloc");
            memcpy(new_buf, buf->data_begin, data_size);
            free(buf->buffer_begin);
            buf->buffer_begin = new_buf;
            buf->data_begin = new_buf;
            buf->data_end = new_buf + data_size;
            buf->buffer_end = new_buf + new_capacity;
        }
    }
    memcpy(buf->data_end, data, len);
    buf->data_end += len;
}

void buf_consume(struct Buffer *buf, size_t n) {
    assert(buf->data_begin + n <= buf->data_end);
    buf->data_begin += n;
    if (buf->data_begin == buf->data_end) {
        buf->data_begin = buf->buffer_begin;
        buf->data_end = buf->buffer_begin;
    }
}
