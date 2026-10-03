/*
 * Link endpoint: UART byte stream <-> validated, dispatched packets, with an
 * optional stop-and-wait ACK/retransmit channel for commands.
 *
 * Concurrency contract:
 *   - tl_link_rx_isr() may be called from ONE interrupt context (it only pushes
 *     into the lock-free SPSC ring buffer).
 *   - Every other function is called from ONE thread/main-loop context.
 *     Handlers run inside tl_link_poll() and may call tl_link_send*().
 *
 * Reliability model:
 *   - tl_link_send(): fire-and-forget (telemetry). Own sequence counter so
 *     receivers can spot gaps.
 *   - tl_link_send_reliable(): one outstanding frame (window = 1). Retransmitted
 *     every `retransmit_timeout_ms` until ACKed or `max_retries` is exhausted;
 *     the outcome is reported via `on_tx_done`. Receivers suppress duplicates by
 *     remembering the last reliable sequence number, giving exactly-once
 *     delivery while the sender has not given up AND the receiver has not
 *     restarted. That memory is in RAM: if the receiver reboots after it
 *     dispatched a command but before its ACK reached the sender, the
 *     retransmission is dispatched again. Across receiver reboots delivery is
 *     at-least-once, so commands should be idempotent (or the application keeps
 *     its own dedupe state in backup/no-init RAM).
 *   - What an ACK means: the frame arrived intact and was accepted by the link
 *     (or was a duplicate of the last accepted frame). It does NOT mean a
 *     handler ran: a frame whose type has no handler is still ACKed and only
 *     counted in `rx_unknown_type` on the receiver. An ACK completes the
 *     pending frame only if its type and seq match (and, for a SYNC frame,
 *     it echoes this boot's session id; see below).
 *
 * Sender restarts (sessions):
 *   The reliable sequence counter starts at 0 after every tl_link_init(). Left
 *   alone, a restarted sender's first command could reuse the seq the receiver
 *   accepted last and be ACKed but dropped as a "duplicate". To prevent that,
 *   every reliable frame carries the SYNC flag and the 4-byte `session_id`
 *   until the first one is ACKed. A receiver that sees SYNC with a session id
 *   different from the one it knows forgets its last accepted seq.
 *   The ACK of a SYNC frame echoes the session id, and a pending SYNC frame is
 *   completed only by an ACK carrying this boot's id. So an ACK addressed to
 *   the sender's previous boot (still buffered in a USB-UART adapter, say)
 *   cannot complete the first command of the new boot.
 *   `session_id` MUST differ between boots of a node that sends reliable
 *   frames (a boot counter kept in flash/backup RAM, or a hardware RNG value).
 */
#ifndef TL_LINK_H
#define TL_LINK_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tl/cobs.h"
#include "tl/dispatch.h"
#include "tl/hal.h"
#include "tl/packet.h"
#include "tl/ringbuf.h"

typedef void (*tl_tx_done_fn)(uint8_t seq, bool delivered, void *ctx);

typedef struct {
    uint32_t retransmit_timeout_ms; /* must be > 0 */
    uint8_t max_retries;            /* attempts = 1 + max_retries */
    uint32_t session_id;            /* must change on every boot; see "Sender restarts" */
    tl_tx_done_fn on_tx_done;       /* optional */
    void *tx_done_ctx;
} tl_link_config;

typedef struct {
    /* receive path */
    uint32_t rx_frames_ok; /* parsed and accepted (incl. ACKs) */
    uint32_t rx_bad_crc;
    uint32_t rx_framing_errors; /* COBS block cut short by a delimiter, or decode error */
    uint32_t rx_overflow;       /* frame too long: > TL_MAX_RAW, or payload > TL_MAX_PAYLOAD */
    uint32_t rx_bad_header;     /* too short, reserved bits, type 0 */
    uint32_t rx_bad_length;     /* CRC passed but the length byte disagrees (lost bytes) */
    uint32_t rx_duplicates;     /* reliable frame seen again (our ACK was lost) */
    uint32_t rx_unknown_type;   /* no handler registered */
    uint32_t rx_stale_acks;     /* ACK that matches nothing in flight (type, seq, session) */
    uint32_t rx_new_sessions;   /* SYNC frames that announced a new peer session */
    /* transmit path */
    uint32_t tx_frames; /* every frame written, incl. ACKs and retransmits */
    uint32_t tx_retransmits;
    uint32_t tx_delivered;    /* reliable frames ACKed */
    uint32_t tx_failed;       /* reliable frames abandoned after max_retries */
    uint32_t tx_write_errors; /* HAL accepted fewer bytes than requested */
    uint32_t acks_sent;
} tl_link_stats;

typedef enum {
    TL_OK = 0,
    TL_ERR_INVALID, /* bad argument (type 0, payload too long, NULL) */
    TL_ERR_BUSY     /* a reliable frame is already in flight */
} tl_status;

/* The fields are visible only so callers can allocate a tl_link statically.
 * Treat them as private: use tl_link_get_stats(), tl_link_rx_pending(),
 * tl_link_rx_ring_dropped() and tl_link_tx_busy() instead. */
typedef struct {
    tl_hal hal;
    tl_link_config cfg;

    /* ISR -> main-loop handoff */
    tl_ringbuf rx_ring;
    _Atomic uint32_t rx_ring_dropped; /* written only by the ISR */

    tl_cobs_stream decoder;
    uint8_t rx_raw[TL_MAX_RAW];
    tl_dispatcher dispatcher;
    tl_link_stats stats;

    uint8_t tx_seq_unreliable;
    uint8_t tx_seq_reliable;
    bool tx_synced; /* a reliable frame of this session was ACKed: stop sending SYNC */

    bool rx_have_session; /* rx_session holds the peer's last announced session id */
    uint32_t rx_session;
    bool rx_have_last_seq;
    uint8_t rx_last_seq;

    struct {
        bool active;
        bool sync; /* sent with SYNC: only an ACK echoing our session_id completes it */
        uint8_t type;
        uint8_t seq;
        uint8_t retries;
        uint32_t sent_at_ms;
        size_t wire_len;
        uint8_t wire[TL_MAX_WIRE]; /* encoded once, resent verbatim */
    } pending;
} tl_link;

/* rx_storage: power-of-two sized buffer for the ISR ring (e.g. 128 or 256). */
tl_status tl_link_init(tl_link *link, const tl_link_config *cfg, const tl_hal *hal,
                       uint8_t *rx_storage, size_t rx_capacity);

tl_reg_result tl_link_register(tl_link *link, uint8_t type, tl_handler_fn fn, void *ctx);

/* ISR-safe. Returns false if the ring was full and the byte was dropped. */
bool tl_link_rx_isr(tl_link *link, uint8_t byte);

/* Main loop: drain received bytes, handle frames/ACKs, run retransmit timers. */
void tl_link_poll(tl_link *link);

tl_status tl_link_send(tl_link *link, uint8_t type, const uint8_t *payload, size_t len);

tl_status tl_link_send_reliable(tl_link *link, uint8_t type, const uint8_t *payload, size_t len,
                                uint8_t *seq_out);

bool tl_link_tx_busy(const tl_link *link);

/* Bytes dropped because the ISR ring was full (main loop too slow). */
uint32_t tl_link_rx_ring_dropped(const tl_link *link);

/* Counters (main-loop context). The pointer stays valid as long as `link`. */
const tl_link_stats *tl_link_get_stats(const tl_link *link);

/* Received bytes waiting in the ISR ring for the next tl_link_poll()
 * (main-loop context; may under-report while the ISR is adding bytes). */
size_t tl_link_rx_pending(const tl_link *link);

#endif /* TL_LINK_H */
