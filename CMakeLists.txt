cmake_minimum_required(VERSION 3.16)
project(gtk-network-manager VERSION 0.1.0 LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LIBNM REQUIRED IMPORTED_TARGET libnm)

add_executable(gtk-network-manager src/main.c)

target_compile_options(gtk-network-manager PRIVATE -Wall -Wextra -std=gnu11)

target_link_libraries(gtk-network-manager PRIVATE
    PkgConfig::GTK4
    PkgConfig::LIBNM
)

include(GNUInstallDirs)
install(TARGETS gtk-network-manager
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
