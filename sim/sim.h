/*
 * Two-endpoint simulation over noisy UART channels.
 *
 *   HOST  --reliable commands (type 0x10)-->  DEVICE
 *   HOST  <--telemetry, fire-and-forget (0x20)-- DEVICE
 *   (ACKs flow back over the opposite channel and are subject to noise too.)
 *
 * Every payload is a deterministic function of its message id, so the
 * receiver can tell an intact message from one corrupted in a way the CRC
 * failed to catch ("undetected corruption").
 */
#ifndef TL_SIM_H
#define TL_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "channel.h"
#include "tl/link.h"

#define TL_SIM_MAX_MESSAGES 65536u

typedef struct {
    uint32_t seed;
    uint32_t flip_ppm;
    uint32_t drop_ppm;
    uint32_t commands;  /* reliable messages host -> device */
    uint32_t telemetry; /* unreliable messages device -> host */
    uint32_t telemetry_period_ms;
    uint32_t bytes_per_ms; /* 11 ~= 115200 baud 8N1 */
    uint32_t retransmit_timeout_ms;
    uint8_t max_retries;
    uint32_t max_sim_ms; /* safety stop */
} tl_sim_config;

typedef struct {
    uint32_t commands_sent;             /* first transmissions */
    uint32_t commands_acked;            /* sender saw the ACK */
    uint32_t commands_failed;           /* sender gave up */
    uint32_t commands_delivered;        /* unique, intact, handed to the handler */
    uint32_t commands_dup_delivered;    /* handler saw the same id twice (must be 0) */
    uint32_t commands_corrupt_accepted; /* passed CRC but payload wrong */
    uint32_t telemetry_sent;
    uint32_t telemetry_delivered; /* intact */
    uint32_t telemetry_corrupt_accepted;
    uint32_t sim_ms;
    bool timed_out; /* hit max_sim_ms before everything was sent and resolved */
    tl_link_stats host_stats;
    tl_link_stats device_stats;
    tl_channel_stats host_to_device;
    tl_channel_stats device_to_host;
    uint32_t host_ring_dropped;
    uint32_t device_ring_dropped;
} tl_sim_result;

tl_sim_config tl_sim_default_config(void);

/* Runs to completion (all commands resolved, telemetry sent, wires drained)
 * or until max_sim_ms, in which case out->timed_out is set and the counts are
 * partial. Returns 0 if the run happened (check timed_out), -1 on invalid config. */
int tl_sim_run(const tl_sim_config *cfg, tl_sim_result *out);

#endif /* TL_SIM_H */
