/*
 * SPSC stress test with real threads: a producer thread plays the UART ISR,
 * the main thread plays the main loop. Every byte must arrive exactly once and
 * in order. Run it under ThreadSanitizer (TL_SANITIZE=thread) to check the
 * acquire/release contract, not just the outcome on this x86 host.
 */
#include <pthread.h>
#include <stdint.h>

#include "tl/ringbuf.h"
#include "tl_test.h"

#define TOTAL_BYTES 2000000u

static tl_ringbuf rb;
static uint8_t storage[64]; /* small, so the buffer is constantly full/empty */
static uint32_t producer_full_spins;

static void *producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < TOTAL_BYTES; i++) {
        /* (i * 7) mod 256 is a known sequence; any loss/reorder breaks it. */
        while (!tl_rb_push(&rb, (uint8_t)(i * 7u))) {
            producer_full_spins++;
        }
    }
    return NULL;
}

static void test_spsc_two_threads(void)
{
    CHECK(tl_rb_init(&rb, storage, sizeof storage));
    pthread_t t;
    CHECK(pthread_create(&t, NULL, producer, NULL) == 0);
    uint32_t received = 0;
    uint32_t mismatches = 0;
    uint8_t chunk[16];
    while (received < TOTAL_BYTES) {
        size_t n;
        if (received % 2u == 0u) {
            n = tl_rb_pop_many(&rb, chunk, sizeof chunk);
        } else {
            n = tl_rb_pop(&rb, &chunk[0]) ? 1u : 0u;
        }
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != (uint8_t)(received * 7u)) {
                mismatches++;
            }
            received++;
        }
    }
    CHECK(pthread_join(t, NULL) == 0);
    CHECK_EQ(received, TOTAL_BYTES);
    CHECK_EQ(mismatches, 0u);
    CHECK_EQ(tl_rb_count(&rb), 0u);
    printf("  transferred %u bytes, producer saw full %u times\n", received, producer_full_spins);
}

int main(void)
{
    RUN_TEST(test_spsc_two_threads);
    return TEST_REPORT();
}
