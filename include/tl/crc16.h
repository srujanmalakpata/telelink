/*
 * CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0x0000.
 * Catalogue check value: CRC("123456789") == 0x29B1.
 */
#ifndef TL_CRC16_H
#define TL_CRC16_H

#include <stddef.h>
#include <stdint.h>

#define TL_CRC16_INIT 0xFFFFu

/* Table-driven (256-entry const table in flash). Feed `TL_CRC16_INIT` as the
 * first `crc`; chain calls to process data incrementally. */
uint16_t tl_crc16_update(uint16_t crc, const uint8_t *data, size_t len);

/* One-shot convenience: tl_crc16_update(TL_CRC16_INIT, data, len). */
uint16_t tl_crc16(const uint8_t *data, size_t len);

/* Bit-at-a-time reference implementation (no table). Used by the tests to
 * cross-check the table, and an option for flash-starved parts. */
uint16_t tl_crc16_bitwise(const uint8_t *data, size_t len);

#endif /* TL_CRC16_H */
