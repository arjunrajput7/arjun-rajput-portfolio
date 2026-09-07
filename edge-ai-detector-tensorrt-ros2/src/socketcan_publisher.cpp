// SPDX-License-Identifier: MIT
#include "edge_ai_detector/socketcan_publisher.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>

namespace edge_ai {

SocketCanPublisher::SocketCanPublisher(std::string iface, std::uint32_t base_id)
    : iface_(std::move(iface)), base_id_(base_id) {}

SocketCanPublisher::~SocketCanPublisher() {
    if (fd_ >= 0) ::close(fd_);
}

bool SocketCanPublisher::open() {
    fd_ = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC, CAN_RAW);
    if (fd_ < 0) {
        std::perror("[can] socket");
        return false;
    }

    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface_.c_str(), IFNAMSIZ - 1);
    if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
        std::fprintf(stderr, "[can] interface %s not found\n", iface_.c_str());
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    sockaddr_can addr{};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[can] bind");
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    std::fprintf(stderr, "[can] bound to %s, base id 0x%X\n", iface_.c_str(), base_id_);
    return true;
}

bool SocketCanPublisher::publish(const std::vector<Detection>& dets, int frame_w, int frame_h) {
    if (fd_ < 0 || frame_w <= 0 || frame_h <= 0) return false;

    const int total = std::min<int>(dets.size(), 255);
    bool ok = true;
    for (int i = 0; i < total; ++i) {
        const Detection& d = dets[i];
        const float cx = (d.x + d.w * 0.5f) / float(frame_w);
        const float cy = (d.y + d.h * 0.5f) / float(frame_h);
        const std::uint16_t nx = std::uint16_t(std::clamp(cx, 0.f, 1.f) * 65535.f);
        const std::uint16_t ny = std::uint16_t(std::clamp(cy, 0.f, 1.f) * 65535.f);

        can_frame fr{};
        fr.can_id  = base_id_;
        fr.can_dlc = 8;
        fr.data[0] = std::uint8_t(i);
        fr.data[1] = std::uint8_t(total);
        fr.data[2] = std::uint8_t(std::clamp(d.class_id, 0, 255));
        fr.data[3] = std::uint8_t(std::clamp(d.score, 0.f, 1.f) * 255.f);
        fr.data[4] = std::uint8_t(nx >> 8);
        fr.data[5] = std::uint8_t(nx & 0xff);
        fr.data[6] = std::uint8_t(ny >> 8);
        fr.data[7] = std::uint8_t(ny & 0xff);

        const ssize_t w = ::write(fd_, &fr, sizeof(fr));
        if (w != ssize_t(sizeof(fr))) {
            std::perror("[can] write");
            ok = false;
            break;
        }
    }
    return ok;
}

} // namespace edge_ai
