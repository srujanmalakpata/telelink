# Toolchain file: ARM Cortex-M4 (Thumb-2, hard-float FPU), bare metal.
#   cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
# Other cores: set TL_CPU_FLAGS first and include this file
# (see arm-none-eabi-cortex-m0.cmake).
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR arm-none-eabi-ar)
set(CMAKE_RANLIB arm-none-eabi-ranlib)
set(CMAKE_SIZE arm-none-eabi-size)

# Compiler checks would try to link a hosted program; build a static lib instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED TL_CPU_FLAGS)
  set(TL_CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")
endif()
set(CMAKE_C_FLAGS_INIT "${TL_CPU_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os")
set(CMAKE_C_FLAGS_MINSIZEREL_INIT "-Os")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${TL_CPU_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
