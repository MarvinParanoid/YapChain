// SPDX-License-Identifier: Apache-2.0
//
// yapd — the YapChain ABCI++ application daemon. CometBFT connects to it over a Unix domain socket
// (proxy_app = "unix://<socket>", abci = "socket"). State lives in <home>/data (LMDB); the database is
// locked for exclusive writing, and a dev-chain database (yap dev) is refused.
//
// Exit codes: 0 — stopped by SIGINT/SIGTERM; 1 — command line; 2 — fail-stop while running (the state
// at the last durable Commit is intact); 3 — cannot start (storage, lock, socket, corrupt store).
//
// Test hooks exist only in the separate test executable yapd-test-hooks (compiled with
// YAPD_TEST_HOOKS, built with the tests, never installed). The yapd binary contains no hook code and
// ignores the variable below.
// YAPD_TEST_CRASH (yapd-test-hooks only): "before_commit:H" or "after_commit:H" kills the process with
// SIGKILL when Commit of height H arrives (before the durable write), or after the write but before
// the response is sent. Used by the live crash-recovery tests; it never changes what is written.
#include <CLI11.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

#include "yap/abci/application.hpp"
#include "yap/abci/convert.hpp"
#include "yap/abci/server.hpp"
#include "yap/chain/store_layout.hpp"
#include "yap/common/hex.hpp"
#include "yap/storage/lmdb_store.hpp"

namespace {

constexpr int kExitStopped = 0;
constexpr int kExitUsage = 1;
constexpr int kExitFailStop = 2;
constexpr int kExitStartup = 3;

std::mutex g_log_mu;

void log_line(const std::string& msg) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    const std::lock_guard lock(g_log_mu);
    std::cerr << buf << '.' << (ms < 100 ? (ms < 10 ? "00" : "0") : "") << ms << "Z yapd: " << msg << std::endl;
}

#ifdef YAPD_TEST_HOOKS
/// Test-only crash points (see the header comment). Tracks the height of the last FinalizeBlock
/// itself, so it never touches the engine outside the application's lock.
class CrashHook final : public yap::abci::Handler {
public:
    CrashHook(yap::abci::Handler& inner, std::string point, std::int64_t height)
        : inner_(inner), point_(std::move(point)), height_(height) {}

    void handle(const tendermint::abci::Request& req, tendermint::abci::Response& resp) override {
        if (req.has_finalize_block()) last_finalize_.store(req.finalize_block().height());
        const bool target = req.has_commit() && last_finalize_.load() == height_;
        if (target && point_ == "before_commit") die();
        inner_.handle(req, resp);
        if (target && point_ == "after_commit") die();
    }

private:
    void die() const {
        log_line("YAPD_TEST_CRASH: SIGKILL at " + point_ + ":" + std::to_string(height_));
        std::raise(SIGKILL);
    }
    yap::abci::Handler& inner_;
    std::string point_;
    std::int64_t height_;
    std::atomic<std::int64_t> last_finalize_{0};
};

std::optional<std::pair<std::string, std::int64_t>> crash_point_from_env() {
    const char* v = std::getenv("YAPD_TEST_CRASH");
    if (v == nullptr || *v == '\0') return std::nullopt;
    const std::string s(v);
    const auto colon = s.find(':');
    if (colon == std::string::npos) throw std::invalid_argument("YAPD_TEST_CRASH must be <point>:<height>");
    const std::string point = s.substr(0, colon);
    if (point != "before_commit" && point != "after_commit") {
        throw std::invalid_argument("YAPD_TEST_CRASH point must be before_commit or after_commit");
    }
    std::size_t used = 0;
    const auto h = std::stoll(s.substr(colon + 1), &used);
    if (used != s.size() - colon - 1 || h < 1) throw std::invalid_argument("YAPD_TEST_CRASH height must be >= 1");
    return std::make_pair(point, static_cast<std::int64_t>(h));
}
#endif  // YAPD_TEST_HOOKS

std::filesystem::path default_home() {
    if (const char* h = std::getenv("YAPD_HOME"); h != nullptr && *h != '\0') return h;
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') return std::filesystem::path(h) / ".yapd";
    return ".yapd";
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"yapd — YapChain ABCI++ application for CometBFT v0.40.0"};
    std::string home, socket;
    std::size_t max_connections = 8;
    std::size_t map_size_mib = 1024;
    bool version = false;
    bool trace = false;
    app.add_option("--home", home, "Node directory: <home>/data is the database (default: $YAPD_HOME or ~/.yapd)");
    app.add_option("--socket", socket, "ABCI Unix socket path (default: <home>/yapd.sock)");
    app.add_option("--max-connections", max_connections, "Maximum concurrent ABCI connections (CometBFT uses 4)")
        ->check(CLI::Range(4, 64))
        ->capture_default_str();
    app.add_option("--map-size-mib", map_size_mib, "Maximum database size in MiB")
        ->check(CLI::Range(16, 1 << 22))
        ->capture_default_str();
    app.add_flag("--trace-abci", trace, "Log every ABCI request (method, height, sizes)");
    app.add_flag("--version", version, "Print the version and exit");
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        const int rc = app.exit(e);
        return rc == 0 ? 0 : kExitUsage;
    }
    if (version) {
        std::cout << "yapd " << YAP_VERSION << " (ABCI app_version " << yap::abci::kAppVersion << ", CometBFT v0.40.0)"
#ifdef YAPD_TEST_HOOKS
                  << " [TEST BUILD: crash hooks]"
#endif
                  << "\n";
        return 0;
    }

    const std::filesystem::path home_dir = home.empty() ? default_home() : std::filesystem::path(home);
    const std::filesystem::path socket_path = socket.empty() ? home_dir / "yapd.sock" : std::filesystem::path(socket);

    // Signals are handled synchronously by the main thread only: block them before any thread starts.
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, nullptr);

#ifdef YAPD_TEST_HOOKS
    std::optional<std::pair<std::string, std::int64_t>> crash;
    try {
        crash = crash_point_from_env();
    } catch (const std::exception& e) {
        log_line(std::string("invalid YAPD_TEST_CRASH: ") + e.what());
        return kExitUsage;
    }
#endif

    std::unique_ptr<yap::storage::LmdbStore> store;
    std::unique_ptr<yap::chain::Engine> engine;
    std::unique_ptr<yap::abci::Application> application;
#ifdef YAPD_TEST_HOOKS
    std::unique_ptr<CrashHook> hook;
#endif
    std::unique_ptr<yap::abci::Server> server;
    std::atomic<bool> fatal{false};
    try {
        std::filesystem::create_directories(home_dir);
        yap::storage::LmdbStore::Options o{home_dir / "data"};
        o.map_size = map_size_mib << 20;
        store = std::make_unique<yap::storage::LmdbStore>(o);  // takes the exclusive writer lock
        engine = std::make_unique<yap::chain::Engine>(*store);  // start-up consistency checks
        yap::abci::ApplicationOptions ao;
        ao.software_version = YAP_VERSION;
        ao.log = log_line;
        ao.trace = trace;
        application = std::make_unique<yap::abci::Application>(*engine, ao);  // refuses dev-chain stores
        yap::abci::Handler* handler = application.get();
#ifdef YAPD_TEST_HOOKS
        log_line("TEST BUILD with crash hooks: not for production use");
        if (crash) {
            hook = std::make_unique<CrashHook>(*application, crash->first, crash->second);
            handler = hook.get();
            log_line("YAPD_TEST_CRASH armed: " + crash->first + ":" + std::to_string(crash->second));
        }
#endif
        yap::abci::ServerOptions so;
        so.socket_path = socket_path;
        so.max_connections = max_connections;
        so.on_fatal = [&fatal](const std::string& e) {
            log_line("fail-stop: " + e);
            fatal.store(true);
        };
        server = std::make_unique<yap::abci::Server>(so, *handler);
        server->start();
    } catch (const std::exception& e) {
        log_line(std::string("cannot start: ") + e.what());
        return kExitStartup;
    }
    if (engine->initialized()) {
        const auto info = engine->info();
        log_line("chain " + info.chain_id + " at height " + std::to_string(info.last_height) + ", state root " +
                 yap::to_hex(info.last_state_root));
    } else {
        log_line("empty database: waiting for InitChain");
    }
    log_line("listening on unix://" + socket_path.string());

    int rc = kExitStopped;
    while (true) {
        timespec timeout{0, 200'000'000};
        const int sig = ::sigtimedwait(&sigs, nullptr, &timeout);
        if (sig == SIGINT || sig == SIGTERM) {
            log_line(std::string("received ") + (sig == SIGINT ? "SIGINT" : "SIGTERM") + ", stopping");
            break;
        }
        if (fatal.load()) {
            rc = kExitFailStop;
            break;
        }
    }
    server->stop();  // closes every connection and joins all threads; nothing is half-written
    if (application->failure()) rc = kExitFailStop;
    if (engine->initialized()) {
        log_line("stopped at height " + std::to_string(engine->info().last_height));
    }
    server.reset();
    application.reset();
    engine.reset();
    store.reset();  // releases the lock
    return rc;
}
