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

# The player binary. Currently a milestone-1 stub; grows KMS/ALSA/evdev
# backends (Linux target only) as platform/linux/ is implemented.
add_executable(pidvd-player src/player/main.c)
target_link_libraries(pidvd-player pidvd_core)

install(TARGETS pidvd-player dvdinfo RUNTIME DESTINATION bin)
