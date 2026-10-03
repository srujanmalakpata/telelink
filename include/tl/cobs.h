/*
 * Consistent Overhead Byte Stuffing (COBS).
 *
 * COBS rewrites a buffer so it contains no 0x00 bytes, at a cost of at most
 * one byte per 254 bytes of payload (+1). A 0x00 can then be used as an
 * unambiguous frame delimiter on a byte stream such as a UART: a receiver that
 * joins mid-stream or loses bytes re-synchronises at the next 0x00.
 *
 * The encoder emits the canonical encoding (matches the examples in
 * Cheshire & Baker, "Consistent Overhead Byte Stuffing", 1999). It does NOT
 * append the 0x00 delimiter; the framing layer does that.
 */
#ifndef TL_COBS_H
#define TL_COBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Worst-case encoded size for `n` input bytes (no delimiter). */
#define TL_COBS_MAX_ENCODED(n) ((n) + ((n) / 254u) + 1u)

/* Encode `len` bytes. Requires dst_cap >= TL_COBS_MAX_ENCODED(len).
 * Returns the encoded length (always >= 1), or 0 if dst_cap is too small. */
size_t tl_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap);

/* Decode one complete COBS block sequence (delimiter already removed).
 * Returns false on malformed input (zero byte, truncated block, empty input),
 * a NULL pointer, or if the output would exceed dst_cap. */
bool tl_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap, size_t *out_len);

/* ---- Streaming decoder: fed one byte at a time, e.g. from a UART ISR ring. */

typedef enum {
    TL_COBS_NEED_MORE = 0, /* byte consumed, no frame boundary yet */
    TL_COBS_FRAME_READY,   /* delimiter seen; buf[0..frame_len) holds a frame */
    TL_COBS_ERR_FRAMING,   /* delimiter arrived in the middle of a block */
    TL_COBS_ERR_OVERFLOW   /* frame longer than the buffer; it was discarded */
} tl_cobs_status;

typedef struct {
    uint8_t *buf; /* caller-provided decode buffer */
    size_t cap;
    size_t len;        /* decoded bytes so far in the current frame */
    uint8_t code;      /* code byte of the current block, 0 = no block yet */
    uint8_t remaining; /* data bytes still expected in the current block */
    bool discarding;   /* overflowed: drop bytes until the next delimiter */
} tl_cobs_stream;

void tl_cobs_stream_init(tl_cobs_stream *s, uint8_t *buf, size_t cap);

/* Drop any partial frame (e.g. after a UART framing/noise error). */
void tl_cobs_stream_reset(tl_cobs_stream *s);

/* Feed one received byte. Every 0x00 delimiter that ends a non-empty frame
 * produces exactly one of FRAME_READY / ERR_FRAMING / ERR_OVERFLOW; idle
 * delimiters (empty frames) produce NEED_MORE. On FRAME_READY, *frame_len is
 * set (if frame_len is not NULL) and the frame stays valid in buf until the
 * next call. `s` must have been initialised with tl_cobs_stream_init. */
tl_cobs_status tl_cobs_stream_feed(tl_cobs_stream *s, uint8_t byte, size_t *frame_len);

#endif /* TL_COBS_H */
