cmake_minimum_required(VERSION 3.16)
project(pidvd C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DVDREAD REQUIRED IMPORTED_TARGET dvdread)

# STATIC explicitly: Buildroot passes BUILD_SHARED_LIBS=ON, which would
# turn this into a .so that the install step doesn't ship
add_library(pidvd_core STATIC
    src/core/disc.c
)
target_include_directories(pidvd_core PUBLIC src)
target_link_libraries(pidvd_core PUBLIC PkgConfig::DVDREAD)

# Host/dev tool: dump disc structure and detected video standard from an ISO
add_executable(dvdinfo tools/dvdinfo.c)
target_link_libraries(dvdinfo pidvd_core)

# Web remote: a dependency-free HTTP server that writes the picker's control
# words to /tmp/pidvd-ctl. Pure POSIX sockets, so it builds on the host too
# (run it against a FIFO you `cat` to iterate the page without a Pi).
add_executable(pidvd-remote tools/remote.c)

# Picker UI, pure rendering half — host-buildable so screens can be
# verified as PPMs (tools/uipreview.c) before touching the Pi.
add_library(pidvd_ui_render STATIC
    src/ui/font8.c
    src/ui/draw.c
    src/ui/render.c
    src/ui/settings.c
)
target_include_directories(pidvd_ui_render PUBLIC src)

add_executable(uipreview tools/uipreview.c)
target_link_libraries(uipreview pidvd_ui_render)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The player proper: KMS/DRM output (VEC), demux, libmpeg2 decode.
    # Linux-only — the host (macOS) build provides dvdinfo for core work.
    pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
    pkg_check_modules(MPEG2 REQUIRED IMPORTED_TARGET libmpeg2 libmpeg2convert)
    pkg_check_modules(DVDNAV REQUIRED IMPORTED_TARGET dvdnav)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

    add_executable(pidvd-player
        src/player/main.c
        src/ui/catalog.c
        src/ui/picker.c
        src/nav/engine.c
        src/demux/ps.c
        src/decode/video_mpeg2.c
        src/decode/audio_a52.c
        src/decode/spu.c
        src/present/presenter.c
        platform/linux/video_kms.c
        platform/linux/audio_alsa.c
        platform/linux/input_evdev.c
    )
    target_include_directories(pidvd-player PRIVATE platform)
    target_link_libraries(pidvd-player
        pidvd_core pidvd_ui_render PkgConfig::DRM PkgConfig::MPEG2
        PkgConfig::DVDNAV PkgConfig::ALSA a52 m pthread)

    install(TARGETS pidvd-player pidvd-remote dvdinfo RUNTIME DESTINATION bin)
else()
    install(TARGETS dvdinfo RUNTIME DESTINATION bin)
endif()
