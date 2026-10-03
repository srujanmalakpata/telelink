#include "tl/ringbuf.h"

bool tl_rb_init(tl_ringbuf *rb, uint8_t *storage, size_t capacity)
{
    if (rb == NULL || storage == NULL || capacity < 2u || capacity > (size_t)0x80000000u ||
        (capacity & (capacity - 1u)) != 0u) {
        return false;
    }
    rb->buf = storage;
    rb->mask = (uint32_t)(capacity - 1u);
    atomic_init(&rb->head, 0u);
    atomic_init(&rb->tail, 0u);
    return true;
}

bool tl_rb_push(tl_ringbuf *rb, uint8_t byte)
{
    const uint32_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    if ((uint32_t)(head - tail) > rb->mask) {
        return false; /* full: head - tail == capacity */
    }
    rb->buf[head & rb->mask] = byte;
    atomic_store_explicit(&rb->head, head + 1u, memory_order_release);
    return true;
}

bool tl_rb_pop(tl_ringbuf *rb, uint8_t *out)
{
    const uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    if (head == tail) {
        return false;
    }
    *out = rb->buf[tail & rb->mask];
    atomic_store_explicit(&rb->tail, tail + 1u, memory_order_release);
    return true;
}

size_t tl_rb_pop_many(tl_ringbuf *rb, uint8_t *out, size_t max)
{
    const uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    size_t n = (size_t)(uint32_t)(head - tail);
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = rb->buf[(tail + (uint32_t)i) & rb->mask];
    }
    if (n > 0u) {
        atomic_store_explicit(&rb->tail, tail + (uint32_t)n, memory_order_release);
    }
    return n;
}

size_t tl_rb_count(const tl_ringbuf *rb)
{
    /* From either owner this is exact-or-conservative: the caller's own counter
     * is stable and the other counter only moves in the "safe" direction.
     * The clamp keeps a call from a third context inside [0, capacity]. */
    const uint32_t tail = atomic_load_explicit(&rb->tail, memory_order_acquire);
    const uint32_t head = atomic_load_explicit(&rb->head, memory_order_acquire);
    const uint32_t n = head - tail;
    return (size_t)(n > rb->mask + 1u ? rb->mask + 1u : n);
}

size_t tl_rb_capacity(const tl_ringbuf *rb)
{
    return (size_t)rb->mask + 1u;
}
