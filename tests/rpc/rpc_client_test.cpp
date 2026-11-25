// SPDX-License-Identifier: Apache-2.0
//
// CometBFT RPC client (M2.6a) against a local mock HTTP server: valid answers, JSON-RPC errors,
// malformed envelopes and fields, HTTP errors, refused connections, disconnects, stalls, total
// timeouts, oversized responses, and that no request is ever repeated.
#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "yap/common/hex.hpp"
#include "yap/rpc/comet.hpp"

using namespace yap;
using namespace std::chrono_literals;

namespace {

/// What the mock does with one request: the raw bytes to send (status line, headers, body), then an
/// optional pause before / during sending, and whether to close without answering.
struct Reply {
    std::string raw;
    std::chrono::milliseconds delay_before{0};
    bool close_without_answer = false;
    std::size_t trickle_bytes = 0;  ///< send the first N bytes one per 200 ms (stall / slow server)
};

Reply http(int status, const std::string& body, const std::string& extra_headers = "") {
    return {"HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body.size()) + "\r\nConnection: close\r\n" + extra_headers + "\r\n" + body};
}

std::string ok(std::uint64_t id, const std::string& result) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":)" + result + "}";
}

std::string err(std::uint64_t id, long code, const std::string& msg, const std::string& data) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"error":{"code":)" + std::to_string(code) +
           R"(,"message":")" + msg + R"(","data":")" + data + R"("}})";
}

class MockServer {
public:
    using Handler = std::function<Reply(const std::string& request_body, std::uint64_t id)>;

    explicit MockServer(Handler h) : handler_(std::move(h)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        socklen_t len = sizeof(a);
        ::getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        REQUIRE(::listen(fd_, 16) == 0);
        thread_ = std::thread([this] { loop(); });
    }
    ~MockServer() {
        stop_ = true;
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        thread_.join();
    }
    [[nodiscard]] std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    [[nodiscard]] int requests() const { return requests_.load(); }
    [[nodiscard]] std::string last_request() {
        const std::lock_guard l(mu_);
        return last_;
    }

private:
    void loop() {
        while (!stop_) {
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) return;
            serve(c);
            ::close(c);
        }
    }
    void serve(int c) {
        std::string req;
        char buf[4096];
        std::size_t need = std::string::npos;
        while (true) {
            const auto n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) return;
            req.append(buf, static_cast<std::size_t>(n));
            const auto hdr_end = req.find("\r\n\r\n");
            if (hdr_end != std::string::npos && need == std::string::npos) {
                const auto cl = req.find("Content-Length: ");
                need = hdr_end + 4 + (cl == std::string::npos ? 0 : std::stoul(req.substr(cl + 16)));
            }
            if (need != std::string::npos && req.size() >= need) break;
        }
        ++requests_;
        const std::string body = req.substr(req.find("\r\n\r\n") + 4);
        {
            const std::lock_guard l(mu_);
            last_ = req;
        }
        std::uint64_t id = 0;
        if (const auto p = body.find("\"id\":"); p != std::string::npos) id = std::stoull(body.substr(p + 5));
        const Reply r = handler_(body, id);
        std::this_thread::sleep_for(r.delay_before);
        if (r.close_without_answer) return;
        std::size_t sent = 0;
        while (sent < r.trickle_bytes && sent < r.raw.size() && !stop_) {
            if (::send(c, r.raw.data() + sent, 1, MSG_NOSIGNAL) <= 0) return;
            ++sent;
            std::this_thread::sleep_for(200ms);
        }
        while (sent < r.raw.size()) {
            const auto n = ::send(c, r.raw.data() + sent, r.raw.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }
    Handler handler_;
    int fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> requests_{0};
    std::mutex mu_;
    std::string last_;
};

rpc::HttpOptions fast() {
    rpc::HttpOptions o;
    o.connect_timeout_ms = 2'000;
    o.read_timeout_ms = 1'000;
    o.total_timeout_ms = 5'000;
    o.max_response_bytes = 64 * 1024;
    return o;
}

const char* kStatus = R"({"node_info":{"id":"0123456789abcdef0123456789abcdef01234567","network":"yapchain-localnet-1",
 "moniker":"v0","version":"0.39.0"},"sync_info":{"latest_block_hash":"AB12000000000000000000000000000000000000000000000000000000000000",
 "latest_app_hash":"CD34000000000000000000000000000000000000000000000000000000000000","latest_block_height":"42",
 "latest_block_time":"2026-11-25T10:00:00.123Z","earliest_block_height":"1","catching_up":false},
 "validator_info":{"voting_power":"10"}})";

template <class E>
E thrown(const std::function<void()>& f) {
    try {
        f();
    } catch (const E& e) {
        return e;
    }
    FAIL("expected exception not thrown");
    throw std::logic_error("unreachable");
}

}  // namespace

TEST_CASE("rpc: status, net_info, abci_query, block, tx, broadcast parse strictly", "[rpc]") {
    MockServer s([](const std::string& body, std::uint64_t id) {
        if (body.find("\"status\"") != std::string::npos) return http(200, ok(id, kStatus));
        if (body.find("\"net_info\"") != std::string::npos) {
            return http(200, ok(id, R"({"listening":true,"n_peers":"1","peers":[{"node_info":{"id":"aa","moniker":"m"},"remote_ip":"127.0.0.1","is_outbound":true}]})"));
        }
        if (body.find("\"abci_query\"") != std::string::npos) {
            return http(200, ok(id, R"({"response":{"code":0,"log":"","height":"41","value":"eyJhIjoxfQ=="}})"));
        }
        if (body.find("\"block\"") != std::string::npos) {
            return http(200, ok(id, R"({"block_id":{"hash":"0000000000000000000000000000000000000000000000000000000000000001"},
              "block":{"header":{"chain_id":"yapchain-localnet-1","height":"5","time":"t",
              "app_hash":"0000000000000000000000000000000000000000000000000000000000000002",
              "proposer_address":"00000000000000000000000000000000000000AA"},"data":{"txs":["AQID"]},
              "last_commit":{"signatures":[{},{}]}}})"));
        }
        if (body.find("\"broadcast_tx_sync\"") != std::string::npos) {
            return http(200, ok(id, R"({"code":10,"log":"nonce","hash":"039058C6F2C0CB492C533B0A4D14EF77CC0F78ABCCCED5287D84A1A2011CFB81"})"));
        }
        return http(500, err(id, -32601, "Method not found", ""));
    });
    rpc::Client c(s.url(), fast());
    const auto st = c.status();
    CHECK(st.chain_id == "yapchain-localnet-1");
    CHECK(st.latest_height == 42);
    CHECK(st.voting_power == 10);
    CHECK_FALSE(st.catching_up);
    const auto ni = c.net_info();
    CHECK(ni.n_peers == 1);
    CHECK(ni.peers.at(0).outbound);
    const auto q = c.abci_query("/chain");
    CHECK(q.height == 41);
    CHECK(std::string(q.value.begin(), q.value.end()) == R"({"a":1})");
    const auto b = c.block(5);
    REQUIRE(b);
    CHECK(b->height == 5);
    CHECK(b->txs.size() == 1);
    CHECK(b->last_commit_signatures == 2);
    const auto br = c.broadcast_tx_sync(Bytes{1, 2, 3});
    CHECK(br.code == 10);
    CHECK(s.last_request().find(R"("tx":"AQID")") != std::string::npos);  // base64 of 01 02 03
    // Requests carry only public JSON: no credentials, cookies or authorization headers.
    CHECK(s.last_request().find("Authorization") == std::string::npos);
    CHECK(s.last_request().find("Cookie") == std::string::npos);
}

TEST_CASE("rpc: not found is distinguished from errors", "[rpc]") {
    const Hash32 h{};
    const std::string hx(64, '0');
    std::string mode;
    MockServer s([&](const std::string&, std::uint64_t id) {
        if (mode == "tx-missing") return http(500, err(id, -32603, "Internal error", "tx (" + hx + ") not found"));
        if (mode == "indexer-off") return http(500, err(id, -32603, "Internal error", "transaction indexing is disabled"));
        if (mode == "block-high") {
            return http(500, err(id, -32603, "Internal error",
                                 "height 99 must be less than or equal to the current blockchain height 5"));
        }
        return http(500, err(id, -32603, "Internal error", "something else"));
    });
    rpc::Client c(s.url(), fast());
    mode = "tx-missing";
    CHECK_FALSE(c.tx(h).has_value());
    mode = "indexer-off";
    CHECK(thrown<rpc::RpcError>([&] { (void)c.tx(h); }).data() == "transaction indexing is disabled");
    mode = "block-high";
    CHECK_FALSE(c.block(99).has_value());
    mode = "other";
    CHECK_THROWS_AS(c.block(1), rpc::RpcError);
}

TEST_CASE("rpc: malformed envelopes and fields are rejected", "[rpc]") {
    std::vector<std::function<std::string(std::uint64_t)>> bodies = {
        [](std::uint64_t) { return std::string("not json"); },
        [](std::uint64_t) { return std::string("[]"); },
        [](std::uint64_t id) { return ok(id + 1, kStatus); },                                     // wrong id
        [](std::uint64_t id) { return R"({"jsonrpc":"1.0","id":)" + std::to_string(id) + R"(,"result":{}})"; },
        [](std::uint64_t id) { return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + "}"; },  // neither
        [](std::uint64_t id) {
            return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"result":{},"error":{"code":1,"message":"x"}})";
        },
        [](std::uint64_t id) { return ok(id, R"({"node_info":{}})"); },  // missing fields
        [](std::uint64_t id) {  // height not a decimal string
            std::string st = kStatus;
            st.replace(st.find(R"("42")"), 4, R"("4x")");
            return ok(id, st);
        },
        [](std::uint64_t id) {  // negative height
            std::string st = kStatus;
            st.replace(st.find(R"("42")"), 4, R"("-1")");
            return ok(id, st);
        },
        [](std::uint64_t id) {  // lower-case hash
            std::string st = kStatus;
            st.replace(st.find("AB12"), 4, "ab12");
            return ok(id, st);
        },
        [](std::uint64_t id) { return ok(id, R"({"node_info":"x","sync_info":{}})"); },
    };
    std::size_t k = 0;
    MockServer s([&](const std::string&, std::uint64_t id) { return http(200, bodies[k](id)); });
    rpc::Client c(s.url(), fast());
    for (k = 0; k < bodies.size(); ++k) {
        INFO(k);
        CHECK_THROWS_AS(c.status(), rpc::MalformedResponse);
    }
    // HTTP error without a JSON-RPC body.
    MockServer s2([](const std::string&, std::uint64_t) { return http(502, "<html>bad gateway</html>"); });
    rpc::Client c2(s2.url(), fast());
    const auto e = thrown<rpc::MalformedResponse>([&] { (void)c2.status(); });
    CHECK(std::string(e.what()).find("HTTP 502") != std::string::npos);
}

TEST_CASE("rpc: transport failures are classified, bounded and never retried", "[rpc]") {
    SECTION("refused connection: nothing was sent") {
        int port = 0;
        {
            MockServer tmp([](const std::string&, std::uint64_t) { return Reply{}; });
            port = std::stoi(tmp.url().substr(tmp.url().rfind(':') + 1));
        }
        rpc::Client c("http://127.0.0.1:" + std::to_string(port), fast());
        const auto e = thrown<rpc::TransportError>([&] { (void)c.status(); });
        CHECK(e.kind() == rpc::TransportErrorKind::Connect);
        CHECK_FALSE(e.request_sent());
    }
    SECTION("closed without an answer: the request was sent (broadcast outcome unknown)") {
        MockServer s([](const std::string&, std::uint64_t) { return Reply{"", 0ms, true}; });
        rpc::Client c(s.url(), fast());
        const auto e = thrown<rpc::TransportError>([&] { (void)c.broadcast_tx_sync(Bytes{1}); });
        CHECK(e.kind() == rpc::TransportErrorKind::Disconnect);
        CHECK(e.request_sent());
        CHECK(s.requests() == 1);  // not retried
    }
    SECTION("stalled server: read timeout") {
        MockServer s([](const std::string&, std::uint64_t) { return Reply{"", 3000ms}; });
        auto o = fast();
        o.read_timeout_ms = 1'000;
        rpc::Client c(s.url(), o);
        const auto t0 = std::chrono::steady_clock::now();
        const auto e = thrown<rpc::TransportError>([&] { (void)c.broadcast_tx_sync(Bytes{1}); });
        CHECK(e.kind() == rpc::TransportErrorKind::Timeout);
        CHECK(e.request_sent());
        CHECK(std::chrono::steady_clock::now() - t0 < 2900ms);
        CHECK(s.requests() == 1);
    }
    SECTION("slow trickle: total timeout") {
        MockServer s([](const std::string&, std::uint64_t id) {
            Reply r = http(200, ok(id, kStatus));
            r.trickle_bytes = 100;  // 5 bytes/s: never stalls, never finishes in time
            return r;
        });
        auto o = fast();
        o.total_timeout_ms = 1'500;
        o.read_timeout_ms = 3'000;
        rpc::Client c(s.url(), o);
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(thrown<rpc::TransportError>([&] { (void)c.status(); }).kind() == rpc::TransportErrorKind::Timeout);
        CHECK(std::chrono::steady_clock::now() - t0 < 3s);
    }
    SECTION("oversized response: stopped at the limit, declared or not") {
        const std::string big(200 * 1024, 'x');
        MockServer s([&](const std::string&, std::uint64_t) { return http(200, big); });
        rpc::Client c(s.url(), fast());  // limit 64 KiB
        CHECK(thrown<rpc::TransportError>([&] { (void)c.status(); }).kind() == rpc::TransportErrorKind::TooLarge);
        MockServer s2([&](const std::string&, std::uint64_t) {  // no Content-Length: chunk-less stream
            return Reply{"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + big};
        });
        rpc::Client c2(s2.url(), fast());
        CHECK(thrown<rpc::TransportError>([&] { (void)c2.status(); }).kind() == rpc::TransportErrorKind::TooLarge);
    }
    SECTION("redirects are not followed") {
        MockServer s([](const std::string&, std::uint64_t) {
            return Reply{"HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};
        });
        rpc::Client c(s.url(), fast());
        CHECK_THROWS_AS(c.status(), rpc::MalformedResponse);
        CHECK(s.requests() == 1);
    }
}

TEST_CASE("rpc: endpoints are validated", "[rpc]") {
    for (const char* bad : {"ftp://x", "127.0.0.1:26657", "http://", "http://user:pw@host:1", "http://h/#f",
                            "http://h /x", "file:///etc/passwd"}) {
        INFO(bad);
        CHECK_THROWS_AS(rpc::Client(bad, fast()), rpc::TransportError);
    }
    CHECK_NOTHROW(rpc::Client("https://rpc.example:443", fast()));
    CHECK_NOTHROW(rpc::Client("http://127.0.0.1:26657/", fast()));
}

TEST_CASE("rpc: strict base64", "[rpc]") {
    CHECK(rpc::base64_encode(Bytes{}) == "");
    CHECK(rpc::base64_encode(Bytes{1, 2, 3}) == "AQID");
    CHECK(rpc::base64_encode(Bytes{0xff}) == "/w==");
    for (std::size_t n = 0; n < 40; ++n) {
        Bytes b(n);
        for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<Byte>(i * 37 + 11);
        CHECK(rpc::base64_decode(rpc::base64_encode(b)) == b);
    }
    for (const char* bad : {"A", "AQI", "AQID=", "A===", "/x==", "AQ=D", "AQID AQID", "-_==", "AQ==AQID"}) {
        INFO(bad);
        CHECK_FALSE(rpc::base64_decode(bad).has_value());
    }
}
