# Toolchain file: ARM Cortex-M0 (ARMv6-M, Thumb-1, no FPU, no LDREX/STREX).
# Library-only check: the example firmware targets the Cortex-M4, so configure
# with -DTL_BUILD_FIRMWARE_EXAMPLE=OFF.
#   cmake -B build-m0 -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-cortex-m0.cmake \
#         -DTL_BUILD_FIRMWARE_EXAMPLE=OFF
set(TL_CPU_FLAGS "-mcpu=cortex-m0 -mthumb -mfloat-abi=soft")
include(${CMAKE_CURRENT_LIST_DIR}/arm-none-eabi.cmake)
