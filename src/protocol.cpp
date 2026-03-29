#include "protocol.h"
#include "buffer.h"

const size_t k_max_msg = 32 << 20;
const size_t k_max_args = 200 * 1000;

bool read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if(cur + 4 > end) {
        return false;
    }

    memcpy(&out, cur, 4);
    cur += 4;

    return true;
}

bool read_str(const uint8_t *&cur, const uint8_t *end, uint32_t n, std::string &out) {
    if(cur + n > end) {
        return false;
    }

    out.assign(cur, cur + n);
    cur += n;
    return true;
}

int32_t parse_request(const uint8_t *data, size_t size, std::vector<std::string> &out){
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

void make_response(const Response &resp, struct Buffer *out) {
    uint32_t resp_len = 4 + (uint32_t)resp.data.size();
    buf_append(out, (const uint8_t *)&resp_len, 4);
    buf_append(out, (const uint8_t *)&resp.status, 4);
    buf_append(out, resp.data.data(), resp.data.size());
}
