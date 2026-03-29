#pragma once

#include <stdint.h>
#include <string>
#include <vector>

// ===========
// Protocol
// ===========

bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out);
bool read_str(const uint8_t *&cur, const uint8_t *end, uint32_t n, std::string &out);

// Parse a request and populate a vector of command strings
// Returns 0 on success, -1 on failure
int32_t parse_request(const uint8_t *data, size_t size, std::vector<std::string> &out);

struct Response {
    uint32_t status = 0;
    std::vector<uint8_t> data;
};

// Response status codes
enum ResponseStatus {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

struct Buffer;
void make_response(const Response &resp, struct Buffer *out);
