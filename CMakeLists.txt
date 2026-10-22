cmake_minimum_required(VERSION 3.25)
project(ShaolinMonksNX VERSION 0.2.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# headless: Linux, no window/GPU/audio, the SDK's CPU reference renderer (development and regression runs)
# switch: the Nintendo Switch NRO
set(PORT_PLATFORM "headless" CACHE STRING "headless, linux-vk or switch")
set(UPSTREAM "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ShaolinMonksRecomp" CACHE PATH "Pinned ShaolinMonksRecomp checkout")
set(SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/XboxXBERecomp" CACHE PATH "Pinned XboxXBERecomp checkout")
set(GEN "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/shaolin" CACHE PATH "tools/codegen.sh output")
set(GENERATED_OPT "-O2" CACHE STRING "Optimization of the recompiled game code")
set(NVK_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../work/mesa-26.2.4/opt/devkitpro/portlibs/switch" CACHE PATH "Mesa Horizon SDK (portlibs/switch): NVK and the Vulkan headers")
option(NX_RENDER_THREAD "Switch: record draws for a render thread of their own" ON)
if(NOT EXISTS "${GEN}/x86_func_mapping.cpp")
    message(FATAL_ERROR "Run tools/codegen.sh <default.xbe> first (no generated code in ${GEN})")
endif()
foreach(rev_dir_pair "${SDK};40e9c35451305f09ca9d86f677e4b9c2c37ff26b")
    list(GET rev_dir_pair 0 dir)
    list(GET rev_dir_pair 1 rev)
    execute_process(COMMAND git -C "${dir}" rev-parse HEAD OUTPUT_VARIABLE have OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT have STREQUAL rev)
        message(FATAL_ERROR "${dir} is at ${have}, the port is reviewed against ${rev}")
    endif()
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(FetchContent)
FetchContent_Declare(tomlplusplus GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git GIT_TAG v3.4.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus)
# the quick menu, the same Dear ImGui release as the SDK's
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
set(IMGUI_CORE "${imgui_SOURCE_DIR}/imgui.cpp" "${imgui_SOURCE_DIR}/imgui_draw.cpp" "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
set(IMGUI_VULKAN "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/vk_overlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/bc_decode.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/button_icons.cpp")
include_directories(SYSTEM "${imgui_SOURCE_DIR}")
# the menu opens nothing in a shell, and Horizon has no execvp
add_compile_definitions(IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

# upstream's host adaptations of the SDK, then the port's on top of them; both write only into the build folder
set(ADAPTED "${CMAKE_CURRENT_BINARY_DIR}/adapted")
set(PORTED "${CMAKE_CURRENT_BINARY_DIR}/ported")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${UPSTREAM}/tools/prepare_recomp_runtime.py" --sdk "${SDK}" --out "${ADAPTED}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/port_runtime.py" --sdk "${SDK}" --upstream "${UPSTREAM}"
    --adapted "${ADAPTED}" --out "${PORTED}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${UPSTREAM}/tools/prepare_recomp_runtime.py" "${CMAKE_CURRENT_SOURCE_DIR}/tools/port_runtime.py")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/upstream_mods.cmake")

# the recompiled game; each function reaches mod routines through upstream's bridge
set(ROUTINE_BRIDGE "${UPSTREAM}/src/mods/routine_bridge.h")
file(GLOB GEN_SOURCES "${GEN}/x86_recomp.*.cpp")
list(APPEND GEN_SOURCES "${GEN}/x86_func_mapping.cpp" "${GEN}/xbe_sha1.cpp")
if(PORT_PLATFORM STREQUAL "switch")
    # Clang, outside the GCC toolchain the rest uses (cmake/switch.cmake explains why)
    set(GAME_OBJECTS)
    foreach(src ${GEN_SOURCES})
        get_filename_component(name "${src}" NAME)
        string(REGEX REPLACE "\\.cpp$" ".o" name "${name}")
        set(obj "${CMAKE_CURRENT_BINARY_DIR}/game/${name}")
        add_custom_command(OUTPUT "${obj}" DEPENDS "${src}" "${ROUTINE_BRIDGE}"
            COMMAND "${NX_GAME_CLANG}" -std=c++20 ${NX_GAME_CLANG_FLAGS} ${GENERATED_OPT} -g0 -fno-strict-aliasing -w
                    -I "${SDK}/runtime" -I "${GEN}" -include "${ROUTINE_BRIDGE}" -c "${src}" -o "${obj}"
            COMMENT "game code ${name}" VERBATIM)
        list(APPEND GAME_OBJECTS "${obj}")
    endforeach()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/game")
else()
    add_library(shaolin_game OBJECT ${GEN_SOURCES})
    target_include_directories(shaolin_game PRIVATE "${SDK}/runtime" "${GEN}")
    target_compile_options(shaolin_game PRIVATE ${GENERATED_OPT} -g0 -fno-strict-aliasing -w -include "${ROUTINE_BRIDGE}")
    set(GAME_OBJECTS $<TARGET_OBJECTS:shaolin_game>)
endif()

# the SDK runtime, less what upstream adapts and what each platform supplies
file(GLOB RUNTIME_SOURCES "${SDK}/runtime/*/*.cpp")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/app/|/core/input\\.cpp$|/audio/xaudio2\\.cpp$|/gpu/d3d12\\.cpp$")
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/kernel/(files|boot|threads|memory)\\.cpp$|/xdk/(d3d8|xapi|dsound)\\.cpp$|/gpu/(vertex|shaders|render_thread)\\.cpp$|/cpu/dispatch\\.cpp$|/xdk/crt_math\\.cpp$|/core/log\\.cpp$")
set(PORTED_SOURCES boot files threads memory d3d8 xapi dsound vertex shaders render_thread dispatch crt_math launcher_settings log)
list(TRANSFORM PORTED_SOURCES PREPEND "${PORTED}/")
list(TRANSFORM PORTED_SOURCES APPEND ".cpp")
set(SHAOLIN_SOURCES "${UPSTREAM}/src/game_preferences.cpp" "${UPSTREAM}/src/raster_compat.cpp" "${UPSTREAM}/src/mouse_state.cpp"
    ${IMGUI_CORE} ${UPSTREAM_MOD_SOURCES})
file(GLOB PORT_COMMON "${CMAKE_CURRENT_SOURCE_DIR}/port/common/*.cpp")

if(PORT_PLATFORM STREQUAL "headless")
    set(H "${SDK}/harness")
    add_executable(shaolin_headless ${RUNTIME_SOURCES} ${PORTED_SOURCES} ${SHAOLIN_SOURCES} ${PORT_COMMON}
        "${CMAKE_CURRENT_SOURCE_DIR}/port/linux/threads.cpp"
        "${H}/app_headless.cpp" "${H}/main_test.cpp" "${H}/null_audio.cpp" "${H}/null_backend.cpp" "${H}/pad_script.cpp"
        "${H}/reference_renderer.cpp" ${GAME_OBJECTS})
    target_include_directories(shaolin_headless PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port/common" "${UPSTREAM}/src"
        "${SDK}/runtime" "${GEN}" "${H}" "${H}/compat")
    target_compile_definitions(shaolin_headless PRIVATE XBE_RAW_VERTICES=0 XBE_RENDER_THREAD=0)
    target_compile_options(shaolin_headless PRIVATE -fno-strict-aliasing -O1 -g)
    target_link_libraries(shaolin_headless PRIVATE tomlplusplus::tomlplusplus ${UPSTREAM_MOD_LIBS} pthread)
    target_compile_options(shaolin_game PRIVATE -march=x86-64-v2)
endif()

# Linux with the Vulkan renderer drawing offscreen: the renderer's test bed (lavapipe works without a GPU)
if(PORT_PLATFORM STREQUAL "linux-vk")
    find_package(glslang CONFIG REQUIRED)
    # the Vulkan headers of the Mesa SDK the Switch build uses, the system's loader
    set(VULKAN_INCLUDE "${NVK_SDK}/include" CACHE PATH "Vulkan headers")
    find_library(VULKAN_LOADER NAMES vulkan libvulkan.so.1 REQUIRED)
    set(H "${SDK}/harness")
    add_executable(shaolin_vk ${RUNTIME_SOURCES} ${PORTED_SOURCES} ${SHAOLIN_SOURCES} ${PORT_COMMON}
        "${CMAKE_CURRENT_SOURCE_DIR}/port/linux/threads.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/port/linux/vk_offscreen.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/vk_backend.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/vk_shader.cpp" ${IMGUI_VULKAN}
        "${CMAKE_CURRENT_SOURCE_DIR}/port/linux/main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/port/linux/test_hooks.cpp"
        "${H}/app_headless.cpp" "${H}/null_audio.cpp" "${H}/pad_script.cpp" ${GAME_OBJECTS})
    target_include_directories(shaolin_vk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port" "${CMAKE_CURRENT_SOURCE_DIR}/port/common"
        "${UPSTREAM}/src" "${SDK}/runtime" "${GEN}" "${H}" "${H}/compat")
    target_compile_definitions(shaolin_vk PRIVATE XBE_RAW_VERTICES=0 ENABLE_HLSL=1)
    target_compile_options(shaolin_vk PRIVATE -fno-strict-aliasing -O1 -g)
    target_include_directories(shaolin_vk SYSTEM PRIVATE "${VULKAN_INCLUDE}")
    target_link_libraries(shaolin_vk PRIVATE tomlplusplus::tomlplusplus ${UPSTREAM_MOD_LIBS} pthread "${VULKAN_LOADER}"
        glslang::glslang glslang::SPIRV glslang::glslang-default-resource-limits)
    target_compile_options(shaolin_game PRIVATE -march=x86-64-v2)
endif()

if(PORT_PLATFORM STREQUAL "switch")
    file(GLOB PORT_NX "${CMAKE_CURRENT_SOURCE_DIR}/port/nx/*.cpp")
    find_package(glslang CONFIG REQUIRED)
    add_executable(shaolin_nx ${RUNTIME_SOURCES} ${PORTED_SOURCES} ${SHAOLIN_SOURCES} ${PORT_COMMON} ${PORT_NX} ${GAME_OBJECTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/vk_backend.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/port/vk/vk_shader.cpp" ${IMGUI_VULKAN})
    set_source_files_properties(${GAME_OBJECTS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    # newlib's limits.h gives C++ no LLONG_MAX unless <climits> came first, and Lua's luaconf.h needs it
    set_source_files_properties("${UPSTREAM}/src/mods/routines.cpp" PROPERTIES COMPILE_OPTIONS "-include;climits")
    target_include_directories(shaolin_nx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port" "${CMAKE_CURRENT_SOURCE_DIR}/port/common"
        "${UPSTREAM}/src" "${SDK}/runtime" "${GEN}" "${SDK}/harness/compat")
    target_compile_definitions(shaolin_nx PRIVATE PORT_VERSION="${PROJECT_VERSION}" XBE_RAW_VERTICES=0 XBE_RENDER_THREAD=$<BOOL:${NX_RENDER_THREAD}> ENABLE_HLSL=1)
    target_include_directories(shaolin_nx SYSTEM PRIVATE "${NVK_SDK}/include")
    # the Thorhax intro, the startup video; port/nx/compat gives it the log macros it is written with
    include("${CMAKE_CURRENT_SOURCE_DIR}/port/thorhax_intro/thorhax_intro.cmake")
    thorhax_intro_add(shaolin_nx)
    target_include_directories(shaolin_nx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/port/nx/compat")
    target_compile_options(shaolin_nx PRIVATE -fno-strict-aliasing -O2 -g -Wno-psabi)
    target_link_options(shaolin_nx PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/shaolin_nx.map")
    # NVK by full path (an older generic libvulkan.a may sit in portlibs). The SDK's archive index leaves out its
    # Rust core members, so the linker would never pull them: a copy indexed again is linked instead, in a group,
    # as its Rust objects reference each other in every order.
    set(NVK_LIB "${CMAKE_CURRENT_BINARY_DIR}/nvk/libvulkan.a")
    add_custom_command(OUTPUT "${NVK_LIB}" DEPENDS "${NVK_SDK}/lib/libvulkan.a"
        COMMAND ${CMAKE_COMMAND} -E copy "${NVK_SDK}/lib/libvulkan.a" "${NVK_LIB}"
        COMMAND "${DKA64}/bin/aarch64-none-elf-ranlib" "${NVK_LIB}"
        COMMENT "indexing NVK" VERBATIM)
    add_custom_target(nvk_indexed DEPENDS "${NVK_LIB}")
    add_dependencies(shaolin_nx nvk_indexed)
    target_link_libraries(shaolin_nx PRIVATE tomlplusplus::tomlplusplus ${UPSTREAM_MOD_LIBS} glslang::glslang glslang::SPIRV
        glslang::glslang-default-resource-limits
        -Wl,--start-group "${NVK_LIB}" expat zstd z nx m -Wl,--end-group)
    # NVK's Rust libraries each bundle the Rust runtime
    target_link_options(shaolin_nx PRIVATE -Wl,--wrap=abort -pthread -Wl,--gc-sections -Wl,--allow-multiple-definition -Wl,--no-relax)
    set_target_properties(shaolin_nx PROPERTIES OUTPUT_NAME shaolinmonks)
    # switch.specs finds libnx's linker script through the environment
    set_property(TARGET shaolin_nx PROPERTY RULE_LAUNCH_LINK "${CMAKE_COMMAND} -E env DEVKITPRO=${DEVKITPRO}")

    set(NRO_ICON "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.jpg")
    # a new icon remakes the NRO, which is made after each link
    set_property(TARGET shaolin_nx APPEND PROPERTY LINK_DEPENDS "${NRO_ICON}")
    add_custom_command(TARGET shaolin_nx POST_BUILD
        COMMAND "${NX_TOOLS}/nacptool" --create "Shaolin Monks NX" "Thorhax" "${PROJECT_VERSION}" "${CMAKE_CURRENT_BINARY_DIR}/shaolinmonks.nacp"
        COMMAND "${NX_TOOLS}/elf2nro" "$<TARGET_FILE:shaolin_nx>" "${CMAKE_CURRENT_BINARY_DIR}/shaolinmonks.nro"
                "--icon=${NRO_ICON}" "--nacp=${CMAKE_CURRENT_BINARY_DIR}/shaolinmonks.nacp"
        COMMENT "shaolinmonks.nro" VERBATIM)
endif()
