cmake_minimum_required(VERSION 3.16)
project(gtk-network-manager VERSION 0.1.0 LANGUAGES C)

# Default to Release so that debug symbols are not shipped inadvertently.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBNM REQUIRED IMPORTED_TARGET libnm)

add_executable(gtk-network-manager src/main.c)

# Base compile flags
target_compile_options(gtk-network-manager PRIVATE
    -Wall -Wextra -std=gnu11
    # Binary hardening — stack canary, FORTIFY wrappers, PIE (for ASLR)
    -fstack-protector-strong
    -D_FORTIFY_SOURCE=2
    -fPIE
)

# Linker hardening — read-only GOT (RELRO) + immediate binding (BIND_NOW)
target_link_options(gtk-network-manager PRIVATE
    -pie
    -Wl,-z,relro,-z,now
)

# Debug builds: enable Address Sanitizer and UBSan to catch memory bugs early.
# Build with:  cmake -DCMAKE_BUILD_TYPE=Debug ..
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(gtk-network-manager PRIVATE
        -fsanitize=address,undefined -g -O0 -fno-omit-frame-pointer)
    target_link_options(gtk-network-manager PRIVATE
        -fsanitize=address,undefined)
endif()

target_link_libraries(gtk-network-manager PRIVATE
    PkgConfig::GTK4
    PkgConfig::LIBNM
)

include(GNUInstallDirs)
install(TARGETS gtk-network-manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE
        GROUP_READ GROUP_EXECUTE
        WORLD_READ WORLD_EXECUTE
)
