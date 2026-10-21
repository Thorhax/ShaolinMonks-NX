# devkitA64 for the host code, with libnx's thread pointer model (-mtp=soft) and newlib.
# The recompiled game code is built with Clang instead (NX_GAME_CLANG): devkitA64's GCC stops with an internal
# compiler error on it, upstream builds it with Clang too, and it holds no thread-local data, so the two mix.
#   -DDEVKITPRO=<devkitpro root with devkitA64 and libnx>
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(DEVKITPRO "$ENV{DEVKITPRO}" CACHE PATH "devkitPro root")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DEVKITPRO)
if(NOT EXISTS "${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-g++")
    message(FATAL_ERROR "Set DEVKITPRO to a devkitPro root with devkitA64 and libnx")
endif()
set(DKA64 "${DEVKITPRO}/devkitA64")
set(CMAKE_C_COMPILER "${DKA64}/bin/aarch64-none-elf-gcc")
set(CMAKE_CXX_COMPILER "${DKA64}/bin/aarch64-none-elf-g++")
set(CMAKE_ASM_COMPILER "${DKA64}/bin/aarch64-none-elf-gcc")
set(CMAKE_AR "${DKA64}/bin/aarch64-none-elf-gcc-ar" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${DKA64}/bin/aarch64-none-elf-gcc-ranlib" CACHE FILEPATH "" FORCE)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# local-exec TLS: linker relaxation of other TLS models has corrupted big static PIEs on secondary threads (sa2-nx),
# and a library built with -fPIC (glslang adds it) gets TLS descriptors, which GCC 15 miscompiles with -mtp=soft
# (the thread pointer call overwrites the variable's offset). Build every library with this file.
set(NX_ARCH "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE")
set(NX_TLS "-ftls-model=local-exec")
set(CMAKE_C_FLAGS_INIT "${NX_ARCH} ${NX_TLS} -ffunction-sections -fdata-sections -D__SWITCH__ -isystem ${DEVKITPRO}/libnx/include")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-specs=${DEVKITPRO}/libnx/switch.specs ${NX_ARCH} -L${DEVKITPRO}/libnx/lib -L${DEVKITPRO}/portlibs/switch/lib")
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

set(CMAKE_FIND_ROOT_PATH "${DEVKITPRO}/portlibs/switch" "${DEVKITPRO}/libnx")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(NX_TOOLS "${DEVKITPRO}/tools/bin")

# Clang for the game code, against devkitA64's C++ headers
find_program(NX_GAME_CLANG NAMES clang++-19 clang++ REQUIRED)
file(GLOB _gcc_versions LIST_DIRECTORIES true "${DKA64}/aarch64-none-elf/include/c++/*")
list(GET _gcc_versions 0 _cxx_inc)
set(NX_GAME_CLANG_FLAGS --target=aarch64-none-elf -mcpu=cortex-a57+crc+crypto -fPIE -ffunction-sections -fdata-sections
    -D__SWITCH__ -nostdinc++ -isystem ${_cxx_inc} -isystem ${_cxx_inc}/aarch64-none-elf
    -isystem ${DKA64}/aarch64-none-elf/include)
