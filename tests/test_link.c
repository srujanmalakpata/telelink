/*
 * Link-layer tests with a fake HAL: writes are captured in a buffer and the
 * millisecond tick is a plain variable the test advances by hand.
 */
#include <string.h>

#include "tl/crc16.h"
#include "tl/link.h"
#include "tl_test.h"

typedef struct {
    uint8_t buf[2048];
    size_t len;
    uint32_t now_ms;
    size_t accept_limit; /* simulate a short write when non-zero */
} fake_hal;

static size_t fake_write(void *ctx, const uint8_t *data, size_t len)
{
    fake_hal *h = ctx;
    size_t n = len;
    if (h->accept_limit != 0u && n > h->accept_limit) {
        n = h->accept_limit;
    }
    if (h->len + n > sizeof h->buf) {
        n = sizeof h->buf - h->len;
    }
    memcpy(h->buf + h->len, data, n);
    h->len += n;
    return n;
}

static uint32_t fake_tick(void *ctx)
{
    return ((fake_hal *)ctx)->now_ms;
}

typedef struct {
    int calls;
    tl_packet last;
} recorder;

static void record(const tl_packet *pkt, void *ctx)
{
    recorder *r = ctx;
    r->calls++;
    r->last = *pkt;
}

typedef struct {
    int delivered;
    int failed;
    uint8_t last_seq;
} tx_log;

static void on_done(uint8_t seq, bool delivered, void *ctx)
{
    tx_log *l = ctx;
    l->last_seq = seq;
    if (delivered) {
        l->delivered++;
    } else {
        l->failed++;
    }
}

typedef struct {
    fake_hal hal_state;
    uint8_t rx_storage[256];
    tl_link link;
    tx_log log;
} node;

/* (Re)boot a node. `session` plays the per-boot value (RNG or boot counter). */
static void node_boot(node *n, uint32_t timeout_ms, uint8_t retries, uint32_t session)
{
    memset(n, 0, sizeof *n);
    const tl_hal hal = {.ctx = &n->hal_state, .uart_write = fake_write, .get_tick_ms = fake_tick};
    const tl_link_config cfg = {.retransmit_timeout_ms = timeout_ms,
                                .max_retries = retries,
                                .session_id = session,
                                .on_tx_done = on_done,
                                .tx_done_ctx = &n->log};
    CHECK(tl_link_init(&n->link, &cfg, &hal, n->rx_storage, sizeof n->rx_storage) == TL_OK);
}

static void node_init(node *n, uint32_t timeout_ms, uint8_t retries)
{
    node_boot(n, timeout_ms, retries, 0xB0070001u);
}

/* Decode the first frame `n` has written (to inspect flags on the wire). */
static tl_packet first_frame_written(const node *n)
{
    tl_packet pkt;
    memset(&pkt, 0, sizeof pkt);
    const uint8_t *end = memchr(n->hal_state.buf, 0, n->hal_state.len);
    uint8_t raw[TL_MAX_RAW];
    size_t raw_len = 0;
    CHECK(end != NULL);
    if (end != NULL) {
        const size_t enc_len = (size_t)(end - n->hal_state.buf);
        CHECK(tl_cobs_decode(n->hal_state.buf, enc_len, raw, sizeof raw, &raw_len));
        CHECK(tl_packet_parse(raw, raw_len, &pkt) == TL_PARSE_OK);
    }
    return pkt;
}

/* Move everything `from` has written into `to`'s RX ISR, then poll `to`. */
static void transfer(node *from, node *to)
{
    for (size_t i = 0; i < from->hal_state.len; i++) {
        CHECK(tl_link_rx_isr(&to->link, from->hal_state.buf[i]));
    }
    from->hal_state.len = 0;
    tl_link_poll(&to->link);
}

static void feed_bytes(node *to, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        CHECK(tl_link_rx_isr(&to->link, data[i]));
    }
    tl_link_poll(&to->link);
}

static node a;
static node b;

static void test_init_rejects_bad_config(void)
{
    tl_link link;
    uint8_t storage[64];
    fake_hal h = {0};
    const tl_hal hal = {.ctx = &h, .uart_write = fake_write, .get_tick_ms = fake_tick};
    tl_link_config cfg = {.retransmit_timeout_ms = 0, .max_retries = 1};
    CHECK(tl_link_init(&link, &cfg, &hal, storage, sizeof storage) == TL_ERR_INVALID);
    cfg.retransmit_timeout_ms = 10;
    CHECK(tl_link_init(&link, &cfg, &hal, storage, 48) == TL_ERR_INVALID); /* not 2^n */
    const tl_hal no_write = {.ctx = &h, .uart_write = NULL, .get_tick_ms = fake_tick};
    CHECK(tl_link_init(&link, &cfg, &no_write, storage, sizeof storage) == TL_ERR_INVALID);
    CHECK(tl_link_init(&link, &cfg, &hal, storage, sizeof storage) == TL_OK);
}

static void test_unreliable_send_is_dispatched(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x20, record, &rec) == TL_REG_OK);

    const uint8_t payload[] = {0x00, 0x01, 0x00, 0xFF};
    CHECK(tl_link_send(&a.link, 0x20, payload, sizeof payload) == TL_OK);
    CHECK(tl_link_send(&a.link, 0x20, payload, 2) == TL_OK);
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 2);
    CHECK_EQ(rec.last.len, 2u);
    CHECK_EQ(rec.last.seq, 1u); /* unreliable counter advanced */
    CHECK(memcmp(rec.last.payload, payload, 2) == 0);
    CHECK_EQ(b.link.stats.rx_frames_ok, 2u);
    CHECK_EQ(b.link.stats.acks_sent, 0u);
    CHECK_EQ(b.hal_state.len, 0u); /* nothing sent back */
}

static void test_send_validates_arguments(void)
{
    node_init(&a, 50, 3);
    uint8_t big[TL_MAX_PAYLOAD + 1] = {0};
    CHECK(tl_link_send(&a.link, 0, big, 1) == TL_ERR_INVALID);
    CHECK(tl_link_send(&a.link, 1, big, sizeof big) == TL_ERR_INVALID);
    CHECK(tl_link_send(&a.link, 1, NULL, 3) == TL_ERR_INVALID);
    CHECK(tl_link_send(&a.link, 1, NULL, 0) == TL_OK);
    CHECK(tl_link_send_reliable(&a.link, 1, big, sizeof big, NULL) == TL_ERR_INVALID);
    CHECK(!tl_link_tx_busy(&a.link));
}

static void test_reliable_roundtrip_with_ack(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);

    const uint8_t cmd[] = {1, 2, 3};
    uint8_t seq = 0xEE;
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, &seq) == TL_OK);
    CHECK_EQ(seq, 0u);
    CHECK(tl_link_tx_busy(&a.link));
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_ERR_BUSY);

    transfer(&a, &b); /* command reaches b, b queues an ACK */
    CHECK_EQ(rec.calls, 1);
    CHECK_EQ(b.link.stats.acks_sent, 1u);
    transfer(&b, &a); /* ACK reaches a */
    CHECK(!tl_link_tx_busy(&a.link));
    CHECK_EQ(a.log.delivered, 1);
    CHECK_EQ(a.log.last_seq, 0u);
    CHECK_EQ(a.link.stats.tx_delivered, 1u);

    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, &seq) == TL_OK);
    CHECK_EQ(seq, 1u);
}

static void test_retransmit_then_give_up(void)
{
    node_init(&a, 100, 2);
    const uint8_t cmd[] = {9};
    a.hal_state.now_ms = 1000;
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    const size_t frame_len = a.hal_state.len;
    CHECK(frame_len > 0u);

    a.hal_state.now_ms = 1099;
    tl_link_poll(&a.link);
    CHECK_EQ(a.hal_state.len, frame_len); /* not yet */
    a.hal_state.now_ms = 1100;
    tl_link_poll(&a.link);
    CHECK_EQ(a.hal_state.len, 2u * frame_len); /* retry 1 */
    CHECK(memcmp(a.hal_state.buf, a.hal_state.buf + frame_len, frame_len) == 0);
    a.hal_state.now_ms = 1200;
    tl_link_poll(&a.link);
    CHECK_EQ(a.link.stats.tx_retransmits, 2u);
    CHECK(tl_link_tx_busy(&a.link));
    a.hal_state.now_ms = 1300;
    tl_link_poll(&a.link); /* retries exhausted */
    CHECK(!tl_link_tx_busy(&a.link));
    CHECK_EQ(a.log.failed, 1);
    CHECK_EQ(a.link.stats.tx_failed, 1u);
    CHECK_EQ(a.link.stats.tx_frames, 3u); /* 1 original + 2 retries */
}

static void test_retransmit_timer_survives_tick_wrap(void)
{
    node_init(&a, 100, 1);
    const uint8_t cmd[] = {9};
    a.hal_state.now_ms = 0xFFFFFFC0u; /* 64 ms before wrap */
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    a.hal_state.now_ms = 0x00000010u; /* 80 ms later, after wrap */
    tl_link_poll(&a.link);
    CHECK_EQ(a.link.stats.tx_retransmits, 0u);
    a.hal_state.now_ms = 0x00000024u; /* 100 ms later */
    tl_link_poll(&a.link);
    CHECK_EQ(a.link.stats.tx_retransmits, 1u);
}

static void test_lost_ack_causes_duplicate_which_is_suppressed(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    const uint8_t cmd[] = {7, 7};
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 1);
    b.hal_state.len = 0; /* the ACK is lost on the wire */

    a.hal_state.now_ms = 50;
    tl_link_poll(&a.link); /* retransmit */
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 1); /* not dispatched twice */
    CHECK_EQ(b.link.stats.rx_duplicates, 1u);
    CHECK_EQ(b.link.stats.acks_sent, 2u); /* but re-ACKed */
    transfer(&b, &a);
    CHECK_EQ(a.log.delivered, 1);
    CHECK(!tl_link_tx_busy(&a.link));
}

static void test_stale_ack_is_ignored(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    /* Hand-craft an ACK for seq 5 while nothing is in flight. */
    const tl_packet ack = {.type = 0x10, .flags = TL_FLAG_ACK, .seq = 5, .len = 0};
    uint8_t wire[TL_MAX_WIRE];
    const size_t n = tl_frame_encode(&ack, wire, sizeof wire);
    feed_bytes(&a, wire, n);
    CHECK_EQ(a.link.stats.rx_stale_acks, 1u);
    CHECK_EQ(a.log.delivered, 0);
}

static void test_error_counters(void)
{
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x20, record, &rec) == TL_REG_OK);

    const tl_packet p = {.type = 0x20, .flags = 0, .seq = 1, .len = 3, .payload = {1, 2, 3}};
    uint8_t wire[TL_MAX_WIRE];
    const size_t n = tl_frame_encode(&p, wire, sizeof wire);

    /* Bad CRC: corrupt a payload byte (still non-zero, so framing is intact). */
    uint8_t bad[TL_MAX_WIRE];
    memcpy(bad, wire, n);
    bad[3] ^= 0x40u;
    feed_bytes(&b, bad, n);
    CHECK_EQ(b.link.stats.rx_bad_crc, 1u);

    /* Framing error: lose the last data byte, so the final COBS block is cut
     * short by the delimiter. */
    memcpy(bad, wire, n - 2);
    bad[n - 2] = 0x00;
    feed_bytes(&b, bad, n - 1);
    CHECK_EQ(b.link.stats.rx_framing_errors, 1u);

    /* Overflow: a frame longer than any valid packet. */
    uint8_t flood[TL_MAX_RAW + 10];
    memset(flood, 0x55, sizeof flood);
    feed_bytes(&b, flood, sizeof flood);
    const uint8_t delim = 0;
    feed_bytes(&b, &delim, 1);
    CHECK_EQ(b.link.stats.rx_overflow, 1u);

    /* Too short: valid COBS of 2 bytes. */
    const uint8_t tiny[] = {0x03, 0x01, 0x02, 0x00};
    feed_bytes(&b, tiny, sizeof tiny);
    CHECK_EQ(b.link.stats.rx_bad_header, 1u);

    /* Length byte disagrees with the frame (a lost byte whose CRC collided):
     * the CRC is recomputed here to model that collision. */
    uint8_t raw[TL_MAX_RAW] = {0x20, 0x00, 0x01, 0x04, 0x01, 0x02, 0x03};
    const uint16_t crc = tl_crc16(raw, 7);
    raw[7] = (uint8_t)(crc >> 8);
    raw[8] = (uint8_t)crc;
    const size_t enc = tl_cobs_encode(raw, 9, bad, sizeof bad - 1u);
    CHECK(enc > 0u);
    bad[enc] = 0x00;
    feed_bytes(&b, bad, enc + 1u);
    CHECK_EQ(b.link.stats.rx_bad_length, 1u);
    CHECK_EQ(rec.calls, 0);

    /* Unknown type. */
    const tl_packet u = {.type = 0x77, .flags = 0, .seq = 0, .len = 0};
    const size_t un = tl_frame_encode(&u, wire, sizeof wire);
    feed_bytes(&b, wire, un);
    CHECK_EQ(b.link.stats.rx_unknown_type, 1u);

    /* After all that garbage, a good frame still gets through. */
    const size_t gn = tl_frame_encode(&p, wire, sizeof wire);
    feed_bytes(&b, wire, gn);
    CHECK_EQ(rec.calls, 1);
}

static void test_isr_ring_overflow_is_counted(void)
{
    node_init(&b, 50, 3);
    size_t accepted = 0;
    for (int i = 0; i < 300; i++) {
        accepted += tl_link_rx_isr(&b.link, 0x11) ? 1u : 0u;
    }
    CHECK_EQ(accepted, sizeof b.rx_storage);
    CHECK_EQ(tl_link_rx_ring_dropped(&b.link), 300u - sizeof b.rx_storage);
    CHECK_EQ(tl_link_rx_pending(&b.link), sizeof b.rx_storage);
    tl_link_poll(&b.link);
    CHECK_EQ(tl_link_rx_pending(&b.link), 0u);
    CHECK(tl_link_get_stats(&b.link) == &b.link.stats);
}

static void test_short_write_counted(void)
{
    node_init(&a, 50, 3);
    a.hal_state.accept_limit = 3;
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    CHECK(tl_link_send(&a.link, 0x20, payload, sizeof payload) == TL_OK);
    CHECK_EQ(a.link.stats.tx_write_errors, 1u);
}

typedef struct {
    tl_link *link;
    int replies;
} echo_ctx;

static void echo_handler(const tl_packet *pkt, void *ctx)
{
    echo_ctx *e = ctx;
    /* Handlers run inside tl_link_poll and may transmit. */
    if (tl_link_send(e->link, 0x21, pkt->payload, pkt->len) == TL_OK) {
        e->replies++;
    }
}

static void test_handler_can_reply(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    echo_ctx e = {&b.link, 0};
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, echo_handler, &e) == TL_REG_OK);
    CHECK(tl_link_register(&a.link, 0x21, record, &rec) == TL_REG_OK);
    const uint8_t q[] = {0xCA, 0xFE};
    CHECK(tl_link_send_reliable(&a.link, 0x10, q, sizeof q, NULL) == TL_OK);
    transfer(&a, &b);
    transfer(&b, &a); /* ACK + echo reply */
    CHECK_EQ(e.replies, 1);
    CHECK_EQ(rec.calls, 1);
    CHECK(memcmp(rec.last.payload, q, 2) == 0);
    CHECK_EQ(a.log.delivered, 1);
}

/* Regression: a sender that restarts (e.g. a host tool that sends one command
 * per run) starts again at seq 0. A new session id prevents the receiver from ACKing
 * that frame and dropping it as a duplicate of the previous run's seq 0. */
static void test_sender_restart_with_new_session_is_dispatched(void)
{
    node_boot(&a, 50, 3, 0x1111u);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);

    const uint8_t first[] = {0xAA};
    CHECK(tl_link_send_reliable(&a.link, 0x10, first, sizeof first, NULL) == TL_OK);
    transfer(&a, &b);
    transfer(&b, &a);
    CHECK_EQ(a.log.delivered, 1);

    node_boot(&a, 50, 3, 0x2222u); /* sender reboots: seq restarts at 0 */
    const uint8_t second[] = {0xBB};
    uint8_t seq = 0xEE;
    CHECK(tl_link_send_reliable(&a.link, 0x10, second, sizeof second, &seq) == TL_OK);
    CHECK_EQ(seq, 0u); /* same seq as the command the receiver accepted last */
    transfer(&a, &b);
    transfer(&b, &a);
    CHECK_EQ(rec.calls, 2); /* dispatched, not swallowed */
    CHECK_EQ(rec.last.payload[0], 0xBBu);
    CHECK_EQ(b.link.stats.rx_duplicates, 0u);
    CHECK_EQ(b.link.stats.rx_new_sessions, 2u);
    CHECK_EQ(a.log.delivered, 1);
}

/* Documents the contract: if session_id does NOT change across boots, the
 * receiver cannot tell a restart from a retransmit. */
static void test_sender_restart_with_same_session_looks_like_duplicate(void)
{
    node_boot(&a, 50, 3, 0x1111u);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    const uint8_t cmd[] = {1};
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    transfer(&b, &a);
    node_boot(&a, 50, 3, 0x1111u); /* same session id after "reboot" */
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 1);
    CHECK_EQ(b.link.stats.rx_duplicates, 1u);
}

/* A receiver reboot AFTER the ACK reached the sender loses nothing: the next
 * command has a new seq and is accepted by the fresh receiver. */
static void test_receiver_restart_after_ack_is_harmless(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    const uint8_t cmd[] = {1};
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    transfer(&b, &a);

    node_init(&b, 50, 3); /* receiver reboots and forgets everything */
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    transfer(&b, &a);
    CHECK_EQ(rec.calls, 2);
    CHECK_EQ(a.log.delivered, 2);
}

/* Documents a limit: duplicate memory is in RAM. If the receiver reboots after
 * dispatching a command but before its ACK reaches the sender, the sender
 * retransmits and the rebooted receiver dispatches the SAME command again.
 * Across receiver reboots delivery is at-least-once. */
static void test_receiver_restart_in_ack_window_duplicates(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    const uint8_t cmd[] = {0x42};
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 1);

    node_init(&b, 50, 3); /* reboot before the ACK left: it is lost */
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    a.hal_state.now_ms = 50;
    tl_link_poll(&a.link); /* sender times out and retransmits */
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 2); /* the same command ran twice */
    CHECK_EQ(b.link.stats.rx_duplicates, 0u);
    transfer(&b, &a);
    CHECK_EQ(a.log.delivered, 1);
}

/* A sender that restarts while the ACK for its previous boot's seq 0 is still
 * on the way (e.g. buffered by a USB-UART adapter) must not take that ACK for
 * the new boot's seq 0 command. The ACK of a SYNC frame echoes the session. */
static void test_stale_ack_from_previous_boot_is_ignored(void)
{
    node_boot(&a, 50, 3, 0x1111u);
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x10, record, &rec) == TL_REG_OK);
    const uint8_t old_cmd[] = {0x01};
    CHECK(tl_link_send_reliable(&a.link, 0x10, old_cmd, sizeof old_cmd, NULL) == TL_OK);
    transfer(&a, &b); /* b dispatches and ACKs, but the ACK is still in b's buffer */
    CHECK_EQ(rec.calls, 1);

    node_boot(&a, 50, 3, 0x2222u); /* sender reboots */
    const uint8_t new_cmd[] = {0x02};
    CHECK(tl_link_send_reliable(&a.link, 0x10, new_cmd, sizeof new_cmd, NULL) == TL_OK);
    a.hal_state.len = 0; /* the new command is lost on the wire */

    transfer(&b, &a); /* the stale ACK (type 0x10, seq 0, session 0x1111) arrives */
    CHECK_EQ(a.log.delivered, 0);
    CHECK(tl_link_tx_busy(&a.link));
    CHECK_EQ(a.link.stats.rx_stale_acks, 1u);

    a.hal_state.now_ms = 50;
    tl_link_poll(&a.link); /* retransmit reaches b this time */
    transfer(&a, &b);
    CHECK_EQ(rec.calls, 2);
    CHECK_EQ(rec.last.payload[0], 0x02u);
    transfer(&b, &a);
    CHECK_EQ(a.log.delivered, 1);
    CHECK(!tl_link_tx_busy(&a.link));
}

static void test_sync_flag_sent_only_until_first_ack(void)
{
    node_boot(&a, 50, 3, 0xCAFEF00Du);
    node_init(&b, 50, 3);
    const uint8_t cmd[] = {5};
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    tl_packet first = first_frame_written(&a);
    CHECK_EQ(first.flags, TL_FLAG_RELIABLE | TL_FLAG_SYNC);
    CHECK_EQ(first.session, 0xCAFEF00Du);
    CHECK_EQ(first.len, 1u);

    /* No ACK yet: the retransmission is the same SYNC frame. */
    a.hal_state.len = 0;
    a.hal_state.now_ms = 50;
    tl_link_poll(&a.link);
    first = first_frame_written(&a);
    CHECK_EQ(first.flags, TL_FLAG_RELIABLE | TL_FLAG_SYNC);

    transfer(&a, &b);
    const tl_packet ack = first_frame_written(&b); /* the ACK echoes the session */
    CHECK_EQ(ack.flags, TL_FLAG_ACK | TL_FLAG_SYNC);
    CHECK_EQ(ack.session, 0xCAFEF00Du);
    CHECK_EQ(ack.len, 0u);
    transfer(&b, &a);
    CHECK_EQ(a.log.delivered, 1);
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK);
    const tl_packet second = first_frame_written(&a);
    CHECK_EQ(second.flags, TL_FLAG_RELIABLE);
    transfer(&a, &b);
    const tl_packet plain_ack = first_frame_written(&b);
    CHECK_EQ(plain_ack.flags, TL_FLAG_ACK);
    transfer(&b, &a);
    CHECK_EQ(a.log.delivered, 2);
}

static void test_ack_must_match_type_seq_and_session(void)
{
    node_boot(&a, 50, 3, 0xB0070001u);
    const uint8_t cmd[] = {9};
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK); /* SYNC */
    const uint8_t sync_ack = TL_FLAG_ACK | TL_FLAG_SYNC;
    const tl_packet stale[] = {
        {.type = 0x55, .flags = sync_ack, .seq = 0, .session = 0xB0070001u}, /* wrong type */
        {.type = 0x10, .flags = sync_ack, .seq = 1, .session = 0xB0070001u}, /* wrong seq */
        {.type = 0x10, .flags = sync_ack, .seq = 0, .session = 0xB0070000u}, /* old session */
        {.type = 0x10, .flags = TL_FLAG_ACK, .seq = 0},                      /* no session */
    };
    uint8_t wire[TL_MAX_WIRE];
    for (size_t i = 0; i < sizeof stale / sizeof stale[0]; i++) {
        const size_t n = tl_frame_encode(&stale[i], wire, sizeof wire);
        CHECK(n > 0u);
        feed_bytes(&a, wire, n);
    }
    CHECK_EQ(a.link.stats.rx_stale_acks, 4u);
    CHECK(tl_link_tx_busy(&a.link));
    CHECK_EQ(a.log.delivered, 0);

    const tl_packet right = {.type = 0x10, .flags = sync_ack, .seq = 0, .session = 0xB0070001u};
    size_t n = tl_frame_encode(&right, wire, sizeof wire);
    feed_bytes(&a, wire, n);
    CHECK(!tl_link_tx_busy(&a.link));
    CHECK_EQ(a.log.delivered, 1);

    /* After the first ACK the sender is synced: a plain ACK completes, a SYNC one does not. */
    CHECK(tl_link_send_reliable(&a.link, 0x10, cmd, sizeof cmd, NULL) == TL_OK); /* seq 1 */
    const tl_packet late_sync = {.type = 0x10, .flags = sync_ack, .seq = 1, .session = 0xB0070001u};
    n = tl_frame_encode(&late_sync, wire, sizeof wire);
    feed_bytes(&a, wire, n);
    CHECK(tl_link_tx_busy(&a.link));
    const tl_packet plain = {.type = 0x10, .flags = TL_FLAG_ACK, .seq = 1};
    n = tl_frame_encode(&plain, wire, sizeof wire);
    feed_bytes(&a, wire, n);
    CHECK(!tl_link_tx_busy(&a.link));
    CHECK_EQ(a.log.delivered, 2);
}

/* Pins the documented semantics: ACK = "received intact", not "handled". */
static void test_ack_means_received_not_handled(void)
{
    node_init(&a, 50, 3);
    node_init(&b, 50, 3); /* no handler registered for 0x30 */
    const uint8_t cmd[] = {1, 2};
    CHECK(tl_link_send_reliable(&a.link, 0x30, cmd, sizeof cmd, NULL) == TL_OK);
    transfer(&a, &b);
    transfer(&b, &a);
    CHECK_EQ(b.link.stats.rx_unknown_type, 1u);
    CHECK_EQ(a.log.delivered, 1);
}

/* A frame without the 4-byte session fits the TL_MAX_RAW decode buffer even
 * with a payload of up to TL_MAX_PAYLOAD + 4 bytes; the parser must reject it. */
static void test_oversized_payload_counts_as_overflow(void)
{
    node_init(&b, 50, 3);
    recorder rec = {0};
    CHECK(tl_link_register(&b.link, 0x20, record, &rec) == TL_REG_OK);
    uint8_t raw[TL_MAX_RAW];
    const size_t payload_len = TL_MAX_PAYLOAD + 1u;
    raw[0] = 0x20;
    raw[1] = 0;
    raw[2] = 0;
    raw[3] = (uint8_t)payload_len;
    memset(&raw[TL_HEADER_SIZE], 0x5A, payload_len);
    const size_t body = TL_HEADER_SIZE + payload_len;
    const uint16_t crc = tl_crc16(raw, body);
    raw[body] = (uint8_t)(crc >> 8);
    raw[body + 1] = (uint8_t)crc;
    uint8_t wire[TL_MAX_WIRE];
    const size_t enc = tl_cobs_encode(raw, body + 2, wire, sizeof wire - 1);
    CHECK(enc > 0u);
    wire[enc] = 0;
    feed_bytes(&b, wire, enc + 1);
    CHECK_EQ(b.link.stats.rx_overflow, 1u);
    CHECK_EQ(b.link.stats.rx_bad_header, 0u);
    CHECK_EQ(rec.calls, 0);
}

int main(void)
{
    RUN_TEST(test_init_rejects_bad_config);
    RUN_TEST(test_unreliable_send_is_dispatched);
    RUN_TEST(test_send_validates_arguments);
    RUN_TEST(test_reliable_roundtrip_with_ack);
    RUN_TEST(test_retransmit_then_give_up);
    RUN_TEST(test_retransmit_timer_survives_tick_wrap);
    RUN_TEST(test_lost_ack_causes_duplicate_which_is_suppressed);
    RUN_TEST(test_stale_ack_is_ignored);
    RUN_TEST(test_error_counters);
    RUN_TEST(test_isr_ring_overflow_is_counted);
    RUN_TEST(test_short_write_counted);
    RUN_TEST(test_handler_can_reply);
    RUN_TEST(test_sender_restart_with_new_session_is_dispatched);
    RUN_TEST(test_sender_restart_with_same_session_looks_like_duplicate);
    RUN_TEST(test_receiver_restart_after_ack_is_harmless);
    RUN_TEST(test_receiver_restart_in_ack_window_duplicates);
    RUN_TEST(test_stale_ack_from_previous_boot_is_ignored);
    RUN_TEST(test_sync_flag_sent_only_until_first_ack);
    RUN_TEST(test_ack_must_match_type_seq_and_session);
    RUN_TEST(test_ack_means_received_not_handled);
    RUN_TEST(test_oversized_payload_counts_as_overflow);
    return TEST_REPORT();
}
