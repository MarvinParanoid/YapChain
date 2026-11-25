// SPDX-License-Identifier: Apache-2.0
// Deterministic mutation driver for fuzz_rpc (runs under GCC too).
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

extern "C" int yap_fuzz_rpc(const std::uint8_t* data, std::size_t size);

using namespace std::string_literals;

int main(int argc, char** argv) {
    const unsigned long iterations = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 100000UL;
    std::mt19937_64 rng(0x525043);
    const std::vector<std::string> seeds = {
        "\x00{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"a\":1}}"s,
        "\x00{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"m\",\"data\":\"d\"}}"s,
        "\x01{\"node_info\":{\"id\":\"x\",\"network\":\"c\"},\"sync_info\":{\"latest_block_hash\":\"\",\"latest_app_hash\":\"\","
        "\"latest_block_height\":\"5\",\"latest_block_time\":\"t\",\"earliest_block_height\":\"1\",\"catching_up\":false}}"s,
        "\x02{\"listening\":true,\"n_peers\":\"0\",\"peers\":[]}"s,
        "\x03{\"response\":{\"code\":0,\"height\":\"3\",\"value\":\"eyJhIjoxfQ==\"}}"s,
        "\x05{\"hash\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"height\":\"2\",\"index\":0,"
        "\"tx_result\":{\"code\":0},\"tx\":\"AQID\"}"s,
        "\x06{\"code\":0,\"hash\":\"0000000000000000000000000000000000000000000000000000000000000000\"}"s,
        "\x07QUJD"s,
        "\x00[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]"s,
    };
    for (unsigned long i = 0; i < iterations; ++i) {
        const std::string& s = seeds[i % seeds.size()];
        std::vector<std::uint8_t> m(s.begin(), s.end());
        for (int k = 0; k < 1 + static_cast<int>(rng() % 4); ++k) {
            switch (rng() % 4) {
                case 0: m[rng() % m.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8)); break;
                case 1: m[1 + rng() % (m.size() - 1)] = static_cast<std::uint8_t>(rng()); break;
                case 2: m.insert(m.begin() + 1 + static_cast<long>(rng() % (m.size() - 1)), static_cast<std::uint8_t>(rng())); break;
                default: m.resize(2 + rng() % (m.size() - 1)); break;
            }
        }
        yap_fuzz_rpc(m.data(), m.size());
    }
    return 0;
}
