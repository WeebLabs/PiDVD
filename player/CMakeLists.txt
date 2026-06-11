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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The player proper: KMS/DRM output (VEC), demux, libmpeg2 decode.
    # Linux-only — the host (macOS) build provides dvdinfo for core work.
    pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
    pkg_check_modules(MPEG2 REQUIRED IMPORTED_TARGET libmpeg2 libmpeg2convert)

    add_executable(pidvd-player
        src/player/main.c
        src/demux/ps.c
        src/decode/video_mpeg2.c
        src/present/presenter.c
        platform/linux/video_kms.c
    )
    target_include_directories(pidvd-player PRIVATE platform)
    target_link_libraries(pidvd-player
        pidvd_core PkgConfig::DRM PkgConfig::MPEG2 pthread)

    install(TARGETS pidvd-player dvdinfo RUNTIME DESTINATION bin)
else()
    install(TARGETS dvdinfo RUNTIME DESTINATION bin)
endif()
