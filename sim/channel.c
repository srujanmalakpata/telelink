#include "channel.h"

void tl_rng_seed(tl_rng *rng, uint32_t seed)
{
    rng->state = seed != 0u ? seed : 0x9E3779B9u; /* xorshift must not start at 0 */
}

uint32_t tl_rng_next(tl_rng *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

bool tl_rng_chance_ppm(tl_rng *rng, uint32_t ppm)
{
    return ppm > 0u && (tl_rng_next(rng) % 1000000u) < ppm;
}

void tl_channel_init(tl_channel *ch, uint32_t seed, uint32_t flip_ppm, uint32_t drop_ppm)
{
    ch->head = 0u;
    ch->count = 0u;
    tl_rng_seed(&ch->rng, seed);
    ch->flip_ppm = flip_ppm;
    ch->drop_ppm = drop_ppm;
    ch->stats = (tl_channel_stats){0};
}

size_t tl_channel_write(tl_channel *ch, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        ch->stats.bytes_written++;
        if (tl_rng_chance_ppm(&ch->rng, ch->drop_ppm)) {
            ch->stats.bytes_dropped++;
            continue;
        }
        if (tl_rng_chance_ppm(&ch->rng, ch->flip_ppm)) {
            b ^= (uint8_t)(1u << (tl_rng_next(&ch->rng) % 8u));
            ch->stats.bytes_flipped++;
        }
        if (ch->count == TL_SIM_CHANNEL_FIFO) {
            ch->stats.bytes_fifo_overflow++;
            continue;
        }
        ch->fifo[(ch->head + ch->count) % TL_SIM_CHANNEL_FIFO] = b;
        ch->count++;
    }
    /* Like a real UART driver, the write is always "accepted"; loss happens
     * on the wire, invisible to the sender. */
    return len;
}

bool tl_channel_pop(tl_channel *ch, uint8_t *out)
{
    if (ch->count == 0u) {
        return false;
    }
    *out = ch->fifo[ch->head];
    ch->head = (ch->head + 1u) % TL_SIM_CHANNEL_FIFO;
    ch->count--;
    return true;
}

size_t tl_channel_pending(const tl_channel *ch)
{
    return ch->count;
}
