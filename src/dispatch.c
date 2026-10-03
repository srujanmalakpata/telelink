#include "tl/dispatch.h"

void tl_dispatch_init(tl_dispatcher *d)
{
    d->count = 0u;
}

static const tl_handler_entry *find(const tl_dispatcher *d, uint8_t type)
{
    for (size_t i = 0; i < d->count; i++) {
        if (d->entries[i].type == type) {
            return &d->entries[i];
        }
    }
    return NULL;
}

tl_reg_result tl_dispatch_register(tl_dispatcher *d, uint8_t type, tl_handler_fn fn, void *ctx)
{
    if (type == 0u || fn == NULL) {
        return TL_REG_INVALID;
    }
    if (find(d, type) != NULL) {
        return TL_REG_DUPLICATE;
    }
    if (d->count >= TL_MAX_HANDLERS) {
        return TL_REG_FULL;
    }
    d->entries[d->count].type = type;
    d->entries[d->count].fn = fn;
    d->entries[d->count].ctx = ctx;
    d->count++;
    return TL_REG_OK;
}

bool tl_dispatch(const tl_dispatcher *d, const tl_packet *pkt)
{
    const tl_handler_entry *e = find(d, pkt->type);
    if (e == NULL) {
        return false;
    }
    e->fn(pkt, e->ctx);
    return true;
}
