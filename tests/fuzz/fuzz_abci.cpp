// SPDX-License-Identifier: Apache-2.0
// ABCI framing + Request parsing on arbitrary bytes, split into arbitrary chunks: must never crash,
// hang or allocate beyond the frame limit; every decoded frame either parses or is rejected.
#include <cstdint>
#include <string>

#include "tendermint/abci/types.pb.h"
#include "yap/abci/framing.hpp"

#ifdef YAP_FUZZ_SMOKE
#define YAP_FUZZ_ENTRY yap_fuzz_abci
#else
#define YAP_FUZZ_ENTRY LLVMFuzzerTestOneInput
#endif

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size);

extern "C" int YAP_FUZZ_ENTRY(const std::uint8_t* data, std::size_t size) {
    if (size == 0) return 0;
    yap::abci::FrameDecoder d(1 << 20);
    const std::size_t chunk = 1 + data[0] % 17;  // first byte chooses the fragmentation
    std::string frame;
    for (std::size_t i = 1; i < size; i += chunk) {
        const std::size_t n = std::min(chunk, size - i);
        d.feed(reinterpret_cast<const char*>(data + i), n);
        for (;;) {
            const auto st = d.next(frame);
            if (st != yap::abci::FrameDecoder::Status::Frame) break;
            tendermint::abci::Request req;
            if (req.ParseFromString(frame)) {
                std::string again;
                (void)req.SerializeToString(&again);
            }
        }
    }
    return 0;
}
