// SPDX-License-Identifier: Apache-2.0
// Property: any input accepted by the strict decoder re-encodes to exactly the same bytes.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "sample.hpp"

#ifdef YAP_FUZZ_SMOKE
#define YAP_FUZZ_ENTRY yap_fuzz_ycs
#else
#define YAP_FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size);

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size) {
    const yap::ByteView in(data, size);
    auto decoded = yap::ycs::decode<yap::test::Sample>(in);
    if (decoded) {
        const auto again = yap::ycs::encode(*decoded);
        if (again.size() != size || (size != 0 && std::memcmp(again.data(), data, size) != 0)) {
            std::abort();  // non-canonical acceptance
        }
    }
    return 0;
}
