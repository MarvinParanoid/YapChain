// SPDX-License-Identifier: Apache-2.0
//
// Compares the old full SMT recomputation with incremental persistent updates.
// Usage: yap_smt_bench [n_leaves ...]   (build in Release for meaningful numbers)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

#include "yap/merkle/persistent_smt.hpp"

using namespace yap;
using Clock = std::chrono::steady_clock;

namespace {
struct Nodes final : merkle::NodeReader {
    std::map<Hash32, merkle::SmtNode> m;
    [[nodiscard]] std::optional<merkle::SmtNode> node(const Hash32& h) const override {
        auto it = m.find(h);
        return it == m.end() ? std::nullopt : std::optional<merkle::SmtNode>(it->second);
    }
};
Hash32 rnd(std::mt19937_64& rng) {
    Hash32 h{};
    for (auto& b : h) b = static_cast<Byte>(rng());
    return h;
}
double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
}  // namespace

int main(int argc, char** argv) {
    std::vector<std::size_t> sizes{1'000, 10'000, 100'000};
    if (argc > 1) {
        sizes.clear();
        for (int i = 1; i < argc; ++i) sizes.push_back(std::strtoull(argv[i], nullptr, 10));
    }
    constexpr std::size_t kUpdates = 20;  // keys touched by a typical block
    std::printf("%10s %16s %22s %20s\n", "leaves", "full root (ms)", "20 updates incr. (ms)", "speed-up");
    for (const auto n : sizes) {
        std::mt19937_64 rng(n);
        std::vector<merkle::SmtLeaf> leaves;
        Nodes nodes;
        merkle::SmtUpdater build(nodes, Hash32{});
        for (std::size_t i = 0; i < n; ++i) {
            leaves.push_back({rnd(rng), rnd(rng)});
            build.update(leaves.back().key, leaves.back().value_hash);
        }
        for (const auto& [h, node] : build.changes()) {
            if (node) nodes.m[h] = *node;
        }
        const Hash32 root = build.root();

        // Before (M1.2): recompute from all leaves after changing kUpdates values.
        for (std::size_t i = 0; i < kUpdates; ++i) leaves[rng() % n].value_hash = rnd(rng);
        const auto t0 = Clock::now();
        const Hash32 full = merkle::smt_root(leaves);
        const auto t1 = Clock::now();
        // After (M1.3): incremental update of the same number of keys on the stored tree.
        merkle::SmtUpdater u(nodes, root);
        for (std::size_t i = 0; i < kUpdates; ++i) u.update(leaves[rng() % n].key, rnd(rng));
        const auto t2 = Clock::now();
        (void)full;
        std::printf("%10zu %16.3f %22.3f %19.0fx\n", n, ms(t1 - t0), ms(t2 - t1), ms(t1 - t0) / ms(t2 - t1));
    }
    return 0;
}
