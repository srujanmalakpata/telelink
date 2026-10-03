/*
 * Command dispatcher: a fixed-size table mapping message type -> handler.
 * No allocation; a linear search is simple and cheap for the handful of
 * message types a small device handles.
 */
#ifndef TL_DISPATCH_H
#define TL_DISPATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tl/packet.h"

#ifndef TL_MAX_HANDLERS
#define TL_MAX_HANDLERS 8u
#endif

typedef void (*tl_handler_fn)(const tl_packet *pkt, void *ctx);

typedef struct {
    uint8_t type;
    tl_handler_fn fn;
    void *ctx;
} tl_handler_entry;

typedef struct {
    tl_handler_entry entries[TL_MAX_HANDLERS];
    size_t count;
} tl_dispatcher;

typedef enum {
    TL_REG_OK = 0,
    TL_REG_INVALID,   /* type 0 or NULL handler */
    TL_REG_DUPLICATE, /* a handler for this type already exists */
    TL_REG_FULL       /* TL_MAX_HANDLERS reached */
} tl_reg_result;

void tl_dispatch_init(tl_dispatcher *d);

tl_reg_result tl_dispatch_register(tl_dispatcher *d, uint8_t type, tl_handler_fn fn, void *ctx);

/* Calls the handler for pkt->type. Returns false if no handler is registered. */
bool tl_dispatch(const tl_dispatcher *d, const tl_packet *pkt);

#endif /* TL_DISPATCH_H */
