#include "kvstore.h"
#include "hashtable.h"
#include <cassert>
#include <cstring>

# define container_of(ptr, T, member) \
    ((T *)( (char *)ptr - offsetof(T, member) ))

// Global data store
KVStoreData g_data = KVStoreData{};

bool entry_eq(HNode *lhs, HNode *rhs) {
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
uint32_t str_hash(const uint8_t *data, size_t len) {
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

void do_get(std::vector<std::string>& cmd, Response &out) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *) key.key.data(), key.key.size());
    // query the table
    HNode *node = hm_lookup(&g_data.db, &key.node, &entry_eq);
    if(!node) {
        out.status = RES_NX;
        return;
    }
    std::string &val = container_of(node, Entry, node)->val;
    assert(val.size() < (32 << 20));  // k_max_msg
    out.data.assign(val.begin(), val.end());
}

void do_set(std::vector<std::string> &cmd, Response &) {
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
}

void do_del(std::vector<std::string> &cmd, Response &) {
    Entry key;
    key.key.swap(cmd[1]);
    key.node.hcode = str_hash((uint8_t *)key.key.data(), key.key.size());
    HNode *node = hm_delete(&g_data.db, &key.node, &entry_eq);
    if (node) {
        delete container_of(node, Entry, node);
    }
}

void do_request(std::vector<std::string> &cmd, Response &out) {
    if (cmd.size() == 2 && cmd[0] == "get") {
        return do_get(cmd, out);
    } else if (cmd.size() == 3 && cmd[0] == "set") {
        return do_set(cmd, out);
    } else if (cmd.size() == 2 && cmd[0] == "del") {
        return do_del(cmd, out);
    } else {
        out.status = RES_ERR;
    }
}
