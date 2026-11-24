// SPDX-License-Identifier: Apache-2.0
#include "yap/abci/server.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "tendermint/abci/types.pb.h"

namespace yap::abci {
namespace {

bool send_all(int fd, const std::string& data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_response(int fd, const tendermint::abci::Response& resp) {
    std::string body;
    if (!resp.SerializeToString(&body)) return false;
    std::string frame;
    append_uvarint(frame, body.size());
    frame += body;
    return send_all(fd, frame);
}

void write_exception(int fd, const std::string& error) {
    tendermint::abci::Response resp;
    resp.mutable_exception()->set_error(error);
    (void)write_response(fd, resp);
}

}  // namespace

Server::Server(ServerOptions options, Handler& handler) : opts_(std::move(options)), handler_(handler) {}

Server::~Server() { stop(); }

void Server::start() {
    const std::string path = opts_.socket_path.string();
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) throw std::runtime_error("ABCI socket path is empty or too long");
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) throw std::runtime_error(path + " exists and is not a socket");
        ::unlink(path.c_str());  // stale socket from a previous run
    }
    if (::pipe2(wake_pipe_, O_CLOEXEC) != 0) throw std::runtime_error("pipe2 failed");
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    const mode_t old_mask = ::umask(0177);  // the socket is created 0600: only this user may connect
    const int rc = ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::umask(old_mask);
    if (rc != 0) throw std::runtime_error("bind " + path + ": " + std::strerror(errno));
    ::chmod(path.c_str(), 0600);
    if (::listen(listen_fd_, 16) != 0) throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    acceptor_ = std::thread([this] { accept_loop(); });
}

void Server::accept_loop() {
    while (!stopping_.load()) {
        pollfd fds[2] = {{listen_fd_, POLLIN, 0}, {wake_pipe_[0], POLLIN, 0}};
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (stopping_.load() || (fds[1].revents & POLLIN) != 0) break;
        if ((fds[0].revents & POLLIN) == 0) continue;
        const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) continue;
        std::lock_guard<std::mutex> lock(conns_mu_);
        if (stopping_.load() || active_.load() >= opts_.max_connections) {
            ::close(fd);
            continue;
        }
        ++active_;
        conn_fds_.push_back(fd);
        conn_threads_.emplace_back([this, fd] { serve(fd); });
    }
}

void Server::serve(int fd) {
    FrameDecoder decoder(opts_.max_message_bytes);
    std::string frame;
    char buf[64 * 1024];
    bool open = true;
    while (open && !stopping_.load()) {
        const auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;  // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        decoder.feed(buf, static_cast<std::size_t>(n));
        for (;;) {
            const auto status = decoder.next(frame);
            if (status == FrameDecoder::Status::NeedMore) break;
            if (status == FrameDecoder::Status::Error) {
                write_exception(fd, "abci framing: " + decoder.error());
                open = false;
                break;
            }
            tendermint::abci::Request req;
            if (!req.ParseFromString(frame) || req.value_case() == tendermint::abci::Request::VALUE_NOT_SET) {
                write_exception(fd, "abci: malformed or empty Request");
                open = false;
                break;
            }
            tendermint::abci::Response resp;
            if (req.has_echo()) {
                resp.mutable_echo()->set_message(req.echo().message());
            } else if (req.has_flush()) {
                resp.mutable_flush();  // responses are already written as soon as they are ready
            } else {
                try {
                    handler_.handle(req, resp);
                } catch (const std::exception& e) {
                    write_exception(fd, e.what());
                    if (!failed_.exchange(true) && opts_.on_fatal) opts_.on_fatal(e.what());
                    open = false;
                    break;
                }
            }
            if (!write_response(fd, resp)) {
                open = false;
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        conn_fds_.erase(std::remove(conn_fds_.begin(), conn_fds_.end(), fd), conn_fds_.end());
    }
    ::close(fd);
    --active_;
}

void Server::stop() {
    if (stopping_.exchange(true)) {
        if (acceptor_.joinable()) acceptor_.join();
        return;
    }
    if (wake_pipe_[1] >= 0) (void)!::write(wake_pipe_[1], "x", 1);
    if (acceptor_.joinable()) acceptor_.join();
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(conns_mu_);
        for (const int fd : conn_fds_) ::shutdown(fd, SHUT_RDWR);  // unblocks recv in serve()
        threads.swap(conn_threads_);
    }
    for (auto& t : threads) t.join();
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        ::unlink(opts_.socket_path.c_str());
    }
    for (int& p : wake_pipe_) {
        if (p >= 0) ::close(p);
        p = -1;
    }
}

}  // namespace yap::abci
