"""Write valid wire frames to fuzz/seeds/ so the fuzzers start past the CRC check.

Uses only the Python standard library: binascii.crc_hqx(data, 0xFFFF) is
CRC-16/CCITT-FALSE, an implementation independent of the C code under test.
"""

import binascii
import pathlib


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_idx, code = 0, 1
    for i, b in enumerate(data):
        if b == 0:
            out[code_idx] = code
            code_idx, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 0xFF and i + 1 < len(data):
                out[code_idx] = code
                code_idx, code = len(out), 1
                out.append(0)
    out[code_idx] = code
    return bytes(out)


def frame(
    msg_type: int,
    flags: int,
    seq: int,
    payload: bytes = b"",
    session: int | None = None,
) -> bytes:
    """One wire frame: type | flags | seq | len | [session] | payload | CRC16.

    `session` is inserted after the length byte when the SYNC flag (0x04) is set.
    """
    body = bytes([msg_type, flags, seq, len(payload)])
    if session is not None:
        body += session.to_bytes(4, "big")
    body += payload
    crc = binascii.crc_hqx(body, 0xFFFF)
    return cobs_encode(body + crc.to_bytes(2, "big")) + b"\x00"


def main() -> None:
    seeds = pathlib.Path(__file__).parent / "seeds"
    seeds.mkdir(exist_ok=True)
    step = b"\x14"  # fuzz_link reads the first byte as a tick step
    (seeds / "telemetry.bin").write_bytes(step + frame(0x20, 0x00, 1, bytes(range(8))))
    (seeds / "reliable_cmd.bin").write_bytes(
        step + frame(0x10, 0x01, 0, b"\x00\x01\x02")
    )
    (seeds / "ack_seq0.bin").write_bytes(step + frame(0x10, 0x02, 0))
    # ACK of a SYNC frame: echoes the session id fuzz_link's sender uses, so it
    # completes the command fuzz_link has in flight.
    (seeds / "sync_ack_seq0.bin").write_bytes(
        step + frame(0x10, 0x06, 0, session=0x12345678)
    )
    (seeds / "dup_cmd.bin").write_bytes(step + frame(0x10, 0x01, 5) * 2)
    (seeds / "sync_cmd.bin").write_bytes(
        step + frame(0x10, 0x05, 0, b"\x2a", session=0xDEADBEEF)
    )
    # Raw COBS inputs (mostly for fuzz_cobs): a block cut short by the
    # delimiter, and two back-to-back frames whose data contains zeros.
    (seeds / "truncated.bin").write_bytes(bytes([0x05, 0x11, 0x22, 0x00]))
    (seeds / "two_frames.bin").write_bytes(
        cobs_encode(b"\x11\x22\x00\x33") + b"\x00\x01\x01\x00"
    )


if __name__ == "__main__":
    main()
