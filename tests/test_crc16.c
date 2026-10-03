#include <string.h>

#include "tl/crc16.h"
#include "tl_test.h"

static const uint8_t check_input[] = "123456789";

static void test_catalogue_check_value(void)
{
    /* CRC RevEng catalogue, CRC-16/IBM-3740 (a.k.a. CCITT-FALSE): check=0x29b1 */
    CHECK_EQ(tl_crc16(check_input, 9), 0x29B1u);
    CHECK_EQ(tl_crc16_bitwise(check_input, 9), 0x29B1u);
}

static void test_vectors_from_python_binascii(void)
{
    /* Independent reference: Python's binascii.crc_hqx(data, 0xFFFF). */
    CHECK_EQ(tl_crc16(NULL, 0), 0xFFFFu);
    CHECK_EQ(tl_crc16((const uint8_t *)"A", 1), 0xB915u);
    const uint8_t zeros[4] = {0, 0, 0, 0};
    CHECK_EQ(tl_crc16(zeros, sizeof zeros), 0x84C0u);
    const uint8_t ff[2] = {0xFF, 0xFF};
    CHECK_EQ(tl_crc16(ff, sizeof ff), 0x0000u); /* init 0xFFFF cancels out */
    const char *fox = "The quick brown fox jumps over the lazy dog";
    CHECK_EQ(tl_crc16((const uint8_t *)fox, strlen(fox)), 0x8FDDu);
}

static void test_table_matches_bitwise(void)
{
    /* Every single-byte input exercises exactly one table entry. */
    for (unsigned v = 0; v < 256; v++) {
        const uint8_t b = (uint8_t)v;
        CHECK_EQ(tl_crc16(&b, 1), tl_crc16_bitwise(&b, 1));
    }
    uint8_t buf[300];
    uint32_t x = 12345u;
    for (size_t i = 0; i < sizeof buf; i++) {
        x = x * 1103515245u + 12345u;
        buf[i] = (uint8_t)(x >> 16);
    }
    for (size_t len = 0; len <= sizeof buf; len += 7) {
        CHECK_EQ(tl_crc16(buf, len), tl_crc16_bitwise(buf, len));
    }
}

static void test_incremental_equals_one_shot(void)
{
    const uint16_t part = tl_crc16_update(TL_CRC16_INIT, check_input, 4);
    CHECK_EQ(tl_crc16_update(part, check_input + 4, 5), 0x29B1u);
}

int main(void)
{
    RUN_TEST(test_catalogue_check_value);
    RUN_TEST(test_vectors_from_python_binascii);
    RUN_TEST(test_table_matches_bitwise);
    RUN_TEST(test_incremental_equals_one_shot);
    return TEST_REPORT();
}
