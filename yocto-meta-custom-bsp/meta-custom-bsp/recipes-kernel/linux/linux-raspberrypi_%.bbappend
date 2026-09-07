# Enable netfilter / nftables in the RPi kernel via a config fragment.
# For a QEMU build rename this file to linux-yocto_%.bbappend -- the body is
# identical because both recipes inherit kernel-yocto and honour *.cfg fragments.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://netfilter.cfg"
