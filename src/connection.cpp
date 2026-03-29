#include "connection.h"
#include "protocol.h"
#include "kvstore.h"
#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <cassert>
#include <fcntl.h>

const size_t k_max_msg = 32 << 20;

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void msg_errno(const char *msg) {
    fprintf(stderr, "[errno: %d] %s\n", errno, msg);
}

static void fd_set_nb(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if(errno) {
        perror("fcntl() error");
        abort();
    }
    flags |= O_NONBLOCK;

    errno = 0;
    (void)fcntl(fd, F_SETFL, flags);
    if(errno) {
        perror("fcntl() error");
        abort();
    }
}

Conn* handle_accept(int fd) {
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

bool try_one_request(Conn* conn) {
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

    Response resp;
    do_request(cmd, resp);
    make_response(resp, conn->outgoing);

    // consume the processed request frame: 4-byte len header + body
    buf_consume(conn->incoming, 4 + len);

    return true;
}

void handle_write(Conn* conn) {
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

void handle_read(Conn* conn) {
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
