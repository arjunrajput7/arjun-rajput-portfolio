// SPDX-License-Identifier: MIT
//
// can_tx - vehicle telemetry simulator.
//
//   can_tx [iface] [--hz N] [--redline]
//
// Broadcasts ENGINE_DATA (0x0C0) and VEHICLE_DATA (0x0D0) frames on a SocketCAN
// interface (default vcan0). Values follow a smooth synthetic drive cycle;
// --redline forces RPM / coolant past the alert thresholds so the receiver trips.
#include "can_common.h"

#include <errno.h>
#include <math.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/can.h>
#include <linux/can/raw.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    const char *iface = "vcan0";
    double hz = 10.0;
    int redline = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hz") && i + 1 < argc) hz = atof(argv[++i]);
        else if (!strcmp(argv[i], "--redline")) redline = 1;
        else if (argv[i][0] != '-') iface = argv[i];
        else { fprintf(stderr, "usage: %s [iface] [--hz N] [--redline]\n", argv[0]); return 2; }
    }

    int s = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (s < 0) { perror("socket"); return 1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "%s: %s (bring it up: scripts/setup_vcan.sh)\n", iface, strerror(errno));
        return 1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    const long period_ns = (long)(1e9 / (hz > 0 ? hz : 10.0));
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    printf("can_tx: %s @ %.1f Hz%s\n", iface, hz, redline ? "  [REDLINE]" : "");

    double t = 0.0;
    uint32_t odo_m = 0;

    while (g_run) {
        /* synthetic drive cycle */
        double throttle = 40.0 + 40.0 * sin(t * 0.15);            /* % */
        double rpm      = 900.0 + 30.0 * throttle + 400.0 * sin(t * 0.9);
        double speed    = fmax(0.0, 0.02 * rpm - 20.0);           /* km/h */
        double ect      = 80.0 + 15.0 * (1.0 - exp(-t / 40.0))
                          + 3.0 * sin(t * 0.05);                  /* degC */
        int    mil      = 0;

        if (redline) { rpm += 2500.0; ect += 40.0; mil = 1; }

        if (rpm < 0) rpm = 0;
        if (rpm > 16383 * RPM_SCALE) rpm = 16383 * RPM_SCALE;

        odo_m += (uint32_t)(speed / 3.6 / (hz > 0 ? hz : 10.0));  /* m this tick */

        struct can_frame f;

        /* ENGINE_DATA */
        memset(&f, 0, sizeof(f));
        f.can_id  = CANID_ENGINE_DATA;
        f.can_dlc = 8;
        be16_put(&f.data[0], (uint16_t)(rpm / RPM_SCALE));
        f.data[2] = (uint8_t)((int)lround(ect) - ECT_OFFSET);
        f.data[3] = (uint8_t)lround(throttle / THROTTLE_SCALE);
        f.data[4] = mil ? 0x01 : 0x00;
        if (write(s, &f, sizeof(f)) != (ssize_t)sizeof(f)) { perror("write"); break; }

        /* VEHICLE_DATA */
        memset(&f, 0, sizeof(f));
        f.can_id  = CANID_VEHICLE_DATA;
        f.can_dlc = 8;
        be16_put(&f.data[0], (uint16_t)lround(speed / SPEED_SCALE));
        be24_put(&f.data[2], odo_m);
        if (write(s, &f, sizeof(f)) != (ssize_t)sizeof(f)) { perror("write"); break; }

        t += 1.0 / (hz > 0 ? hz : 10.0);

        next.tv_nsec += period_ns;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    printf("\ncan_tx: stopped\n");
    close(s);
    return 0;
}
