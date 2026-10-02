cmake_minimum_required(VERSION 3.16)
project(wasmcart-native C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# MSVC: force C++20, proper __cplusplus, and static CRT (match libnode)
if(MSVC)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/std:c++20>" "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ─── Node.js / libnode ───────────────────────────────────────────────────

set(LIBNODE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/libnode" CACHE PATH "Path to libnode.a + headers")

if(NOT EXISTS "${LIBNODE_DIR}/libnode.a" AND NOT EXISTS "${LIBNODE_DIR}/libnode.lib")
    message(FATAL_ERROR "libnode not found at ${LIBNODE_DIR}\n"
        "Download from: https://github.com/wasmcart/build-libnode/releases\n"
        "  mkdir -p deps/libnode && curl -sL <url>/libnode-linux-x86_64.tar.gz | tar xz -C deps/libnode")
endif()

# ─── Source files ────────────────────────────────────────────────────────

set(LIBWASMCART_SOURCES
    src/cart_host.cpp
    src/gl_imports.cpp
    src/gl_procs.c
    src/asset_loader.c
    deps/miniz.c
    deps/cJSON.c
)

# ─── libwasmcart (static core library) ──────────────────────────────────

add_library(wasmcart STATIC ${LIBWASMCART_SOURCES})
target_include_directories(wasmcart PUBLIC include)
target_include_directories(wasmcart PRIVATE
    src
    deps
    ${LIBNODE_DIR}/include
)
if(WIN32)
    target_link_libraries(wasmcart PUBLIC ${LIBNODE_DIR}/libnode.lib winmm dbghelp ws2_32 crypt32 iphlpapi psapi userenv bcrypt)
elseif(APPLE)
    # libnode's crypto reads the macOS keychain (ReadMacOSKeychainCertificates,
    # kSecPolicyAppleSSL), so Security and CoreFoundation are libnode's
    # dependencies, not SDL's. They were only listed in the SDL2_STATIC branch
    # below, which does not run when SDL comes from Homebrew -- so every macOS
    # CI build failed at link with "symbol(s) not found for architecture".
    target_link_libraries(wasmcart PUBLIC ${LIBNODE_DIR}/libnode.a pthread dl m
        "-framework Security" "-framework CoreFoundation")
else()
    target_link_libraries(wasmcart PUBLIC ${LIBNODE_DIR}/libnode.a pthread dl m)
endif()

# GLES3 + EGL — use ANGLE on macOS/Windows, system libs on Linux
set(ANGLE_DIR "" CACHE PATH "Path to ANGLE (lib/ + include/)")
if(ANGLE_DIR AND EXISTS "${ANGLE_DIR}/include")
    message(STATUS "Using ANGLE from: ${ANGLE_DIR}")
    target_include_directories(wasmcart PUBLIC ${ANGLE_DIR}/include)
    if(APPLE)
        target_link_libraries(wasmcart PUBLIC
            ${ANGLE_DIR}/lib/libEGL.dylib
            ${ANGLE_DIR}/lib/libGLESv2.dylib)
    elseif(WIN32)
        target_link_libraries(wasmcart PUBLIC
            ${ANGLE_DIR}/lib/libEGL.lib
            ${ANGLE_DIR}/lib/libGLESv2.lib)
    endif()
else()
    find_library(GLESV2_LIB GLESv2)
    find_library(EGL_LIB_FOR_WASMCART EGL)
    if(GLESV2_LIB)
        target_link_libraries(wasmcart PUBLIC ${GLESV2_LIB})
    endif()
    if(EGL_LIB_FOR_WASMCART)
        target_link_libraries(wasmcart PUBLIC ${EGL_LIB_FOR_WASMCART})
    endif()
endif()

# ─── wasmcart-run (standalone player) ───────────────────────────────────

if(APPLE)
    # mac_layer.m: NSWindow->CALayer resolution + CAMetalLayer vsync/scale
    # for ANGLE's Metal backend (see src/mac_layer.h).
    add_executable(wasmcart-run src/main.c src/egl_context.c src/mac_layer.m)
    target_link_libraries(wasmcart-run PUBLIC "-framework Cocoa" "-framework QuartzCore")
else()
    add_executable(wasmcart-run src/main.c src/egl_context.c)
endif()
target_link_libraries(wasmcart-run PRIVATE wasmcart)

# SDL2 — prefer static linking for single-binary distribution
set(SDL2_DIR "" CACHE PATH "Path to SDL2 (Windows)")
if(WIN32 AND SDL2_DIR)
    target_include_directories(wasmcart-run PRIVATE ${SDL2_DIR}/include)
    target_link_libraries(wasmcart-run PRIVATE ${SDL2_DIR}/lib/x64/SDL2.lib ${SDL2_DIR}/lib/x64/SDL2main.lib)
    set(SDL2_HANDLED TRUE)
elseif(NOT WIN32)
    find_library(SDL2_STATIC libSDL2.a PATHS
        /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/local/lib
        /opt/homebrew/lib /usr/local/opt/sdl2/lib)
endif()
if(SDL2_STATIC AND NOT SDL2_HANDLED)
    message(STATUS "Using static SDL2: ${SDL2_STATIC}")
    target_link_libraries(wasmcart-run PRIVATE ${SDL2_STATIC})
    # Platform-specific SDL2 system deps
    if(APPLE)
        target_link_libraries(wasmcart-run PRIVATE
            "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreHaptics"
            "-framework GameController" "-framework CoreVideo" "-framework Cocoa"
            "-framework IOKit" "-framework ForceFeedback" "-framework Carbon"
            "-framework CoreFoundation" "-framework Metal" "-framework QuartzCore"
            "-framework Security" "-framework SystemConfiguration"
            iconv)
    elseif(WIN32)
        target_link_libraries(wasmcart-run PRIVATE
            winmm imm32 setupapi version ole32 oleaut32 gdi32 user32 shell32 advapi32)
    else()
        target_link_libraries(wasmcart-run PRIVATE
            asound pulse samplerate
            X11 Xext Xcursor Xi Xfixes Xrandr Xss
            wayland-client wayland-egl wayland-cursor
            xkbcommon decor-0
            drm gbm)
    endif()
else()
    # Fallback to shared SDL2
    find_package(PkgConfig)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SDL2 sdl2)
        if(SDL2_FOUND)
            target_include_directories(wasmcart-run PRIVATE ${SDL2_INCLUDE_DIRS})
            # Drop SDL2main: sdl2.pc still advertises it, but Homebrew's arm64
            # SDL2 ships no libSDL2main, so the link fails with
            # "ld: library 'SDL2main' not found". It is a WinMain shim we do not
            # need -- src/main.c defines a plain main() and SDL_MAIN_HANDLED is
            # the norm on unix. Windows links SDL2main.lib explicitly above.
            set(SDL2_LINK_LIBS "")
            foreach(_lib ${SDL2_LIBRARIES})
                if(NOT _lib STREQUAL "SDL2main")
                    list(APPEND SDL2_LINK_LIBS ${_lib})
                endif()
            endforeach()
            # pkg_check_modules gives library NAMES; without the matching -L
            # the linker cannot find them. On arm64 macOS Homebrew lives in
            # /opt/homebrew, not /usr/local, so omitting this failed with
            # "ld: library 'SDL2' not found".
            target_link_directories(wasmcart-run PRIVATE ${SDL2_LIBRARY_DIRS})
            target_link_libraries(wasmcart-run PRIVATE ${SDL2_LINK_LIBS})
        endif()
    endif()
endif()

# SDL2 headers (needed regardless of static/shared)
find_path(SDL2_INCLUDE SDL2/SDL.h)
if(SDL2_INCLUDE)
    target_include_directories(wasmcart-run PRIVATE ${SDL2_INCLUDE})
endif()

# EGL
find_library(EGL_LIB EGL)
if(EGL_LIB)
    target_link_libraries(wasmcart-run PRIVATE ${EGL_LIB})
endif()

# GL carts on native Wayland: EGL has to share SDL's wl_display, and the
# player owns a wl_egl_window on SDL's wl_surface (SDL only makes one for an
# SDL_WINDOW_OPENGL window). Without these the Wayland path compiles out and
# GL carts need X11/Xwayland (SDL_VIDEODRIVER=x11).
if(UNIX AND NOT APPLE)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(WAYLAND_EGL QUIET IMPORTED_TARGET wayland-client wayland-egl)
    endif()
    if(WAYLAND_EGL_FOUND)
        target_compile_definitions(wasmcart-run PRIVATE WC_HAVE_WAYLAND_EGL)
        target_link_libraries(wasmcart-run PRIVATE PkgConfig::WAYLAND_EGL)
    endif()
endif()

# ─── WebGPU (SPEC.md, "WebGPU") ─────────────────────────────────────────
#
# WebGPU carts run on the reference host's JavaScript (wasmcart's src/wgpu)
# and Dawn's Node addon (native-dawn), loaded at runtime from a `wgpu/`
# directory beside the executable. Without both paths the build has no
# WebGPU: WebGPU-only carts are refused with that reason, carts that also
# import GL run on GL.
#
#   -DWASMCART_WGPU_JS_DIR=<wasmcart checkout>/src/wgpu
#   -DNATIVE_DAWN_DIR=<native-dawn>/dist/<platform>-<arch>
set(WASMCART_WGPU_JS_DIR "" CACHE PATH "wasmcart's src/wgpu directory (host.js and the generated glue)")
set(NATIVE_DAWN_DIR "" CACHE PATH "native-dawn install prefix for this platform")
if(WASMCART_WGPU_JS_DIR AND NATIVE_DAWN_DIR)
    set(WGPU_OUT "$<TARGET_FILE_DIR:wasmcart-run>/wgpu")
    file(WRITE "${CMAKE_BINARY_DIR}/wgpu-package.json" "{ \"type\": \"module\" }\n")
    # A target of its own, run on every build: as a POST_BUILD step it only ran
    # when the executable relinked, so edited JS was silently left stale.
    add_custom_target(wasmcart-wgpu ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WGPU_OUT}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/src/wgpu_bridge.cjs"
            "${WASMCART_WGPU_JS_DIR}/host.js"
            "${WASMCART_WGPU_JS_DIR}/glue-loader.js"
            "${WASMCART_WGPU_JS_DIR}/emdawnwebgpu-glue.mjs"
            "${WASMCART_WGPU_JS_DIR}/emdawnwebgpu-glue-manifest.js"
            "${NATIVE_DAWN_DIR}/lib/dawn.node"
            "${NATIVE_DAWN_DIR}/lib/libwebgpu_dawn.so"
            "${WGPU_OUT}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/wgpu-package.json" "${WGPU_OUT}/package.json"
        COMMENT "WebGPU support -> wgpu/")
    add_dependencies(wasmcart-wgpu wasmcart-run)
endif()

# Native addons (dawn.node above) resolve N-API and libuv from the process
# that loads them. libnode is linked statically, so its symbols are inside
# this executable but not exported until listed here.
if(UNIX AND NOT APPLE)
    file(WRITE "${CMAKE_BINARY_DIR}/node-api-exports.list" "{ napi_*; node_api_*; uv_*; };\n")
    target_link_options(wasmcart-run PRIVATE "-Wl,--dynamic-list=${CMAKE_BINARY_DIR}/node-api-exports.list")
endif()

# ─── Status ─────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "wasmcart-native:")
message(STATUS "  libnode: ${LIBNODE_DIR}")
message(STATUS "  SDL2:    ${SDL2_STATIC}")
message(STATUS "  EGL:     ${EGL_LIB}")
message(STATUS "  GLESv2:  ${GLESV2_LIB}")
message(STATUS "  Wayland: ${WAYLAND_EGL_FOUND}")
message(STATUS "")
