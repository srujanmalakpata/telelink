/*
 * Simulated noisy UART wire (host only). Bytes written by one endpoint pass
 * through a deterministic noise model and wait in a FIFO until the simulator
 * clocks them into the receiver's RX ISR at a fixed baud-equivalent rate.
 */
#ifndef TL_SIM_CHANNEL_H
#define TL_SIM_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TL_SIM_CHANNEL_FIFO 4096u

/* xorshift32: tiny, deterministic, good enough for noise injection. */
typedef struct {
    uint32_t state;
} tl_rng;

void tl_rng_seed(tl_rng *rng, uint32_t seed);
uint32_t tl_rng_next(tl_rng *rng);
/* True with probability ppm / 1e6. */
bool tl_rng_chance_ppm(tl_rng *rng, uint32_t ppm);

typedef struct {
    uint32_t bytes_written; /* offered by the sender */
    uint32_t bytes_flipped; /* delivered with one bit inverted */
    uint32_t bytes_dropped; /* lost on the wire */
    uint32_t bytes_fifo_overflow;
} tl_channel_stats;

typedef struct {
    uint8_t fifo[TL_SIM_CHANNEL_FIFO];
    size_t head;
    size_t count;
    tl_rng rng;
    uint32_t flip_ppm; /* per-byte probability of a single-bit flip */
    uint32_t drop_ppm; /* per-byte probability the byte vanishes */
    tl_channel_stats stats;
} tl_channel;

void tl_channel_init(tl_channel *ch, uint32_t seed, uint32_t flip_ppm, uint32_t drop_ppm);
size_t tl_channel_write(tl_channel *ch, const uint8_t *data, size_t len);
bool tl_channel_pop(tl_channel *ch, uint8_t *out);
size_t tl_channel_pending(const tl_channel *ch);

#endif /* TL_SIM_CHANNEL_H */
