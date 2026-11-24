// SPDX-License-Identifier: Apache-2.0
// Deterministic mutation driver for fuzz_abci (runs under GCC too).
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "tendermint/abci/types.pb.h"
#include "yap/abci/framing.hpp"

extern "C" int yap_fuzz_abci(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
    const unsigned long iterations = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 100000UL;
    std::mt19937_64 rng(0x41424349);
    std::string seed(1, '\x03');
    for (int i = 0; i < 3; ++i) {
        tendermint::abci::Request r;
        auto* fb = r.mutable_finalize_block();
        fb->set_height(7 + i);
        fb->add_txs(std::string(40, 'a'));
        fb->mutable_decided_last_commit()->add_votes()->set_block_id_flag(tendermint::types::BLOCK_ID_FLAG_COMMIT);
        const auto body = r.SerializeAsString();
        yap::abci::append_uvarint(seed, body.size());
        seed += body;
    }
    std::vector<std::uint8_t> base(seed.begin(), seed.end());
    for (unsigned long i = 0; i < iterations; ++i) {
        auto m = base;
        for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) {
            switch (rng() % 4) {
                case 0: m[rng() % m.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8)); break;
                case 1: m[rng() % m.size()] = static_cast<std::uint8_t>(rng()); break;
                case 2: m.insert(m.begin() + static_cast<long>(rng() % m.size()), static_cast<std::uint8_t>(rng())); break;
                default: m.resize(1 + rng() % m.size()); break;
            }
        }
        yap_fuzz_abci(m.data(), m.size());
    }
    yap_fuzz_abci(base.data(), base.size());
    std::printf("abci fuzz smoke: %lu iterations OK\n", iterations);
    return 0;
}
