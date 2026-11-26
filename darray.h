#ifndef DARRAY_H
#define DARRAY_H

/*
    hermes.h - header-only C11 utility library.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *buf;
    size_t len;
    size_t cap;
    size_t elem_size;
} darray;


/*
    Buffer overflow safe multiplication.
    Returns 0 on success, -1 on overflow.
*/
static inline int multiply(size_t a, size_t b, size_t *out) {
    if (a != 0) {
        if (b > SIZE_MAX / a) {
            return -1;
        }
    }

    *out = a * b;
    return 0;
}


/*
    Returns the address of the slot at idx.
*/
static inline unsigned char *darray_peek(const darray *da, size_t idx) {
    size_t byte_offset = idx * da->elem_size;
    return da->buf + byte_offset;
}


/*
    Checks into zero initialization into allocating memory with total_bytes and
    assignment of mem and initial_cap to the dynamic array
*/
static inline int darray_init(darray *da, size_t elem_size, size_t initial_cap) {
    if (da == NULL) {
        return -1;
    }

    if (elem_size == 0) {
        return -1;
    }

    da->buf = NULL;
    da->len = 0;
    da->cap = 0;
    da->elem_size = elem_size;

    if (initial_cap == 0) {
        return 0;
    }

    size_t total_bytes = 0;
    int multiply_result = multiply(initial_cap, elem_size, &total_bytes);
    if (multiply_result != 0) {
        return -1;
    }

    unsigned char *mem = (unsigned char *)malloc(total_bytes);
    if (mem == NULL) {
        return -1;
    }

    da->buf = mem;
    da->cap = initial_cap;
    return 0;
}


/*
    Ensure the array has capacity for at least min_cap elements.
    Grows by doubling the current capacity until min_cap is reached.
*/
static inline int darray_reserve(darray *da, size_t min_cap) {
    if (da == NULL) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    if (min_cap <= da->cap) {
        return 0;
    }

    size_t new_cap = da->cap;
    if (new_cap == 0) {
        new_cap = 8;
    }

    while (new_cap < min_cap) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = min_cap;
            break;
        }

        new_cap = new_cap * 2;
    }

    size_t total_bytes = 0;
    int multiply_result = multiply(new_cap, da->elem_size, &total_bytes);
    if (multiply_result != 0) {
        return -1;
    }

    unsigned char *grown = (unsigned char *)realloc(da->buf, total_bytes);
    if (grown == NULL) {
        return -1;
    }

    da->buf = grown;
    da->cap = new_cap;
    return 0;
}


/*
    Checks into appending an element to the end of the array. Also expands darray
    if we are close to edge with darray_reserve
*/
static inline int darray_push(darray *da, const void *elem) {
    if (da == NULL) {
        return -1;
    }

    if (elem == NULL) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    if (da->len == SIZE_MAX) {
        return -1;
    }

    if (da->len == da->cap) {
        int reserve_result = darray_reserve(da, da->len + 1);
        if (reserve_result != 0) {
            return -1;
        }
    }

    unsigned char *dest = darray_peek(da, da->len);
    memcpy(dest, elem, da->elem_size);
    da->len = da->len + 1;
    return 0;
}


/*
    Remove the last element from the array.
    If out is not NULL, the removed element is copied into out.
*/
static inline int darray_pop(darray *da, void *out) {
    if (da == NULL) {
        return -1;
    }

    if (da->len == 0) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    da->len = da->len - 1;

    if (out != NULL) {
        const unsigned char *src = darray_peek(da, da->len);
        memcpy(out, src, da->elem_size);
    }

    return 0;
}


/*
    Copy the element at idx into out.
    Same as pop but the value isnt removed.
*/
static inline int darray_get(const darray *da, size_t idx, void *out) {
    if (da == NULL) {
        return -1;
    }

    if (out == NULL) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    if (idx >= da->len) {
        return -1;
    }

    const unsigned char *src = darray_peek(da, idx);
    memcpy(out, src, da->elem_size);
    return 0;
}


/*
    Insert an element at idx, shifting elements at and after idx to the right.
    O(n) time complexity for the move everything down operation on insert. 
*/
static inline int darray_insert(darray *da, size_t idx, const void *elem) {
    if (da == NULL) {
        return -1;
    }

    if (elem == NULL) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    if (idx > da->len) {
        return -1;
    }

    if (da->len == SIZE_MAX) {
        return -1;
    }

    if (da->len == da->cap) {
        int reserve_result = darray_reserve(da, da->len + 1);
        if (reserve_result != 0) {
            return -1;
        }
    }

    if (idx < da->len) {
        unsigned char *dest = darray_peek(da, idx + 1);
        unsigned char *src = darray_peek(da, idx);
        size_t bytes_to_move = (da->len - idx) * da->elem_size;
        memmove(dest, src, bytes_to_move);
    }

    unsigned char *dest = darray_peek(da, idx);
    memcpy(dest, elem, da->elem_size);
    da->len = da->len + 1;
    return 0;
}


/*
    Remove the element at idx by moving the last element into its place.
    
    Super fast BUT this does not preserve element order!!!!

    so if there was an array like the following and you wanted to remove 2
    [1, 2, 3, 4 ,5]
    [1, 5, 3, 4]
    
    reduces len and changes the values but no shift happened so this entire thing
    was O(1) time.

    If out is not NULL, the removed element is copied into out.
*/
static inline int darray_remove_unordered(darray *da, size_t idx, void *out) {
    if (da == NULL) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    if (idx >= da->len) {
        return -1;
    }

    if (out != NULL) {
        const unsigned char *src = darray_peek(da, idx);
        memcpy(out, src, da->elem_size);
    }

    da->len = da->len - 1;

    if (idx < da->len) {
        unsigned char *dest = darray_peek(da, idx);
        const unsigned char *src = darray_peek(da, da->len);
        memcpy(dest, src, da->elem_size);
    }

    return 0;
}


/*
    Remove the element at idx and shift later elements left to preserve order.
    If out is not NULL, the removed element is copied into out.
*/
static inline int darray_remove_ordered(darray *da, size_t idx, void *out) {
    if (da == NULL) {
        return -1;
    }

    if (da->elem_size == 0) {
        return -1;
    }

    if (idx >= da->len) {
        return -1;
    }

    if (out != NULL) {
        const unsigned char *src = darray_peek(da, idx);
        memcpy(out, src, da->elem_size);
    }

    da->len = da->len - 1;

    if (idx < da->len) {
        unsigned char *dest = darray_peek(da, idx);
        const unsigned char *src = darray_peek(da, idx + 1);
        size_t bytes_to_move = (da->len - idx) * da->elem_size;
        memmove(dest, src, bytes_to_move);
    }

    return 0;
}


/*
    Set the array length to 0.
    If zero_memory is true, the allocated buffer is zeroed.
*/
static inline void darray_clear(darray *da, bool zero_memory) {
    if (da == NULL) {
        return;
    }

    if (zero_memory) {
        bool has_buffer = da->buf != NULL;
        bool has_capacity = da->cap > 0;
        bool has_elem_size = da->elem_size > 0;

        if (has_buffer && has_capacity && has_elem_size) {
            size_t total_bytes = 0;
            int multiply_result = multiply(da->cap, da->elem_size, &total_bytes);
            if (multiply_result == 0) {
                memset(da->buf, 0, total_bytes);
            }
        }
    }

    da->len = 0;
}


/*
    Free the array buffer and reset all fields.
*/
static inline void darray_free(darray *da) {
    if (da == NULL) {
        return;
    }

    free(da->buf);
    da->buf = NULL;
    da->len = 0;
    da->cap = 0;
    da->elem_size = 0;
}

#endif /* DARRAY_H */
