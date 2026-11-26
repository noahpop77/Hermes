#ifndef HERMES_H
#define HERMES_H

/*
    hermes.h - header-only C11 utility library.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Pre Processor macro to assist in aligning the data structure
// to be divisible by 64
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#define HERMES_ALIGNAS(n) _Alignas(n)
#else
#define HERMES_ALIGNAS(n)
#endif

#define HERMES_CACHELINE 64

typedef struct ringbuf {
    unsigned char *buf; /* owns allocated storage when initialized via ringbuf_init */
    size_t cap;         /* capacity in number of elements; always a power of two */
    size_t mask;        /* cap - 1, used for index masking */
    size_t elem_size;   /* bytes per element */

    /* Separate cache lines to reduce false sharing between producer and consumer. */
    HERMES_ALIGNAS(HERMES_CACHELINE) size_t head; /* total elements written */
    HERMES_ALIGNAS(HERMES_CACHELINE) size_t tail; /* total elements read */
} ringbuf;


/*

    Round up to the next power of two. Returns 0 on overflow. 
    
    If a number is already a power of 2 and passed to the function it is
    cheaper/faster to recalculate the power of 2 than to run a second logical 
    branch with a conditional to traverse and calculate it manually.

    x = 8

    x--         = 7
    Bit shift   = 8
    
    (Bit Shift) (Bit Smear)
    So the target number of
    40 = 00101000 becomes
    63 = 00111111 and since it gets + 1 it becomes
    64 = 01000000
    
*/
static inline size_t next_pow2(size_t x) {
    if (x <= 1) {
        return 1;
    }

    x--;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
        x |= x >> shift;
    }
    x++;

    if (x == 0) {
        /* Overflowed size_t. */
        return 0;
    }

    return x;
}

/*
    Initialize a ring buffer.
    - capacity is rounded up to a power of two (minimum 1).
    - elem_size must be non-zero.
    Returns 0 on success, -1 on invalid input or allocation failure.
    On failure, rb is zeroed and owns no storage.
    
    TLDR:
    A bunch of saftey checks in input parameters, defeault value assignment,
    more checks, and finally assigning the function parameters to the rinf buffer
    passed by reference
*/
static inline int ringbuf_init(ringbuf *rb, size_t capacity, size_t elem_size) {
    if (rb == NULL || elem_size == 0) {
        return -1;
    }

    rb->buf = NULL;
    rb->cap = 0;
    rb->mask = 0;
    rb->elem_size = 0;
    rb->head = 0;
    rb->tail = 0;

    size_t cap = next_pow2(capacity);
    if (cap == 0) {
        return -1;
    }

    /* Ensure cap * elem_size does not overflow. */
    if (cap > (SIZE_MAX / elem_size)) {
        return -1;
    }

    unsigned char *buf = (unsigned char *)malloc(cap * elem_size);
    if (buf == NULL) {
        return -1;
    }

    rb->buf = buf;
    rb->cap = cap;
    rb->mask = cap - 1;
    rb->elem_size = elem_size;
    rb->head = 0;
    rb->tail = 0;
    return 0;
}



/* 
    Very easy way of counting how many elements are in the buffer
*/
static inline size_t ringbuf_count(const ringbuf *rb) {
    if (rb == NULL) {
        return 0;
    }
    return rb->head - rb->tail;
}

/*
    Simple check to see if the buffer is empty. If the head and tail overlap
    without overwriting happening it means its empty. If the buffer is NULL then
    its obv also empty.
*/
static inline bool ringbuf_empty(const ringbuf *rb) {
    return rb == NULL || rb->head == rb->tail;
}

/*
    Opposite of ringbuf_empty. We check if the buffer is NULL and if 
    existing count in the buffer is the same size as the capcity.
*/
static inline bool ringbuf_full(const ringbuf *rb) {
    return rb != NULL && (rb->head - rb->tail) == rb->cap;
}


/*
    Checks to see if the buffer is instantiated properly, not full and if the element
    being pushed is not NULL as well.

    idx is gotten from taking the heade location and wrapping it around with the
    mask (cap - 1). Basically the ring in ring buffer.

    rb->buf is the start location of the buffer, from there we multiply idx and the
    element size to get the write/destination location. 

    From there we advance the head and return.
*/
static inline int ringbuf_push(ringbuf *rb, const void *elem) {
    if (rb == NULL || rb->buf == NULL || elem == NULL || rb->cap == 0) {
        return -1;
    }
    if (ringbuf_full(rb)) {
        return -1;
    }

    size_t idx = rb->head & rb->mask;   // Essentially head location mod length to wrap around
    memcpy(rb->buf + (idx * rb->elem_size), elem, rb->elem_size);
    rb->head++;
    return 0;
}


/*
    Legit same thing except it advances the tail hence overwriting the oldest
    element.
*/
static inline int ringbuf_push_overwrite(ringbuf *rb, const void *elem) {
    if (rb == NULL || rb->buf == NULL || elem == NULL || rb->cap == 0) {
        return -1;
    }

    if (ringbuf_full(rb)) {
        rb->tail++; /* drop oldest */
    }

    size_t idx = rb->head & rb->mask;
    memcpy(rb->buf + (idx * rb->elem_size), elem, rb->elem_size);
    rb->head++;
    return 0;
}

/*
    idx is looking for the tail index this time.

    Copies the value at the idx location to the variable location "out" and then
    advances the tail head. This leaves it outside of the tail - head scope and
    treated as non-existent. Available to be overwritten without the explicit
    overwrite function
*/
static inline int ringbuf_pop(ringbuf *rb, void *out) {
    if (rb == NULL || rb->buf == NULL || out == NULL || rb->cap == 0) {
        return -1;
    }
    if (ringbuf_empty(rb)) {
        return -1;
    }

    size_t idx = rb->tail & rb->mask;
    memcpy(out, rb->buf + (idx * rb->elem_size), rb->elem_size);
    rb->tail++;
    return 0;
}

/*
    Same usual checks.
    
    Copies the value located at the index to "out" and does nothing with the play head
    effectively just a "get me the value at X location". Simple.

*/
static inline int ringbuf_peek(const ringbuf *rb, void *out) {
    if (rb == NULL || rb->buf == NULL || out == NULL || rb->cap == 0) {
        return -1;
    }
    if (ringbuf_empty(rb)) {
        return -1;
    }

    size_t idx = rb->tail & rb->mask;
    memcpy(out, rb->buf + (idx * rb->elem_size), rb->elem_size);
    return 0;
}

/*
    Setting the play and tail to 0 technically zeroes out the buffer in the same way
    a quick format zeroes out your hard drive. Soft reset cause data is still there
    but able to be overwritten by default now.
*/
static inline void ringbuf_clear(ringbuf *rb) {
    if (rb == NULL) {
        return;
    }
    rb->head = 0;
    rb->tail = 0;
}

/*
    True clear, kek
*/
static inline void ringbuf_true_clear(ringbuf *rb) {
    if (rb == NULL) {
        return;
    }
    if (rb->buf != NULL && rb->cap > 0 && rb->elem_size > 0) {
        memset(rb->buf, 0, rb->cap * rb->elem_size);
    }
    rb->head = 0;
    rb->tail = 0;
}

/*

    Frees an instantiated ring buffer and zeroes out the data at the memory 
    location of its members.

    If we do not zero out the elements it the data can still exist in memory
    and be used if the allocator loops around to it. Can lead to inconsistent
    data.

*/
static inline void ringbuf_free(ringbuf *rb) {
    if (rb == NULL) {
        return;
    }
    free(rb->buf);
    rb->buf = NULL;
    rb->cap = 0;
    rb->mask = 0;
    rb->elem_size = 0;
    rb->head = 0;
    rb->tail = 0;
}


#endif /* HERMES_H */

