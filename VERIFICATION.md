# telelink test record

- **Date:** 2026-10-03
- **Machine:** 4-vCPU Linux container, x86_64, Ubuntu 24.04.4 LTS, shared with other jobs
  (load average about 16 during these runs, so timings and fuzz execution counts are low and
  noisy)
- **Toolchains:** gcc 13.3.0, clang 18.1.3 (+ libclang-rt-18-dev for libFuzzer/sanitizers),
  arm-none-eabi-gcc 13.2.1 (Ubuntu package `gcc-arm-none-eabi`), CMake 3.28.3, Ninja,
  clang-format 18, actionlint 1.7.7, Ruff (version not recorded), Python 3.11

The results below describe tests and measurements in the shared 4-vCPU Linux container above.
Timings are wall-clock measurements unless labelled as simulated time. Mutation and comparison
checks use isolated source copies; they leave the project sources unchanged. Quoted outputs
retain the recorded results. CI is validated locally, never deployed or run on GitHub; firmware
is built and size-checked, never flashed or executed on hardware or QEMU.

## Summary

| # | Command | Result | Key output |
|---|---|---|---|
| 1 | gcc Release build (`-Wall -Wextra -Werror`, library also `-Wconversion -Wsign-conversion`) | PASS | 0 warnings |
| 2 | `ctest` (gcc) | PASS | 9/9 test binaries, 62 test cases, 23,212 checks |
| 3 | clang Release build + `ctest` | PASS | 0 warnings, 9/9 |
| 4 | clang `TL_SANITIZE=address,undefined` build + `ctest` | PASS | 9/9, no sanitizer reports |
| 5 | clang `TL_SANITIZE=thread` build + `ctest` | PASS | 9/9, no data races (64.1 s) |
| 6 | TSan mutation checks, 6 runs per mutant: ring-buffer mutants and a link mutant for the threaded link test | PASS (detector checked; rates recorded) | ring: all atomics relaxed 6/6 runs report a race, `head` store relaxed 5/6, all `head` ops relaxed 4/6; link: ISR touching a main-loop counter 4/6 |
| 7 | `fuzz_cobs` 60 s (libFuzzer + ASan + UBSan) | PASS | 2,367,700 runs, cov 51, ft 315, no crash |
| 8 | `fuzz_link` 60 s (libFuzzer + ASan + UBSan) | PASS | 911,209 runs, cov 132, ft 740, no crash |
| 9 | ARM Cortex-M4 cross-compile (`-mcpu=cortex-m4 -mthumb -Os`) | PASS | 0 warnings; library .text 2,800 B |
| 10 | `arm-none-eabi-size` example firmware | PASS | text 3644, data 4, bss 548 |
| 11 | `-fstack-usage` (ARM Cortex-M4) | PASS | largest frame `tl_link_poll` 296 B; worst library call chain 624 B + user handler |
| 12 | ARM Cortex-M0 library cross-compile (`-mcpu=cortex-m0 -mthumb`) | PASS | 0 warnings; library .text 2,888 B |
| 13 | Cortex-M0: `arm-none-eabi-nm -u libtl.a` has no `__atomic_*`/`__sync_*` | PASS | grep found nothing (exit 1); `tl_rb_push` = `ldr`/`str` + 2 `dmb ish` |
| 14 | gcov line coverage of `src/` | PASS | 418/423 lines (98.8%) |
| 15 | `tl_sim --sweep --seed 42` (gcc and clang), single runs at 1% and 2% | PASS | exit 0, identical output; 0 duplicate deliveries, 0 undetected corruption |
| 16 | `tl_sim` on a run that cannot finish | PASS | prints `INCOMPLETE`, exit 3; partial counts |
| 17 | `clang-format --dry-run -Werror` on all C sources | PASS | no diffs |
| 18 | CI YAML: PyYAML `safe_load` + `actionlint` | PASS | jobs: format, host, sanitizers, fuzz, arm |
| 19 | `ruff check` + `ruff format --check` (fuzz/make_seeds.py); `make_seeds.py` reproduces the committed corpus | PASS | All checks passed; `diff -r` against a fresh copy empty (8 seed files) |
| 20 | Regression tests reject mutants of `link.c`, `packet.c` and the simulator | PASS | see details |
| 21 | Fuzz oracles catch a decoder that swallows truncated frames | PASS | both fuzzers abort on the seed corpus |
| 22 | Wire-level flips/drops with and without the length check | PASS | with: 0 wrong packets; without: 10 (flip to 0x00) + 11 (drop) |
| 23 | 1,000 simulator seeds at 1% flips + 1% drops, with and without the length check | PASS | with: 0 corrupted deliveries; without: 64 |
| 24 | GitHub Actions run | NOT_RUN | workflow validated locally, never deployed or run on GitHub |
| 25 | Firmware on real hardware / QEMU | NOT_RUN | no board or emulator available; image only built and size-checked |

## Details

### 1-2. gcc build and tests

```sh
cmake -S . -B build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build build/gcc -j2            # 0 warnings (grep -ci warning -> 0)
ctest --test-dir build/gcc --output-on-failure
```
```
100% tests passed, 0 tests failed out of 9
Total Test time (real) =   4.79 sec
```
Per binary (`./build/gcc/test_<name> | tail -1`):
```
crc16            4 test cases, 307 checks, 0 failed checks, 0 failed cases
cobs             11 test cases, 18775 checks, 0 failed checks, 0 failed cases
ringbuf          4 test cases, 672 checks, 0 failed checks, 0 failed cases
packet           11 test cases, 2459 checks, 0 failed checks, 0 failed cases
dispatch         3 test cases, 21 checks, 0 failed checks, 0 failed cases
link             21 test cases, 915 checks, 0 failed checks, 0 failed cases
sim              6 test cases, 47 checks, 0 failed checks, 0 failed cases
ringbuf_threads  1 test cases, 6 checks, 0 failed checks, 0 failed cases
link_threads     1 test cases, 10 checks, 0 failed checks, 0 failed cases
```
Extra lines the tests print:
```
  657984 flips + 82248 drops: data flips 565621 (0 accepted), flips to 0x00 6913 (0 wrong accepted), code-byte flips 85450 (0 wrong accepted), drops (0 wrong accepted)
  20000 frames dispatched in order, ring full 18315563 times
  transferred 2000000 bytes, producer saw full 491529751 times
```
Coverage includes:

- `test_packet.c`: `test_wire_bit_flips_and_drops` (every bit of 2,000 random encoded frames
  flipped, every byte dropped, results counted per error kind), `test_length_byte_must_match_frame`,
  SYNC-ACK header rules in `test_sync_header_rules`.
- `test_link.c`: `test_receiver_restart_in_ack_window_duplicates` (documents at-least-once across
  a receiver reboot: the handler runs twice), `test_receiver_restart_after_ack_is_harmless`
  (reboot after the ACK), `test_stale_ack_from_previous_boot_is_ignored`,
  `test_ack_must_match_type_seq_and_session`, the SYNC ACK checks in
  `test_sync_flag_sent_only_until_first_ack`, the `rx_bad_length` counter in
  `test_error_counters`, and the accessors in `test_isr_ring_overflow_is_counted`.
- `test_link_threads.c`: a producer thread calls `tl_link_rx_isr` with 20,000
  encoded frames while the main thread loops `tl_link_poll` and reads `tl_link_rx_ring_dropped`;
  every frame must be dispatched in order, no error counter may move, and the drop counter must
  equal the number of refused pushes the producer saw.
- `test_sim.c`: `test_time_limit_is_reported`, plus `!timed_out` checks in the other runs.

### 3-5. clang, ASan+UBSan, TSan

```sh
cmake -S . -B build/clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang
cmake -S . -B build/asan  -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DTL_SANITIZE=address,undefined
cmake -S . -B build/tsan  -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DTL_SANITIZE=thread
# each: cmake --build ... -j2 && ctest --test-dir ... --output-on-failure
```
```
clang: 100% tests passed, 0 tests failed out of 9   (9.57 s)
asan : 100% tests passed, 0 tests failed out of 9   (7.45 s)
tsan : 100% tests passed, 0 tests failed out of 9   (64.11 s; ringbuf_threads 56.24 s, link_threads 1.54 s)
```
Sanitizer builds use `-fno-sanitize-recover=all`, so any report fails the test.

### 6. ThreadSanitizer mutation checks

**Ring buffer.** Mutated copies of `src/ringbuf.c` are built with the thread test and checked
6 times each:
```sh
sed 's/memory_order_release/memory_order_relaxed/g; s/memory_order_acquire/memory_order_relaxed/g' src/ringbuf.c > rb_all.c
sed '24s/memory_order_release/memory_order_relaxed/' src/ringbuf.c > rb_headstore.c          # push: head store
sed '24s/.../; 31s/memory_order_acquire/memory_order_relaxed/; 43s/.../' src/ringbuf.c > rb_headall.c  # + head loads in pop, pop_many
clang -std=c11 -g -O1 -fsanitize=thread -Iinclude -Itests rb_<variant>.c tests/test_ringbuf_threads.c -lpthread
```
```
all relaxed (8 operations)  : exit 66 (data race reported) in 6 of 6 runs
head store relaxed          : exit 66 in 5 of 6 runs (run 2: exit 0, no report)
head store + 2 loads relaxed: exit 66 in 4 of 6 runs (runs 3 and 5: exit 0, no report)
```

**Link.** A mutant `link.c` whose `tl_link_rx_isr` also copies
`stats.rx_frames_ok` (a main-loop counter) into another counter, i.e. an ISR that touches
main-loop state without synchronisation, is built with `test_link_threads.c` under TSan and
checked in 6 runs:
```
race reported (exit 66) in 4 of 6 runs; in the other 2 the test still failed (exit 1) on its
own counter checks, without a TSan report
```
Why the rates vary: TSan is a dynamic detector. It reports a race only for a pair of accesses it
actually observes without a happens-before edge, and only while the earlier access is still in
its per-location shadow history, so it depends on thread scheduling, which on this loaded
machine changes from run to run. A clean TSan run is evidence, not proof; the ordering argument
in `include/tl/ringbuf.h` carries the rest.

### 7-8. Fuzzing

```sh
CC=clang cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTL_BUILD_FUZZ=ON -DTL_BUILD_TESTS=OFF -DTL_BUILD_SIM=OFF
cmake --build build/fuzz -j2
cd build/fuzz
mkdir -p corpus_cobs corpus_link
./fuzz_cobs -max_total_time=60 -print_final_stats=1 corpus_cobs ../../fuzz/seeds
./fuzz_link -max_total_time=60 -print_final_stats=1 corpus_link ../../fuzz/seeds
```
```
fuzz_cobs exit=0
#9	INITED cov: 38 ft: 81 corp: 6/68b exec/s: 0 rss: 33Mb
#2367700	DONE   cov: 51 ft: 315 corp: 101/36Kb lim: 4096 exec/s: 38814 rss: 444Mb
fuzz_link exit=0
#9	INITED cov: 116 ft: 204 corp: 8/95b exec/s: 0 rss: 32Mb
#911209	DONE   cov: 132 ft: 740 corp: 194/61Kb lim: 4096 exec/s: 14937 rss: 416Mb
```
The run produces no `crash-*`, `leak-*`, `timeout-*` or `oom-*` files. `fuzz_cobs` compares
each segment's outcome with a reference model (a non-empty segment must give exactly one of
FRAME_READY / ERR_FRAMING / ERR_OVERFLOW, and the right one), and `fuzz_link` requires the
outcome counters to equal the number of non-empty delimited frames. The 8 seeds come from
`fuzz/make_seeds.py` (Python `binascii.crc_hqx`) and include the length byte and
`sync_ack_seq0.bin` (an ACK that echoes the fuzz target's session id). Execution counts depend
on machine load.

### 9-11. ARM Cortex-M4 cross-compile

```sh
cmake -S . -B build/arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build/arm -j2          # 0 warnings
arm-none-eabi-size build/arm/libtl.a build/arm/firmware.elf
```
```
Memory region         Used Size  Region Size  %age Used
           FLASH:        3648 B         1 MB      0.35%
             RAM:         552 B       128 KB      0.42%
   text	   data	    bss	    dec	    hex	filename
    360	      0	      0	    360	    168	cobs.c.obj (ex build/arm/libtl.a)
    608	      0	      0	    608	    260	crc16.c.obj (ex build/arm/libtl.a)
    120	      0	      0	    120	     78	dispatch.c.obj (ex build/arm/libtl.a)
   1104	      0	      0	   1104	    450	link.c.obj (ex build/arm/libtl.a)
    402	      0	      0	    402	    192	packet.c.obj (ex build/arm/libtl.a)
    206	      0	      0	    206	     ce	ringbuf.c.obj (ex build/arm/libtl.a)
   3644	      4	    548	   4196	   1064	build/arm/firmware.elf
```
Library total: 2,800 bytes of `.text`. The length byte, session echo in ACKs and state
accessors account for 90 bytes combined (2,710 bytes without those features).

Stack usage (`cat build/arm/CMakeFiles/tl.dir/src/*.su | sort -t$'\t' -k2 -n -r | head -7`):
```
link.c:283:6:tl_link_poll	296	static
link.c:91:11:tl_link_send	168	static
link.c:107:11:tl_link_send_reliable	104	static
packet.c:108:8:tl_frame_encode	96	static
packet.c:61:17:tl_packet_parse	32	static
packet.c:29:8:tl_packet_serialize	32	static
link.c:7:11:tl_link_init	24	static
```
Whole call chain: the call graph from `-fcallgraph-info=su` (same CPU flags and `-Os`) gives
`tl_link_poll` 296 -> `tl_dispatch` 24 -> *user handler* ->
`tl_link_send` 168 -> `tl_frame_encode` 96 -> `tl_packet_serialize` 32 -> `tl_crc16` 0 ->
`tl_crc16_update` 8 = **624 bytes plus the handler's own frame** (and newlib's `memcpy`).

### 12-13. ARM Cortex-M0 (ARMv6-M) library build

```sh
cmake -S . -B build/m0 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-cortex-m0.cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DTL_BUILD_FIRMWARE_EXAMPLE=OFF
cmake --build build/m0 -j2           # 0 warnings
arm-none-eabi-size build/m0/libtl.a
arm-none-eabi-nm -u build/m0/libtl.a | grep -E '__(atomic|sync)_'   # exit 1 = nothing found
arm-none-eabi-objdump -d build/m0/libtl.a --disassemble=tl_rb_push
```
```
   text	   data	    bss	    dec	    hex	filename
    382	      0	      0	    382	    17e	cobs.c.obj (ex build/m0/libtl.a)
    624	      0	      0	    624	    270	crc16.c.obj (ex build/m0/libtl.a)
    122	      0	      0	    122	     7a	dispatch.c.obj (ex build/m0/libtl.a)
   1118	      0	      0	   1118	    45e	link.c.obj (ex build/m0/libtl.a)
    430	      0	      0	    430	    1ae	packet.c.obj (ex build/m0/libtl.a)
    212	      0	      0	    212	     d4	ringbuf.c.obj (ex build/m0/libtl.a)

   2:	6882      	ldr	r2, [r0, #8]      ; head (relaxed, own counter)
   4:	68c5      	ldr	r5, [r0, #12]     ; tail
   6:	f3bf 8f5b 	dmb	ish               ; acquire
  1a:	5501      	strb	r1, [r0, r4]      ; slot write
  20:	f3bf 8f5b 	dmb	ish               ; release
  24:	609a      	str	r2, [r3, #8]      ; publish head
```
Library total 2,888 bytes.

### 14. Coverage

```sh
cmake -S . -B build/cov -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_C_FLAGS="--coverage -O0" -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build/cov -j2 && ctest --test-dir build/cov
cd build/cov/CMakeFiles/tl.dir/src && gcov -n *.gcno
```
```
cobs.c      100.00% of 78
crc16.c     100.00% of 16
dispatch.c  100.00% of 26
link.c       98.37% of 184
packet.c     97.37% of 76
ringbuf.c   100.00% of 43
Lines executed:98.82% of 423
```
Total: 418 of 423 lines. Uncovered: `tl_frame_encode` failing inside `tl_link_send` and
`tl_link_send_reliable` after the arguments were already validated, the `n == 0` break in
`tl_link_poll`, the NULL/oversized guard in `tl_packet_serialize`, the invalid-packet/NULL guard
in `tl_frame_encode`, and `packet.c:16` (`return false` for type 0 / reserved flag bits). That
last one is a gcov attribution artifact at `-O0`: `test_parse_length_and_header_errors` asserts
`TL_PARSE_BAD_HEADER` for exactly those headers, and that line is the only way to produce it.

### 15-16. Simulator

```sh
./build/gcc/tl_sim --sweep --seed 42 > sweep_gcc.txt                 # exit 0
./build/clang/tl_sim --sweep --seed 42 | cmp - sweep_gcc.txt         # identical, exit 0
./build/gcc/tl_sim --seed 42 --flip-ppm 10000 --drop-ppm 10000       # exit 0
./build/gcc/tl_sim --seed 42 --flip-ppm 20000 --drop-ppm 20000       # exit 0
./build/gcc/tl_sim --drop-ppm 300000 --timeout-ms 100000 --retries 255   # exit 3
```
```
| flip ppm | drop ppm | cmds delivered | cmds failed | retransmits | telemetry delivered | bad CRC | framing | dup deliveries | undetected | sim ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 | 1000/1000 (100.00%) | 0 | 0 | 1000/1000 (100.00%) | 0 | 0 | 0 | 0 | 4999 |
| 100 | 100 | 1000/1000 (100.00%) | 0 | 11 | 996/1000 (99.60%) | 6 | 10 | 0 | 0 | 5175 |
| 1000 | 1000 | 1000/1000 (100.00%) | 0 | 80 | 934/1000 (93.40%) | 67 | 73 | 0 | 0 | 6323 |
| 5000 | 5000 | 1000/1000 (100.00%) | 0 | 393 | 706/1000 (70.60%) | 338 | 314 | 0 | 0 | 12236 |
| 10000 | 10000 | 1000/1000 (100.00%) | 0 | 901 | 519/1000 (51.90%) | 619 | 706 | 0 | 0 | 22205 |
| 20000 | 20000 | 985/1000 (98.50%) | 74 | 2517 | 270/1000 (27.00%) | 1291 | 1846 | 0 | 0 | 55595 |

seed=42 flip_ppm=10000 drop_ppm=10000 timeout_ms=20 max_retries=8 bytes_per_ms=11
simulated time         : 22205 ms
wire host->device      : 45632 bytes, 442 flipped, 429 dropped
wire device->host      : 41492 bytes, 389 flipped, 416 dropped
commands (reliable)    : sent 1000, acked 1000, failed 0, delivered 1000 (100.00%)
  retransmits          : 901, duplicates suppressed at device: 186
  duplicate deliveries : 0, undetected corruption: 0
telemetry (best effort): sent 1000, delivered 519 (51.90%), undetected corruption: 0
rx errors device       : bad_crc 356, framing 340, overflow 0, bad_header 0, bad_length 0
rx errors host         : bad_crc 263, framing 366, overflow 0, bad_header 5, bad_length 0

seed=42 flip_ppm=20000 drop_ppm=20000 timeout_ms=20 max_retries=8 bytes_per_ms=11
commands (reliable)    : sent 1000, acked 926, failed 74, delivered 985 (98.50%)
  retransmits          : 2517, duplicates suppressed at device: 354
  duplicate deliveries : 0, undetected corruption: 0
rx errors host         : bad_crc 375, framing 676, overflow 2, bad_header 6, bad_length 1

FAIL: simulated-time limit reached before the run finished
seed=1 flip_ppm=1000 drop_ppm=300000 timeout_ms=100000 max_retries=255 bytes_per_ms=11
simulated time         : 600000 ms (INCOMPLETE: hit the time limit, counts are partial)
commands (reliable)    : sent 1, acked 0, failed 0, delivered 0 (0.00%)
```
"Simulated ms" is simulated time (the virtual millisecond tick), not wall-clock time.
Percentages use the requested counts (`--commands`, `--telemetry`) as their denominator.
In the 2% run the host's length check rejects one truncated frame whose CRC matches
(`bad_length 1`). The length byte and the 4-byte session echo in the first ACK change the
alignment of frames against the seeded noise stream; CRC-only formats have different results.

### 17-19. Lint and config checks

```sh
find include src sim tests fuzz examples -name '*.[ch]' -print0 | xargs -0 clang-format --dry-run -Werror   # exit 0
uv run --no-project --with pyyaml python -c "import yaml,sys; d=yaml.safe_load(open(sys.argv[1])); print(list(d['jobs']))" .github/workflows/ci.yml
#   ['format', 'host', 'sanitizers', 'fuzz', 'arm']
actionlint .github/workflows/ci.yml                                   # exit 0 (no shellcheck installed)
ruff check fuzz/make_seeds.py && ruff format --check fuzz/make_seeds.py   # All checks passed!
d=$(mktemp -d); cp -r fuzz/seeds "$d"/ && python3 fuzz/make_seeds.py && diff -r fuzz/seeds "$d"/seeds   # no output, 8 files
```
The CI `host` job (gcc leg) runs `make_seeds.py` and fails on `git diff --exit-code fuzz/seeds`
or untracked seed files.

### 20. Regression tests reject faulty implementations

Each mutant changes one property in a copy of the implementation, linked with the unchanged
test file:
```
ACK session check removed (ack_matches_pending returns true)
  -> [FAIL] test_stale_ack_from_previous_boot_is_ignored, [FAIL] test_ack_must_match_type_seq_and_session
session echo removed from send_ack
  -> [FAIL] test_reliable_roundtrip_with_ack and 8 more link tests (sender never completes a SYNC frame)
length check removed from tl_packet_parse
  -> [FAIL] test_length_byte_must_match_frame, [FAIL] test_wire_bit_flips_and_drops
sim: timed_out forced to false
  -> [FAIL] test_time_limit_is_reported
```

### 21. Fuzz oracles

A mutant `cobs.c` whose streaming decoder returns `TL_COBS_NEED_MORE` instead of
`TL_COBS_ERR_FRAMING` (it silently swallows truncated frames), fuzzed for up to 60 s with
`-seed=1`: `fuzz_cobs` and `fuzz_link` both abort (`deadly signal`, exit 77) on the seed corpus
(`truncated.bin`) before any mutation.

### 22. Wire-level integrity with and without the length byte check

`test_wire_bit_flips_and_drops` linked against a copy of `src/` whose parser skips the length
comparison (the length byte is still on the wire, just not checked):
```
with check   : 657984 flips + 82248 drops: data flips 565621 (0 accepted), flips to 0x00 6913 (0 wrong accepted), code-byte flips 85450 (0 wrong accepted), drops (0 wrong accepted)
without check: 657984 flips + 82248 drops: data flips 565621 (0 accepted), flips to 0x00 6913 (10 wrong accepted), code-byte flips 85450 (0 wrong accepted), drops (11 wrong accepted)
```
A flip inside a COBS data byte is one raw bit, which CRC-16 always detects. A flip to `0x00` or a
dropped byte shortens the frame and leaves the CRC as a 16-bit random check; the length byte
turns those into deterministic rejections unless the length byte itself was hit. Code-byte flips
keep the length and still rely on the CRC alone (0 of 85,450 got through here, which is
consistent with a ~2^-16 rate per parsed frame, not a guarantee).

**CRC-only format, no length byte: FAIL.** The seed-42 sweep at 1% flips + 1% drops delivers
one truncated command (15 of 16 payload bytes, CRC matched), and `tl_sim` exits 1. This is a
separate format comparison, not the length-check-disabled parser above: omitting the byte
changes the alignment of frames against the seeded noise stream.

### 23. Simulator over 1,000 seeds

```sh
for seed in $(seq 1 1000); do tl_sim --seed $seed --flip-ppm 10000 --drop-ppm 10000; done   # both builds
```
```
length check disabled: 64 runs with a corrupted delivery (52 commands, 12 telemetry frames), 64 corrupted messages in total
length check enabled : 0 runs, 0 corrupted messages
```

## NOT_RUN and known limits

- **GitHub Actions: NOT_RUN.** The workflow is validated with PyYAML and actionlint only,
  never deployed or run on GitHub. The TSan job sets `vm.mmap_rnd_bits=28` because of a known
  incompatibility between Ubuntu 24.04 runners and clang 18 TSan. The ASLR adjustment is
  NOT_RUN in this record.
- **Hardware / QEMU: NOT_RUN.** Neither the Cortex-M4 example nor the Cortex-M0 library has
  executed on a core. Register addresses in the example are unverified on a real part.
