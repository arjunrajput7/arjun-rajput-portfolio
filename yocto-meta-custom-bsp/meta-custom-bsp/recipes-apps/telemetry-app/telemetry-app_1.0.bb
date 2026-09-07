SUMMARY = "Headless telemetry sample application + systemd unit"
DESCRIPTION = "Cross-compiled C++17 daemon started automatically on boot."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://telemetry_app.cpp \
           file://telemetry-app.service"

S = "${WORKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "telemetry-app.service"
SYSTEMD_AUTO_ENABLE = "enable"

do_compile() {
    ${CXX} ${CXXFLAGS} ${LDFLAGS} -std=c++17 -O2 -Wall \
        ${WORKDIR}/telemetry_app.cpp -o ${B}/telemetry-app
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/telemetry-app ${D}${bindir}/telemetry-app

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/telemetry-app.service \
        ${D}${systemd_system_unitdir}/telemetry-app.service
}

FILES:${PN} += "${systemd_system_unitdir}/telemetry-app.service"
