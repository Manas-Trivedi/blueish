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
#include <string>
// project
#include "hashtable.h"
#include "protocol.h"
#include "connection.h"
#include "kvstore.h"
#include "buffer.h"

// =============
// Utilities
// =============

static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
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
