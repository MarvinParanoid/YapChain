// SPDX-License-Identifier: Apache-2.0
//
// yap-abci-probe — checks that an ABCI server answers on a Unix socket (M2.7). Used as the container
// healthcheck of yapd and by the CometBFT container to wait until its yapd is ready, so that start-up
// ordering never relies on fixed sleeps.
//
// It connects, sends one length-delimited Request{echo{message: ""}} — bytes 02 0a 00 (uvarint length 2,
// field 1 "echo", empty message) — and expects exactly Response{echo{message: ""}} — bytes 02 12 00.
// Echo is answered by the transport without entering the application, so it never waits for or
// changes consensus state, and the connection is closed immediately (yapd allows --max-connections,
// CometBFT uses 4). A stale socket file (no listener) fails with "connection refused".
//
// It has no dependencies beyond libc/libstdc++ (no protobuf), so the same binary can be copied into
// the CometBFT image.
//
//   yap-abci-probe --socket PATH [--timeout-ms N] [--wait-ms N] [--quiet]
//
// --wait-ms N retries every 200 ms until the server answers or N ms have passed (a readiness wait);
// without it the probe is a single attempt. Exit codes: 0 answered, 1 not answering, 64 usage.
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::array<std::uint8_t, 3> kRequest{0x02, 0x0a, 0x00};
constexpr std::array<std::uint8_t, 3> kResponse{0x02, 0x12, 0x00};

using Clock = std::chrono::steady_clock;

int remaining_ms(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// One attempt. Returns an empty string on success, otherwise the reason.
std::string probe_once(const std::string& path, int timeout_ms) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) return "socket path is empty or too long";
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return std::string("socket: ") + std::strerror(errno);
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != EINPROGRESS && errno != EAGAIN) return "connect " + path + ": " + std::strerror(errno);
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, remaining_ms(deadline)) <= 0) return "connect timed out";
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            return "connect " + path + ": " + std::strerror(err != 0 ? err : errno);
        }
    }
    std::size_t sent = 0;
    while (sent < kRequest.size()) {
        const auto n = ::send(fd, kRequest.data() + sent, kRequest.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {  // == EWOULDBLOCK on Linux
            pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, remaining_ms(deadline)) <= 0) return "send timed out";
            continue;
        }
        return std::string("send: ") + std::strerror(errno);
    }
    std::array<std::uint8_t, kResponse.size()> got{};
    std::size_t have = 0;
    while (have < got.size()) {
        pollfd p{fd, POLLIN, 0};
        const int pr = ::poll(&p, 1, remaining_ms(deadline));
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return "no answer within the timeout";
        const auto n = ::recv(fd, got.data() + have, got.size() - have, 0);
        if (n == 0) return "connection closed before the answer";
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;  // EAGAIN == EWOULDBLOCK on Linux
            return std::string("recv: ") + std::strerror(errno);
        }
        have += static_cast<std::size_t>(n);
    }
    if (got != kResponse) return "unexpected answer (not an ABCI echo)";
    return {};
}

bool parse_ms(std::string_view s, int& out) {
    if (s.empty() || s.size() > 9) return false;
    int v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

int usage() {
    std::fputs("usage: yap-abci-probe --socket PATH [--timeout-ms N] [--wait-ms N] [--quiet]\n", stderr);
    return 64;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path;
    int timeout_ms = 2000;
    int wait_ms = 0;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--socket" && has_value) {
            path = argv[++i];
        } else if (a == "--timeout-ms" && has_value) {
            if (!parse_ms(argv[++i], timeout_ms) || timeout_ms < 1) return usage();
        } else if (a == "--wait-ms" && has_value) {
            if (!parse_ms(argv[++i], wait_ms)) return usage();
        } else if (a == "--quiet") {
            quiet = true;
        } else {
            return usage();
        }
    }
    if (path.empty()) return usage();
    const auto deadline = Clock::now() + std::chrono::milliseconds(wait_ms);
    std::string why;
    for (;;) {
        why = probe_once(path, timeout_ms);
        if (why.empty()) {
            if (!quiet) std::printf("abci: %s answers\n", path.c_str());
            return 0;
        }
        if (Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::fprintf(stderr, "abci: %s does not answer: %s\n", path.c_str(), why.c_str());
    return 1;
}
