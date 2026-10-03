#include "tl/ringbuf.h"
#include "tl_test.h"

static void test_init_validates_capacity(void)
{
    tl_ringbuf rb;
    uint8_t storage[64];
    CHECK(!tl_rb_init(&rb, storage, 0));
    CHECK(!tl_rb_init(&rb, storage, 1));
    CHECK(!tl_rb_init(&rb, storage, 3));
    CHECK(!tl_rb_init(&rb, storage, 48));
    CHECK(!tl_rb_init(&rb, NULL, 64));
    CHECK(tl_rb_init(&rb, storage, 2));
    CHECK(tl_rb_init(&rb, storage, 64));
    CHECK_EQ(tl_rb_capacity(&rb), 64u);
    CHECK_EQ(tl_rb_count(&rb), 0u);
}

static void test_fifo_order_and_full_empty(void)
{
    tl_ringbuf rb;
    uint8_t storage[8];
    CHECK(tl_rb_init(&rb, storage, sizeof storage));
    uint8_t out = 0;
    CHECK(!tl_rb_pop(&rb, &out));
    for (uint8_t i = 0; i < 8; i++) {
        CHECK(tl_rb_push(&rb, (uint8_t)(i + 100)));
    }
    /* All 8 slots usable: free-running counters need no sacrificial slot. */
    CHECK_EQ(tl_rb_count(&rb), 8u);
    CHECK(!tl_rb_push(&rb, 0xEE));
    for (uint8_t i = 0; i < 8; i++) {
        CHECK(tl_rb_pop(&rb, &out));
        CHECK_EQ(out, i + 100u);
    }
    CHECK(!tl_rb_pop(&rb, &out));
    CHECK_EQ(tl_rb_count(&rb), 0u);
}

static void test_counter_wraparound(void)
{
    tl_ringbuf rb;
    uint8_t storage[4];
    CHECK(tl_rb_init(&rb, storage, sizeof storage));
    /* Start just below 2^32 so head/tail overflow during the test. */
    atomic_store(&rb.head, 0xFFFFFFFEu);
    atomic_store(&rb.tail, 0xFFFFFFFEu);
    uint8_t out = 0;
    for (uint8_t round = 0; round < 10; round++) {
        for (uint8_t i = 0; i < 4; i++) {
            CHECK(tl_rb_push(&rb, (uint8_t)(round * 4 + i)));
        }
        CHECK(!tl_rb_push(&rb, 0xEE));
        CHECK_EQ(tl_rb_count(&rb), 4u);
        for (uint8_t i = 0; i < 4; i++) {
            CHECK(tl_rb_pop(&rb, &out));
            CHECK_EQ(out, round * 4u + i);
        }
        CHECK(!tl_rb_pop(&rb, &out));
    }
}

static void test_pop_many(void)
{
    tl_ringbuf rb;
    uint8_t storage[16];
    CHECK(tl_rb_init(&rb, storage, sizeof storage));
    uint8_t out[16] = {0};
    uint8_t next_in = 0;
    uint8_t next_out = 0;
    /* Interleave so the read window straddles the physical end of storage. */
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 11; i++) {
            CHECK(tl_rb_push(&rb, next_in++));
        }
        const size_t n = tl_rb_pop_many(&rb, out, 5);
        CHECK_EQ(n, 5u);
        size_t rest = tl_rb_pop_many(&rb, out + 5, sizeof out - 5);
        CHECK_EQ(rest, 6u);
        for (size_t i = 0; i < n + rest; i++) {
            CHECK_EQ(out[i], next_out);
            next_out++;
        }
    }
    CHECK_EQ(tl_rb_pop_many(&rb, out, sizeof out), 0u);
}

int main(void)
{
    RUN_TEST(test_init_validates_capacity);
    RUN_TEST(test_fifo_order_and_full_empty);
    RUN_TEST(test_counter_wraparound);
    RUN_TEST(test_pop_many);
    return TEST_REPORT();
}
