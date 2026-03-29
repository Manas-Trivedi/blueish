#pragma once

#include "buffer.h"
#include <stdbool.h>

// ======================
// Connection Handling
// ======================

struct Conn {
    int fd = -1;
    // intention for event loop
    bool want_read = false;
    bool want_write = false;
    bool want_close = false;
    // individual buffers for each connection
    struct Buffer *incoming;
    struct Buffer *outgoing;
};

Conn* handle_accept(int fd);
void handle_read(Conn* conn);
void handle_write(Conn* conn);
bool try_one_request(Conn* conn);
