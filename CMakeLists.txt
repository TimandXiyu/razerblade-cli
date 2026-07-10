# razerctl (client, no root) + razerctld (daemon, root via systemd).
# Build:
#     cmake -B build && cmake --build build
# Install (installs both binaries, the systemd unit, and enables+starts it):
#     sudo cmake --install build

cmake_minimum_required(VERSION 3.10)
project(razerctl
        VERSION 2.0
        DESCRIPTION "Razer Blade 16 fan / perf / kbd / battery-limit / dGPU-undervolt control"
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
target_link_libraries(razerctl PRIVATE ${CMAKE_DL_LIBS})   # dlopen libnvidia-api.so / libnvidia-ml.so (dGPU undervolt page only)

add_executable(razerctld razerctld.c)
target_compile_options(razerctld PRIVATE -O2 -Wall)

# ---- install ----
include(GNUInstallDirs)
install(TARGETS razerctl razerctld RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES razerctl.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES razerctld.service DESTINATION /etc/systemd/system)
install(PROGRAMS zz-razerctld-resume DESTINATION /usr/lib/systemd/system-sleep)
install(CODE "execute_process(COMMAND systemctl daemon-reload)")
install(CODE "execute_process(COMMAND systemctl enable --now razerctld)")
