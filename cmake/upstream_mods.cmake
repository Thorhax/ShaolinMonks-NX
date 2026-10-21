# Upstream's language packs and mod routines, which its adapted boot.cpp and files.cpp call into. Only the
# runtime half is built: the catalog, installation and Lua routines. The launcher's mod browser and its
# downloads stay out, so installed mods are copied into the mods folder by hand on this host.
# Same pins as upstream (CMakeLists.txt and cmake/Mods.cmake).
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(miniz URL https://codeload.github.com/richgel999/miniz/zip/174573d60290f447c13a2b1b3405de2b96e27d6c
    URL_HASH SHA256=dc188cb49e913f14b4583f4a8aae830035e71627570a036ec573a21f5678d65a)
FetchContent_MakeAvailable(miniz)
target_compile_definitions(miniz PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${UPSTREAM}/tools/prepare_miniz.py"
    --source "${miniz_SOURCE_DIR}/miniz_zip.c" --output "${CMAKE_CURRENT_BINARY_DIR}/adapted/miniz_zip.c"
    COMMAND_ERROR_IS_FATAL ANY)
set_source_files_properties("${miniz_SOURCE_DIR}/miniz_zip.c" DIRECTORY "${miniz_SOURCE_DIR}" PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(miniz PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/adapted/miniz_zip.c")

FetchContent_Declare(shaolin_lua URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)
FetchContent_MakeAvailable(shaolin_lua)
file(GLOB LUA_SOURCES "${shaolin_lua_SOURCE_DIR}/src/*.c")
list(FILTER LUA_SOURCES EXCLUDE REGEX "/(lua|luac)\\.c$")
add_library(shaolin_lua STATIC ${LUA_SOURCES})
target_include_directories(shaolin_lua PUBLIC "${shaolin_lua_SOURCE_DIR}/src")

FetchContent_Declare(xz_embedded URL https://codeload.github.com/tukaani-project/xz-embedded/zip/ae63ae3a36ed01724674e8f3d750dc47bf125410
    URL_HASH SHA256=def1cc76f59db117245670f334599d70406c5f8a3ea99fb79763a84803420abf)
FetchContent_MakeAvailable(xz_embedded)
set(XZ "${xz_embedded_SOURCE_DIR}/linux/lib/xz")
add_library(shaolin_xz STATIC "${XZ}/xz_crc32.c" "${XZ}/xz_crc64.c" "${XZ}/xz_dec_lzma2.c" "${XZ}/xz_dec_stream.c")
target_include_directories(shaolin_xz PUBLIC "${xz_embedded_SOURCE_DIR}/linux/include/linux"
    PRIVATE "${xz_embedded_SOURCE_DIR}/userspace")
target_compile_definitions(shaolin_xz PUBLIC XZ_USE_CRC64 PRIVATE XZ_DEC_DYNALLOC)

# the functions mod routines may hook: every translated one that neither upstream nor the port replaces
set(MOD_TARGETS "${CMAKE_CURRENT_BINARY_DIR}/mods/mod_routine_targets.h")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mods")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mod_targets.py"
    --upstream "${UPSTREAM}" --mapping "${GEN}/x86_func_mapping.cpp" --output "${MOD_TARGETS}"
    "${UPSTREAM}/src" "${PORTED}" "${CMAKE_CURRENT_SOURCE_DIR}/port" COMMAND_ERROR_IS_FATAL ANY)

set(M "${UPSTREAM}/src/mods")
set(UPSTREAM_MOD_SOURCES
    "${UPSTREAM}/src/language_pack.cpp" "${UPSTREAM}/src/language_archive.cpp" "${UPSTREAM}/src/language_credits.cpp"
    "${UPSTREAM}/src/virtual_file.cpp" "${UPSTREAM}/src/file_ops.cpp" "${UPSTREAM}/src/path_preferences.cpp"
    "${M}/runtime.cpp" "${M}/routines.cpp" "${M}/manager.cpp" "${M}/catalog.cpp" "${M}/json.cpp" "${M}/sha256.cpp"
    "${M}/state.cpp" "${M}/tar_xz.cpp" "${M}/vcdiff.cpp" "${M}/activation.cpp" "${M}/game_reader.cpp" "${M}/game_targets.cpp")
set_source_files_properties("${M}/game_targets.cpp" PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}/mods")
set(UPSTREAM_MOD_LIBS miniz shaolin_lua shaolin_xz)
