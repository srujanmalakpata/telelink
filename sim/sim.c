#include "sim.h"

#include <string.h>

#define MSG_CMD 0x10u
#define MSG_TELEMETRY 0x20u
#define CMD_PAYLOAD_LEN 16u
#define TELEMETRY_PAYLOAD_LEN 24u

typedef struct {
    tl_link link;
    uint8_t rx_storage[256];
    tl_channel *tx; /* outgoing wire */
    const uint32_t *clock_ms;
} endpoint;

typedef struct {
    endpoint host;
    endpoint device;
    tl_channel host_to_device;
    tl_channel device_to_host;
    uint32_t now_ms;
    uint8_t cmd_seen[TL_SIM_MAX_MESSAGES / 8u];
    uint8_t telemetry_seen[TL_SIM_MAX_MESSAGES / 8u];
    uint32_t max_cmd;
    uint32_t max_telemetry;
    tl_sim_result *res;
} sim_state;

/* ---- HAL bindings: the link only ever sees these two callbacks. */

static size_t ep_uart_write(void *ctx, const uint8_t *data, size_t len)
{
    endpoint *ep = ctx;
    return tl_channel_write(ep->tx, data, len);
}

static uint32_t ep_get_tick_ms(void *ctx)
{
    const endpoint *ep = ctx;
    return *ep->clock_ms;
}

/* ---- Deterministic payloads so receivers can verify content. */

static void fill_payload(uint8_t *p, size_t len, uint32_t id, uint8_t salt)
{
    p[0] = (uint8_t)(id & 0xFFu);
    p[1] = (uint8_t)((id >> 8) & 0xFFu);
    p[2] = (uint8_t)((id >> 16) & 0xFFu);
    p[3] = (uint8_t)((id >> 24) & 0xFFu);
    for (size_t i = 4; i < len; i++) {
        p[i] = (uint8_t)(id * 31u + (uint32_t)i * 7u + salt);
    }
}

/* Returns the id if the payload is exactly what fill_payload would produce. */
static int verify_payload(const tl_packet *pkt, size_t expect_len, uint8_t salt, uint32_t max_id,
                          uint32_t *id_out)
{
    if (pkt->len != expect_len) {
        return 0;
    }
    const uint32_t id = (uint32_t)pkt->payload[0] | ((uint32_t)pkt->payload[1] << 8) |
                        ((uint32_t)pkt->payload[2] << 16) | ((uint32_t)pkt->payload[3] << 24);
    if (id >= max_id) {
        return 0;
    }
    uint8_t expect[TL_MAX_PAYLOAD];
    fill_payload(expect, expect_len, id, salt);
    if (memcmp(expect, pkt->payload, expect_len) != 0) {
        return 0;
    }
    *id_out = id;
    return 1;
}

static int mark_seen(uint8_t *bitmap, uint32_t id)
{
    const uint8_t bit = (uint8_t)(1u << (id % 8u));
    const int already = (bitmap[id / 8u] & bit) != 0u;
    bitmap[id / 8u] |= bit;
    return already;
}

/* ---- Application handlers */

static void on_command(const tl_packet *pkt, void *ctx)
{
    sim_state *s = ctx;
    uint32_t id;
    if (!verify_payload(pkt, CMD_PAYLOAD_LEN, 0x5Au, s->max_cmd, &id)) {
        s->res->commands_corrupt_accepted++;
    } else if (mark_seen(s->cmd_seen, id)) {
        s->res->commands_dup_delivered++;
    } else {
        s->res->commands_delivered++;
    }
}

static void on_telemetry(const tl_packet *pkt, void *ctx)
{
    sim_state *s = ctx;
    uint32_t id;
    if (!verify_payload(pkt, TELEMETRY_PAYLOAD_LEN, 0xA5u, s->max_telemetry, &id)) {
        s->res->telemetry_corrupt_accepted++;
    } else if (!mark_seen(s->telemetry_seen, id)) {
        s->res->telemetry_delivered++;
    }
}

static void on_command_done(uint8_t seq, bool delivered, void *ctx)
{
    (void)seq;
    sim_state *s = ctx;
    if (delivered) {
        s->res->commands_acked++;
    } else {
        s->res->commands_failed++;
    }
}

tl_sim_config tl_sim_default_config(void)
{
    tl_sim_config c;
    c.seed = 1u;
    c.flip_ppm = 0u;
    c.drop_ppm = 0u;
    c.commands = 1000u;
    c.telemetry = 1000u;
    c.telemetry_period_ms = 5u;
    c.bytes_per_ms = 11u;
    c.retransmit_timeout_ms = 20u;
    c.max_retries = 8u;
    c.max_sim_ms = 10u * 60u * 1000u;
    return c;
}

static int init_endpoint(endpoint *ep, tl_channel *tx, const uint32_t *clock_ms,
                         const tl_link_config *cfg)
{
    ep->tx = tx;
    ep->clock_ms = clock_ms;
    const tl_hal hal = {.ctx = ep, .uart_write = ep_uart_write, .get_tick_ms = ep_get_tick_ms};
    return tl_link_init(&ep->link, cfg, &hal, ep->rx_storage, sizeof ep->rx_storage) == TL_OK ? 0
                                                                                              : -1;
}

static void clock_wire(tl_channel *wire, endpoint *rx, uint32_t bytes)
{
    uint8_t b;
    for (uint32_t i = 0; i < bytes && tl_channel_pop(wire, &b); i++) {
        (void)tl_link_rx_isr(&rx->link, b); /* the "UART RX interrupt" */
    }
}

int tl_sim_run(const tl_sim_config *cfg, tl_sim_result *out)
{
    if (cfg == NULL || out == NULL || cfg->commands > TL_SIM_MAX_MESSAGES ||
        cfg->telemetry > TL_SIM_MAX_MESSAGES || cfg->bytes_per_ms == 0u ||
        cfg->telemetry_period_ms == 0u || cfg->retransmit_timeout_ms == 0u) {
        return -1;
    }
    static sim_state s; /* ~30 KB; static keeps it off small thread stacks */
    memset(&s, 0, sizeof s);
    memset(out, 0, sizeof *out);
    s.res = out;
    s.max_cmd = cfg->commands;
    s.max_telemetry = cfg->telemetry;

    /* Independent noise streams per direction, both derived from one seed. */
    tl_channel_init(&s.host_to_device, cfg->seed, cfg->flip_ppm, cfg->drop_ppm);
    tl_channel_init(&s.device_to_host, cfg->seed ^ 0xA5A5A5A5u, cfg->flip_ppm, cfg->drop_ppm);

    const tl_link_config host_cfg = {.retransmit_timeout_ms = cfg->retransmit_timeout_ms,
                                     .max_retries = cfg->max_retries,
                                     .session_id = 0x484F5354u, /* "HOST" */
                                     .on_tx_done = on_command_done,
                                     .tx_done_ctx = &s};
    const tl_link_config dev_cfg = {.retransmit_timeout_ms = cfg->retransmit_timeout_ms,
                                    .max_retries = cfg->max_retries,
                                    .session_id = 0x44455643u, /* "DEVC" */
                                    .on_tx_done = NULL,
                                    .tx_done_ctx = NULL};
    if (init_endpoint(&s.host, &s.host_to_device, &s.now_ms, &host_cfg) != 0 ||
        init_endpoint(&s.device, &s.device_to_host, &s.now_ms, &dev_cfg) != 0) {
        return -1;
    }
    (void)tl_link_register(&s.device.link, MSG_CMD, on_command, &s);
    (void)tl_link_register(&s.host.link, MSG_TELEMETRY, on_telemetry, &s);

    uint8_t payload[TL_MAX_PAYLOAD];
    uint32_t t = 0u;
    bool completed = false;
    for (; t < cfg->max_sim_ms; t++) {
        s.now_ms = t;
        clock_wire(&s.host_to_device, &s.device, cfg->bytes_per_ms);
        clock_wire(&s.device_to_host, &s.host, cfg->bytes_per_ms);
        tl_link_poll(&s.device.link);
        tl_link_poll(&s.host.link);

        if (out->commands_sent < cfg->commands && !tl_link_tx_busy(&s.host.link)) {
            fill_payload(payload, CMD_PAYLOAD_LEN, out->commands_sent, 0x5Au);
            if (tl_link_send_reliable(&s.host.link, MSG_CMD, payload, CMD_PAYLOAD_LEN, NULL) ==
                TL_OK) {
                out->commands_sent++;
            }
        }
        if (out->telemetry_sent < cfg->telemetry && t % cfg->telemetry_period_ms == 0u) {
            fill_payload(payload, TELEMETRY_PAYLOAD_LEN, out->telemetry_sent, 0xA5u);
            if (tl_link_send(&s.device.link, MSG_TELEMETRY, payload, TELEMETRY_PAYLOAD_LEN) ==
                TL_OK) {
                out->telemetry_sent++;
            }
        }

        const bool all_sent =
            out->commands_sent == cfg->commands && out->telemetry_sent == cfg->telemetry;
        const bool quiet =
            !tl_link_tx_busy(&s.host.link) && tl_channel_pending(&s.host_to_device) == 0u &&
            tl_channel_pending(&s.device_to_host) == 0u && tl_link_rx_pending(&s.host.link) == 0u &&
            tl_link_rx_pending(&s.device.link) == 0u;
        if (all_sent && quiet) {
            completed = true;
            break;
        }
    }
    out->sim_ms = t;
    out->timed_out = !completed;
    out->host_stats = *tl_link_get_stats(&s.host.link);
    out->device_stats = *tl_link_get_stats(&s.device.link);
    out->host_to_device = s.host_to_device.stats;
    out->device_to_host = s.device_to_host.stats;
    out->host_ring_dropped = tl_link_rx_ring_dropped(&s.host.link);
    out->device_ring_dropped = tl_link_rx_ring_dropped(&s.device.link);
    return 0;
}
