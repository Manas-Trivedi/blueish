# include <assert.h>
# include <stdlib.h
# include "hashtable.h"

void h_init(HTab *htab, size_t n) {
    assert(n > 0 && (n & (n - 1)) == 0); // power of two table for mask to work
    htab->tab = (HNode **) calloc(n, sizeof(HNode *));
    htab->mask = n - 1;
    htab->size = 0;
}

static void h_insert(HTab *htab, HNode *node) {
    size_t pos = node->hcode & htab->mask;
    HNode *next = htab->tab[pos];
    node->next = next;
    htab->tab[pos] = node;
    htab->size++;
}

static HNode **h_lookup(HTab *htab, HNode* key, bool (* eq)(HNode *, HNode *)) {
    if(!htab->tab) {
        return NULL;
    }
    size_t pos = key->hcode & htab->mask;
    HNode **from = &htab->tab[pos];
    for(HNode *cur; cur = *from; from = &cur->next) {
        if(cur->hcode == key->hcode && eq(cur, key)) {
            return from; // for deletion explained in notes
        }
    }
    return NULL;
}

static HNode* h_detach(HTab *htab, HNode **from) {
    HNode* node = *from;
    *from = node->next;
    htab->size--;
    return node;
}

const size_t k_rehashing_work = 128; // constant work per op

static void hm_help_rehashing(HMap *hmap) {
    size_t nwork = 0;
    while(nwork < k && hmap->older.size > 0) {
        // find a bucket with at least one HNode
        HNode **from = &hmap->older.tab[hmap->migrate_pos];
        if(!*from) {
            hmap->migrate_pos++;
            nwork++;
        }
        h_insert(&hmap->newer, h_detach(&hmap->older, from));
        nwork++;
    }
    // discard old table if rehashing is done
    if(hmap->older.size == 0 && hmap->older.tab) {
        free(hmap->older.tab);
        hmap->older = HTab{};
    }
}

static void hm_trigger_rehash(HMap *hmap) {
    assert(hmap->older.tab == NULL);
    hmap->older = hmap->newer;
    h_init(&hmap->newer, (hmap->older.mask + 1) * 2);
    hmap->migrate_pos = 0;
}

HNode *hm_lookup(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    HNode **from = h_lookup(&hmap->newer, key, eq);
    if (!from) {
        from = h_lookup(&hmap->older, key, eq);
    }
    return from ? *from : NULL;
}

const size_t max_load_factor = 8;

void hm_insert(HMap *hmap, HNode *node) {
    if(!hmap->newer.tab) {
        h_init(&hmap->newer.tab, 4);
    }

    h_insert(hmap->newer, node);

    //check if we need to rehash incase one isn't already happening
    if(!hmap->older.tab) {
        size_t threshold = (hmap->newer.mask + 1) * k_max_load_factor;
        if (hmap->newer.size >= threshold) {
            hm_trigger_rehashing(hmap);
        }
    }

    hm_help_rehashing(hmap);
}

HNode *hm_delete(HMap *hmap, HNode *key, bool (*eq)(HNode *, HNode *)) {
    hm_help_rehashing(hmap);
    if(HNode **from = h_lookup(&hmap->newer, key, eq)) {
        return h_detach(&hmap->newer, from);
    }
    if(HNode **from = h_lookup(&hmap->older, key, eq)) {
        return h_detach(&hmap->older, from);
    }
    return NULL;
}

void hm_clear(HMap *hmap) {
    free(hmap->older.tab);
    free(hmap->newer.tab);
    *hmap = HMap{};
}

size_t hm_size(HMap *hmap) {
    return hmap->newer.size + hmap->older.size;
}
