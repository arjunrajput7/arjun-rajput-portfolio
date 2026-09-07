SUMMARY = "Minimal headless production image (no GUI, target < 100 MB rootfs)"
LICENSE = "MIT"

inherit core-image

# --- what goes in -----------------------------------------------------------
IMAGE_INSTALL = "packagegroup-core-boot \
                 ${CORE_IMAGE_EXTRA_INSTALL} \
                 telemetry-app \
                 nftables"

# dropbear is much smaller than openssh; drop it entirely for the tightest build
IMAGE_FEATURES += "ssh-server-dropbear"
IMAGE_LINGUAS = ""

# --- strip it down --------------------------------------------------------
DISTRO_FEATURES:remove = "x11 wayland vulkan opengl bluetooth nfc 3g"
IMAGE_FEATURES:remove = "package-management"
PACKAGE_EXCLUDE = "shadow-securetty"

# no slack space; tolerate a tiny rootfs
IMAGE_OVERHEAD_FACTOR = "1.0"
IMAGE_ROOTFS_EXTRA_SPACE = "0"
IMAGE_ROOTFS_SIZE ?= "8192"

# remove docs / locale / static libs from the rootfs
ROOTFS_POSTPROCESS_COMMAND += "rootfs_strip_extras; "
rootfs_strip_extras() {
    rm -rf ${IMAGE_ROOTFS}${mandir} ${IMAGE_ROOTFS}${infodir} \
           ${IMAGE_ROOTFS}${docdir} ${IMAGE_ROOTFS}${datadir}/locale || true
}

# print the final size at the end of the build
addtask do_report_size after do_image_complete
python do_report_size() {
    import os
    root = d.getVar('IMAGE_ROOTFS')
    total = 0
    for dirpath, _, files in os.walk(root):
        for f in files:
            fp = os.path.join(dirpath, f)
            if not os.path.islink(fp):
                total += os.path.getsize(fp)
    bb.plain("custom-headless-image rootfs size: %.1f MiB" % (total / (1024.0 * 1024.0)))
}
