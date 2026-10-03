#include "tl/dispatch.h"
#include "tl_test.h"

typedef struct {
    int calls;
    uint8_t last_type;
} counter;

static void count_handler(const tl_packet *pkt, void *ctx)
{
    counter *c = ctx;
    c->calls++;
    c->last_type = pkt->type;
}

static void other_handler(const tl_packet *pkt, void *ctx)
{
    (void)pkt;
    (*(int *)ctx) += 100;
}

static void test_routes_by_type_with_context(void)
{
    tl_dispatcher d;
    tl_dispatch_init(&d);
    counter a = {0, 0};
    int b = 0;
    CHECK(tl_dispatch_register(&d, 0x10, count_handler, &a) == TL_REG_OK);
    CHECK(tl_dispatch_register(&d, 0x11, other_handler, &b) == TL_REG_OK);

    tl_packet p = {.type = 0x10};
    CHECK(tl_dispatch(&d, &p));
    CHECK_EQ(a.calls, 1);
    CHECK_EQ(a.last_type, 0x10u);
    p.type = 0x11;
    CHECK(tl_dispatch(&d, &p));
    CHECK_EQ(b, 100);
    CHECK_EQ(a.calls, 1);
}

static void test_unknown_type_returns_false(void)
{
    tl_dispatcher d;
    tl_dispatch_init(&d);
    const tl_packet p = {.type = 0x33};
    CHECK(!tl_dispatch(&d, &p));
}

static void test_registration_errors(void)
{
    tl_dispatcher d;
    tl_dispatch_init(&d);
    counter c = {0, 0};
    CHECK(tl_dispatch_register(&d, 0, count_handler, &c) == TL_REG_INVALID);
    CHECK(tl_dispatch_register(&d, 1, NULL, &c) == TL_REG_INVALID);
    CHECK(tl_dispatch_register(&d, 1, count_handler, &c) == TL_REG_OK);
    CHECK(tl_dispatch_register(&d, 1, count_handler, &c) == TL_REG_DUPLICATE);
    for (unsigned t = 2; t <= TL_MAX_HANDLERS; t++) {
        CHECK(tl_dispatch_register(&d, (uint8_t)t, count_handler, &c) == TL_REG_OK);
    }
    CHECK(tl_dispatch_register(&d, 0xFE, count_handler, &c) == TL_REG_FULL);
}

int main(void)
{
    RUN_TEST(test_routes_by_type_with_context);
    RUN_TEST(test_unknown_type_returns_false);
    RUN_TEST(test_registration_errors);
    return TEST_REPORT();
}
