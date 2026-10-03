/* tl_sim: run the two-endpoint noisy-UART simulation and print delivery stats. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim.h"

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--seed N] [--flip-ppm N] [--drop-ppm N] [--commands N]\n"
            "          [--telemetry N] [--timeout-ms N] [--retries N] [--sweep]\n"
            "  flip/drop are per-byte probabilities in parts per million.\n"
            "  --sweep runs a fixed table of noise levels with the given seed.\n"
            "  Exit status 1 if any duplicate or corrupted message reached a handler,\n"
            "  3 if the run hit the simulated-time limit before finishing.\n",
            argv0);
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    const unsigned long v = strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v > 0xFFFFFFFFul) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static double pct(uint32_t num, uint32_t den)
{
    return den == 0u ? 0.0 : 100.0 * (double)num / (double)den;
}

static void print_report(const tl_sim_config *c, const tl_sim_result *r)
{
    printf("seed=%u flip_ppm=%u drop_ppm=%u timeout_ms=%u max_retries=%u bytes_per_ms=%u\n",
           c->seed, c->flip_ppm, c->drop_ppm, c->retransmit_timeout_ms, c->max_retries,
           c->bytes_per_ms);
    printf("simulated time         : %u ms%s\n", r->sim_ms,
           r->timed_out ? " (INCOMPLETE: hit the time limit, counts are partial)" : "");
    printf("wire host->device      : %u bytes, %u flipped, %u dropped\n",
           r->host_to_device.bytes_written, r->host_to_device.bytes_flipped,
           r->host_to_device.bytes_dropped);
    printf("wire device->host      : %u bytes, %u flipped, %u dropped\n",
           r->device_to_host.bytes_written, r->device_to_host.bytes_flipped,
           r->device_to_host.bytes_dropped);
    printf("commands (reliable)    : sent %u, acked %u, failed %u, delivered %u (%.2f%%)\n",
           r->commands_sent, r->commands_acked, r->commands_failed, r->commands_delivered,
           pct(r->commands_delivered, c->commands));
    printf("  retransmits          : %u, duplicates suppressed at device: %u\n",
           r->host_stats.tx_retransmits, r->device_stats.rx_duplicates);
    printf("  duplicate deliveries : %u, undetected corruption: %u\n", r->commands_dup_delivered,
           r->commands_corrupt_accepted);
    printf("telemetry (best effort): sent %u, delivered %u (%.2f%%), undetected corruption: %u\n",
           r->telemetry_sent, r->telemetry_delivered, pct(r->telemetry_delivered, c->telemetry),
           r->telemetry_corrupt_accepted);
    printf("rx errors device       : bad_crc %u, framing %u, overflow %u, bad_header %u, "
           "bad_length %u\n",
           r->device_stats.rx_bad_crc, r->device_stats.rx_framing_errors,
           r->device_stats.rx_overflow, r->device_stats.rx_bad_header,
           r->device_stats.rx_bad_length);
    printf("rx errors host         : bad_crc %u, framing %u, overflow %u, bad_header %u, "
           "bad_length %u\n",
           r->host_stats.rx_bad_crc, r->host_stats.rx_framing_errors, r->host_stats.rx_overflow,
           r->host_stats.rx_bad_header, r->host_stats.rx_bad_length);
}

/* The integrity properties the link promises: nothing delivered twice, nothing
 * corrupted delivered. Throughput and loss are reported, not asserted. */
static int invariants_hold(const tl_sim_result *r)
{
    return r->commands_dup_delivered == 0u && r->commands_corrupt_accepted == 0u &&
           r->telemetry_corrupt_accepted == 0u;
}

/* Exit status for one run: integrity failures first, then an incomplete run. */
static int run_status(const tl_sim_result *r)
{
    if (!invariants_hold(r)) {
        fprintf(stderr, "FAIL: a duplicate or corrupted message reached a handler\n");
        return 1;
    }
    if (r->timed_out) {
        fprintf(stderr, "FAIL: simulated-time limit reached before the run finished\n");
        return 3;
    }
    return 0;
}

static int run_sweep(tl_sim_config base)
{
    int status = 0;
    static const uint32_t levels[] = {0u, 100u, 1000u, 5000u, 10000u, 20000u};
    printf("| flip ppm | drop ppm | cmds delivered | cmds failed | retransmits | "
           "telemetry delivered | bad CRC | framing | dup deliveries | undetected | sim ms |\n");
    printf("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++) {
        tl_sim_config c = base;
        c.flip_ppm = levels[i];
        c.drop_ppm = levels[i];
        tl_sim_result r;
        if (tl_sim_run(&c, &r) != 0) {
            return 1;
        }
        printf(
            "| %u | %u | %u/%u (%.2f%%) | %u | %u | %u/%u (%.2f%%) | %u | %u | %u | %u | %u%s |\n",
            c.flip_ppm, c.drop_ppm, r.commands_delivered, c.commands,
            pct(r.commands_delivered, c.commands), r.commands_failed, r.host_stats.tx_retransmits,
            r.telemetry_delivered, c.telemetry, pct(r.telemetry_delivered, c.telemetry),
            r.host_stats.rx_bad_crc + r.device_stats.rx_bad_crc,
            r.host_stats.rx_framing_errors + r.device_stats.rx_framing_errors,
            r.commands_dup_delivered, r.commands_corrupt_accepted + r.telemetry_corrupt_accepted,
            r.sim_ms, r.timed_out ? " (INCOMPLETE)" : "");
        const int st = run_status(&r);
        if (st != 0 && (status == 0 || st < status)) {
            status = st; /* report the most serious failure (1 before 3) */
        }
    }
    return status;
}

int main(int argc, char **argv)
{
    tl_sim_config cfg = tl_sim_default_config();
    cfg.flip_ppm = 1000u;
    cfg.drop_ppm = 1000u;
    int sweep = 0;

    for (int i = 1; i < argc; i++) {
        uint32_t v = 0u;
        const char *a = argv[i];
        if (strcmp(a, "--sweep") == 0) {
            sweep = 1;
            continue;
        }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc || parse_u32(argv[i + 1], &v) != 0) {
            usage(argv[0]);
            return 2;
        }
        i++;
        if (strcmp(a, "--seed") == 0) {
            cfg.seed = v;
        } else if (strcmp(a, "--flip-ppm") == 0) {
            cfg.flip_ppm = v;
        } else if (strcmp(a, "--drop-ppm") == 0) {
            cfg.drop_ppm = v;
        } else if (strcmp(a, "--commands") == 0) {
            cfg.commands = v;
        } else if (strcmp(a, "--telemetry") == 0) {
            cfg.telemetry = v;
        } else if (strcmp(a, "--timeout-ms") == 0) {
            cfg.retransmit_timeout_ms = v;
        } else if (strcmp(a, "--retries") == 0 && v <= 255u) {
            cfg.max_retries = (uint8_t)v;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (sweep) {
        return run_sweep(cfg);
    }
    tl_sim_result r;
    if (tl_sim_run(&cfg, &r) != 0) {
        fprintf(stderr, "invalid configuration\n");
        return 2;
    }
    print_report(&cfg, &r);
    return run_status(&r);
}
