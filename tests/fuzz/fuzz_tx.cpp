// SPDX-License-Identifier: Apache-2.0
// Property: any accepted SignedTx re-encodes to exactly the input bytes (canonical encoding).
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "yap/tx/transaction.hpp"

#ifdef YAP_FUZZ_SMOKE
#define YAP_FUZZ_ENTRY yap_fuzz_tx
#else
#define YAP_FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size);

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size) {
    auto decoded = yap::tx::decode_signed_tx(yap::ByteView(data, size));
    if (decoded) {
        const auto again = yap::ycs::encode(*decoded);
        if (again.size() != size || (size != 0 && std::memcmp(again.data(), data, size) != 0)) {
            std::abort();
        }
    }
    return 0;
}
