// SPDX-License-Identifier: MIT
// Tiny headless daemon: every 5 s it logs board uptime, load average, and CPU
// thermal-zone temperature to stdout (captured by journald under systemd).
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <thread>

static double read_first_double(const char* path, double scale = 1.0) {
    std::ifstream f(path);
    double v = 0.0;
    if (f >> v) return v * scale;
    return -1.0;
}

int main() {
    std::cout << "telemetry-app started\n" << std::flush;
    std::uint64_t tick = 0;

    for (;;) {
        const double uptime  = read_first_double("/proc/uptime");
        const double loadavg = read_first_double("/proc/loadavg");
        const double temp_c  = read_first_double("/sys/class/thermal/thermal_zone0/temp", 0.001);

        std::cout << "tick=" << tick++
                  << " uptime_s=" << uptime
                  << " load1=" << loadavg
                  << " cpu_c=" << temp_c
                  << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    return 0;
}
