# razerctl — Razer Blade 16 control tool (sudo-only).
# Build:
#     cmake -B build && cmake --build build
# Install:
#     sudo cmake --install build

cmake_minimum_required(VERSION 3.10)
project(razerctl
        VERSION 1.0
        DESCRIPTION "Razer Blade 16 fan / perf / kbd / dGPU-undervolt control over hidraw"
        LANGUAGES C)

# Linux-only: it talks to /dev/hidraw and NVIDIA NvAPI/NVML.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "razerctl is Linux-only (uses hidraw + libnvidia-api).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)

add_executable(razerctl razerctl.c)
target_compile_options(razerctl PRIVATE -O2 -Wall)
target_link_libraries(razerctl PRIVATE ${CMAKE_DL_LIBS})   # dlopen libnvidia-api.so / libnvidia-ml.so

# ---- install ----
include(GNUInstallDirs)
install(TARGETS razerctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES razerctl.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
