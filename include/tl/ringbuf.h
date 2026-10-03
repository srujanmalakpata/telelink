/*
 * Lock-free single-producer / single-consumer (SPSC) byte ring buffer.
 *
 * Intended use: a UART RX interrupt (the producer) hands bytes to the main
 * loop (the consumer) without disabling interrupts.
 *
 * Memory-ordering contract
 * ------------------------
 *  - `head` is written ONLY by the producer, `tail` ONLY by the consumer.
 *  - Both are free-running 32-bit counters; the slot index is `counter & mask`.
 *    Unsigned wrap-around makes `head - tail` the fill level even after the
 *    counters overflow, and lets every slot be used (no "one empty slot" rule).
 *  - Producer: write the byte into the slot, THEN publish it with a
 *    store-release of `head`. The consumer's load-acquire of `head` therefore
 *    happens-after the slot write, so it never reads a stale byte.
 *  - Consumer: read the byte, THEN free the slot with a store-release of
 *    `tail`. The producer's load-acquire of `tail` guarantees it never
 *    overwrites a slot the consumer is still reading.
 *  - Each side loads its OWN counter with relaxed order (nobody else writes it).
 *  - Only atomic loads and stores are used, never read-modify-write, so the
 *    code is lock-free on cores without LDREX/STREX (ARMv6-M: Cortex-M0/M0+)
 *    as long as aligned 32-bit loads/stores are single-copy atomic (true on
 *    all ARM Cortex-M parts). GCC emits plain `ldr`/`str` plus `dmb` barriers
 *    for the acquire/release operations on both Cortex-M0 and Cortex-M4; on a
 *    single-core MCU the barriers are cheap.
 *  - Exactly ONE producer context and ONE consumer context. Two ISRs that can
 *    preempt each other must not both push.
 */
#ifndef TL_RINGBUF_H
#define TL_RINGBUF_H

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Lock-free guard. The macros below are compared by VALUE: if `int` is 32 bits
 * wide we check ATOMIC_INT_LOCK_FREE, otherwise ATOMIC_LONG_LOCK_FREE. (On
 * arm-none-eabi uint32_t is `unsigned long`, but int is also 32 bits there, so
 * the first branch applies; the two macros have the same value on that target.)
 *
 * The C11 macros describe ALL atomic operations, including read-modify-write.
 * On ARMv6-M (Cortex-M0/M0+) there is no LDREX/STREX, so GCC reports 1
 * ("sometimes lock-free"): RMW atomics would call libatomic. This code never
 * uses RMW, and plain atomic loads/stores of an aligned word compile to inline
 * `ldr`/`str` (+ `dmb`), so on ARMv6-M we accept 1. CI builds libtl.a for
 * cortex-m0 and fails if it references any __atomic_* / __sync_* symbol.
 */
#if defined(__ARM_ARCH_6M__)
#define TL_RB_REQUIRED_LOCK_FREE 1
#else
#define TL_RB_REQUIRED_LOCK_FREE 2
#endif

#if UINT32_MAX == UINT_MAX
#if ATOMIC_INT_LOCK_FREE < TL_RB_REQUIRED_LOCK_FREE
#error "tl_ringbuf requires lock-free 32-bit atomic loads and stores"
#endif
#elif ATOMIC_LONG_LOCK_FREE < TL_RB_REQUIRED_LOCK_FREE
#error "tl_ringbuf requires lock-free 32-bit atomic loads and stores"
#endif

typedef struct {
    uint8_t *buf;          /* caller-provided storage, never allocated here */
    uint32_t mask;         /* capacity - 1, capacity is a power of two */
    _Atomic uint32_t head; /* next slot to write; producer-owned */
    _Atomic uint32_t tail; /* next slot to read; consumer-owned */
} tl_ringbuf;

/* Capacity must be a power of two in [2, 2^31]. Returns false otherwise. */
bool tl_rb_init(tl_ringbuf *rb, uint8_t *storage, size_t capacity);

/* Producer side. Returns false (and drops the byte) when the buffer is full. */
bool tl_rb_push(tl_ringbuf *rb, uint8_t byte);

/* Consumer side. Returns false when the buffer is empty. */
bool tl_rb_pop(tl_ringbuf *rb, uint8_t *out);

/* Consumer side: pop up to `max` bytes with a single acquire/release pair. */
size_t tl_rb_pop_many(tl_ringbuf *rb, uint8_t *out, size_t max);

/* Snapshot of the fill level. From the consumer it may under-report (the
 * producer can add bytes meanwhile); from the producer it may over-report (the
 * consumer can free slots meanwhile). Each is the safe direction for that
 * caller. Always clamped to [0, capacity]. */
size_t tl_rb_count(const tl_ringbuf *rb);

size_t tl_rb_capacity(const tl_ringbuf *rb);

#endif /* TL_RINGBUF_H */
