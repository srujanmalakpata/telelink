/*
 * End-to-end: two full link stacks talking through the simulated noisy UART.
 */
#include <string.h>

#include "sim.h"
#include "tl_test.h"

static void test_clean_wire_delivers_everything_without_retries(void)
{
    tl_sim_config c = tl_sim_default_config();
    c.commands = 300;
    c.telemetry = 300;
    tl_sim_result r;
    CHECK_EQ(tl_sim_run(&c, &r), 0);
    CHECK_EQ(r.commands_sent, 300u);
    CHECK_EQ(r.commands_delivered, 300u);
    CHECK_EQ(r.commands_acked, 300u);
    CHECK_EQ(r.commands_failed, 0u);
    CHECK_EQ(r.telemetry_delivered, 300u);
    CHECK_EQ(r.host_stats.tx_retransmits, 0u);
    CHECK_EQ(r.host_stats.rx_bad_crc + r.device_stats.rx_bad_crc, 0u);
    CHECK_EQ(r.host_ring_dropped + r.device_ring_dropped, 0u);
    CHECK(r.sim_ms < c.max_sim_ms);
    CHECK(!r.timed_out);
}

static void test_noisy_wire_recovers_with_exactly_once_delivery(void)
{
    tl_sim_config c = tl_sim_default_config();
    c.seed = 42;
    c.flip_ppm = 5000; /* 0.5% of bytes get a bit flip */
    c.drop_ppm = 5000; /* 0.5% of bytes vanish */
    c.commands = 1000;
    c.telemetry = 1000;
    tl_sim_result r;
    CHECK_EQ(tl_sim_run(&c, &r), 0);
    /* Confirm injected noise, detected errors and retransmissions. */
    CHECK(r.host_to_device.bytes_flipped > 0u && r.host_to_device.bytes_dropped > 0u);
    CHECK(r.device_stats.rx_bad_crc + r.device_stats.rx_framing_errors > 0u);
    CHECK(r.host_stats.tx_retransmits > 0u);
    CHECK(r.device_stats.rx_duplicates > 0u); /* some ACKs were lost */
    /* Every reliable command resolves with an ACK for this seed. */
    CHECK_EQ(r.commands_sent, 1000u);
    CHECK_EQ(r.commands_acked + r.commands_failed, 1000u);
    CHECK_EQ(r.commands_failed, 0u);
    CHECK_EQ(r.commands_delivered, 1000u);
    CHECK_EQ(r.commands_dup_delivered, 0u);
    CHECK_EQ(r.commands_corrupt_accepted, 0u);
    /* Telemetry loses frames; no corrupted payload is accepted for this seed. */
    CHECK(r.telemetry_delivered < r.telemetry_sent);
    CHECK(!r.timed_out);
    /* 32 wire bytes per telemetry frame (30 raw, 31 after COBS, + delimiter) at
     * ~1% byte error rate: ~0.99^32 = 72.5% expected. */
    CHECK(r.telemetry_delivered > r.telemetry_sent * 6u / 10u);
    CHECK_EQ(r.telemetry_corrupt_accepted, 0u);
}

static void test_same_seed_is_deterministic(void)
{
    tl_sim_config c = tl_sim_default_config();
    c.seed = 7;
    c.flip_ppm = 2000;
    c.drop_ppm = 2000;
    c.commands = 200;
    c.telemetry = 200;
    tl_sim_result r1;
    tl_sim_result r2;
    CHECK_EQ(tl_sim_run(&c, &r1), 0);
    CHECK_EQ(tl_sim_run(&c, &r2), 0);
    CHECK_EQ(r1.sim_ms, r2.sim_ms);
    CHECK_EQ(r1.host_stats.tx_retransmits, r2.host_stats.tx_retransmits);
    CHECK_EQ(r1.telemetry_delivered, r2.telemetry_delivered);
    CHECK_EQ(r1.host_to_device.bytes_flipped, r2.host_to_device.bytes_flipped);
    c.seed = 8;
    tl_sim_result r3;
    CHECK_EQ(tl_sim_run(&c, &r3), 0);
    CHECK(r3.host_to_device.bytes_flipped != r1.host_to_device.bytes_flipped ||
          r3.host_stats.tx_retransmits != r1.host_stats.tx_retransmits);
}

static void test_hopeless_wire_reports_failures_instead_of_hanging(void)
{
    tl_sim_config c = tl_sim_default_config();
    c.seed = 3;
    c.drop_ppm = 300000; /* 30% of bytes lost: almost no frame survives */
    c.commands = 20;
    c.telemetry = 20;
    c.max_retries = 2;
    tl_sim_result r;
    CHECK_EQ(tl_sim_run(&c, &r), 0);
    CHECK_EQ(r.commands_sent, 20u);
    CHECK_EQ(r.commands_acked + r.commands_failed, 20u);
    CHECK(r.commands_failed > 0u);
    CHECK_EQ(r.commands_dup_delivered, 0u);
    CHECK(r.sim_ms < c.max_sim_ms);
    CHECK(!r.timed_out);
}

/* A run that cannot finish within max_sim_ms must say so instead of reporting
 * partial counts as if they were the result. */
static void test_time_limit_is_reported(void)
{
    tl_sim_config c = tl_sim_default_config();
    c.seed = 5;
    c.drop_ppm = 300000;
    c.commands = 50;
    c.telemetry = 50;
    c.retransmit_timeout_ms = 100000;
    c.max_retries = 255;
    c.max_sim_ms = 20000;
    tl_sim_result r;
    CHECK_EQ(tl_sim_run(&c, &r), 0);
    CHECK(r.timed_out);
    CHECK_EQ(r.sim_ms, c.max_sim_ms);
    CHECK(r.commands_sent < c.commands);
}

static void test_rejects_invalid_config(void)
{
    tl_sim_config c = tl_sim_default_config();
    tl_sim_result r;
    c.bytes_per_ms = 0;
    CHECK_EQ(tl_sim_run(&c, &r), -1);
    c = tl_sim_default_config();
    c.commands = TL_SIM_MAX_MESSAGES + 1u;
    CHECK_EQ(tl_sim_run(&c, &r), -1);
}

int main(void)
{
    RUN_TEST(test_clean_wire_delivers_everything_without_retries);
    RUN_TEST(test_noisy_wire_recovers_with_exactly_once_delivery);
    RUN_TEST(test_same_seed_is_deterministic);
    RUN_TEST(test_hopeless_wire_reports_failures_instead_of_hanging);
    RUN_TEST(test_time_limit_is_reported);
    RUN_TEST(test_rejects_invalid_config);
    return TEST_REPORT();
}
