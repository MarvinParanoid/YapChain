// SPDX-License-Identifier: Apache-2.0
//
// Deterministic mutation driver for the fuzz targets, so that every compiler (including
// GCC, which has no libFuzzer) runs them in CI under ASan/UBSan. Not a replacement for
// coverage-guided fuzzing with the Clang `fuzz` preset.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "sample.hpp"
#include "yap/crypto/address.hpp"
#include "yap/dev/dev_block.hpp"
#include "yap/tx/transaction.hpp"

extern "C" int yap_fuzz_ycs(const std::uint8_t* data, std::size_t size);
extern "C" int yap_fuzz_bech32m(const std::uint8_t* data, std::size_t size);
extern "C" int yap_fuzz_tx(const std::uint8_t* data, std::size_t size);
extern "C" int yap_fuzz_devblock(const std::uint8_t* data, std::size_t size);

namespace {

using Input = std::vector<std::uint8_t>;

Input mutate(const Input& seed, std::mt19937_64& rng) {
    Input out = seed;
    const auto ops = 1 + rng() % 4;
    for (std::uint64_t i = 0; i < ops; ++i) {
        switch (rng() % 5) {
            case 0:  // flip a bit
                if (!out.empty()) out[rng() % out.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
                break;
            case 1:  // random byte
                if (!out.empty()) out[rng() % out.size()] = static_cast<std::uint8_t>(rng());
                break;
            case 2:  // insert
                out.insert(out.begin() + static_cast<std::ptrdiff_t>(rng() % (out.size() + 1)),
                           static_cast<std::uint8_t>(rng()));
                break;
            case 3:  // erase
                if (!out.empty()) out.erase(out.begin() + static_cast<std::ptrdiff_t>(rng() % out.size()));
                break;
            default:  // truncate
                if (!out.empty()) out.resize(rng() % out.size());
                break;
        }
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    const unsigned long iterations = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 100000UL;
    std::mt19937_64 rng(0x5941502d46555a5aULL);  // fixed seed: reproducible runs

    yap::test::Sample s;
    s.version = 1;
    s.name = "fuzz";
    s.tag = {1, 2, 3, 4};
    s.amount = 1000;
    s.maybe = 5;
    s.items = {{1}, {2, 3}};
    s.flag = true;
    s.kind_value = 7;
    const Input ycs_seed = yap::ycs::encode(s);

    yap::crypto::AccountId id{};
    id.fill(0x42);
    const std::string addr = yap::crypto::encode_address("tyap", id);
    const Input b32_seed(addr.begin(), addr.end());

    yap::crypto::Seed seed{};
    seed.fill(3);
    const auto key = yap::crypto::SecretKey::from_seed(seed);
    yap::tx::TxBody body;
    body.chain_id = "yapchain-testnet-1";
    body.sender_pubkey = key.public_key();
    body.fee = 1000;
    body.expires_at_height = 9;
    body.memo = {1, 2, 3};
    body.payload = yap::tx::Transfer{id, 77};
    const Input tx_seed = yap::ycs::encode(yap::tx::sign_tx(body, key));
    yap::dev::DevBlock block;
    block.header.chain_id = "yapchain-testnet-1";
    block.header.height = 3;
    block.header.producer_pubkey = key.public_key();
    block.txs = {tx_seed, {1, 2, 3}};
    block.signature = yap::dev::sign_header(block.header, key);
    const Input block_seed = yap::ycs::encode(block);

    for (unsigned long i = 0; i < iterations; ++i) {
        const Input a = mutate(ycs_seed, rng);
        yap_fuzz_ycs(a.data(), a.size());
        const Input b = mutate(b32_seed, rng);
        yap_fuzz_bech32m(b.data(), b.size());
        const Input c = mutate(tx_seed, rng);
        yap_fuzz_tx(c.data(), c.size());
        const Input e = mutate(block_seed, rng);
        yap_fuzz_devblock(e.data(), e.size());
    }
    yap_fuzz_devblock(block_seed.data(), block_seed.size());
    yap_fuzz_tx(tx_seed.data(), tx_seed.size());
    // Also feed the unmutated seeds (they must round-trip).
    yap_fuzz_ycs(ycs_seed.data(), ycs_seed.size());
    yap_fuzz_bech32m(b32_seed.data(), b32_seed.size());
    std::printf("fuzz smoke: %lu iterations x 4 targets OK\n", iterations);
    return 0;
}
