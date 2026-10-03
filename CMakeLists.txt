cmake_minimum_required(VERSION 3.20)
project(telelink VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CROSSCOMPILING)
  set(_tl_host_default OFF)
else()
  set(_tl_host_default ON)
endif()

option(TL_BUILD_TESTS "Build host unit/integration tests" ${_tl_host_default})
option(TL_BUILD_SIM "Build the noisy-UART simulator" ${_tl_host_default})
option(TL_BUILD_FUZZ "Build libFuzzer targets (requires clang)" OFF)
option(TL_BUILD_FIRMWARE_EXAMPLE "Build the Cortex-M4 example image (cross only)" ${CMAKE_CROSSCOMPILING})
set(TL_SANITIZE "" CACHE STRING "Sanitizers for host builds: '', 'address,undefined', or 'thread'")

# Strict warnings everywhere; the library gets the extra conversion checks
# that matter on 8/16/32-bit targets.
set(TL_WARNINGS -Wall -Wextra -Werror -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes)
set(TL_LIB_WARNINGS ${TL_WARNINGS} -Wconversion -Wsign-conversion -Wcast-align)

if(TL_SANITIZE)
  add_compile_options(-fsanitize=${TL_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
  add_link_options(-fsanitize=${TL_SANITIZE})
endif()

# ---- The portable library: no malloc, no clock, no I/O.
add_library(tl STATIC
  src/cobs.c
  src/crc16.c
  src/dispatch.c
  src/link.c
  src/packet.c
  src/ringbuf.c
)
target_include_directories(tl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(tl PRIVATE ${TL_LIB_WARNINGS})
if(CMAKE_CROSSCOMPILING)
  # Per-function stack usage (.su files next to the objects) for RAM budgeting.
  target_compile_options(tl PRIVATE -fstack-usage)
endif()

# ---- Host simulator (library + CLI)
if(TL_BUILD_SIM OR TL_BUILD_TESTS)
  add_library(tl_sim_core STATIC sim/channel.c sim/sim.c)
  target_include_directories(tl_sim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sim)
  target_link_libraries(tl_sim_core PUBLIC tl)
  target_compile_options(tl_sim_core PRIVATE ${TL_WARNINGS})
endif()

if(TL_BUILD_SIM)
  add_executable(tl_sim sim/main.c)
  target_link_libraries(tl_sim PRIVATE tl_sim_core)
  target_compile_options(tl_sim PRIVATE ${TL_WARNINGS})
endif()

# ---- Tests (minimal C11 harness in tests/tl_test.h, run by CTest)
if(TL_BUILD_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)
  foreach(t crc16 cobs ringbuf packet dispatch link sim ringbuf_threads link_threads)
    add_executable(test_${t} tests/test_${t}.c)
    target_link_libraries(test_${t} PRIVATE tl_sim_core Threads::Threads)
    target_compile_options(test_${t} PRIVATE ${TL_WARNINGS} -Wno-missing-prototypes)
    add_test(NAME ${t} COMMAND test_${t})
  endforeach()
  set_tests_properties(ringbuf_threads link_threads PROPERTIES TIMEOUT 300)
endif()

# ---- libFuzzer targets
if(TL_BUILD_FUZZ)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "TL_BUILD_FUZZ requires clang (CC=clang)")
  endif()
  file(GLOB TL_LIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
  foreach(f cobs link)
    add_executable(fuzz_${f} fuzz/fuzz_${f}.c ${TL_LIB_SOURCES})
    target_include_directories(fuzz_${f} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_options(fuzz_${f} PRIVATE ${TL_WARNINGS} -Wno-missing-prototypes -g
      -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(fuzz_${f} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
endif()

# ---- Bare-metal example image for Cortex-M4 (size check; not run on hardware)
if(TL_BUILD_FIRMWARE_EXAMPLE)
  add_executable(firmware.elf examples/cortex-m4/main.c examples/cortex-m4/startup.c)
  target_link_libraries(firmware.elf PRIVATE tl)
  target_compile_options(firmware.elf PRIVATE ${TL_WARNINGS})
  target_link_options(firmware.elf PRIVATE
    -T${CMAKE_CURRENT_SOURCE_DIR}/examples/cortex-m4/cortex_m4.ld
    -nostartfiles --specs=nano.specs --specs=nosys.specs
    -Wl,--gc-sections -Wl,-Map=firmware.map -Wl,--print-memory-usage)
  set_target_properties(firmware.elf PROPERTIES LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/examples/cortex-m4/cortex_m4.ld)
endif()
