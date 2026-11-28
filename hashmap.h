#ifndef HASHMAP_H
#define HASHMAP_H

/*
    hashmap.h - header-only C11 generic open-addressing hashmap.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    HASHMAP_SLOT_EMPTY = 0,
    HASHMAP_SLOT_OCCUPIED = 1,
    HASHMAP_SLOT_TOMBSTONE = 2
} hashmap_slot_state;


typedef struct {
    unsigned char *keys;    // Ex, 64 bytes     // 4 bytes per (int = 4 bytes)
    unsigned char *vals;    // Ex. 64 bytes     // 4 bytes per (int = 4 bytes)
    unsigned char *states;  // Ex. 16 bytes     // 1 byte per  (enum values = 1 byte)
    size_t cap;             // Ex. 16 entity cap
    size_t len;             // Number of live cells - used for hashmap growth functions
    size_t tombstones;      // Tombstone count - used for same capacity rehash functions
    size_t key_size;        // Key size in bytes
    size_t val_size;        // Val size in bytes
    size_t (*hash)(const void *key, void *ctx);             // Function pointer for hash
    int (*eq)(const void *a, const void *b, void *ctx);     // Function pointer for eq
    void *ctx;      // Used to pass the same info to multiple functions, in this case hash() and eq() 
} hashmap;

// Buffer overflow safe multiplication.
static inline int hash_multiply(size_t a, size_t b, size_t *out) {
    if (a != 0) {
        if (b > SIZE_MAX / a) {
            return -1;
        }
    }

    *out = a * b;
    return 0;
}

// Round up to the next power of two.
static inline size_t hashmap_next_pow2(size_t x) {
    if (x <= 1) {
        return 1;
    }

    x = x - 1;

    size_t shift = 1;
    size_t bit_count = sizeof(size_t) * 8;
    while (shift < bit_count) {
        x = x | (x >> shift);
        shift = shift << 1;
    }

    x = x + 1;

    if (x == 0) {
        return 0;
    }

    return x;
}

/*
    Basically same two functions. 
    They use the index and math out the byte offset to get the location of the key or val
*/
static inline unsigned char *hashmap_key_at(const hashmap *hm, size_t idx) {
    size_t byte_offset = idx * hm->key_size;
    return hm->keys + byte_offset;
}
static inline unsigned char *hashmap_val_at(const hashmap *hm, size_t idx) {
    size_t byte_offset = idx * hm->val_size;
    return hm->vals + byte_offset;
}

/*
    Initialize a hashmap.
    
    capacity is rounded up to a power of two (minimum 8). Hash and eq must be non-NULL.
    
    TON of preflight and sanity checks
*/
static inline int hashmap_init(
    hashmap *hm,        // hashmap address
    size_t key_size,    // key size
    size_t val_size,    // val size
    size_t capacity,    // capacity
    size_t (*hash)(const void *key, void *ctx),         // hash function pointer
    int (*eq)(const void *a, const void *b, void *ctx), // eq function pointer
    void *ctx           // void ctx for sending context to 2 hash and eq at same time
) {
    if (hm == NULL) {
        return -1;
    }
    
    // Effectively zeroed out hashmap
    hm->keys = NULL;
    hm->vals = NULL;
    hm->states = NULL;
    hm->cap = 0;
    hm->len = 0;
    hm->tombstones = 0;
    hm->key_size = 0;
    hm->val_size = 0;
    hm->hash = NULL;
    hm->eq = NULL;
    hm->ctx = NULL;

    if (key_size == 0) {
        return -1;
    }

    if (val_size == 0) {
        return -1;
    }

    if (hash == NULL) {
        return -1;
    }

    if (eq == NULL) {
        return -1;
    }
    
    // Minimum capacity required for hashmap
    size_t cap = capacity;
    if (cap < 8) {
        cap = 8;
    }
    
    // Rounds to pow of 2 so we can use bitwiwse operations on it for SPEEEEED
    cap = hashmap_next_pow2(cap);
    if (cap == 0) {
        return -1;
    }

    // Calculates key and val size by bytes
    size_t keys_bytes = 0;
    int keys_mul_result = hash_multiply(cap, key_size, &keys_bytes);
    if (keys_mul_result != 0) {
        return -1;
    }
    size_t vals_bytes = 0;
    int vals_mul_result = hash_multiply(cap, val_size, &vals_bytes);
    if (vals_mul_result != 0) {
        return -1;
    }

    // More checks
    unsigned char *keys = (unsigned char *)malloc(keys_bytes);
    if (keys == NULL) {
        return -1;
    }
    unsigned char *vals = (unsigned char *)malloc(vals_bytes);
    if (vals == NULL) {
        free(keys);
        return -1;
    }
    unsigned char *states = (unsigned char *)malloc(cap);
    if (states == NULL) {
        free(vals);
        free(keys);
        return -1;
    }

    // Sets passed in values post sanity checks to the hashmap
    memset(states, HASHMAP_SLOT_EMPTY, cap);
    hm->keys = keys;
    hm->vals = vals;
    hm->states = states;
    hm->cap = cap;
    hm->len = 0;
    hm->tombstones = 0;
    hm->key_size = key_size;
    hm->val_size = val_size;
    hm->hash = hash;
    hm->eq = eq;
    hm->ctx = ctx;
    return 0;
}

/*
    Slams the free and zeroes out the memory on a passed in hashmap
*/
static inline void hashmap_free(hashmap *hm) {
    if (hm == NULL) {
        return;
    }

    free(hm->keys);
    free(hm->vals);
    free(hm->states);

    hm->keys = NULL;
    hm->vals = NULL;
    hm->states = NULL;
    hm->cap = 0;
    hm->len = 0;
    hm->tombstones = 0;
    hm->key_size = 0;
    hm->val_size = 0;
    hm->hash = NULL;
    hm->eq = NULL;
    hm->ctx = NULL;
}

/*
    Remove all entries but keep allocated storage.
    Makes it usable for the future but if you look at the memory location
    in isolation you will see the values located there
*/
static inline void hashmap_clear(hashmap *hm) {
    if (hm == NULL) {
        return;
    }

    if (hm->states != NULL) {
        if (hm->cap > 0) {
            memset(hm->states, HASHMAP_SLOT_EMPTY, hm->cap);
        }
    }

    hm->len = 0;
    hm->tombstones = 0;
}

// minus 1 kek
static inline size_t hashmap_mask(const hashmap *hm) {
    return hm->cap - 1;
}

/*
    Find the slot for a key.
    If found, sets *found_idx to the slot index and returns true.
    If not found, sets *insert_idx to the slot where the key may be inserted
    (first tombstone if any, otherwise first empty slot) and returns false.
*/
static inline bool hashmap_find_slot(
    const hashmap *hm,
    const void *key,
    size_t *found_idx,
    size_t *insert_idx
) {
    size_t hash_value = hm->hash(key, hm->ctx);
    size_t idx = hash_value & hashmap_mask(hm);

    bool has_tombstone = false;
    size_t first_tombstone = 0;

    size_t probed = 0;
    while (probed < hm->cap) {
        unsigned char state = hm->states[idx];

        if (state == HASHMAP_SLOT_EMPTY) {
            if (has_tombstone) {
                *insert_idx = first_tombstone;
            } else {
                *insert_idx = idx;
            }

            return false;
        }

        if (state == HASHMAP_SLOT_TOMBSTONE) {
            if (!has_tombstone) {
                has_tombstone = true;
                first_tombstone = idx;
            }
        }

        if (state == HASHMAP_SLOT_OCCUPIED) {
            const unsigned char *existing_key = hashmap_key_at(hm, idx);
            int keys_equal = hm->eq(existing_key, key, hm->ctx);
            if (keys_equal != 0) {
                *found_idx = idx;
                return true;
            }
        }

        idx = (idx + 1) & hashmap_mask(hm);
        probed = probed + 1;
    }

    if (has_tombstone) {
        *insert_idx = first_tombstone;
        return false;
    }

    *insert_idx = hm->cap;
    return false;
}

/*
    Rebuild the hashmap with a new capacity.
    Returns 0 on success, -1 on invalid input or allocation failure.
*/
static inline int hashmap_rehash(hashmap *hm, size_t new_capacity) {
    if (hm == NULL) {
        return -1;
    }

    if (hm->key_size == 0) {
        return -1;
    }

    if (hm->val_size == 0) {
        return -1;
    }

    size_t cap = new_capacity;
    if (cap < 8) {
        cap = 8;
    }

    cap = hashmap_next_pow2(cap);
    if (cap == 0) {
        return -1;
    }

    if (cap < hm->len) {
        return -1;
    }

    size_t keys_bytes = 0;
    int keys_mul_result = hash_multiply(cap, hm->key_size, &keys_bytes);
    if (keys_mul_result != 0) {
        return -1;
    }

    size_t vals_bytes = 0;
    int vals_mul_result = hash_multiply(cap, hm->val_size, &vals_bytes);
    if (vals_mul_result != 0) {
        return -1;
    }

    unsigned char *new_keys = (unsigned char *)malloc(keys_bytes);
    if (new_keys == NULL) {
        return -1;
    }

    unsigned char *new_vals = (unsigned char *)malloc(vals_bytes);
    if (new_vals == NULL) {
        free(new_keys);
        return -1;
    }

    unsigned char *new_states = (unsigned char *)malloc(cap);
    if (new_states == NULL) {
        free(new_vals);
        free(new_keys);
        return -1;
    }

    memset(new_states, HASHMAP_SLOT_EMPTY, cap);

    hashmap new_hm;
    new_hm.keys = new_keys;
    new_hm.vals = new_vals;
    new_hm.states = new_states;
    new_hm.cap = cap;
    new_hm.len = 0;
    new_hm.tombstones = 0;
    new_hm.key_size = hm->key_size;
    new_hm.val_size = hm->val_size;
    new_hm.hash = hm->hash;
    new_hm.eq = hm->eq;
    new_hm.ctx = hm->ctx;

    size_t old_idx = 0;
    while (old_idx < hm->cap) {
        if (hm->states[old_idx] == HASHMAP_SLOT_OCCUPIED) {
            const unsigned char *key = hashmap_key_at(hm, old_idx);
            const unsigned char *val = hashmap_val_at(hm, old_idx);

            size_t found_idx = 0;
            size_t insert_idx = 0;
            bool found = hashmap_find_slot(&new_hm, key, &found_idx, &insert_idx);
            if (found) {
                free(new_states);
                free(new_vals);
                free(new_keys);
                return -1;
            }

            if (insert_idx >= new_hm.cap) {
                free(new_states);
                free(new_vals);
                free(new_keys);
                return -1;
            }

            memcpy(hashmap_key_at(&new_hm, insert_idx), key, new_hm.key_size);
            memcpy(hashmap_val_at(&new_hm, insert_idx), val, new_hm.val_size);
            new_hm.states[insert_idx] = HASHMAP_SLOT_OCCUPIED;
            new_hm.len = new_hm.len + 1;
        }

        old_idx = old_idx + 1;
    }

    free(hm->keys);
    free(hm->vals);
    free(hm->states);

    hm->keys = new_hm.keys;
    hm->vals = new_hm.vals;
    hm->states = new_hm.states;
    hm->cap = new_hm.cap;
    hm->len = new_hm.len;
    hm->tombstones = 0;

    return 0;
}

/*
    Ensure there is room for at least one more insertion without exceeding
    a 70% load factor. Rehashes if needed.
    Returns 0 on success, -1 on failure.
*/
static inline int hashmap_reserve(hashmap *hm, size_t min_count) {
    if (hm == NULL) {
        return -1;
    }

    size_t required = min_count;
    if (required < hm->len) {
        required = hm->len;
    }

    size_t needed_cap = required;
    if (needed_cap > SIZE_MAX / 10) {
        return -1;
    }

    needed_cap = (needed_cap * 10) / 7;
    needed_cap = needed_cap + 1;

    if (needed_cap <= hm->cap) {
        if (hm->tombstones > hm->cap / 4) {
            return hashmap_rehash(hm, hm->cap);
        }

        return 0;
    }

    return hashmap_rehash(hm, needed_cap);
}

/*
    Insert or update a key/value pair.
    If the key already exists, its value is replaced.
    Returns 0 on success, -1 on invalid input or allocation failure.
*/
static inline int hashmap_put(hashmap *hm, const void *key, const void *val) {
    if (hm == NULL) {
        return -1;
    }

    if (key == NULL) {
        return -1;
    }

    if (val == NULL) {
        return -1;
    }

    if (hm->cap == 0) {
        return -1;
    }

    int reserve_result = hashmap_reserve(hm, hm->len + 1);
    if (reserve_result != 0) {
        return -1;
    }

    size_t found_idx = 0;
    size_t insert_idx = 0;
    bool found = hashmap_find_slot(hm, key, &found_idx, &insert_idx);

    if (found) {
        unsigned char *dest_val = hashmap_val_at(hm, found_idx);
        memcpy(dest_val, val, hm->val_size);
        return 0;
    }

    if (insert_idx >= hm->cap) {
        return -1;
    }

    if (hm->states[insert_idx] == HASHMAP_SLOT_TOMBSTONE) {
        if (hm->tombstones > 0) {
            hm->tombstones = hm->tombstones - 1;
        }
    }

    unsigned char *dest_key = hashmap_key_at(hm, insert_idx);
    unsigned char *dest_val = hashmap_val_at(hm, insert_idx);

    memcpy(dest_key, key, hm->key_size);
    memcpy(dest_val, val, hm->val_size);
    hm->states[insert_idx] = HASHMAP_SLOT_OCCUPIED;
    hm->len = hm->len + 1;

    return 0;
}

/*
    Retrieve the value for a key.
    If out is not NULL and the key is found, the value is copied into out.
    Returns 0 if found, -1 if not found or invalid input.
*/
static inline int hashmap_get(const hashmap *hm, const void *key, void *out) {
    if (hm == NULL) {
        return -1;
    }

    if (key == NULL) {
        return -1;
    }

    if (hm->cap == 0) {
        return -1;
    }

    size_t found_idx = 0;
    size_t insert_idx = 0;
    bool found = hashmap_find_slot(hm, key, &found_idx, &insert_idx);
    if (!found) {
        return -1;
    }

    if (out != NULL) {
        const unsigned char *src_val = hashmap_val_at(hm, found_idx);
        memcpy(out, src_val, hm->val_size);
    }

    return 0;
}

/*
    Check whether a key exists in the hashmap.
*/
static inline bool hashmap_contains(const hashmap *hm, const void *key) {
    int get_result = hashmap_get(hm, key, NULL);
    if (get_result == 0) {
        return true;
    }

    return false;
}

/*
    Remove a key from the hashmap.
    If out is not NULL and the key is found, the value is copied into out.
    Returns 0 if removed, -1 if not found or invalid input.
*/
static inline int hashmap_remove(hashmap *hm, const void *key, void *out) {
    if (hm == NULL) {
        return -1;
    }

    if (key == NULL) {
        return -1;
    }

    if (hm->cap == 0) {
        return -1;
    }

    size_t found_idx = 0;
    size_t insert_idx = 0;
    bool found = hashmap_find_slot(hm, key, &found_idx, &insert_idx);
    if (!found) {
        return -1;
    }

    if (out != NULL) {
        const unsigned char *src_val = hashmap_val_at(hm, found_idx);
        memcpy(out, src_val, hm->val_size);
    }

    hm->states[found_idx] = HASHMAP_SLOT_TOMBSTONE;
    if (hm->len > 0) {
        hm->len = hm->len - 1;
    }
    hm->tombstones = hm->tombstones + 1;

    return 0;
}

/*
    Return the number of occupied entries.
*/
static inline size_t hashmap_count(const hashmap *hm) {
    if (hm == NULL) {
        return 0;
    }

    return hm->len;
}

#endif /* HASHMAP_H */
