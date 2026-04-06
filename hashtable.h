# pragma once
# include <stddef.h>
# include <stdint.h>

struct HNode {
    HNode *next = NULL;
    int hcode = 0;
};

struct HTab { // array of HNode*
    // pointer to the first HNode
    HNode **tab = NULL;
    size_t mask = 0; // to avoid modulus
    size_t size = 0;
};

struct HMap { //main structure for incremental resizing
    HTab newer;
    HTab older;
    size_t migrate_pos;
};

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
void   hm_insert(HMap *hmap, HNode *node);
HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *));
void   hm_clear(HMap *hmap);
size_t hm_size(HMap *hmap);
void hm_foreach(HMap *, bool (*cb)(HNode *, void *), void *);