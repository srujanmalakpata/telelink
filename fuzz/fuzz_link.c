/*
 * libFuzzer target for the whole receive path: arbitrary bytes arrive through
 * the "ISR", then the link decodes, validates, ACKs and dispatches them.
 * The first input byte is used as a tick step so retransmit timers also run.
 * Invariants: no crash/UB; no byte is lost in the ISR ring; every non-empty
 * delimited frame is counted exactly once (OK, bad CRC, framing, overflow or
 * bad header), so a frame silently swallowed by the RX path fails the run.
 */
#include <stdlib.h>
#include <string.h>

#include "tl/link.h"

static uint32_t now_ms;
static size_t bytes_out;

static size_t sink_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    (void)data;
    bytes_out += len;
    return len;
}

static uint32_t tick(void *ctx)
{
    (void)ctx;
    return now_ms;
}

static unsigned handled;

static void handler(const tl_packet *pkt, void *ctx)
{
    (void)ctx;
    if (pkt->len > TL_MAX_PAYLOAD) {
        abort();
    }
    handled++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static tl_link link;
    static uint8_t rx_storage[64];
    if (size == 0u) {
        return 0;
    }
    now_ms = 0u;
    bytes_out = 0u;
    handled = 0u;
    const tl_hal hal = {.ctx = NULL, .uart_write = sink_write, .get_tick_ms = tick};
    const tl_link_config cfg = {
        .retransmit_timeout_ms = 10u, .max_retries = 2u, .session_id = 0x12345678u};
    if (tl_link_init(&link, &cfg, &hal, rx_storage, sizeof rx_storage) != TL_OK) {
        abort();
    }
    (void)tl_link_register(&link, 0x10, handler, NULL);
    (void)tl_link_register(&link, 0x20, handler, NULL);
    const uint8_t cmd[] = {1, 2, 3};
    (void)tl_link_send_reliable(&link, 0x10, cmd, sizeof cmd, NULL);

    const uint32_t step = data[0];
    for (size_t i = 1; i < size; i++) {
        if (!tl_link_rx_isr(&link, data[i])) {
            tl_link_poll(&link); /* ring full: drain like a main loop would */
            if (!tl_link_rx_isr(&link, data[i])) {
                abort(); /* poll drains the whole ring, so this must succeed */
            }
        }
        if ((i & 15u) == 0u) {
            now_ms += step;
            tl_link_poll(&link);
        }
    }
    tl_link_poll(&link);

    const tl_link_stats *st = tl_link_get_stats(&link);
    size_t frames = 0; /* non-empty segments closed by a delimiter */
    size_t seg_len = 0;
    for (size_t i = 1; i < size; i++) {
        if (data[i] == 0u) {
            frames += seg_len > 0u;
            seg_len = 0;
        } else {
            seg_len++;
        }
    }
    const size_t outcomes = (size_t)st->rx_frames_ok + st->rx_bad_crc + st->rx_framing_errors +
                            st->rx_overflow + st->rx_bad_header + st->rx_bad_length;
    if (outcomes != frames || handled > st->rx_frames_ok) {
        abort();
    }
    return 0;
}
