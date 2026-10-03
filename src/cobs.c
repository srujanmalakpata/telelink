#include "tl/cobs.h"

size_t tl_cobs_encode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap)
{
    if (dst == NULL || (src == NULL && len > 0u) || dst_cap < TL_COBS_MAX_ENCODED(len)) {
        return 0u;
    }
    size_t code_idx = 0u; /* where the current block's code byte goes */
    size_t out = 1u;
    uint8_t code = 1u; /* 1 + number of data bytes in the current block */

    for (size_t i = 0; i < len; i++) {
        if (src[i] == 0u) {
            dst[code_idx] = code;
            code_idx = out++;
            code = 1u;
        } else {
            dst[out++] = src[i];
            code++;
            /* A full block (254 data bytes) carries no implied zero. Only open
             * a new block if more input follows: this keeps the encoding
             * canonical (no redundant trailing 0x01). */
            if (code == 0xFFu && i + 1u < len) {
                dst[code_idx] = code;
                code_idx = out++;
                code = 1u;
            }
        }
    }
    dst[code_idx] = code;
    return out;
}

bool tl_cobs_decode(const uint8_t *src, size_t len, uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (src == NULL || dst == NULL || len == 0u || out_len == NULL) {
        return false;
    }
    size_t in = 0u;
    size_t out = 0u;
    while (in < len) {
        const uint8_t code = src[in++];
        if (code == 0u) {
            return false;
        }
        for (uint8_t k = 1u; k < code; k++) {
            if (in >= len || src[in] == 0u || out >= dst_cap) {
                return false;
            }
            dst[out++] = src[in++];
        }
        /* Every block except a full one, and except the last, ends in a zero. */
        if (code != 0xFFu && in < len) {
            if (out >= dst_cap) {
                return false;
            }
            dst[out++] = 0u;
        }
    }
    *out_len = out;
    return true;
}

void tl_cobs_stream_init(tl_cobs_stream *s, uint8_t *buf, size_t cap)
{
    s->buf = buf;
    s->cap = cap;
    tl_cobs_stream_reset(s);
}

void tl_cobs_stream_reset(tl_cobs_stream *s)
{
    s->len = 0u;
    s->code = 0u;
    s->remaining = 0u;
    s->discarding = false;
}

static bool stream_append(tl_cobs_stream *s, uint8_t byte)
{
    if (s->len >= s->cap) {
        s->discarding = true;
        return false;
    }
    s->buf[s->len++] = byte;
    return true;
}

tl_cobs_status tl_cobs_stream_feed(tl_cobs_stream *s, uint8_t byte, size_t *frame_len)
{
    if (byte == 0u) {
        tl_cobs_status status;
        if (s->discarding) {
            status = TL_COBS_ERR_OVERFLOW;
        } else if (s->code == 0u) {
            status = TL_COBS_NEED_MORE; /* empty frame: idle line / back-to-back delimiters */
        } else if (s->remaining != 0u) {
            status = TL_COBS_ERR_FRAMING;
        } else {
            if (frame_len != NULL) {
                *frame_len = s->len;
            }
            status = TL_COBS_FRAME_READY;
        }
        tl_cobs_stream_reset(s); /* buf contents survive until the next byte */
        return status;
    }

    if (s->discarding) {
        return TL_COBS_NEED_MORE;
    }
    if (s->remaining == 0u) {
        /* Code byte. The previous block (if any, and not full) implied a zero
         * that is only emitted now that we know the frame continues. */
        if (s->code != 0u && s->code != 0xFFu) {
            (void)stream_append(s, 0u);
        }
        s->code = byte;
        s->remaining = (uint8_t)(byte - 1u);
    } else {
        (void)stream_append(s, byte);
        s->remaining--;
    }
    return TL_COBS_NEED_MORE;
}
