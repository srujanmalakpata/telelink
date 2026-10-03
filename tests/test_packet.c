#include <stdio.h>
#include <string.h>

#include "tl/crc16.h"
#include "tl/packet.h"
#include "tl_test.h"

static tl_packet make_packet(uint8_t type, uint8_t flags, uint8_t seq, uint8_t len)
{
    tl_packet p;
    memset(&p, 0, sizeof p);
    p.type = type;
    p.flags = flags;
    p.seq = seq;
    p.len = len;
    for (uint8_t i = 0; i < len; i++) {
        p.payload[i] = (uint8_t)(i * 13u); /* includes zeros, exercises COBS */
    }
    return p;
}

static void test_serialize_layout(void)
{
    tl_packet p = make_packet(0x42, TL_FLAG_RELIABLE, 7, 2);
    p.payload[0] = 0xAB;
    p.payload[1] = 0xCD;
    uint8_t raw[TL_MAX_RAW];
    const size_t n = tl_packet_serialize(&p, raw, sizeof raw);
    CHECK_EQ(n, 8u);
    CHECK_EQ(raw[0], 0x42u);
    CHECK_EQ(raw[1], TL_FLAG_RELIABLE);
    CHECK_EQ(raw[2], 7u);
    CHECK_EQ(raw[3], 2u); /* payload length */
    CHECK_EQ(raw[4], 0xABu);
    CHECK_EQ(raw[5], 0xCDu);
    const uint16_t crc = tl_crc16(raw, 6);
    CHECK_EQ(raw[6], crc >> 8); /* big-endian CRC */
    CHECK_EQ(raw[7], crc & 0xFFu);
}

static void test_roundtrip_all_lengths(void)
{
    for (unsigned len = 0; len <= TL_MAX_PAYLOAD; len++) {
        const tl_packet p = make_packet(9, 0, (uint8_t)len, (uint8_t)len);
        uint8_t raw[TL_MAX_RAW];
        const size_t n = tl_packet_serialize(&p, raw, sizeof raw);
        CHECK_EQ(n, TL_MIN_RAW + len);
        tl_packet q;
        CHECK(tl_packet_parse(raw, n, &q) == TL_PARSE_OK);
        CHECK_EQ(q.type, 9u);
        CHECK_EQ(q.seq, len);
        CHECK_EQ(q.len, len);
        CHECK(memcmp(q.payload, p.payload, len) == 0);
    }
}

static void test_serialize_rejects_invalid(void)
{
    uint8_t raw[TL_MAX_RAW];
    tl_packet p = make_packet(0, 0, 0, 0); /* type 0 reserved */
    CHECK_EQ(tl_packet_serialize(&p, raw, sizeof raw), 0u);
    p = make_packet(1, 0x80, 0, 0); /* reserved flag bit */
    CHECK_EQ(tl_packet_serialize(&p, raw, sizeof raw), 0u);
    p = make_packet(1, 0, 0, 4);
    CHECK_EQ(tl_packet_serialize(&p, raw, 9), 0u); /* needs 10 */
    CHECK_EQ(tl_packet_serialize(&p, raw, 10), 10u);
}

static void test_crc_catches_every_single_bit_flip(void)
{
    const tl_packet p = make_packet(0x10, TL_FLAG_RELIABLE, 200, 32);
    uint8_t raw[TL_MAX_RAW];
    const size_t n = tl_packet_serialize(&p, raw, sizeof raw);
    tl_packet q;
    unsigned detected = 0;
    for (size_t bit = 0; bit < n * 8u; bit++) {
        raw[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
        detected += tl_packet_parse(raw, n, &q) == TL_PARSE_BAD_CRC;
        raw[bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
    }
    CHECK_EQ(detected, n * 8u);
    CHECK(tl_packet_parse(raw, n, &q) == TL_PARSE_OK);
}

static void test_parse_length_and_header_errors(void)
{
    tl_packet q;
    uint8_t raw[TL_MAX_RAW + 1] = {0};
    CHECK(tl_packet_parse(raw, TL_MIN_RAW - 1u, &q) == TL_PARSE_TOO_SHORT);
    CHECK(tl_packet_parse(raw, TL_MAX_RAW + 1, &q) == TL_PARSE_TOO_LONG);

    /* Valid CRC but semantically bad headers. */
    const uint8_t bad_headers[][4] = {
        {0x00, 0x00, 0x01, 0x00},                          /* type 0 */
        {0x01, 0x08, 0x01, 0x00},                          /* reserved flag */
        {0x01, TL_FLAG_ACK | TL_FLAG_RELIABLE, 0x01, 0x00} /* ACK must not be reliable */
    };
    for (size_t i = 0; i < 3; i++) {
        memcpy(raw, bad_headers[i], 4);
        const uint16_t crc = tl_crc16(raw, 4);
        raw[4] = (uint8_t)(crc >> 8);
        raw[5] = (uint8_t)crc;
        CHECK(tl_packet_parse(raw, 6, &q) == TL_PARSE_BAD_HEADER);
    }
    /* ACK carrying a payload is rejected too. */
    const uint8_t ack_with_payload[5] = {0x01, TL_FLAG_ACK, 0x01, 0x01, 0x55};
    memcpy(raw, ack_with_payload, 5);
    const uint16_t crc = tl_crc16(raw, 5);
    raw[5] = (uint8_t)(crc >> 8);
    raw[6] = (uint8_t)crc;
    CHECK(tl_packet_parse(raw, 7, &q) == TL_PARSE_BAD_HEADER);
}

/* Write header bytes + a valid CRC into raw; returns the raw length. */
static size_t with_crc(uint8_t *raw, const uint8_t *body, size_t body_len)
{
    memcpy(raw, body, body_len);
    const uint16_t crc = tl_crc16(raw, body_len);
    raw[body_len] = (uint8_t)(crc >> 8);
    raw[body_len + 1] = (uint8_t)crc;
    return body_len + 2;
}

static void test_sync_frame_carries_session(void)
{
    tl_packet p = make_packet(0x10, TL_FLAG_RELIABLE | TL_FLAG_SYNC, 3, TL_MAX_PAYLOAD);
    p.session = 0x01020304u;
    uint8_t raw[TL_MAX_RAW];
    const size_t n = tl_packet_serialize(&p, raw, sizeof raw);
    CHECK_EQ(n, TL_MAX_RAW);          /* the largest possible packet */
    CHECK_EQ(raw[3], TL_MAX_PAYLOAD); /* length byte counts the payload only */
    CHECK_EQ(raw[4], 0x01u);          /* session is big-endian, right after len */
    CHECK_EQ(raw[7], 0x04u);
    CHECK_EQ(raw[8], p.payload[0]);
    tl_packet q;
    CHECK(tl_packet_parse(raw, n, &q) == TL_PARSE_OK);
    CHECK_EQ(q.session, 0x01020304u);
    CHECK_EQ(q.len, TL_MAX_PAYLOAD);
    CHECK(memcmp(q.payload, p.payload, TL_MAX_PAYLOAD) == 0);

    /* Without SYNC there is no session on the wire and it parses as 0. */
    p.flags = TL_FLAG_RELIABLE;
    const size_t m = tl_packet_serialize(&p, raw, sizeof raw);
    CHECK_EQ(m, TL_MAX_RAW - TL_SESSION_SIZE);
    CHECK(tl_packet_parse(raw, m, &q) == TL_PARSE_OK);
    CHECK_EQ(q.session, 0u);
}

static void test_sync_header_rules(void)
{
    uint8_t raw[TL_MAX_RAW];
    tl_packet q;
    tl_packet p = make_packet(0x10, TL_FLAG_SYNC, 0, 0); /* SYNC needs RELIABLE */
    CHECK_EQ(tl_packet_serialize(&p, raw, sizeof raw), 0u);
    p.flags = TL_FLAG_ACK; /* an ACK never carries a payload */
    p.len = 1;
    CHECK_EQ(tl_packet_serialize(&p, raw, sizeof raw), 0u);
    p.flags = TL_FLAG_SYNC | TL_FLAG_ACK; /* not even with SYNC */
    CHECK_EQ(tl_packet_serialize(&p, raw, sizeof raw), 0u);
    p.flags = TL_FLAG_SYNC | TL_FLAG_ACK | TL_FLAG_RELIABLE; /* an ACK is never reliable */
    p.len = 0;
    CHECK_EQ(tl_packet_serialize(&p, raw, sizeof raw), 0u);

    /* The ACK of a SYNC frame is (type, seq, session) and round-trips. */
    p.flags = TL_FLAG_SYNC | TL_FLAG_ACK;
    p.session = 0xA1B2C3D4u;
    const size_t n = tl_packet_serialize(&p, raw, sizeof raw);
    CHECK_EQ(n, TL_HEADER_SIZE + TL_SESSION_SIZE + TL_CRC_SIZE);
    CHECK(tl_packet_parse(raw, n, &q) == TL_PARSE_OK);
    CHECK_EQ(q.flags, TL_FLAG_SYNC | TL_FLAG_ACK);
    CHECK_EQ(q.session, 0xA1B2C3D4u);
    CHECK_EQ(q.len, 0u);

    /* Parser: a SYNC ACK must carry exactly the 4-byte session. */
    const uint8_t ack_short[] = {0x10, TL_FLAG_ACK | TL_FLAG_SYNC, 0, 0, 0xAA, 0xBB, 0xCC};
    CHECK(tl_packet_parse(raw, with_crc(raw, ack_short, sizeof ack_short), &q) ==
          TL_PARSE_BAD_HEADER);
    const uint8_t ack_long[] = {0x10, TL_FLAG_ACK | TL_FLAG_SYNC, 0, 1, 1, 2, 3, 4, 5};
    CHECK(tl_packet_parse(raw, with_crc(raw, ack_long, sizeof ack_long), &q) ==
          TL_PARSE_BAD_HEADER);

    /* Parser: a SYNC frame too short to hold the 4-byte session. */
    const uint8_t short_sync[] = {0x10, TL_FLAG_RELIABLE | TL_FLAG_SYNC, 0, 0, 0xAA, 0xBB, 0xCC};
    CHECK(tl_packet_parse(raw, with_crc(raw, short_sync, sizeof short_sync), &q) ==
          TL_PARSE_BAD_HEADER);
    const uint8_t sync_no_rel[] = {0x10, TL_FLAG_SYNC, 0, 0, 1, 2, 3, 4};
    CHECK(tl_packet_parse(raw, with_crc(raw, sync_no_rel, sizeof sync_no_rel), &q) ==
          TL_PARSE_BAD_HEADER);
}

/* A non-SYNC frame may be up to TL_MAX_RAW bytes long on the wire, but its
 * payload must still be <= TL_MAX_PAYLOAD. */
static void test_parse_rejects_oversized_payload_without_session(void)
{
    uint8_t body[TL_HEADER_SIZE + TL_MAX_PAYLOAD + TL_SESSION_SIZE] = {0x20, 0, 0, TL_MAX_PAYLOAD};
    uint8_t raw[TL_MAX_RAW];
    tl_packet q;
    for (size_t extra = 1; extra <= TL_SESSION_SIZE; extra++) {
        const size_t body_len = TL_HEADER_SIZE + TL_MAX_PAYLOAD + extra;
        CHECK(tl_packet_parse(raw, with_crc(raw, body, body_len), &q) == TL_PARSE_TOO_LONG);
    }
    CHECK(tl_packet_parse(raw, with_crc(raw, body, TL_HEADER_SIZE + TL_MAX_PAYLOAD), &q) ==
          TL_PARSE_OK);
}

/* The length byte catches a frame that lost (or gained) bytes even when the
 * last two bytes happen to pass as a CRC: here the CRC is recomputed to model
 * exactly that collision. */
static void test_length_byte_must_match_frame(void)
{
    const tl_packet p = make_packet(0x20, 0, 3, 10);
    uint8_t raw[TL_MAX_RAW];
    uint8_t body[TL_MAX_RAW];
    tl_packet q;
    const size_t n = tl_packet_serialize(&p, raw, sizeof raw);
    const size_t body_len = n - TL_CRC_SIZE;
    memcpy(body, raw, body_len);

    /* Truncated: last payload byte lost, CRC "collides". */
    CHECK(tl_packet_parse(raw, with_crc(raw, body, body_len - 1u), &q) == TL_PARSE_BAD_LENGTH);
    /* Extended: an extra byte, CRC "collides". */
    body[body_len] = 0x77;
    CHECK(tl_packet_parse(raw, with_crc(raw, body, body_len + 1u), &q) == TL_PARSE_BAD_LENGTH);
    /* Length byte itself wrong. */
    body[3] = 9;
    CHECK(tl_packet_parse(raw, with_crc(raw, body, body_len), &q) == TL_PARSE_BAD_LENGTH);
    body[3] = 10;
    CHECK(tl_packet_parse(raw, with_crc(raw, body, body_len), &q) == TL_PARSE_OK);
}

static void test_frame_encode_has_single_delimiter(void)
{
    const tl_packet p = make_packet(0x20, 0, 1, TL_MAX_PAYLOAD);
    uint8_t wire[TL_MAX_WIRE];
    const size_t n = tl_frame_encode(&p, wire, sizeof wire);
    CHECK(n > 0u && n <= TL_MAX_WIRE);
    CHECK_EQ(wire[n - 1], 0u);
    CHECK(memchr(wire, 0, n - 1) == NULL);
    /* Decode back through COBS + parse. */
    uint8_t raw[TL_MAX_RAW];
    size_t raw_len = 0;
    CHECK(tl_cobs_decode(wire, n - 1, raw, sizeof raw, &raw_len));
    tl_packet q;
    CHECK(tl_packet_parse(raw, raw_len, &q) == TL_PARSE_OK);
    CHECK_EQ(q.len, TL_MAX_PAYLOAD);
    CHECK_EQ(tl_frame_encode(&p, wire, 10), 0u);
}

/* ---- Wire-level single bit flips (what a noisy UART actually does). */

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Marks which bytes of an encoded frame (delimiter excluded) are COBS code bytes. */
static void mark_code_bytes(const uint8_t *enc, size_t enc_len, uint8_t *is_code)
{
    memset(is_code, 0, enc_len);
    size_t i = 0;
    while (i < enc_len) {
        is_code[i] = 1;
        i += enc[i];
    }
}

typedef struct {
    unsigned long trials;
    unsigned long data_flips; /* data byte flipped and still non-zero: one raw bit changes */
    unsigned long data_ok;    /* ...frames that still parsed OK (must be 0) */
    unsigned long zero_flips; /* a byte flipped into 0x00: early delimiter */
    unsigned long zero_wrong; /* ...wrong packets accepted */
    unsigned long code_flips; /* COBS code byte flipped, still non-zero: layout shifts */
    unsigned long code_wrong; /* ...wrong packets accepted (CRC collision) */
    unsigned long drops;      /* one byte of the frame lost */
    unsigned long drop_wrong; /* ...wrong packets accepted */
} flip_stats;

/* Feed `wire` through a fresh stream decoder; count frames that parse OK and
 * frames that parse OK but differ from `want`. */
static void feed_wire(const uint8_t *wire, size_t n, const tl_packet *want, unsigned long *ok,
                      unsigned long *wrong)
{
    uint8_t buf[TL_MAX_RAW];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, buf, sizeof buf);
    for (size_t i = 0; i < n; i++) {
        size_t len = 0;
        if (tl_cobs_stream_feed(&s, wire[i], &len) != TL_COBS_FRAME_READY) {
            continue;
        }
        tl_packet got;
        if (tl_packet_parse(buf, len, &got) != TL_PARSE_OK) {
            continue;
        }
        (*ok)++;
        if (got.type != want->type || got.flags != want->flags || got.seq != want->seq ||
            got.len != want->len || got.session != want->session ||
            memcmp(got.payload, want->payload, got.len) != 0) {
            (*wrong)++;
        }
    }
}

/* Pins what the CRC + length byte really guarantee on the wire.
 * test_crc_catches_every_single_bit_flip flips bits of the DECODED packet;
 * here every bit of the ENCODED frame is flipped, and every byte is dropped. */
static void test_wire_bit_flips_and_drops(void)
{
    flip_stats st = {0};
    uint32_t rng = 0x2545F491u;
    for (unsigned k = 0; k < 2000u; k++) {
        const uint32_t r = xorshift32(&rng);
        tl_packet p;
        memset(&p, 0, sizeof p);
        p.type = (uint8_t)(1u + r % 255u);
        p.flags = (r & 0x100u) != 0u ? (uint8_t)(TL_FLAG_RELIABLE | TL_FLAG_SYNC) : 0u;
        p.seq = (uint8_t)(r >> 16);
        p.session = p.flags != 0u ? xorshift32(&rng) : 0u;
        p.len = (uint8_t)(xorshift32(&rng) % (TL_MAX_PAYLOAD + 1u));
        for (size_t i = 0; i < p.len; i++) {
            const uint32_t b = xorshift32(&rng);
            p.payload[i] = (b & 7u) == 0u ? 0u : (uint8_t)(b >> 8); /* 1 in 8 is a zero */
        }
        uint8_t wire[TL_MAX_WIRE];
        const size_t n = tl_frame_encode(&p, wire, sizeof wire);
        CHECK(n > 1u);
        if (n < 2u) {
            continue;
        }
        uint8_t is_code[TL_MAX_WIRE];
        mark_code_bytes(wire, n - 1u, is_code);

        uint8_t bad[TL_MAX_WIRE];
        for (size_t i = 0; i + 1u < n; i++) { /* every byte except the delimiter */
            for (unsigned bit = 0; bit < 8u; bit++) {
                memcpy(bad, wire, n);
                bad[i] ^= (uint8_t)(1u << bit);
                unsigned long ok = 0;
                unsigned long wrong = 0;
                feed_wire(bad, n, &p, &ok, &wrong);
                st.trials++;
                if (bad[i] == 0u) {
                    st.zero_flips++;
                    st.zero_wrong += wrong;
                } else if (is_code[i] != 0u) {
                    st.code_flips++;
                    st.code_wrong += wrong;
                } else {
                    st.data_flips++;
                    st.data_ok += ok;
                }
            }
            /* Drop byte i. */
            memcpy(bad, wire, i);
            memcpy(bad + i, wire + i + 1u, n - i - 1u);
            unsigned long ok = 0;
            st.drops++;
            feed_wire(bad, n - 1u, &p, &ok, &st.drop_wrong);
        }
    }
    printf("  %lu flips + %lu drops: data flips %lu (%lu accepted), flips to 0x00 %lu "
           "(%lu wrong accepted), code-byte flips %lu (%lu wrong accepted), drops "
           "(%lu wrong accepted)\n",
           st.trials, st.drops, st.data_flips, st.data_ok, st.zero_flips, st.zero_wrong,
           st.code_flips, st.code_wrong, st.drop_wrong);
    /* Guarantee: a flip that keeps the COBS layout changes exactly one raw bit,
     * and CRC-16 detects every single-bit error. */
    CHECK(st.data_flips > 0u);
    CHECK_EQ(st.data_ok, 0u);
    /* An early delimiter or a lost byte changes the frame length; the length
     * byte rejects that unless the error hit the length byte itself, where only
     * the CRC is left. Measured: none got through with this seed. */
    CHECK(st.zero_flips > 0u);
    CHECK_EQ(st.zero_wrong, 0u);
    CHECK_EQ(st.drop_wrong, 0u);
    /* No guarantee: a code-byte flip moves the implied zeros but keeps the
     * length, so only the CRC catches it (~2^-16 slip through per bad frame). */
    CHECK(st.code_flips > 0u);
    CHECK(st.code_wrong < st.code_flips / 1000u);
}

int main(void)
{
    RUN_TEST(test_serialize_layout);
    RUN_TEST(test_roundtrip_all_lengths);
    RUN_TEST(test_serialize_rejects_invalid);
    RUN_TEST(test_crc_catches_every_single_bit_flip);
    RUN_TEST(test_parse_length_and_header_errors);
    RUN_TEST(test_frame_encode_has_single_delimiter);
    RUN_TEST(test_sync_frame_carries_session);
    RUN_TEST(test_sync_header_rules);
    RUN_TEST(test_parse_rejects_oversized_payload_without_session);
    RUN_TEST(test_length_byte_must_match_frame);
    RUN_TEST(test_wire_bit_flips_and_drops);
    return TEST_REPORT();
}
