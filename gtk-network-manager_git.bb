SUMMARY = "Touch-first GTK4/libnm Wi-Fi manager for Weston/Wayland"
DESCRIPTION = "GTK4 application that provides a touch-friendly Wi-Fi management \
UI using NetworkManager's libnm for scanning, connecting, and toggling wireless \
interfaces. Targets Weston DRM backend on Wayland."
HOMEPAGE = "https://github.com/Ashwin-Prabhakar/gtk-network-manager"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

SRC_URI = "git://github.com/Ashwin-Prabhakar/gtk-network-manager.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"

PV = "0.1.0+git${SRCPV}"

inherit cmake pkgconfig

# Build-time dependencies
DEPENDS = " \
    gtk4 \
    networkmanager \
    virtual/libglib-2.0 \
"

# Runtime dependencies
RDEPENDS:${PN} = " \
    gtk4 \
    networkmanager \
"
