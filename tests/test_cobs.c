#include <string.h>

#include "tl/cobs.h"
#include "tl_test.h"

/* Encode `in`, compare with `expect`, then decode back. */
static void check_vector(const uint8_t *in, size_t in_len, const uint8_t *expect, size_t expect_len)
{
    uint8_t enc[600];
    uint8_t dec[600];
    const size_t n = tl_cobs_encode(in, in_len, enc, sizeof enc);
    CHECK_EQ(n, expect_len);
    CHECK(n == expect_len && memcmp(enc, expect, n) == 0);
    size_t dec_len = 0;
    CHECK(tl_cobs_decode(enc, n, dec, sizeof dec, &dec_len));
    CHECK_EQ(dec_len, in_len);
    CHECK(memcmp(dec, in, in_len) == 0);
}

static void test_paper_examples(void)
{
    /* The standard examples (as listed in the COBS Wikipedia article). */
    check_vector((const uint8_t[]){0x00}, 1, (const uint8_t[]){0x01, 0x01}, 2);
    check_vector((const uint8_t[]){0x00, 0x00}, 2, (const uint8_t[]){0x01, 0x01, 0x01}, 3);
    check_vector((const uint8_t[]){0x00, 0x11, 0x00}, 3, (const uint8_t[]){0x01, 0x02, 0x11, 0x01},
                 4);
    check_vector((const uint8_t[]){0x11, 0x22, 0x00, 0x33}, 4,
                 (const uint8_t[]){0x03, 0x11, 0x22, 0x02, 0x33}, 5);
    check_vector((const uint8_t[]){0x11, 0x22, 0x33, 0x44}, 4,
                 (const uint8_t[]){0x05, 0x11, 0x22, 0x33, 0x44}, 5);
    check_vector((const uint8_t[]){0x11, 0x00, 0x00, 0x00}, 4,
                 (const uint8_t[]){0x02, 0x11, 0x01, 0x01, 0x01}, 5);
}

static void test_long_runs(void)
{
    uint8_t in[300];
    uint8_t expect[310];

    /* 01..FE (254 bytes) -> FF 01..FE : a full block, no trailing 01 */
    for (int i = 0; i < 254; i++) {
        in[i] = (uint8_t)(i + 1);
    }
    expect[0] = 0xFF;
    memcpy(expect + 1, in, 254);
    check_vector(in, 254, expect, 255);

    /* 00 01..FE (255 bytes) -> 01 FF 01..FE */
    in[0] = 0x00;
    for (int i = 1; i < 255; i++) {
        in[i] = (uint8_t)i;
    }
    expect[0] = 0x01;
    expect[1] = 0xFF;
    memcpy(expect + 2, in + 1, 254);
    check_vector(in, 255, expect, 256);

    /* 01..FF (255 bytes) -> FF 01..FE 02 FF */
    for (int i = 0; i < 255; i++) {
        in[i] = (uint8_t)(i + 1);
    }
    expect[0] = 0xFF;
    memcpy(expect + 1, in, 254);
    expect[255] = 0x02;
    expect[256] = 0xFF;
    check_vector(in, 255, expect, 257);

    /* 02..FF 00 (255 bytes) -> FF 02..FF 01 01 */
    for (int i = 0; i < 254; i++) {
        in[i] = (uint8_t)(i + 2);
    }
    in[254] = 0x00;
    expect[0] = 0xFF;
    memcpy(expect + 1, in, 254);
    expect[255] = 0x01;
    expect[256] = 0x01;
    check_vector(in, 255, expect, 257);

    /* 03..FF 00 01 (255 bytes) -> FE 03..FF 02 01 */
    for (int i = 0; i < 253; i++) {
        in[i] = (uint8_t)(i + 3);
    }
    in[253] = 0x00;
    in[254] = 0x01;
    expect[0] = 0xFE;
    memcpy(expect + 1, in, 253);
    expect[254] = 0x02;
    expect[255] = 0x01;
    check_vector(in, 255, expect, 256);
}

static void test_empty_input(void)
{
    uint8_t enc[4];
    CHECK_EQ(tl_cobs_encode(NULL, 0, enc, sizeof enc), 1u);
    CHECK_EQ(enc[0], 0x01u);
    size_t len = 99;
    uint8_t dec[4];
    CHECK(tl_cobs_decode(enc, 1, dec, sizeof dec, &len));
    CHECK_EQ(len, 0u);
}

static void test_roundtrip_random(void)
{
    uint8_t in[600];
    uint8_t enc[TL_COBS_MAX_ENCODED(600u)];
    uint8_t dec[600];
    uint32_t x = 0xC0FFEEu;
    for (size_t len = 0; len <= sizeof in; len++) {
        for (size_t i = 0; i < len; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            /* Bias toward zeros so blocks of every size appear. */
            in[i] = (x % 5u == 0u) ? 0u : (uint8_t)x;
        }
        const size_t n = tl_cobs_encode(in, len, enc, sizeof enc);
        CHECK(n >= 1u && n <= TL_COBS_MAX_ENCODED(len));
        CHECK(memchr(enc, 0, n) == NULL);
        size_t dec_len = 0;
        CHECK(tl_cobs_decode(enc, n, dec, sizeof dec, &dec_len));
        CHECK_EQ(dec_len, len);
        CHECK(memcmp(dec, in, len) == 0);
    }
}

static void test_encode_rejects_small_buffer(void)
{
    const uint8_t in[4] = {1, 2, 3, 4};
    uint8_t enc[4];
    CHECK_EQ(tl_cobs_encode(in, sizeof in, enc, sizeof enc), 0u);
}

static void test_decode_rejects_malformed(void)
{
    uint8_t dec[16];
    size_t len = 0;
    CHECK(!tl_cobs_decode((const uint8_t[]){0x01}, 0, dec, sizeof dec, &len)); /* empty */
    CHECK(!tl_cobs_decode((const uint8_t[]){0x00, 0x01}, 2, dec, sizeof dec, &len));
    CHECK(!tl_cobs_decode((const uint8_t[]){0x03, 0x11}, 2, dec, sizeof dec, &len)); /* truncated */
    CHECK(!tl_cobs_decode((const uint8_t[]){0x03, 0x11, 0x00}, 3, dec, sizeof dec, &len));
    /* Output capacity enforced, including the implied zero. */
    CHECK(!tl_cobs_decode((const uint8_t[]){0x05, 1, 2, 3, 4}, 5, dec, 3, &len));
    CHECK(!tl_cobs_decode((const uint8_t[]){0x02, 1, 0x01}, 3, dec, 1, &len));
    CHECK(tl_cobs_decode((const uint8_t[]){0x02, 1, 0x01}, 3, dec, 2, &len));
    CHECK_EQ(len, 2u);
    /* NULL pointers are rejected, not dereferenced. */
    CHECK(!tl_cobs_decode((const uint8_t[]){0x02, 1}, 2, NULL, 0, &len));
    CHECK(!tl_cobs_decode((const uint8_t[]){0x02, 1}, 2, dec, sizeof dec, NULL));
}

static void test_stream_frame_len_is_optional(void)
{
    uint8_t buf[8];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, buf, sizeof buf);
    CHECK(tl_cobs_stream_feed(&s, 0x02, NULL) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0x7E, NULL) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0x00, NULL) == TL_COBS_FRAME_READY);
    CHECK_EQ(buf[0], 0x7Eu);
}

/* ---- streaming decoder */

static void test_stream_back_to_back_frames(void)
{
    uint8_t buf[32];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, buf, sizeof buf);
    /* idle 00, frame [11 22 00 33], 00 00 idle, frame [00], trailing 00 */
    const uint8_t wire[] = {0x00, 0x03, 0x11, 0x22, 0x02, 0x33, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00};
    int frames = 0;
    for (size_t i = 0; i < sizeof wire; i++) {
        size_t len = 0;
        const tl_cobs_status st = tl_cobs_stream_feed(&s, wire[i], &len);
        CHECK(st == TL_COBS_NEED_MORE || st == TL_COBS_FRAME_READY);
        if (st == TL_COBS_FRAME_READY) {
            frames++;
            if (frames == 1) {
                CHECK_EQ(len, 4u);
                CHECK(memcmp(buf, (const uint8_t[]){0x11, 0x22, 0x00, 0x33}, 4) == 0);
            } else {
                CHECK_EQ(len, 1u);
                CHECK_EQ(buf[0], 0x00u);
            }
        }
    }
    CHECK_EQ(frames, 2);
}

static void test_stream_truncated_block_then_recovers(void)
{
    uint8_t buf[32];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, buf, sizeof buf);
    size_t len = 0;
    /* Block claims 4 data bytes, delimiter after 2: a byte was lost on the wire. */
    const uint8_t bad[] = {0x05, 0x11, 0x22};
    for (size_t i = 0; i < sizeof bad; i++) {
        CHECK(tl_cobs_stream_feed(&s, bad[i], &len) == TL_COBS_NEED_MORE);
    }
    CHECK(tl_cobs_stream_feed(&s, 0x00, &len) == TL_COBS_ERR_FRAMING);
    /* Next frame decodes normally. */
    CHECK(tl_cobs_stream_feed(&s, 0x02, &len) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0x7E, &len) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0x00, &len) == TL_COBS_FRAME_READY);
    CHECK_EQ(len, 1u);
    CHECK_EQ(buf[0], 0x7Eu);
}

static void test_stream_overflow_then_recovers(void)
{
    uint8_t buf[4];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, buf, sizeof buf);
    size_t len = 0;
    CHECK(tl_cobs_stream_feed(&s, 0x08, &len) == TL_COBS_NEED_MORE);
    for (uint8_t i = 1; i <= 7; i++) {
        CHECK(tl_cobs_stream_feed(&s, i, &len) == TL_COBS_NEED_MORE);
    }
    CHECK(s.discarding);
    CHECK(tl_cobs_stream_feed(&s, 0x00, &len) == TL_COBS_ERR_OVERFLOW);
    CHECK(tl_cobs_stream_feed(&s, 0x03, &len) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0xAA, &len) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0xBB, &len) == TL_COBS_NEED_MORE);
    CHECK(tl_cobs_stream_feed(&s, 0x00, &len) == TL_COBS_FRAME_READY);
    CHECK_EQ(len, 2u);
}

static void test_stream_matches_one_shot(void)
{
    /* For random encodings, byte-by-byte decoding equals the one-shot decoder. */
    uint8_t in[300];
    uint8_t enc[TL_COBS_MAX_ENCODED(300u)];
    uint8_t one[300];
    uint8_t sbuf[300];
    tl_cobs_stream s;
    tl_cobs_stream_init(&s, sbuf, sizeof sbuf);
    uint32_t x = 7u;
    for (size_t len = 0; len <= sizeof in; len += 3) {
        for (size_t i = 0; i < len; i++) {
            x = x * 1664525u + 1013904223u;
            in[i] = (x >> 24) % 4u == 0u ? 0u : (uint8_t)(x >> 8);
        }
        const size_t n = tl_cobs_encode(in, len, enc, sizeof enc);
        size_t one_len = 0;
        CHECK(tl_cobs_decode(enc, n, one, sizeof one, &one_len));
        size_t frame_len = 0;
        for (size_t i = 0; i < n; i++) {
            CHECK(tl_cobs_stream_feed(&s, enc[i], &frame_len) == TL_COBS_NEED_MORE);
        }
        CHECK(tl_cobs_stream_feed(&s, 0x00, &frame_len) == TL_COBS_FRAME_READY);
        CHECK_EQ(frame_len, one_len);
        CHECK(memcmp(sbuf, one, one_len) == 0);
    }
}

int main(void)
{
    RUN_TEST(test_paper_examples);
    RUN_TEST(test_long_runs);
    RUN_TEST(test_empty_input);
    RUN_TEST(test_roundtrip_random);
    RUN_TEST(test_encode_rejects_small_buffer);
    RUN_TEST(test_decode_rejects_malformed);
    RUN_TEST(test_stream_back_to_back_frames);
    RUN_TEST(test_stream_truncated_block_then_recovers);
    RUN_TEST(test_stream_overflow_then_recovers);
    RUN_TEST(test_stream_matches_one_shot);
    RUN_TEST(test_stream_frame_len_is_optional);
    return TEST_REPORT();
}
