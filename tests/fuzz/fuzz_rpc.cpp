// SPDX-License-Identifier: Apache-2.0
// CometBFT JSON-RPC response parsers (M2.6) on arbitrary bytes: every input is either accepted with
// well-typed fields or rejected with MalformedResponse / RpcError — never a crash, hang or other
// exception. The first byte selects the parser; the rest is the response body / result object.
#include <cstdint>
#include <string>

#include "yap/rpc/comet.hpp"

#ifdef YAP_FUZZ_SMOKE
#define YAP_FUZZ_ENTRY yap_fuzz_rpc
#else
#define YAP_FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size);

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    const std::string in(reinterpret_cast<const char*>(data + 1), size - 1);
    try {
        switch (data[0] % 8) {
            case 0: (void)yap::rpc::parse_envelope(in, 1); break;
            case 1: (void)yap::rpc::parse_status(in); break;
            case 2: (void)yap::rpc::parse_net_info(in); break;
            case 3: (void)yap::rpc::parse_abci_query(in); break;
            case 4: (void)yap::rpc::parse_block(in); break;
            case 5: (void)yap::rpc::parse_tx(in); break;
            case 6: (void)yap::rpc::parse_broadcast(in); break;
            default: {
                const auto d = yap::rpc::base64_decode(in);
                if (d && yap::rpc::base64_encode(*d) != in) __builtin_trap();  // canonical round trip
            }
        }
    } catch (const yap::rpc::MalformedResponse&) {
    } catch (const yap::rpc::RpcError&) {
    }
    return 0;
}
