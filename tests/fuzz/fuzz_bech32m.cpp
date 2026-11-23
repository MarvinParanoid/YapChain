// SPDX-License-Identifier: Apache-2.0
// Properties: accepted Bech32m strings re-encode to their lowercase form; accepted
// addresses re-encode to their lowercase form.
#include <cstdint>
#include <cstdlib>
#include <string>

#include "yap/crypto/address.hpp"
#include "yap/crypto/bech32m.hpp"

#ifdef YAP_FUZZ_SMOKE
#define YAP_FUZZ_ENTRY yap_fuzz_bech32m
#else
#define YAP_FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size);

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    std::string lower = text;
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (auto d = yap::crypto::bech32m_decode(text)) {
        if (yap::crypto::bech32m_encode(d->hrp, d->data) != lower) {
            std::abort();
        }
        if (auto id = yap::crypto::decode_address(text, d->hrp)) {
            if (yap::crypto::encode_address(d->hrp, *id) != lower) {
                std::abort();
            }
        }
    }
    return 0;
}
