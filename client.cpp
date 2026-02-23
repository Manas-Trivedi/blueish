#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <string>
#include <vector>
#include <iostream>
#include <sstream>
#include <cctype>

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

static int32_t read_full(int fd, char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = read(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error, or unexpected EOF
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = write(fd, buf, n);
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

// =================
// Protocol Helpers
// =================

const size_t k_max_msg = 4096;

static int32_t send_req(int fd, const std::vector<std::string> &cmd) {
    uint32_t len = 4;
    for (const std::string &s : cmd) {
        len += 4 + s.size();
    }
    if (len > k_max_msg) {
        return -1;
    }

    char wbuf[4 + k_max_msg];
    memcpy(&wbuf[0], &len, 4);  // assume little endian

    uint32_t n = (uint32_t)cmd.size();
    memcpy(&wbuf[4], &n, 4);

    size_t cur = 8;
    for (const std::string &s : cmd) {
        uint32_t p = (uint32_t)s.size();
        memcpy(&wbuf[cur], &p, 4);
        memcpy(&wbuf[cur + 4], s.data(), s.size());
        cur += 4 + s.size();
    }

    return write_all(fd, wbuf, 4 + len);
}

static int32_t read_res(int fd) {
    char rbuf[4 + k_max_msg];

    errno = 0;
    int32_t err = read_full(fd, rbuf, 4);  // header (len)
    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }

    err = read_full(fd, &rbuf[4], len);  // response body
    if (err) {
        msg("read() error");
        return err;
    }

    if (len < 4) {
        msg("bad response");
        return -1;
    }

    uint32_t rescode = 0;
    memcpy(&rescode, &rbuf[4], 4);
    printf("server says: [%u] %.*s\n", rescode, (int)(len - 4), &rbuf[8]);
    return 0;
}

// ==================
// Socket Connection
// ==================

static int connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        die("socket()");
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1

    int rv = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
    if (rv) {
        die("connect");
    }

    return fd;
}

// =====================
// Client Command Modes
// =====================

static std::string to_lower(std::string s) {
    for (char &ch : s) {
        ch = (char)std::tolower((unsigned char)ch);
    }
    return s;
}

static void print_manual_help() {
    msg("commands:\n");
    msg("- GET <key>");
    msg("- SET <key> <value>");
    msg("- DEL <key>");
    msg("- HELP");
    msg("- EXIT");
}

static bool parse_manual_line(const std::string &line, std::vector<std::string> &cmd) {
    cmd.clear();

    std::istringstream input(line);
    std::string token;
    while (input >> token) {
        cmd.push_back(token);
    }

    if (cmd.empty()) {
        return false;
    }

    std::string op = to_lower(cmd[0]);
    if (op == "help" || op == "exit" || op == "quit") {
        cmd[0] = op;
        cmd.resize(1);
        return true;
    }

    if ((op == "get" || op == "del") && cmd.size() == 2) {
        cmd[0] = op;
        return true;
    }

    if (op == "set" && cmd.size() >= 3) {
        std::string value = cmd[2];
        for (size_t i = 3; i < cmd.size(); ++i) {
            value += " ";
            value += cmd[i];
        }
        cmd[0] = "set";
        cmd[2] = value;
        cmd.resize(3);
        return true;
    }

    return false;
}

static int32_t run_single_request_mode(int fd, int argc, char **argv) {
    std::vector<std::string> cmd;
    for (int i = 1; i < argc; ++i) {
        cmd.push_back(argv[i]);
    }

    int32_t err = send_req(fd, cmd);
    if (err) {
        return err;
    }

    return read_res(fd);
}

static int32_t run_manual_mode(int fd) {
    msg("manual mode enabled");
    print_manual_help();

    while (true) {
        printf("> ");
        fflush(stdout);

        std::string line;
        if (!std::getline(std::cin, line)) {
            msg("stdin closed");
            return 0;
        }

        std::vector<std::string> cmd;
        if (!parse_manual_line(line, cmd)) {
            msg("invalid command");
            print_manual_help();
            continue;
        }

        if (cmd[0] == "help") {
            print_manual_help();
            continue;
        }

        if (cmd[0] == "exit" || cmd[0] == "quit") {
            return 0;
        }

        int32_t err = send_req(fd, cmd);
        if (err) {
            msg("write() error");
            return err;
        }

        err = read_res(fd);
        if (err) {
            return err;
        }
    }
}

// ==================
// Client Entry Flow
// ==================

int main(int argc, char **argv) {
    int fd = connect_to_server();

    int32_t err = 0;
    if (argc >= 2 && strcmp(argv[1], "--manual") == 0) {
        err = run_manual_mode(fd);
    } else {
        if (argc < 2) {
            msg("usage: ./client --manual OR ./client <get|set|del> ...");
            goto L_DONE;
        }
        err = run_single_request_mode(fd, argc, argv);
    }

L_DONE:
    close(fd);
    return 0;
}
