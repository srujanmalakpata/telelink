# telelink design notes

This document describes the design constraints, protocol decisions, alternatives and future work.

## Goals and constraints

- Runs on a small microcontroller: portable C11, no `malloc`, bounded stack, no OS.
- Survives a noisy serial line: corrupted, dropped and inserted bytes must never crash the
  receiver or reach the application as valid data.
- Testable on a laptop: no clock, register or blocking call inside the core.
- Small, readable implementation.

## ISR to main-loop handoff: lock-free SPSC ring buffer

**Decision.** The UART RX interrupt only pushes the byte into a single-producer/single-consumer
ring buffer. All decoding happens in the main loop (`tl_link_poll`).

**Why.** Keeping the ISR to a few instructions bounds interrupt latency and keeps the decoder,
CRC and handlers out of interrupt context, so they need no locking.

**Memory ordering.** The producer writes the slot and then publishes it with a store-release of
`head`. The consumer reads `head` with a load-acquire before reading the slot. Freeing works the
same way in the other direction with `tail`. Each side reads its own counter with relaxed order
because nobody else writes it. On Cortex-M4 and on Cortex-M0, GCC compiles the acquire/release
operations to a plain `ldr`/`str` plus `dmb ish`, as shown in the disassembly of
`tl_rb_push` for both cores ([VERIFICATION.md](VERIFICATION.md)). On a single-core MCU the
barrier is mainly a compiler barrier, but it keeps the code correct on multi-core hosts, which
is what lets ThreadSanitizer check it.

**Free-running counters.** `head` and `tail` are never wrapped to the capacity; the index is
`counter & mask`. Unsigned subtraction gives the fill level even after 2^32 bytes, all
`capacity` slots are usable, and "full" and "empty" are never confused. The cost is that the
capacity must be a power of two.

**Load/store only.** The counters are never updated with read-modify-write atomics, because
Cortex-M0 (ARMv6-M) has no LDREX/STREX. The same reasoning applies to `rx_ring_dropped`: it has a
single writer (the ISR), so a relaxed load followed by a relaxed store is enough.

**The lock-free guard and Cortex-M0.** `ringbuf.h` refuses to compile unless 32-bit atomics are
lock-free. The C11 macro `ATOMIC_INT_LOCK_FREE` describes *all* atomic operations, including
read-modify-write, so on ARMv6-M GCC reports 1 ("sometimes lock-free"): an RMW would call
libatomic. Requiring 2 would reject Cortex-M0 even though the load/store operations used here
are lock-free. The guard accepts 1 only when `__ARM_ARCH_6M__` is defined. CI builds
`libtl.a` with `-mcpu=cortex-m0` and fails if it has any undefined `__atomic_*`/`__sync_*` symbol.

**Alternatives.** (a) Disable interrupts around a shared buffer: simple, but it adds latency and
the critical section is easy to get wrong. (b) DMA with idle-line detection: the best option on
real hardware at high baud rates, but it ties the design to a vendor peripheral. It could feed the
same decoder later. (c) Decode inside the ISR: lowest latency, but CRC work and handlers then run
at interrupt priority.

## Framing: COBS with a 0x00 delimiter

**Decision.** Frames are COBS-encoded and end with one `0x00`.

**Why.** The overhead is predictable (at most 1 byte per 254, plus 1). The delimiter cannot
appear inside a frame, so a receiver that starts mid-stream or loses bytes resynchronises at the
next `0x00` without a timeout. The streaming decoder needs only a few bytes of state.

**Alternatives.** SLIP/HDLC byte stuffing can double the size of the worst-case payload. A
length-prefixed format has no resync point: one corrupted length byte can swallow the next several
frames unless you add timeouts and searching. Fixed-size frames waste bandwidth.

**One outcome per delimiter.** The streaming decoder reports exactly one result for each
non-empty frame: `FRAME_READY`, `ERR_FRAMING` (a block was cut short) or `ERR_OVERFLOW` (frame
larger than the buffer; the rest is discarded until the next delimiter). Idle delimiters are
ignored. That makes the error counters easy to reason about, and both fuzzers check the rule
exactly: `fuzz_cobs` compares every segment's outcome with a small reference model (overflow,
framing or ready, never "nothing" for a non-empty segment), and `fuzz_link` requires the link's
outcome counters to equal the number of non-empty delimited segments. A mutant decoder that
silently swallows malformed frames fails both fuzzers on the seed corpus.

**Canonical encoder.** It does not emit a redundant trailing `0x01` after a full 254-byte block.
The output therefore matches the published examples byte for byte, and the tests use them.

## Integrity: CRC-16/CCITT-FALSE, table-driven, plus a length cross-check

**Decision.** A 16-bit CRC, big-endian, over header and payload, computed with a 256-entry
`const` table (512 bytes of flash). A bitwise version is kept as a reference.

**Why.** On the bytes it covers, it detects all single-bit errors, all odd numbers of bit errors
and all bursts up to 16 bits, and it is a widely used variant with a published check value. The
table costs 512 bytes of flash and saves about 8 shifts per byte.

**What that guarantee does and does not cover on a UART.** The CRC covers the *decoded* packet.
Its single-bit guarantee does not apply to every single-bit error in a COBS-encoded frame.
On the wire there are three kinds of single errors:

1. A flipped COBS data byte that stays non-zero changes exactly one raw bit: always detected.
2. A flip that turns a byte into `0x00` is an early delimiter, and a dropped byte removes one
   byte. Either way the decoded frame is shorter, and its last two bytes are read as the CRC,
   which then matches by chance about once in 65,536 times.
3. A flipped COBS code byte that stays non-zero moves the implied zeros around: several raw bytes
   change at once, and the frame length stays the same.

`test_wire_bit_flips_and_drops` flips every bit and drops every byte of 2,000 random encoded
frames and counts what gets through. With the CRC alone (the length check disabled),
10 flips to `0x00` and 11 dropped bytes produced a wrong packet that parsed OK. A CRC-only
format with no length byte also failed the seed-42 simulator sweep (the CI seed) at 1% flips +
1% drops: a command arrived with 15 of its 16 payload bytes and a CRC that happened to match,
so `tl_sim` exited 1. Over 1,000 seeds at that noise level, the build with the length check
disabled delivered a corrupted message in 64 runs (52 commands, 12 telemetry frames). A seed
with no undetected corruption is evidence for that seed, not a property of the design.

**Length cross-check.** Each packet carries its payload length (see [Packet format](#packet-format-a-length-byte-but-not-for-framing)), and the parser
rejects a frame whose length byte disagrees with what arrived (`TL_PARSE_BAD_LENGTH`,
`rx_bad_length`). Kinds 1 and 2 are caught deterministically unless the error hit the length
byte itself (then only the CRC is left). Kind 3 keeps the length, so it is still caught only with
probability about 1 - 2^-16. With the length check enabled: 0 wrong packets in the same wire test,
and 0 corrupted deliveries over the same 1,000 simulator seeds. That is evidence, not a proof:
kind 3 and errors that hit the length byte still rely on the CRC alone.

**Alternatives.** A nibble table (32 bytes, two lookups per byte) for flash-starved parts; the
hardware CRC unit on STM32; CRC-32 for longer frames, which would also shrink kind 3 by a factor
of 65,536 (2 more bytes per frame); Fletcher-16 (cheaper, but weaker). A per-type expected
length would catch kind 2 without a wire byte, but it moves protocol knowledge into the
application and does not work for variable-length messages.

## Packet format: a length byte, but not for framing

`type | flags | seq | len | [session] | payload | CRC16`. Frame boundaries come only from the
COBS delimiter; `len` is never used to find the end of a frame. It is a cross-check, compared
after the CRC with the length that actually arrived. A corrupted length byte can disagree with
the frame. In *length-prefixed framing*, that can desynchronise the stream; as a cross-check,
the disagreement signals corruption. It turns the truncation case
described under [Integrity](#integrity-crc-16ccitt-false-table-driven-plus-a-length-cross-check) from "caught with probability 1 - 2^-16" into "caught". The cost is one byte per
frame (about 3% of a 31-byte telemetry frame). The extra byte changes the alignment of frames
against the simulator's seeded noise stream, so a CRC-only format has different delivery counts.

The 4-byte `session` field is present only when the SYNC flag is set (see [Reliability](#reliability-stop-and-wait-arq-with-duplicate-suppression)). Header checks
run after the CRC: type 0 is reserved, reserved flag bits must be zero, an ACK cannot be
RELIABLE or carry a payload (an ACK with SYNC carries exactly the echoed session), and SYNC on a
non-ACK frame requires RELIABLE and room for the session. These checks catch bugs in the sender,
not line noise. The serializer applies the same rules (one shared `header_valid` function), so
the library cannot emit a frame its own parser rejects.

Because the session is optional, a frame without it may be up to 4 bytes longer than a valid one
and still fit the `TL_MAX_RAW` decode buffer; the parser reports that as `TL_PARSE_TOO_LONG`
(counted in `rx_overflow`). A unit test feeds such a frame through the link.

## Reliability: stop-and-wait ARQ with duplicate suppression

**Decision.** Telemetry is fire-and-forget. Commands are reliable: at most one in flight, a
retransmit every `retransmit_timeout_ms`, and after `max_retries` the sender gives up and reports
failure through `on_tx_done`. The receiver ACKs every reliable frame, including duplicates,
because the earlier ACK may have been lost. It passes a frame to the application only if its
sequence number differs from the last one it accepted.

**Why.** Commands ("set rate", "arm", "reset") are rare and must not run twice. Stop-and-wait is
the simplest scheme that gives exactly-once delivery as long as the sender has not given up and
the receiver has not rebooted (see "Receiver reboots" below), and its state fits in a few bytes
plus one encoded frame (kept so a retransmit is a plain write with no re-encoding).

**Time.** The core never reads a clock; it calls `hal.get_tick_ms()`. The comparison
`(uint32_t)(now - sent_at) >= timeout` is correct across the 2^32 wrap (after 49.7 days), and a
test covers it.

**What an ACK means.** "This frame arrived intact and the link accepted it (or it is a repeat of
the one it accepted last)". It does not mean a handler ran: a reliable frame of a type with no
registered handler is ACKed and only counted in the receiver's `rx_unknown_type`. Withholding
the ACK would make the sender retry until it gives up and then report a failure that looks exactly like a dead wire. An application that needs "handled"
should send an application-level reply. An ACK completes the pending frame only if both its
`type` and its `seq` match; anything else counts as `rx_stale_acks`.

**Sender restarts: sessions.** The sender's reliable sequence counter starts at 0 after every
`tl_link_init`, and the receiver suppresses a frame whose seq equals the last one it accepted.
Without a new session id, a restarted sender (for example, a host tool that sends one command
per run) can reuse the last accepted seq. The receiver ACKs it, counts it as a duplicate and
never calls the handler, while the sender reports "delivered". With one command per run and
a constant session id, every run after the first is silently swallowed.

Each link has a `session_id` in its config that must change on every boot (a boot counter in
flash or backup RAM, or a hardware RNG value). Until one of its reliable frames has
been ACKed, the sender sets the SYNC flag on every reliable frame and adds the 4-byte session id.
A receiver that sees SYNC with a session id it does not know forgets `rx_last_seq` before the
duplicate check. Retransmissions of the SYNC frame carry the same session id, so a lost ACK is
still handled as a duplicate. After the first ACK the sender drops the flag, so the steady-state
cost is zero bytes. Unit tests cover the restart with a new id (dispatched), the restart with the
same id (still misread as a duplicate, which is why the id must change), both receiver-restart
cases below, and SYNC being sent only until the first ACK.

**Stale ACKs across a sender restart.** Matching ACKs only by (type, seq) would allow an ACK
for the previous boot's seq 0 to complete the new boot's seq 0 command. A USB-UART adapter can
buffer the stale ACK while the new command is lost: the sender would report "delivered" and
set `tx_synced`, although the receiver never saw the command. The ACK of a SYNC frame echoes
the session id (`ACK|SYNC` + 4 bytes), and a pending SYNC frame is completed only by an ACK carrying this boot's id; a plain ACK, or one with
another id, counts as `rx_stale_acks`. After the first ACK the sender expects plain ACKs again.
Cost: 4 bytes on the first ACK of each boot. `test_stale_ack_from_previous_boot_is_ignored`
replays the scenario. The protection covers the SYNC phase, which is when stale ACKs from a
previous boot arrive; it does not help if a stale ACK is delayed past the first exchange.
Deriving the initial sequence counter from the session id would cost fewer wire bytes, but
with a boot counter as session id, consecutive boots would still collide.

**Receiver reboots: at-least-once.** A receiver restart is harmless only if the reboot happens
after the ACK reached the sender. The receiver's duplicate memory (`rx_session`, `rx_last_seq`) is in RAM. If it reboots after it
dispatched a command but before its ACK got out, the sender retransmits, the fresh receiver has
no memory of the seq, and the command runs twice. So across receiver reboots, delivery is
at-least-once. `test_receiver_restart_in_ack_window_duplicates` pins that (handler called twice),
and `test_receiver_restart_after_ack_is_harmless` covers a reboot after the ACK. Options if it
matters: idempotent commands (the usual answer: "set rate to 10", not "increase rate"), or keeping
`(rx_session, rx_last_seq)` in backup or `.noinit` RAM that survives a reset, written before the
handler runs.

Alternatives: a separate SYNC handshake message before the first command (an extra round trip
and a second pending frame); a session byte in every frame (cost on every frame); a random
initial sequence number (cheap, but still a 1-in-256 chance of the same bug per restart).

**Remaining gaps.** Delivery is at-least-once across receiver reboots (above). Sequence numbers
are 8 bits and the receiver remembers only the last one.
If 255 reliable frames in a row are abandoned without ever reaching the receiver, the next one
has the same seq as the last accepted frame and is dropped as a duplicate (but ACKed). And if a
node's `session_id` does not change across boots, the restart bug comes back.

**Alternatives.** Go-Back-N or selective repeat for throughput (needs a window of buffered frames
in RAM); NACKs; leaving reliability to the application. A sender that "gives up" can still have
delivered the frame (only the ACK was lost). The simulator shows this, and an application that
cares needs an idempotent command or a query to check the state.

## Dispatcher: linear table

A fixed array of `TL_MAX_HANDLERS` (default 8) `{type, fn, ctx}` entries, searched linearly.
With eight entries the search costs at most eight compares, and the table is about 96 bytes of
RAM. A 256-entry direct lookup table would be O(1) but take 1-2 KB on a 32-bit part. Handlers receive a `ctx` pointer, so no globals are needed. They run inside
`tl_link_poll` and may transmit.

## HAL as a struct of function pointers

`uart_write` and `get_tick_ms` plus a `ctx` pointer. With this one seam, the same object code
runs against the fake HAL in unit tests, the simulated wire, the fuzzer's sink and real
registers. The alternative, link-time substitution (weak symbols or separate board files),
avoids an indirect call but allows only one link per binary; the simulator needs two.

## Testing strategy

- **Unit tests** per module with a small harness (`tests/tl_test.h`): known-answer vectors
  (COBS paper examples, CRC catalogue, Python `binascii`), boundary cases (counter wrap, tick
  wrap, full buffers), and every error counter.
- **Property tests in plain C**: random round trips, streaming decoder equal to the one-shot
  decoder, every single-bit flip of a decoded packet detected by the CRC, and a wire-level test
  that flips every bit and drops every byte of 2,000 encoded frames and counts by error kind what
  still parses (see [Integrity](#integrity-crc-16ccitt-false-table-driven-plus-a-length-cross-check)).
- **Concurrency**: a two-thread SPSC stress test run under ThreadSanitizer. Mutation checks
  of `ringbuf.c` test the detector ([VERIFICATION.md](VERIFICATION.md) has the detection rates):
  with all atomics relaxed TSan reports a data race in every run, but with only the `head` store relaxed it reports
  one in only some runs. TSan is a dynamic detector: it can only flag a race on an access pair it
  actually sees, close enough in time that the earlier access is still in its shadow-memory
  history, so whether a weakened ordering is caught depends on thread scheduling. A clean TSan run
  is therefore evidence, not proof; the written ordering argument still matters.
  A second threaded test runs the link itself: one thread calls `tl_link_rx_isr` with 20,000
  encoded frames while the main thread loops `tl_link_poll` and reads the drop counter; every
  frame must be dispatched in order. A mutant in which the ISR reads a main-loop counter is
  reported by TSan in some runs (rate in VERIFICATION.md), for the same scheduling reason.
- **End-to-end**: two full stacks over the noisy simulated wire; asserts exactly-once delivery,
  no undetected corruption, determinism per seed, graceful failure on a hopeless wire, and that a
  run which hits its simulated-time limit says so (`timed_out`) instead of printing partial
  counts as a result.
- **Fuzzing**: `fuzz_cobs` (differential stream-vs-one-shot, outcome vs a reference model, round
  trip) and `fuzz_link` (whole RX path with timers; outcome counters must equal the number of
  delimited frames) under ASan+UBSan.
- **Cross-compile** for Cortex-M4 (library + example image) and Cortex-M0 (library, plus the
  no-libatomic check) with `-Wconversion -Wsign-conversion`, which catch integer width bugs that
  x86 builds hide.
- **Simulator exit status**: `tl_sim` exits 1 if any duplicate or corrupted message reached a
  handler and 3 if a run did not finish, so the CI sweep step fails on what it reports. "No
  corrupted delivery" is probabilistic (see [Integrity](#integrity-crc-16ccitt-false-table-driven-plus-a-length-cross-check)): a passing seed is evidence, and the
  1,000-seed comparison in VERIFICATION.md is the better measure.

The test harness uses a minimal C11 header instead of GoogleTest: the code under test is C,
and a 60-line header with no dependencies builds the same way in CI, under every sanitizer, and could even be
built for a target board. The cost is no fixtures, no test filtering and plainer failure output.

## Bugs found by testing and fixed

| What broke | Fix | Regression check |
|---|---|---|
| Requiring `ATOMIC_INT_LOCK_FREE == 2` rejected Cortex-M0 despite using only lock-free loads/stores. | Accept 1 on ARMv6-M only. | Cortex-M0 build and CI rejection of undefined `__atomic_*` / `__sync_*` symbols. |
| A restarted sender's seq 0 command was ACKed but dropped as a duplicate. | Announce a new per-boot session with SYNC before duplicate checking. | `test_sender_restart_with_new_session_is_dispatched` and `test_sender_restart_with_same_session_looks_like_duplicate`. |
| An ACK from a sender's previous boot completed a lost command in its new boot. | Echo the session in SYNC ACKs and match it against the pending frame. | `test_stale_ack_from_previous_boot_is_ignored`, `test_ack_must_match_type_seq_and_session` and SYNC header tests. |
| A shortened frame could pass CRC and deliver a truncated payload. | Cross-check the payload length after CRC validation. | `test_length_byte_must_match_frame`, `test_wire_bit_flips_and_drops` and the 1,000-seed simulator comparison. |
| A simulator run that hit its time limit returned success with partial counts. | Set `timed_out`, print `INCOMPLETE`, return exit 3 and use requested counts for percentages. | `test_time_limit_is_reported` and completion checks in the simulator tests. |

## Future work

- Run it on a real board (STM32 Nucleo-F401RE or similar) with a USB-UART adapter, and a Python
  host tool that uses the same framing.
- A 16-bit sequence number, and a serialize-from-parts API so a send does not copy the payload
  into a temporary `tl_packet` and a raw buffer before COBS (it is copied three times now;
  cheap at 64 bytes, not at 255).
- DMA + idle-line RX feeding the same decoder; measure CPU load at 921600 baud.
- Run the example under QEMU (`-M netduinoplus2` emulates an STM32F405 USART) in CI.
- Burst-error and baud-mismatch models in the simulator; compare CRC-16 against CRC-32 for
  undetected errors with billions of frames (CRC-32 would also cover the code-byte flips that
  the length byte cannot).
- Optional persistence of the receiver's `(rx_session, rx_last_seq)` in `.noinit` RAM through a
  HAL hook, for exactly-once across receiver resets.
- A sliding window (Go-Back-N) for bulk transfers such as firmware updates.
