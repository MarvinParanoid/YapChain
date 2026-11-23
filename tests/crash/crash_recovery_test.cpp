// SPDX-License-Identifier: Apache-2.0
//
// Crash-recovery tests: a child process executes blocks on an LMDB store and is killed with
// SIGKILL at a chosen fault point inside the commit of a chosen block. The parent (a different
// process; the test runner itself is never killed) reopens the store, verifies it, checks that
// exactly the previous or the fully committed height is visible, finishes the chain and compares
// every root with an uninterrupted in-memory execution.
#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <functional>
#include <random>

#include "temp_dir.hpp"
#include "testnet.hpp"
#include "yap/state/state_root.hpp"
#include "yap/storage/lmdb_store.hpp"

using namespace yap;
using namespace yap::test;

namespace {

/// Shared reference: genesis, journal and the roots of an uninterrupted MemoryStore run.
struct Reference {
    TestNet net = TestNet::make(40);
    std::vector<chain::ReplayBlock> blocks;
    std::vector<Hash32> roots;
    Hash32 genesis_root{};

    Reference(std::size_t n_blocks, std::size_t txs_per_block, std::uint64_t seed)
        : blocks(net.journal(n_blocks, txs_per_block, seed)) {
        roots = reference_roots(net, blocks);  // also stores expected roots in the journal
        storage::MemoryStore s;
        chain::Engine e(s);
        genesis_root = e.init_chain(net.genesis);
    }
    [[nodiscard]] Hash32 root_at(std::uint64_t h) const { return h == 0 ? genesis_root : roots[h - 1]; }
};

const Reference& short_ref() {
    static const Reference r(60, 10, 1234);
    return r;
}

storage::LmdbStore::Options opts(const std::filesystem::path& dir) { return storage::LmdbStore::Options{dir}; }

/// Runs `body` in a forked child. Returns the raw wait status.
int in_child(const std::function<void()>& body) {
    const pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        int code = 0;
        try {
            body();
        } catch (...) {
            code = 2;
        }
        _exit(code);  // no atexit handlers, no Catch2 output from the child
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

bool killed_by_sigkill(int status) { return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL; }

/// Child body: open the store, initialise if needed, execute blocks up to `kill_height - 1`
/// normally, then arm the fault hook and commit `kill_height`.
void crash_during_commit(const std::filesystem::path& dir, const Reference& ref, std::uint64_t kill_height,
                         const std::string& point) {
    storage::LmdbStore store(opts(dir));
    chain::Engine e(store);
    if (!e.initialized()) {
        if (kill_height == 0) {
            storage::set_fault_hook([point](std::string_view p) {
                if (p == point) raise(SIGKILL);
            });
        }
        e.init_chain(ref.net.genesis);
    }
    chain::replay(e, std::span(ref.blocks).first(kill_height - 1));
    storage::set_fault_hook([point](std::string_view p) {
        if (p == point) raise(SIGKILL);
    });
    chain::replay(e, std::span(ref.blocks).subspan(kill_height - 1, 1));
}

/// Parent side: reopen, verify, check the visible height, finish the chain.
void recover_and_finish(const std::filesystem::path& dir, const Reference& ref, std::uint64_t expect_height,
                        std::uint64_t finish_height) {
    storage::LmdbStore store(opts(dir));
    const auto rep = chain::verify_state(store);  // integrity of everything on disk
    CHECK(rep.height == expect_height);
    CHECK(rep.state_root == ref.root_at(expect_height));

    chain::Engine e(store);
    CHECK(e.info().last_height == expect_height);
    state::Overlay view(&e.committed());
    const auto cs = state::State(view).chain_state();
    CHECK(cs.height == expect_height);
    CHECK(cs.total_supply == cs.total_minted - cs.total_burned);

    // Continue: the journal's expected roots make replay fail on any divergence.
    chain::replay(e, std::span(ref.blocks).first(finish_height));
    CHECK(e.info().last_height == finish_height);
    CHECK(e.info().last_state_root == ref.root_at(finish_height));
    CHECK_NOTHROW((void)chain::verify_state(store));
}

}  // namespace

TEST_CASE("SIGKILL at every fault point of a block commit", "[crash][lmdb]") {
    const auto& ref = short_ref();
    constexpr std::uint64_t kKill = 37;
    struct Case {
        const char* point;
        std::uint64_t visible;  // height expected after the crash
    };
    for (const Case c : {Case{"before_txn", kKill - 1}, Case{"mid_accounts", kKill - 1}, Case{"after_accounts", kKill - 1},
                         Case{"after_chain_state", kKill - 1}, Case{"after_smt", kKill - 1},
                         Case{"before_commit", kKill - 1}, Case{"after_commit", kKill}}) {
        INFO("fault point " << c.point);
        TempDir dir;
        const int status = in_child([&] { crash_during_commit(dir.path(), ref, kKill, c.point); });
        REQUIRE(killed_by_sigkill(status));
        recover_and_finish(dir.path(), ref, c.visible, ref.blocks.size());
    }
}

TEST_CASE("SIGKILL during init_chain leaves an empty store that can be initialised", "[crash][lmdb]") {
    const auto& ref = short_ref();
    for (const char* point : {"mid_accounts", "after_smt", "before_commit"}) {
        INFO("fault point " << point);
        TempDir dir;
        const int status = in_child([&] {
            storage::LmdbStore store(opts(dir.path()));
            chain::Engine e(store);
            storage::set_fault_hook([point](std::string_view p) {
                if (p == point) raise(SIGKILL);
            });
            e.init_chain(ref.net.genesis);
        });
        REQUIRE(killed_by_sigkill(status));
        storage::LmdbStore store(opts(dir.path()));
        chain::Engine e(store);
        CHECK_FALSE(e.initialized());  // nothing of the interrupted genesis is visible
        CHECK(e.init_chain(ref.net.genesis) == ref.genesis_root);
        chain::replay(e, std::span(ref.blocks).first(10));
        CHECK(e.info().last_state_root == ref.root_at(10));
    }
}

TEST_CASE("repeated crashes at random points converge to the uninterrupted chain", "[crash][lmdb]") {
    const auto& ref = short_ref();
    const char* points[] = {"before_txn", "mid_accounts", "after_accounts", "after_chain_state",
                            "after_smt", "before_commit", "after_commit"};
    std::mt19937_64 rng(99);
    TempDir dir;
    std::uint64_t height = 0;
    for (int crash = 0; crash < 6; ++crash) {
        const std::uint64_t kill = height + 1 + rng() % 8;
        const std::string point = points[rng() % 7];
        INFO("crash " << crash << " at height " << kill << " point " << point);
        const int status = in_child([&] { crash_during_commit(dir.path(), ref, kill, point); });
        REQUIRE(killed_by_sigkill(status));
        storage::LmdbStore store(opts(dir.path()));
        const auto rep = chain::verify_state(store);
        const std::uint64_t expect = point == "after_commit" ? kill : kill - 1;
        CHECK(rep.height == expect);
        CHECK(rep.state_root == ref.root_at(expect));
        height = rep.height;
    }
    recover_and_finish(dir.path(), ref, height, ref.blocks.size());
}

TEST_CASE("long run: 1000 blocks, 10000+ transactions, restarts, crashes, Memory == LMDB", "[crash][lmdb][long]") {
    // The journal carries the roots of an uninterrupted MemoryStore run, so every replayed LMDB
    // block is compared with it (replay refuses to commit a diverging height).
    static const Reference ref(1000, 11, 4242);
    std::size_t txs = 0;
    for (const auto& b : ref.blocks) txs += b.ctx.txs.size();
    REQUIRE(txs >= 10'000);

    TempDir dir;
    std::mt19937_64 rng(7);
    std::uint64_t height = 0;
    int restarts = 0;
    int crashes = 0;
    while (height < ref.blocks.size()) {
        const std::uint64_t target = std::min<std::uint64_t>(ref.blocks.size(), height + 20 + rng() % 180);
        if (rng() % 3 == 0 && target > height + 1) {
            // Crash inside the commit of `target` at a random point, in a child process.
            const char* points[] = {"before_txn", "mid_accounts", "after_chain_state", "after_smt", "before_commit",
                                    "after_commit"};
            const std::string point = points[rng() % 6];
            const int status = in_child([&] { crash_during_commit(dir.path(), ref, target, point); });
            REQUIRE(killed_by_sigkill(status));
            ++crashes;
        } else {
            // Clean run in this process, then close the store (restart).
            storage::LmdbStore store(opts(dir.path()));
            chain::Engine e(store);
            if (!e.initialized()) e.init_chain(ref.net.genesis);
            chain::replay(e, std::span(ref.blocks).first(target), [&](const chain::ReplayBlock&, const state::BlockResult&) {
                state::check_invariants(e.committed());  // supply invariants after every block
            });
            ++restarts;
        }
        storage::LmdbStore store(opts(dir.path()));
        const auto rep = chain::verify_state(store);
        REQUIRE(rep.state_root == ref.root_at(rep.height));
        height = rep.height;
    }
    CHECK(restarts >= 3);
    CHECK(crashes >= 2);

    // A second, uninterrupted LMDB replay of the same journal gives the same final root.
    TempDir dir2;
    storage::LmdbStore store2(opts(dir2.path()));
    chain::Engine e2(store2);
    e2.init_chain(ref.net.genesis);
    CHECK(chain::replay(e2, ref.blocks) == ref.blocks.size());
    CHECK(e2.info().last_state_root == ref.roots.back());
    CHECK_NOTHROW((void)chain::verify_state(store2));
    WARN("long run: blocks=" << ref.blocks.size() << " txs=" << txs << " restarts=" << restarts << " crashes=" << crashes);
}
