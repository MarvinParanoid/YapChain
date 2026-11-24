// SPDX-License-Identifier: Apache-2.0
//
// ABCI socket server over a Unix domain socket (never TCP: the socket is not reachable from other
// hosts and is created with mode 0600). One thread per connection; CometBFT opens four (consensus,
// mempool, query, snapshot). Responses on a connection are written in request order, immediately,
// so Flush only has to be acknowledged.
#pragma once

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "yap/abci/framing.hpp"

namespace tendermint::abci {
class Request;
class Response;
}  // namespace tendermint::abci

namespace yap::abci {

/// Application side. Called concurrently from several connection threads: implementations must
/// synchronise. Echo and Flush never reach the handler. Throwing makes the server answer with
/// ResponseException and close that connection (CometBFT then stops) — never a fake success.
class Handler {
public:
    virtual ~Handler() = default;
    virtual void handle(const tendermint::abci::Request& request, tendermint::abci::Response& response) = 0;
};

struct ServerOptions {
    std::filesystem::path socket_path;
    std::size_t max_message_bytes = kDefaultMaxMessageBytes;
    std::size_t max_connections = 16;
    /// Called once when a handler fails (e.g. FailStop) so the process can shut down.
    std::function<void(const std::string& error)> on_fatal;
};

class Server {
public:
    Server(ServerOptions options, Handler& handler);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds the socket (0600; a stale socket file is replaced, any other file is refused) and starts
    /// accepting. Throws std::runtime_error.
    void start();
    /// Graceful shutdown: stops accepting, closes every connection, joins all threads. Idempotent.
    void stop();

    [[nodiscard]] std::size_t active_connections() const noexcept { return active_.load(); }
    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }

private:
    void accept_loop();
    void serve(int fd);

    ServerOptions opts_;
    Handler& handler_;
    int listen_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::size_t> active_{0};
    std::thread acceptor_;
    std::mutex conns_mu_;
    std::vector<int> conn_fds_;
    std::vector<std::thread> conn_threads_;
};

}  // namespace yap::abci
