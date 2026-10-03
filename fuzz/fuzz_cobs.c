/*
 * libFuzzer target for the COBS codec.
 *
 * Properties checked on arbitrary input:
 *  1. The streaming decoder (fed byte-by-byte, as from a UART ISR) never
 *     crashes and agrees exactly with the one-shot decoder on every segment
 *     between delimiters: same accept/reject decision, same bytes.
 *  2. Every non-empty segment produces exactly one outcome, and the right one:
 *     ERR_OVERFLOW if it decodes to more than the buffer holds, ERR_FRAMING if
 *     its last block is cut short, FRAME_READY otherwise. Empty segments (idle
 *     delimiters) produce nothing. A decoder that silently swallowed a bad
 *     frame would fail here.
 *  3. encode -> decode round-trips the input, and the encoding has no 0x00.
 * ASan/UBSan catch out-of-bounds or undefined behaviour along the way.
 */
#include <stdlib.h>
#include <string.h>

#include "tl/cobs.h"

#define CAP 300u

/* Reference model of the outcome for one delimited segment (no 0x00 inside):
 * how many bytes a decoder would output, and whether the last block is complete. */
static tl_cobs_status expected_outcome(const uint8_t *seg, size_t seg_len)
{
    if (seg_len == 0u) {
        return TL_COBS_NEED_MORE;
    }
    size_t out = 0;
    size_t i = 0;
    uint8_t prev_code = 0;
    while (i < seg_len) {
        const uint8_t code = seg[i++];
        if (prev_code != 0u && prev_code != 0xFFu) {
            out++; /* the zero implied by the previous block */
        }
        const size_t want = (size_t)code - 1u;
        const size_t have = seg_len - i < want ? seg_len - i : want;
        out += have;
        i += have;
        if (have < want) {
            return out > CAP ? TL_COBS_ERR_OVERFLOW : TL_COBS_ERR_FRAMING;
        }
        prev_code = code;
    }
    return out > CAP ? TL_COBS_ERR_OVERFLOW : TL_COBS_FRAME_READY;
}

static void check_segment(const uint8_t *seg, size_t seg_len, tl_cobs_status st,
                          const uint8_t *stream_buf, size_t frame_len)
{
    if (st != expected_outcome(seg, seg_len)) {
        abort(); /* wrong outcome, or a non-empty frame that produced none */
    }
    if (seg_len == 0u) {
        return;
    }
    uint8_t one[CAP];
    size_t one_len = 0;
    const bool ok = tl_cobs_decode(seg, seg_len, one, sizeof one, &one_len);
    if (ok != (st == TL_COBS_FRAME_READY)) {
        abort();
    }
    if (ok && (one_len != frame_len || memcmp(one, stream_buf, one_len) != 0)) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Properties 1 and 2: stream vs one-shot and vs the reference outcome on
     * each delimited segment. */
    uint8_t sbuf[CAP];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, sbuf, sizeof sbuf);
    size_t seg_start = 0;
    for (size_t i = 0; i < size; i++) {
        size_t frame_len = 0;
        const tl_cobs_status st = tl_cobs_stream_feed(&s, data[i], &frame_len);
        if (data[i] == 0u) {
            check_segment(data + seg_start, i - seg_start, st, sbuf, frame_len);
            seg_start = i + 1u;
        } else if (st != TL_COBS_NEED_MORE) {
            abort(); /* only a delimiter may complete a frame */
        }
    }

    /* Property 3: round trip. */
    if (size <= 1024u) {
        uint8_t enc[TL_COBS_MAX_ENCODED(1024u)];
        uint8_t dec[1024];
        const size_t n = tl_cobs_encode(data, size, enc, sizeof enc);
        if (n == 0u || n > TL_COBS_MAX_ENCODED(size) || memchr(enc, 0, n) != NULL) {
            abort();
        }
        size_t dec_len = 0;
        if (!tl_cobs_decode(enc, n, dec, sizeof dec, &dec_len) || dec_len != size ||
            (size > 0u && memcmp(dec, data, size) != 0)) {
            abort();
        }
    }
    return 0;
}
