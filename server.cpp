// stdlib
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
// system
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <cassert>
#include <poll.h>
#include <fcntl.h>
// C++
#include <vector>
#include <map>
#include <string>
// project
# include "hashtable.h"

# define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

// =============
// Utilities
// =============

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno: %d] %s\n", errno, msg);
}

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}

static void fd_set_nb(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL, 0);
    if(errno) {
        die("fcntl() error");
    }
    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if(errno) {
        die("fcntl() error");
    }
}

const size_t k_max_msg = 32 << 20;

const size_t k_max_args = 200 * 1000;

// =================
// Buffer Helpers
// =================

struct Buffer {
    uint8_t *buffer_begin;
    uint8_t *buffer_end;
    uint8_t *data_begin;
    uint8_t *data_end;
};

size_t buf_size(Buffer *buf) {
    return buf->data_end - buf->data_begin;
}

static void buf_init(struct Buffer *buf, size_t capacity) {
    uint8_t *mem = (uint8_t *)malloc(capacity);
    if (!mem) die("malloc");
    buf->buffer_begin = mem;
    buf->buffer_end = mem + capacity;
    buf->data_begin = mem;
    buf->data_end = mem;
}

static void buf_append(struct Buffer *buf, const uint8_t *data, size_t len) {
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

static void buf_consume(struct Buffer *buf, size_t n) {
    assert(buf->data_begin + n <= buf->data_end);
    buf->data_begin += n;
    if (buf->data_begin == buf->data_end) {
        buf->data_begin = buf->buffer_begin;
        buf->data_end = buf->buffer_begin;
    }
}

// ===========
// Protocol
// ===========

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

static bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if(cur + 4 > end) {
        return false;
    }

    memcpy(&out, cur, 4);
    cur += 4;

    return true;
}

static bool read_str(const uint8_t *&cur, const uint8_t *end, uint32_t n, std::string &out) {
    if(cur + n > end) {
        return false;
    }

    out.assign(cur, cur + n);
    cur += n;
    return true;
}

static int32_t parse_request(const uint8_t *data, size_t size, std::vector<std::string> &out){
    const uint8_t *end = data + size;

    uint32_t nstr = 0;

    if(!read_u32(data, end, nstr)) {
        return -1;
    }

    if(nstr > k_max_args) {
        return -1;
    }

    while(out.size() < nstr) {
        uint32_t len = 0;

        if(!read_u32(data, end, len)) {
            return -1;
        }

        out.push_back(std::string());

        if(!read_str(data, end, len, out.back())) {
            return -1;
        }
    }

    return 0;
}

static void out_nil(Buffer &out) {
    uint8_t tag = TAG_NIL;
    buf_append(&out, &tag, sizeof(tag));
}

static void out_str(Buffer &out, const char *s, size_t size) {
    uint8_t tag = TAG_STR;
    buf_append(&out, &tag, sizeof(tag));

    uint32_t len = (uint32_t)size;
    buf_append(&out, (uint8_t*)&len, sizeof(len));

    buf_append(&out, (const uint8_t *)s, size);
}

static void out_int(Buffer &out, int64_t val) {
    uint8_t tag = TAG_INT;
    buf_append(&out, &tag, sizeof(tag));

    buf_append(&out, (uint8_t*)&val, sizeof(val)); // 8 bytes
}

static void out_arr(Buffer &out, uint32_t n) {
    uint8_t tag = TAG_ARR;
    buf_append(&out, &tag, sizeof(tag));

    buf_append(&out, (uint8_t*)&n, sizeof(n));
}

static void out_err(Buffer &out) {
    uint8_t tag = TAG_ERR;
    buf_append(&out, &tag, 1);

    uint32_t len = 0;
    buf_append(&out, (uint8_t*)&len, 4);
}

// =====================
// Application Logic
// =====================

static struct {
    HMap db;
} g_data;

struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
};

static bool entry_eq(HNode *lhs, HNode *rhs) {
    struct Entry *le = container_of(lhs, struct Entry, node);
    struct Entry *re = container_of(rhs, struct Entry, node);
    return le->key == re->key;
}

static inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

// MurmurHash3 (x86_32)
static uint32_t str_hash(const uint8_t *data, size_t len) {
    const uint32_t seed = 0;
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    uint32_t h = seed;

    size_t nblocks = len / 4;
    for (size_t i = 0; i < nblocks; ++i) {
        uint32_t k = 0;
        memcpy(&k, data + i * 4, 4);

        k *= c1;
        k = rotl32(k, 15);
        k *= c2;

        h ^= k;
        h = rotl32(h, 13);
        h = h * 5 + 0xe6546b64;
    }

    const uint8_t *tail = data + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
        case 3:
            k1 ^= (uint32_t)tail[2] << 16;
            [[fallthrough]];
        case 2:
            k1 ^= (uint32_t)tail[1] << 8;
            [[fallthrough]];
        case 1:
            k1 ^= (uint32_t)tail[0];
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            h ^= k1;
        default:
            break;
    }

    h ^= (uint32_t)len;
    return fmix32(h);
}

static bool cb_keys(HNode *node, void *arg) {
    Buffer &out = *(Buffer *)arg;
    const std::string &key = container_of(node, Entry, node)->key;
    out_str(out, key.data(), key.size());
    return true;
}

static void do_keys(std::vector<std::string> &, Buffer &out) {
    out_arr(out, (uint32_t)hm_size(&g_data.db));
    hm_foreach(&g_data.db, &cb_keys, (void *)&out);
}

static void do_get(std::vector<std::string>& cmd, Buffer &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *) key.key.data(), key.key.size());
    // query the table
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(!node) {
        out_nil(out);
        return;
    }
    std::string &val = container_of(node, Entry, node)->val;
    assert(val.size() < k_max_msg);
    return out_str(out, val.data(), val.size());
}

static void do_set(std::vector<std::string> &cmd, Buffer &out) {
    // a dummy `Entry` just for the lookup
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if (node) {
        // found, update the value
        container_of(node, Entry, node)->val.swap(cmd[2]);
    } else {
        // not found, allocate & insert a new pair
        Entry *ent = new Entry();
        ent->key.swap(key.key);
        ent->node.hcode = key.node.hcode;
        ent->val.swap(cmd[2]);
        hm_insert(&g_data.db, &ent->node);
    }
    return out_nil(out);
}

static void do_del(std::vector<std::string> &cmd, Buffer &out) {
    bool flag = 0;
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) {
        flag = true;
        delete container_of(node, Entry, node);
    }
    return out_int(out, flag ? 1 : 0);
}


static void do_request(std::vector<std::string> &cmd, Buffer &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else if (cmd.size() == 1 && cmd[0] == "keys") {
        return do_keys(cmd, out);
    } else {
        out_err(out);
    }
}

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

static void response_begin(Buffer &out, size_t *header) {
    *header = out.data_end - out.buffer_begin;

    uint32_t zero = 0;
    buf_append(&out, (uint8_t*) &zero, 4);
}

static size_t response_size(Buffer &out, size_t header) {
    return (out.data_end - out.buffer_begin) - header - 4;
}

static void response_end(Buffer &out, size_t header) {
    size_t msg_size = response_size(out, header);

    uint32_t len = (uint32_t)msg_size;

    memcpy(out.buffer_begin + header, &len, 4);
}

static Conn* handle_accept(int fd) {
    struct sockaddr_in client_addr = {};
    socklen_t addrlen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
    if (connfd < 0) {
        msg_errno("accept() error");
        return NULL;
    }
    uint32_t ip = client_addr.sin_addr.s_addr;
    fprintf(stderr, "new client from %u.%u.%u.%u:%u\n",
        ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24,
        ntohs(client_addr.sin_port)
    );

    // set the new connection socket to non blocking
    fd_set_nb(connfd);

    Conn* conn = new Conn();
    conn->fd = connfd;
    conn->want_read = true;

    conn->incoming = new Buffer();
    conn->outgoing = new Buffer();

    buf_init(conn->incoming, 4096);
    buf_init(conn->outgoing, 4096);

    return conn;
}

static bool try_one_request(Conn* conn) {
    if(buf_size(conn->incoming) < 4) {
        return false; // don't even have enough for a header yet
    }

    uint32_t len = 0;
    memcpy(&len, conn->incoming->data_begin, 4);

    if(len > k_max_msg) {
        msg("too long");
        conn->want_close = true;
        return false;
    }

    if(4 + len > buf_size(conn->incoming)) {
        return false; // full data not arrived yet
    }

    const uint8_t *request = conn->incoming->data_begin + 4;

    // application logic : K-V Store (simple without data structures)

    std::vector<std::string> cmd;

    if(parse_request(request, len, cmd) < 0) {
        conn->want_close = true;
        return false;
    }

    size_t header_pos = 0;
    response_begin(*conn->outgoing, &header_pos);
    do_request(cmd, *conn->outgoing);
    response_end(*conn->outgoing, header_pos);

    // consume the processed request frame: 4-byte len header + body
    buf_consume(conn->incoming, 4 + len);

    return true;
}

static void handle_write(Conn* conn) {
    assert(buf_size(conn->outgoing) > 0);
    ssize_t rv = write(conn->fd, conn->outgoing->data_begin, buf_size(conn->outgoing));

    if(rv < 0 && errno == EAGAIN) {
        return;
    }

    if(rv < 0) {
        msg_errno("write() error");
        conn->want_close = true;
        return;
    }

    buf_consume(conn->outgoing, (size_t) rv);

    if(buf_size(conn->outgoing) == 0) {
        conn->want_read = true;
        conn->want_write = false;
    }
}

static void handle_read(Conn* conn) {
    uint8_t buf[64 * 1024];
    ssize_t rv = read(conn->fd, buf, sizeof(buf));

    if(rv < 0 && errno == EAGAIN) {
        return;
    }

    if(rv < 0) {
        msg_errno("read() error");
        conn->want_close = true;
        return;
    }

    if(rv == 0) { //EOF
        if(buf_size(conn->incoming) == 0) {
            msg("client closed");
        } else {
            msg("unexpected EOF");
        }
        conn->want_close = true;
        return;
    }

    buf_append(conn->incoming, buf, (size_t)rv);

    while(try_one_request(conn)) {}

    if (buf_size(conn->outgoing) > 0) {    // has a response
        conn->want_read = false;
        conn->want_write = true;
        return handle_write(conn);
    }
}

int main() {
    // the listening socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }
    int val = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
        die("setsockopt()");
    }

    // bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("bind()");
    }

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    // listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }

    std::vector<Conn *> fd2conn;
    std::vector<struct pollfd> poll_args;

    while(true) {
        poll_args.clear();
        // first the listening socket
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);
        // next the connection sockets
        for(Conn * conn : fd2conn) {
            if(!conn) continue;
            // poll for error first
            struct pollfd pfd = {conn->fd, POLLERR, 0};
            if (conn->want_read) {
                pfd.events |= POLLIN;
            }
            if (conn->want_write) {
                pfd.events |= POLLOUT;
            }
            poll_args.push_back(pfd);
        }

        int rv = poll(poll_args.data(), (nfds_t)poll_args.size(), -1);
        if (rv < 0 && errno == EINTR) {
            continue;
        }

        if(rv < 0) die("poll");

        if(poll_args[0].revents) {
            if(Conn *conn = handle_accept(fd)) {
                if (fd2conn.size() <= (size_t)conn->fd) {
                    fd2conn.resize(conn->fd + 1);
                }
                assert(!fd2conn[conn->fd]);
                fd2conn[conn->fd] = conn;
            }
        }

        // now handle connection sockets
        for (size_t i = 1; i < poll_args.size(); ++i) {
            uint32_t ready = poll_args[i].revents;
            if (ready == 0) {
                continue;
            }

            Conn *conn = fd2conn[poll_args[i].fd];
            if (ready & POLLIN) {
                assert(conn->want_read);
                handle_read(conn);  // application logic
            }
            if (ready & POLLOUT) {
                assert(conn->want_write);
                handle_write(conn); // application logic
            }

            // close the socket from socket error or application logic
            if ((ready & POLLERR) || conn->want_close) {
                (void)close(conn->fd);
                fd2conn[conn->fd] = NULL;
                free(conn->incoming->buffer_begin);
                free(conn->outgoing->buffer_begin);
                delete conn->incoming;
                delete conn->outgoing;
                delete conn;
            }
        }
    }
    return 0;
}