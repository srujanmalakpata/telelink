/*
 * The link's real concurrency path with real threads: a producer thread plays
 * the UART RX interrupt and calls tl_link_rx_isr() with a stream of encoded
 * frames, while the main thread loops tl_link_poll() (drain, COBS decode,
 * CRC, dispatch) and reads tl_link_rx_ring_dropped(). Run under
 * ThreadSanitizer (TL_SANITIZE=thread) this checks the ISR/main-loop contract
 * of tl_link, not only the bare ring buffer.
 *
 * A real ISR cannot wait, so when the ring is full tl_link_rx_isr() drops the
 * byte and counts it. Here the producer then offers the same byte again (as if
 * the UART had held it), so no frame is lost and the test can demand that
 * every frame arrives intact and in order, and that the drop counter equals
 * the number of refused pushes the producer saw.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "tl/link.h"
#include "tl_test.h"

#define FRAMES 20000u
#define MSG_DATA 0x20u

static tl_link g_link;
static uint8_t rx_storage[64]; /* small, so the ring is often full */
static atomic_bool producer_done;
static uint32_t refused_pushes; /* written by the producer, read after join */

typedef struct {
    uint32_t received;
    uint32_t out_of_order;
} rx_log;

static size_t sink_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)data;
    return len; /* the receiver only gets telemetry, so it never writes */
}

static uint32_t zero_tick(void *ctx)
{
    (void)ctx;
    return 0u;
}

/* Frame i: a 4-byte little-endian counter, then (i % 12) bytes including zeros. */
static size_t make_payload(uint32_t i, uint8_t *p)
{
    p[0] = (uint8_t)i;
    p[1] = (uint8_t)(i >> 8);
    p[2] = (uint8_t)(i >> 16);
    p[3] = (uint8_t)(i >> 24);
    const size_t extra = i % 12u;
    for (size_t k = 0; k < extra; k++) {
        p[4 + k] = (uint8_t)((i + k) % 3u == 0u ? 0u : i * 7u + k);
    }
    return 4u + extra;
}

static void on_data(const tl_packet *pkt, void *ctx)
{
    rx_log *log = ctx;
    uint8_t expect[TL_MAX_PAYLOAD];
    const size_t n = make_payload(log->received, expect);
    if (pkt->len != n || memcmp(pkt->payload, expect, n) != 0) {
        log->out_of_order++;
    }
    log->received++;
}

static void *uart_isr_thread(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < FRAMES; i++) {
        tl_packet pkt;
        memset(&pkt, 0, sizeof pkt);
        pkt.type = MSG_DATA;
        pkt.seq = (uint8_t)i;
        pkt.len = (uint8_t)make_payload(i, pkt.payload);
        uint8_t wire[TL_MAX_WIRE];
        const size_t n = tl_frame_encode(&pkt, wire, sizeof wire);
        for (size_t k = 0; k < n; k++) {
            while (!tl_link_rx_isr(&g_link, wire[k])) {
                refused_pushes++;
            }
        }
    }
    atomic_store_explicit(&producer_done, true, memory_order_release);
    return NULL;
}

static void test_isr_thread_and_poll_loop(void)
{
    const tl_hal hal = {.ctx = NULL, .uart_write = sink_write, .get_tick_ms = zero_tick};
    const tl_link_config cfg = {.retransmit_timeout_ms = 10u, .max_retries = 1u};
    CHECK(tl_link_init(&g_link, &cfg, &hal, rx_storage, sizeof rx_storage) == TL_OK);
    rx_log log = {0};
    CHECK(tl_link_register(&g_link, MSG_DATA, on_data, &log) == TL_REG_OK);
    atomic_init(&producer_done, false);

    pthread_t t;
    CHECK(pthread_create(&t, NULL, uart_isr_thread, NULL) == 0);
    uint32_t dropped_seen = 0;
    for (;;) {
        /* Read `done` BEFORE polling: if it was set, everything the producer
         * pushed is visible to this poll, so the final poll drains it all. */
        const bool done = atomic_load_explicit(&producer_done, memory_order_acquire);
        tl_link_poll(&g_link);
        dropped_seen = tl_link_rx_ring_dropped(&g_link); /* concurrent with the ISR's store */
        if (done && tl_link_rx_pending(&g_link) == 0u) {
            break;
        }
    }
    CHECK(pthread_join(t, NULL) == 0);

    const tl_link_stats *st = tl_link_get_stats(&g_link);
    CHECK_EQ(log.received, FRAMES);
    CHECK_EQ(log.out_of_order, 0u);
    CHECK_EQ(st->rx_frames_ok, FRAMES);
    CHECK_EQ(st->rx_bad_crc + st->rx_framing_errors + st->rx_overflow + st->rx_bad_header, 0u);
    CHECK_EQ(tl_link_rx_ring_dropped(&g_link), refused_pushes);
    CHECK(dropped_seen <= refused_pushes);
    printf("  %u frames dispatched in order, ring full %u times\n", log.received, refused_pushes);
}

int main(void)
{
    RUN_TEST(test_isr_thread_and_poll_loop);
    return TEST_REPORT();
}
