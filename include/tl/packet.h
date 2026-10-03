/*
 * Packet format (before COBS):
 *
 *   +------+-------+-----+-----+------------------------+----------------+-----------+
 *   | type | flags | seq | len | session (only if SYNC) | payload (len B)| CRC16 (BE)|
 *   +------+-------+-----+-----+------------------------+----------------+-----------+
 *     1 B    1 B     1 B   1 B   4 B, big-endian          0..TL_MAX_PAYLOAD  2 B
 *
 * The CRC (CRC-16/CCITT-FALSE) covers everything before it. On the wire:
 * COBS(packet) followed by a single 0x00 delimiter. Frame boundaries come from
 * the delimiter, never from `len`: the length byte is only a cross-check that
 * the parser compares with the frame length it actually received.
 *
 * Why the length byte (one extra byte per frame): the CRC's guarantees apply
 * to the DECODED packet. On the wire, a single bit flip inside a COBS data byte
 * that leaves it non-zero changes exactly one raw bit, and CRC-16 always
 * detects that. But a flip that turns a byte into 0x00 (an early delimiter),
 * or a dropped byte, makes the decoded frame shorter while the last two bytes
 * received are read as the CRC, which then passes by chance with probability
 * about 2^-16. With `len`, such a frame is rejected deterministically unless
 * the error hit the length byte itself. A flip that changes a COBS code byte
 * without creating a 0x00 moves the implied zeros and keeps the length; that
 * case is still caught only with probability about 1 - 2^-16. Measured by
 * test_wire_bit_flips_and_drops in tests/test_packet.c.
 *
 * The 4-byte session id is present only when the SYNC flag is set. A sender
 * sets SYNC on its reliable frames until the first one is ACKed, so the
 * receiver learns that the sender restarted, and the ACK of a SYNC frame
 * echoes the session id so the sender can tell it from a stale ACK sent to its
 * previous boot (see tl/link.h).
 */
#ifndef TL_PACKET_H
#define TL_PACKET_H

#include <stddef.h>
#include <stdint.h>

#include "tl/cobs.h"

#ifndef TL_MAX_PAYLOAD
#define TL_MAX_PAYLOAD 64u
#endif

_Static_assert(TL_MAX_PAYLOAD >= 1u && TL_MAX_PAYLOAD <= 255u,
               "TL_MAX_PAYLOAD must fit the uint8_t length field");

#define TL_HEADER_SIZE 4u  /* type, flags, seq, len */
#define TL_SESSION_SIZE 4u /* only present when TL_FLAG_SYNC is set */
#define TL_CRC_SIZE 2u
#define TL_MIN_RAW (TL_HEADER_SIZE + TL_CRC_SIZE)
#define TL_MAX_RAW (TL_HEADER_SIZE + TL_SESSION_SIZE + TL_MAX_PAYLOAD + TL_CRC_SIZE)
/* Encoded frame plus the trailing 0x00 delimiter. */
#define TL_MAX_WIRE (TL_COBS_MAX_ENCODED(TL_MAX_RAW) + 1u)

/* flags */
#define TL_FLAG_RELIABLE 0x01u /* receiver must ACK; duplicates suppressed */
#define TL_FLAG_ACK 0x02u      /* this is an ACK for (type, seq) */
#define TL_FLAG_SYNC 0x04u     /* frame carries a session id (reliable frame or its ACK) */
#define TL_FLAG_MASK (TL_FLAG_RELIABLE | TL_FLAG_ACK | TL_FLAG_SYNC)

typedef struct {
    uint8_t type; /* application message type; 0 is reserved/invalid */
    uint8_t flags;
    uint8_t seq;
    uint8_t len;      /* payload length */
    uint32_t session; /* sender's session id; meaningful only with TL_FLAG_SYNC */
    uint8_t payload[TL_MAX_PAYLOAD];
} tl_packet;

typedef enum {
    TL_PARSE_OK = 0,
    TL_PARSE_TOO_SHORT, /* fewer than header + CRC bytes */
    TL_PARSE_TOO_LONG,  /* payload larger than TL_MAX_PAYLOAD */
    TL_PARSE_BAD_CRC,
    TL_PARSE_BAD_HEADER, /* CRC ok but: type 0, reserved flag bits, ACK that is reliable
                            or carries a payload (an ACK with SYNC carries exactly the
                            4-byte session), SYNC on a frame that is neither RELIABLE nor
                            an ACK, or a SYNC frame too short to hold the session id */
    TL_PARSE_BAD_LENGTH  /* CRC ok but the length byte disagrees with the received frame:
                            bytes were lost or a 0x00 was injected, and the CRC collided */
} tl_parse_result;

/* Serialise header(+session)+payload+CRC into `out`. Returns bytes written, or
 * 0 if the packet is invalid (same header rules as tl_packet_parse) or `cap` is
 * too small (TL_MAX_RAW always suffices). */
size_t tl_packet_serialize(const tl_packet *pkt, uint8_t *out, size_t cap);

/* Validate and decode a raw (already COBS-decoded) packet. */
tl_parse_result tl_packet_parse(const uint8_t *raw, size_t len, tl_packet *out);

/* Serialise + COBS-encode + append the 0x00 delimiter. Returns wire bytes
 * written, or 0 on error (TL_MAX_WIRE always suffices). */
size_t tl_frame_encode(const tl_packet *pkt, uint8_t *wire, size_t cap);

#endif /* TL_PACKET_H */
