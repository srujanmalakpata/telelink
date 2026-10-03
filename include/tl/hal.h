/*
 * Hardware abstraction: the only way the link touches the outside world.
 * Core code never reads a real clock or a UART register, so the whole stack
 * runs unchanged on a host (unit tests, simulator, fuzzer) and on an MCU.
 */
#ifndef TL_HAL_H
#define TL_HAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *ctx; /* passed back to every callback (e.g. a UART handle) */
    /* Queue `len` bytes for transmission. Returns how many were accepted; a
     * short write is counted as a TX error and the frame is treated as sent
     * (the reliability layer retransmits reliable frames). */
    size_t (*uart_write)(void *ctx, const uint8_t *data, size_t len);
    /* Free-running millisecond tick. Wrap-around at 2^32 is handled. */
    uint32_t (*get_tick_ms)(void *ctx);
} tl_hal;

#endif /* TL_HAL_H */
