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

extern "C" int yap_fuzz_ycs(const std::uint8_t* data, std::size_t size);
extern "C" int yap_fuzz_bech32m(const std::uint8_t* data, std::size_t size);

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

    for (unsigned long i = 0; i < iterations; ++i) {
        const Input a = mutate(ycs_seed, rng);
        yap_fuzz_ycs(a.data(), a.size());
        const Input b = mutate(b32_seed, rng);
        yap_fuzz_bech32m(b.data(), b.size());
    }
    // Also feed the unmutated seeds (they must round-trip).
    yap_fuzz_ycs(ycs_seed.data(), ycs_seed.size());
    yap_fuzz_bech32m(b32_seed.data(), b32_seed.size());
    std::printf("fuzz smoke: %lu iterations x 2 targets OK\n", iterations);
    return 0;
}
