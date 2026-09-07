// SPDX-License-Identifier: MIT
//
// can_rx - telemetry receiver, decoder, and alert monitor.
//
//   can_rx [iface]
//
// Installs a CAN_RAW_FILTER for ENGINE_DATA (0x0C0) and VEHICLE_DATA (0x0D0)
// only, decodes each frame per dbc/telemetry.dbc, prints a formatted line, and
// raises an ALERT when RPM / coolant / speed exceed the safe thresholds.
#include "can_common.h"

#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/can.h>
#include <linux/can/raw.h>

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    const char *iface = (argc > 1 && argv[1][0] != '-') ? argv[1] : "vcan0";

    int s = socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (s < 0) { perror("socket"); return 1; }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "%s: %s\n", iface, strerror(errno));
        return 1;
    }

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    /* kernel-side filter: deliver only the two IDs we care about */
    struct can_filter filt[2] = {
        { .can_id = CANID_ENGINE_DATA,  .can_mask = CAN_SFF_MASK },
        { .can_id = CANID_VEHICLE_DATA, .can_mask = CAN_SFF_MASK },
    };
    if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER, &filt, sizeof(filt)) < 0) {
        perror("setsockopt(CAN_RAW_FILTER)");
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    printf("can_rx: listening on %s (filtered: 0x%03X, 0x%03X)\n",
           iface, CANID_ENGINE_DATA, CANID_VEHICLE_DATA);

    double last_speed = 0.0;
    unsigned long rx = 0, alerts = 0;

    while (g_run) {
        struct can_frame f;
        ssize_t n = read(s, &f, sizeof(f));
        if (n < 0) {
            if (errno == EINTR) break;
            perror("read");
            break;
        }
        if (n < (ssize_t)sizeof(struct can_frame))
            continue;
        rx++;

        if (f.can_id == CANID_ENGINE_DATA && f.can_dlc >= 5) {
            double rpm      = be16_get(&f.data[0]) * RPM_SCALE;
            int    ect_c    = (int)f.data[2] + ECT_OFFSET;
            double throttle = f.data[3] * THROTTLE_SCALE;
            int    mil      = f.data[4] & 0x01;

            printf("ENGINE   rpm=%6.0f  ect=%4d C  thr=%5.1f %%  MIL=%d\n",
                   rpm, ect_c, throttle, mil);

            if (rpm > ALERT_RPM_REDLINE) {
                printf("  !! ALERT: RPM %.0f over redline %.0f\n", rpm, ALERT_RPM_REDLINE);
                alerts++;
            }
            if (ect_c > ALERT_ECT_MAX_C) {
                printf("  !! ALERT: coolant %d C over %.0f C\n", ect_c, ALERT_ECT_MAX_C);
                alerts++;
            }
        } else if (f.can_id == CANID_VEHICLE_DATA && f.can_dlc >= 5) {
            double speed = be16_get(&f.data[0]) * SPEED_SCALE;
            uint32_t odo = be24_get(&f.data[2]);

            printf("VEHICLE  speed=%6.2f km/h  odo=%lu m  (d=%+.2f)\n",
                   speed, (unsigned long)odo, speed - last_speed);
            last_speed = speed;

            if (speed > ALERT_SPEED_MAX_KMH) {
                printf("  !! ALERT: speed %.1f over %.0f km/h\n", speed, ALERT_SPEED_MAX_KMH);
                alerts++;
            }
        }
    }

    printf("\ncan_rx: stopped. frames=%lu alerts=%lu\n", rx, alerts);
    close(s);
    return 0;
}
