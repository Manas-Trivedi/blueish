#pragma once

#include "hashtable.h"
#include "protocol.h"
#include <string>
#include <vector>

// =====================
// Application Logic
// =====================

struct Entry {
    struct HNode node;
    std::string key;
    std::string val;
};

struct KVStoreData {
    uint32_t status;
    HMap db;
};

// Global data store
extern KVStoreData g_data;

// Hash comparison function for entries
bool entry_eq(HNode *lhs, HNode *rhs);

// MurmurHash3 (x86_32)
uint32_t str_hash(const uint8_t *data, size_t len);

// Command handlers
void do_get(std::vector<std::string>& cmd, Response &out);
void do_set(std::vector<std::string> &cmd, Response &out);
void do_del(std::vector<std::string> &cmd, Response &out);
void do_request(std::vector<std::string> &cmd, Response &out);
