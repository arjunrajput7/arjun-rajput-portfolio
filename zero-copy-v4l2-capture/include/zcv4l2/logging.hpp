// SPDX-License-Identifier: MIT
// Minimal logging + errno helpers for the zero-copy V4L2 capture engine.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace zcv4l2 {

inline std::string errno_str(int e) {
    char buf[256] = {0};
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    return std::string(strerror_r(e, buf, sizeof(buf)));
#else
    strerror_r(e, buf, sizeof(buf));
    return std::string(buf);
#endif
}

// Throwing wrapper used for unrecoverable setup failures.
class SystemError : public std::runtime_error {
public:
    SystemError(const std::string& what, int e)
        : std::runtime_error(what + ": " + errno_str(e) + " (errno " + std::to_string(e) + ")"),
          code(e) {}
    int code;
};

#define ZCV_LOG(fmt, ...)  std::fprintf(stderr, "[zcv4l2] " fmt "\n", ##__VA_ARGS__)
#define ZCV_WARN(fmt, ...) std::fprintf(stderr, "[zcv4l2][warn] " fmt "\n", ##__VA_ARGS__)

} // namespace zcv4l2
