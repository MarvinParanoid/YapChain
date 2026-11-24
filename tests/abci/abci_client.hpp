// SPDX-License-Identifier: Apache-2.0
// Minimal blocking ABCI socket client for tests (speaks the real framing over a Unix socket).
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>

#include "tendermint/abci/types.pb.h"
#include "yap/abci/framing.hpp"

namespace yap::test {

class AbciClient {
public:
    explicit AbciClient(const std::string& path) {
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            throw std::runtime_error("connect failed");
        }
    }
    ~AbciClient() { close(); }
    AbciClient(const AbciClient&) = delete;
    AbciClient& operator=(const AbciClient&) = delete;

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }
    void send_raw(const std::string& bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            const auto n = ::send(fd_, bytes.data() + done, bytes.size() - done, MSG_NOSIGNAL);
            if (n <= 0) throw std::runtime_error("send failed");
            done += static_cast<std::size_t>(n);
        }
    }
    static std::string frame(const tendermint::abci::Request& req) {
        std::string body = req.SerializeAsString();
        std::string out;
        abci::append_uvarint(out, body.size());
        return out + body;
    }
    void send(const tendermint::abci::Request& req) { send_raw(frame(req)); }
    /// Next response, or nullopt if the server closed the connection.
    std::optional<tendermint::abci::Response> receive() {
        std::string f;
        for (;;) {
            const auto st = decoder_.next(f);
            if (st == abci::FrameDecoder::Status::Frame) {
                tendermint::abci::Response r;
                if (!r.ParseFromString(f)) throw std::runtime_error("bad response");
                return r;
            }
            if (st == abci::FrameDecoder::Status::Error) throw std::runtime_error(decoder_.error());
            char buf[4096];
            const auto n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) return std::nullopt;
            decoder_.feed(buf, static_cast<std::size_t>(n));
        }
    }
    tendermint::abci::Response call(const tendermint::abci::Request& req) {
        send(req);
        auto r = receive();
        if (!r) throw std::runtime_error("connection closed");
        return *r;
    }

private:
    int fd_ = -1;
    abci::FrameDecoder decoder_;
};

}  // namespace yap::test
