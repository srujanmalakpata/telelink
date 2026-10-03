#include "tl/link.h"

#include <string.h>

#define RX_DRAIN_CHUNK 32u

tl_status tl_link_init(tl_link *link, const tl_link_config *cfg, const tl_hal *hal,
                       uint8_t *rx_storage, size_t rx_capacity)
{
    if (link == NULL || cfg == NULL || hal == NULL || hal->uart_write == NULL ||
        hal->get_tick_ms == NULL || cfg->retransmit_timeout_ms == 0u) {
        return TL_ERR_INVALID;
    }
    memset(link, 0, sizeof *link);
    if (!tl_rb_init(&link->rx_ring, rx_storage, rx_capacity)) {
        return TL_ERR_INVALID;
    }
    link->hal = *hal;
    link->cfg = *cfg;
    atomic_init(&link->rx_ring_dropped, 0u);
    tl_cobs_stream_init(&link->decoder, link->rx_raw, sizeof link->rx_raw);
    tl_dispatch_init(&link->dispatcher);
    return TL_OK;
}

tl_reg_result tl_link_register(tl_link *link, uint8_t type, tl_handler_fn fn, void *ctx)
{
    return tl_dispatch_register(&link->dispatcher, type, fn, ctx);
}

bool tl_link_rx_isr(tl_link *link, uint8_t byte)
{
    if (tl_rb_push(&link->rx_ring, byte)) {
        return true;
    }
    /* Single writer (this ISR), so load+store is enough: no RMW needed. */
    const uint32_t n = atomic_load_explicit(&link->rx_ring_dropped, memory_order_relaxed);
    atomic_store_explicit(&link->rx_ring_dropped, n + 1u, memory_order_relaxed);
    return false;
}

uint32_t tl_link_rx_ring_dropped(const tl_link *link)
{
    return atomic_load_explicit(&link->rx_ring_dropped, memory_order_relaxed);
}

const tl_link_stats *tl_link_get_stats(const tl_link *link)
{
    return &link->stats;
}

size_t tl_link_rx_pending(const tl_link *link)
{
    return tl_rb_count(&link->rx_ring);
}

bool tl_link_tx_busy(const tl_link *link)
{
    return link->pending.active;
}

static void write_wire(tl_link *link, const uint8_t *wire, size_t len)
{
    const size_t written = link->hal.uart_write(link->hal.ctx, wire, len);
    link->stats.tx_frames++;
    if (written != len) {
        link->stats.tx_write_errors++;
    }
}

static size_t build_frame(uint8_t type, uint8_t flags, uint8_t seq, uint32_t session,
                          const uint8_t *payload, size_t len, uint8_t *wire, size_t cap)
{
    tl_packet pkt;
    pkt.type = type;
    pkt.flags = flags;
    pkt.seq = seq;
    pkt.len = (uint8_t)len;
    pkt.session = session;
    if (len > 0u) {
        memcpy(pkt.payload, payload, len);
    }
    return tl_frame_encode(&pkt, wire, cap);
}

static bool valid_send_args(uint8_t type, const uint8_t *payload, size_t len)
{
    return type != 0u && len <= TL_MAX_PAYLOAD && (payload != NULL || len == 0u);
}

tl_status tl_link_send(tl_link *link, uint8_t type, const uint8_t *payload, size_t len)
{
    if (!valid_send_args(type, payload, len)) {
        return TL_ERR_INVALID;
    }
    uint8_t wire[TL_MAX_WIRE];
    const size_t n =
        build_frame(type, 0u, link->tx_seq_unreliable, 0u, payload, len, wire, sizeof wire);
    if (n == 0u) {
        return TL_ERR_INVALID;
    }
    link->tx_seq_unreliable++;
    write_wire(link, wire, n);
    return TL_OK;
}

tl_status tl_link_send_reliable(tl_link *link, uint8_t type, const uint8_t *payload, size_t len,
                                uint8_t *seq_out)
{
    if (!valid_send_args(type, payload, len)) {
        return TL_ERR_INVALID;
    }
    if (link->pending.active) {
        return TL_ERR_BUSY;
    }
    const uint8_t seq = link->tx_seq_reliable;
    /* Until the peer has ACKed one frame of this session, announce the session
     * so a receiver that remembers our previous boot resets its dedupe state. */
    const uint8_t flags =
        link->tx_synced ? TL_FLAG_RELIABLE : (uint8_t)(TL_FLAG_RELIABLE | TL_FLAG_SYNC);
    const size_t n = build_frame(type, flags, seq, link->cfg.session_id, payload, len,
                                 link->pending.wire, sizeof link->pending.wire);
    if (n == 0u) {
        return TL_ERR_INVALID;
    }
    link->tx_seq_reliable++;
    link->pending.active = true;
    link->pending.sync = (flags & TL_FLAG_SYNC) != 0u;
    link->pending.type = type;
    link->pending.seq = seq;
    link->pending.retries = 0u;
    link->pending.wire_len = n;
    link->pending.sent_at_ms = link->hal.get_tick_ms(link->hal.ctx);
    if (seq_out != NULL) {
        *seq_out = seq;
    }
    write_wire(link, link->pending.wire, n);
    return TL_OK;
}

static void finish_pending(tl_link *link, bool delivered)
{
    const uint8_t seq = link->pending.seq;
    link->pending.active = false; /* cleared first: the callback may send again */
    if (delivered) {
        link->tx_synced = true;
        link->stats.tx_delivered++;
    } else {
        link->stats.tx_failed++;
    }
    if (link->cfg.on_tx_done != NULL) {
        link->cfg.on_tx_done(seq, delivered, link->cfg.tx_done_ctx);
    }
}

static void send_ack(tl_link *link, const tl_packet *pkt)
{
    /* The ACK of a SYNC frame echoes the sender's session id (see tl/link.h). */
    const bool sync = (pkt->flags & TL_FLAG_SYNC) != 0u;
    const uint8_t flags = sync ? (uint8_t)(TL_FLAG_ACK | TL_FLAG_SYNC) : TL_FLAG_ACK;
    uint8_t wire[TL_MAX_WIRE];
    const size_t n = build_frame(pkt->type, flags, pkt->seq, sync ? pkt->session : 0u, NULL, 0u,
                                 wire, sizeof wire);
    if (n != 0u) {
        write_wire(link, wire, n);
        link->stats.acks_sent++;
    }
}

static bool ack_matches_pending(const tl_link *link, const tl_packet *ack)
{
    if (!link->pending.active || ack->type != link->pending.type || ack->seq != link->pending.seq) {
        return false;
    }
    const bool ack_sync = (ack->flags & TL_FLAG_SYNC) != 0u;
    if (link->pending.sync) {
        /* Only an ACK for THIS boot's session completes a SYNC frame; an ACK
         * meant for our previous boot (same type and seq) is stale. */
        return ack_sync && ack->session == link->cfg.session_id;
    }
    return !ack_sync;
}

static void handle_packet(tl_link *link, const tl_packet *pkt)
{
    if ((pkt->flags & TL_FLAG_ACK) != 0u) {
        if (ack_matches_pending(link, pkt)) {
            finish_pending(link, true);
        } else {
            link->stats.rx_stale_acks++;
        }
        return;
    }
    if ((pkt->flags & TL_FLAG_RELIABLE) != 0u) {
        /* ACK first (also for duplicates: our previous ACK may have been lost).
         * The ACK means "received intact", not "handled" (see tl/link.h). */
        send_ack(link, pkt);
        if ((pkt->flags & TL_FLAG_SYNC) != 0u &&
            (!link->rx_have_session || pkt->session != link->rx_session)) {
            /* The peer restarted: its seq numbers start over, so the seq we
             * accepted from its previous session says nothing about this one. */
            link->rx_have_session = true;
            link->rx_session = pkt->session;
            link->rx_have_last_seq = false;
            link->stats.rx_new_sessions++;
        }
        if (link->rx_have_last_seq && pkt->seq == link->rx_last_seq) {
            link->stats.rx_duplicates++;
            return;
        }
        link->rx_have_last_seq = true;
        link->rx_last_seq = pkt->seq;
    }
    if (!tl_dispatch(&link->dispatcher, pkt)) {
        link->stats.rx_unknown_type++;
    }
}

static void handle_frame(tl_link *link, size_t raw_len)
{
    tl_packet pkt;
    switch (tl_packet_parse(link->rx_raw, raw_len, &pkt)) {
    case TL_PARSE_OK:
        link->stats.rx_frames_ok++;
        handle_packet(link, &pkt);
        break;
    case TL_PARSE_BAD_CRC:
        link->stats.rx_bad_crc++;
        break;
    case TL_PARSE_TOO_LONG:
        link->stats.rx_overflow++;
        break;
    case TL_PARSE_BAD_LENGTH:
        link->stats.rx_bad_length++;
        break;
    case TL_PARSE_TOO_SHORT:
    case TL_PARSE_BAD_HEADER:
    default:
        link->stats.rx_bad_header++;
        break;
    }
}

static void rx_byte(tl_link *link, uint8_t byte)
{
    size_t frame_len = 0u;
    switch (tl_cobs_stream_feed(&link->decoder, byte, &frame_len)) {
    case TL_COBS_FRAME_READY:
        handle_frame(link, frame_len);
        break;
    case TL_COBS_ERR_FRAMING:
        link->stats.rx_framing_errors++;
        break;
    case TL_COBS_ERR_OVERFLOW:
        link->stats.rx_overflow++;
        break;
    case TL_COBS_NEED_MORE:
    default:
        break;
    }
}

static void service_retransmit(tl_link *link)
{
    if (!link->pending.active) {
        return;
    }
    const uint32_t now = link->hal.get_tick_ms(link->hal.ctx);
    /* Unsigned subtraction is correct across the 2^32 tick wrap. */
    if ((uint32_t)(now - link->pending.sent_at_ms) < link->cfg.retransmit_timeout_ms) {
        return;
    }
    if (link->pending.retries >= link->cfg.max_retries) {
        finish_pending(link, false);
        return;
    }
    link->pending.retries++;
    link->pending.sent_at_ms = now;
    link->stats.tx_retransmits++;
    write_wire(link, link->pending.wire, link->pending.wire_len);
}

void tl_link_poll(tl_link *link)
{
    uint8_t chunk[RX_DRAIN_CHUNK];
    /* Only drain what is in the ring now, so a byte flood from the ISR cannot
     * keep this loop running forever and starve the retransmit timer. */
    size_t budget = tl_rb_count(&link->rx_ring);
    while (budget > 0u) {
        const size_t want = budget < sizeof chunk ? budget : sizeof chunk;
        const size_t n = tl_rb_pop_many(&link->rx_ring, chunk, want);
        if (n == 0u) {
            break;
        }
        budget -= n;
        for (size_t i = 0; i < n; i++) {
            rx_byte(link, chunk[i]);
        }
    }
    service_retransmit(link);
}
