#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

// =================
// Buffer Helpers
// =================

struct Buffer {
    uint8_t *buffer_begin;
    uint8_t *buffer_end;
    uint8_t *data_begin;
    uint8_t *data_end;
};

size_t buf_size(Buffer *buf);
void buf_init(struct Buffer *buf, size_t capacity);
void buf_append(struct Buffer *buf, const uint8_t *data, size_t len);
void buf_consume(struct Buffer *buf, size_t n);
