#include "tl/packet.h"

#include <stdbool.h>
#include <string.h>

#include "tl/crc16.h"

/* Header rules shared by the serializer and the parser. `extra_len` is the
 * number of bytes after the 4-byte header (session + payload). */
static bool header_valid(uint8_t type, uint8_t flags, size_t extra_len)
{
    const bool ack = (flags & TL_FLAG_ACK) != 0u;
    const bool reliable = (flags & TL_FLAG_RELIABLE) != 0u;
    const bool sync = (flags & TL_FLAG_SYNC) != 0u;
    if (type == 0u || (flags & (uint8_t)~TL_FLAG_MASK) != 0u) {
        return false;
    }
    if (ack) {
        /* An ACK is (type, seq), plus the echoed session id when it answers a
         * SYNC frame. It never carries a payload and is never itself reliable. */
        return !reliable && extra_len == (sync ? TL_SESSION_SIZE : 0u);
    }
    if (sync && (!reliable || extra_len < TL_SESSION_SIZE)) {
        return false;
    }
    return true;
}

size_t tl_packet_serialize(const tl_packet *pkt, uint8_t *out, size_t cap)
{
    if (pkt == NULL || out == NULL || pkt->len > TL_MAX_PAYLOAD) {
        return 0u;
    }
    const size_t session_len = (pkt->flags & TL_FLAG_SYNC) != 0u ? TL_SESSION_SIZE : 0u;
    if (!header_valid(pkt->type, pkt->flags, session_len + pkt->len)) {
        return 0u;
    }
    const size_t total = TL_HEADER_SIZE + session_len + (size_t)pkt->len + TL_CRC_SIZE;
    if (cap < total) {
        return 0u;
    }
    out[0] = pkt->type;
    out[1] = pkt->flags;
    out[2] = pkt->seq;
    out[3] = pkt->len;
    size_t pos = TL_HEADER_SIZE;
    if (session_len != 0u) {
        out[pos++] = (uint8_t)(pkt->session >> 24);
        out[pos++] = (uint8_t)(pkt->session >> 16);
        out[pos++] = (uint8_t)(pkt->session >> 8);
        out[pos++] = (uint8_t)pkt->session;
    }
    memcpy(&out[pos], pkt->payload, pkt->len);
    const size_t body = pos + (size_t)pkt->len;
    const uint16_t crc = tl_crc16(out, body);
    out[body] = (uint8_t)(crc >> 8);
    out[body + 1u] = (uint8_t)(crc & 0xFFu);
    return total;
}

tl_parse_result tl_packet_parse(const uint8_t *raw, size_t len, tl_packet *out)
{
    if (len < TL_MIN_RAW) {
        return TL_PARSE_TOO_SHORT;
    }
    if (len > TL_MAX_RAW) {
        /* Defensive: the link's stream decoder buffer is TL_MAX_RAW bytes, so
         * through tl_link this case is reported as a COBS overflow instead. */
        return TL_PARSE_TOO_LONG;
    }
    const size_t body = len - TL_CRC_SIZE;
    const uint16_t expected = (uint16_t)(((uint16_t)raw[body] << 8) | raw[body + 1u]);
    if (tl_crc16(raw, body) != expected) {
        return TL_PARSE_BAD_CRC;
    }
    const uint8_t type = raw[0];
    const uint8_t flags = raw[1];
    if (!header_valid(type, flags, body - TL_HEADER_SIZE)) {
        return TL_PARSE_BAD_HEADER;
    }
    size_t pos = TL_HEADER_SIZE;
    uint32_t session = 0u;
    if ((flags & TL_FLAG_SYNC) != 0u) {
        session = ((uint32_t)raw[4] << 24) | ((uint32_t)raw[5] << 16) | ((uint32_t)raw[6] << 8) |
                  (uint32_t)raw[7];
        pos += TL_SESSION_SIZE;
    }
    const size_t payload_len = body - pos;
    if (payload_len > TL_MAX_PAYLOAD) {
        /* Only a frame without the 4-byte session can get here: it fits the
         * TL_MAX_RAW decode buffer but its payload is too big. */
        return TL_PARSE_TOO_LONG;
    }
    if (raw[3] != payload_len) {
        /* The frame is shorter or longer than the sender wrote it, and the
         * last two bytes happened to pass as a CRC (see tl/packet.h). */
        return TL_PARSE_BAD_LENGTH;
    }
    out->type = type;
    out->flags = flags;
    out->seq = raw[2];
    out->len = (uint8_t)payload_len;
    out->session = session;
    memcpy(out->payload, &raw[pos], payload_len);
    return TL_PARSE_OK;
}

size_t tl_frame_encode(const tl_packet *pkt, uint8_t *wire, size_t cap)
{
    uint8_t raw[TL_MAX_RAW];
    const size_t raw_len = tl_packet_serialize(pkt, raw, sizeof raw);
    if (raw_len == 0u || wire == NULL || cap < 1u) {
        return 0u;
    }
    const size_t enc = tl_cobs_encode(raw, raw_len, wire, cap - 1u);
    if (enc == 0u) {
        return 0u;
    }
    wire[enc] = 0u;
    return enc + 1u;
}
