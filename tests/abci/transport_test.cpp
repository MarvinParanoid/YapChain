// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#include "abci_client.hpp"
#include "temp_dir.hpp"
#include "yap/abci/server.hpp"
#include "yap/common/hex.hpp"
#include "yap/crypto/hash.hpp"

using namespace yap;
using tendermint::abci::Request;
using tendermint::abci::Response;

namespace {

Request echo(const std::string& m) {
    Request r;
    r.mutable_echo()->set_message(m);
    return r;
}

/// Handler that answers Info with a fixed height and throws on Query.
class TestHandler final : public abci::Handler {
public:
    std::atomic<int> calls{0};
    void handle(const Request& req, Response& resp) override {
        ++calls;
        if (req.has_info()) {
            resp.mutable_info()->set_last_block_height(42);
            return;
        }
        if (req.has_query()) throw std::runtime_error("handler failure");
        resp.mutable_exception()->set_error("unsupported in test");
    }
};

struct Fixture {
    test::TempDir dir;
    TestHandler handler;
    std::atomic<int> fatal{0};
    abci::ServerOptions opts() {
        abci::ServerOptions o;
        o.socket_path = dir.path() / "abci.sock";
        o.max_message_bytes = 1 << 20;
        o.max_connections = 6;
        o.on_fatal = [this](const std::string&) { ++fatal; };
        return o;
    }
};

}  // namespace

TEST_CASE("framing: fragmented, multiple and boundary frames", "[abci][framing]") {
    std::string stream;
    for (const std::string& m : {std::string(""), std::string("a"), std::string(300, 'x'), std::string(70000, 'y')}) {
        abci::append_uvarint(stream, m.size());
        stream += m;
    }
    // Byte by byte (worst-case fragmentation).
    abci::FrameDecoder d;
    std::vector<std::string> got;
    std::string f;
    for (const char c : stream) {
        d.feed(&c, 1);
        while (d.next(f) == abci::FrameDecoder::Status::Frame) got.push_back(f);
    }
    REQUIRE(got.size() == 4);
    CHECK(got[2] == std::string(300, 'x'));
    CHECK(got[3].size() == 70000);
    // All at once (several frames in one read).
    abci::FrameDecoder d2;
    d2.feed(stream.data(), stream.size());
    int n = 0;
    while (d2.next(f) == abci::FrameDecoder::Status::Frame) ++n;
    CHECK(n == 4);
    CHECK(d2.buffered() == 0);
    // varint encodings
    std::string v;
    abci::append_uvarint(v, 300);
    CHECK(v == std::string("\xac\x02", 2));
}

TEST_CASE("framing: malformed and oversized prefixes are errors without allocation", "[abci][framing]") {
    std::string f;
    abci::FrameDecoder too_long;
    const std::string eleven(11, '\x80');
    too_long.feed(eleven.data(), eleven.size());
    CHECK(too_long.next(f) == abci::FrameDecoder::Status::Error);
    abci::FrameDecoder overflow;
    std::string ov(9, '\xff');
    ov.push_back('\x02');  // 10th byte > 1: more than 64 bits
    overflow.feed(ov.data(), ov.size());
    CHECK(overflow.next(f) == abci::FrameDecoder::Status::Error);
    abci::FrameDecoder big(1000);
    std::string pre;
    abci::append_uvarint(pre, 1001);
    big.feed(pre.data(), pre.size());
    CHECK(big.next(f) == abci::FrameDecoder::Status::Error);  // refused before any payload arrives
    abci::FrameDecoder huge;
    std::string hp;
    abci::append_uvarint(hp, 1ULL << 62);
    huge.feed(hp.data(), hp.size());
    CHECK(huge.next(f) == abci::FrameDecoder::Status::Error);
    abci::FrameDecoder partial;
    std::string p;
    abci::append_uvarint(p, 10);
    p += "abc";
    partial.feed(p.data(), p.size());
    CHECK(partial.next(f) == abci::FrameDecoder::Status::NeedMore);  // truncated: waits, never guesses
}

TEST_CASE("server: echo, flush and ordered responses", "[abci][server]") {
    Fixture fx;
    abci::Server server(fx.opts(), fx.handler);
    server.start();
    struct stat st {};
    REQUIRE(::stat(fx.opts().socket_path.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);  // not reachable by other users

    test::AbciClient c(fx.opts().socket_path.string());
    CHECK(c.call(echo("hello")).echo().message() == "hello");
    // Pipelined requests in one write, responses in order, then Flush.
    Request info;
    info.mutable_info();
    Request flush;
    flush.mutable_flush();
    c.send_raw(test::AbciClient::frame(echo("1")) + test::AbciClient::frame(info) + test::AbciClient::frame(echo("2")) +
               test::AbciClient::frame(flush));
    CHECK(c.receive()->echo().message() == "1");
    CHECK(c.receive()->info().last_block_height() == 42);
    CHECK(c.receive()->echo().message() == "2");
    CHECK(c.receive()->has_flush());
    server.stop();
}

TEST_CASE("server: malformed input closes only that connection", "[abci][server]") {
    Fixture fx;
    abci::Server server(fx.opts(), fx.handler);
    server.start();
    const auto path = fx.opts().socket_path.string();
    {
        test::AbciClient bad(path);  // truncated protobuf inside a well-formed frame
        std::string body = echo("hello world").SerializeAsString();
        body.resize(body.size() - 3);
        std::string fr;
        abci::append_uvarint(fr, body.size());
        bad.send_raw(fr + body);
        const auto r = bad.receive();
        REQUIRE(r.has_value());
        CHECK(r->has_exception());
        CHECK_FALSE(bad.receive().has_value());  // then closed
    }
    {
        test::AbciClient bad(path);  // malformed varint
        bad.send_raw(std::string(11, '\x80'));
        CHECK(bad.receive()->has_exception());
        CHECK_FALSE(bad.receive().has_value());
    }
    {
        test::AbciClient bad(path);  // oversized frame
        std::string fr;
        abci::append_uvarint(fr, (1 << 20) + 1);
        bad.send_raw(fr);
        CHECK(bad.receive()->has_exception());
    }
    {
        test::AbciClient bad(path);  // empty Request (no oneof set)
        bad.send_raw(std::string("\x00", 1));
        CHECK(bad.receive()->has_exception());
    }
    {
        test::AbciClient gone(path);  // disconnect in the middle of a frame
        std::string fr;
        abci::append_uvarint(fr, 50);
        gone.send_raw(fr + "partial");
        gone.close();
    }
    // The server is still healthy for other connections and no handler failure was reported.
    test::AbciClient ok(path);
    CHECK(ok.call(echo("still here")).echo().message() == "still here");
    CHECK(fx.fatal.load() == 0);
    server.stop();
}

TEST_CASE("server: large garbage, deep nesting and a stalled partial frame stay within bounds", "[abci][server][limits]") {
    Fixture fx;
    abci::Server server(fx.opts(), fx.handler);  // max_message_bytes = 1 MiB
    server.start();
    const auto path = fx.opts().socket_path.string();
    const auto t0 = std::chrono::steady_clock::now();

    // A stalled partial frame holds only its own connection: nothing is allocated for the declared
    // length and other connections are served meanwhile.
    test::AbciClient stalled(path);
    {
        std::string fr;
        abci::append_uvarint(fr, 1 << 20);  // exactly the limit, never completed
        stalled.send_raw(fr + std::string(1000, 'x'));
    }
    {
        // A full-size frame of garbage: rejected by the protobuf parser, connection closed.
        test::AbciClient bad(path);
        std::string fr;
        abci::append_uvarint(fr, 1 << 20);
        bad.send_raw(fr + std::string(1 << 20, '\xff'));
        const auto r = bad.receive();
        REQUIRE(r.has_value());
        CHECK(r->has_exception());
        CHECK_FALSE(bad.receive().has_value());
    }
    {
        // 200 000 nested unknown groups (wire type 3): the parser's recursion limit rejects it without
        // exhausting the stack (a crash here would take the whole process down).
        test::AbciClient bad(path);
        std::string body;
        constexpr int kDepth = 200'000;
        body.reserve(kDepth * 4);
        for (int i = 0; i < kDepth; ++i) body += "\xfb\x7f";  // field 2047, START_GROUP (varint 16379)
        std::string fr;
        abci::append_uvarint(fr, body.size());
        bad.send_raw(fr + body);
        const auto r = bad.receive();
        REQUIRE(r.has_value());
        CHECK(r->has_exception());
    }
    test::AbciClient ok(path);
    CHECK(ok.call(echo("served")).echo().message() == "served");
    CHECK(fx.handler.calls.load() == 0);  // nothing malformed reached the application
    CHECK(fx.fatal.load() == 0);
    // Generous bound (sanitizer builds): the point is that nothing waits for the stalled frame.
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
    stalled.close();
    server.stop();  // returns although a connection was idle in the middle of a frame
}

TEST_CASE("server: a failing handler yields ResponseException and on_fatal", "[abci][server]") {
    Fixture fx;
    abci::Server server(fx.opts(), fx.handler);
    server.start();
    test::AbciClient c(fx.opts().socket_path.string());
    Request q;
    q.mutable_query()->set_path("/x");
    const auto r = c.call(q);
    CHECK(r.has_exception());
    CHECK(r.exception().error() == "handler failure");
    CHECK_FALSE(c.receive().has_value());
    CHECK(fx.fatal.load() == 1);
    CHECK(server.failed());
    server.stop();
}

TEST_CASE("server: concurrent connections and the connection limit", "[abci][server]") {
    Fixture fx;
    abci::Server server(fx.opts(), fx.handler);
    server.start();
    const auto path = fx.opts().socket_path.string();
    std::atomic<int> ok{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {  // like CometBFT's consensus/mempool/query/snapshot connections
        threads.emplace_back([&, t] {
            test::AbciClient c(path);
            for (int i = 0; i < 200; ++i) {
                const auto m = std::to_string(t) + ":" + std::to_string(i);
                if (c.call(echo(m)).echo().message() == m) ++ok;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(ok.load() == 800);
    // The server notices the closed clients asynchronously; the limit counts live connections.
    for (int i = 0; i < 500 && server.active_connections() != 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(server.active_connections() == 0);

    std::vector<std::unique_ptr<test::AbciClient>> held;
    for (int i = 0; i < 6; ++i) {
        held.push_back(std::make_unique<test::AbciClient>(path));
        CHECK(held.back()->call(echo("x")).echo().message() == "x");
    }
    test::AbciClient extra(path);  // 7th connection is refused (closed immediately)
    bool answered = false;
    try {
        extra.send(echo("y"));  // may already fail with EPIPE: the server closed the socket
        answered = extra.receive().has_value();
    } catch (const std::runtime_error&) {
    }
    CHECK_FALSE(answered);
    server.stop();  // graceful with connections still open
    CHECK(server.active_connections() == 0);
    CHECK_FALSE(std::filesystem::exists(fx.opts().socket_path));
}

TEST_CASE("server: refuses a socket another server listens on, replaces a stale one", "[abci][server]") {
    Fixture fx;
    abci::Server first(fx.opts(), fx.handler);
    first.start();
    abci::Server second(fx.opts(), fx.handler);
    CHECK_THROWS_AS(second.start(), std::runtime_error);
    test::AbciClient c(fx.opts().socket_path.string());  // the first server is untouched
    CHECK(c.call(echo("still here")).echo().message() == "still here");
    first.stop();
    // A leftover socket file without a listener is stale and replaced.
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    const auto p = fx.opts().socket_path.string();
    std::memcpy(a.sun_path, p.c_str(), p.size() + 1);
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    ::close(fd);
    abci::Server third(fx.opts(), fx.handler);
    third.start();
    test::AbciClient c3(p);
    CHECK(c3.call(echo("new")).echo().message() == "new");
    third.stop();
}

TEST_CASE("server: refuses to replace a non-socket file", "[abci][server]") {
    Fixture fx;
    std::ofstream(fx.opts().socket_path) << "not a socket";
    abci::Server server(fx.opts(), fx.handler);
    CHECK_THROWS_AS(server.start(), std::runtime_error);
}

TEST_CASE("vendored CometBFT proto files match their checksums", "[abci][proto]") {
    const std::filesystem::path root = YAP_PROTO_DIR;
    std::ifstream sums(root / "SHA256SUMS");
    std::string line;
    int n = 0;
    while (std::getline(sums, line)) {
        const auto hash = line.substr(0, 64);
        const auto file = line.substr(66);
        std::ifstream in(root / file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        INFO(file);
        CHECK(to_hex(crypto::sha256(as_bytes(ss.str()))) == hash);
        ++n;
    }
    CHECK(n == 6);
}
