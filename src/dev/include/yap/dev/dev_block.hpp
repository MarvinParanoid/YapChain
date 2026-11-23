// SPDX-License-Identifier: Apache-2.0
//
// Development blocks (protocol.md §14). Local development only: they do not replace BFT
// consensus and are never used by a testnet.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "yap/codec/ycs.hpp"
#include "yap/crypto/ed25519.hpp"

namespace yap::dev {

inline constexpr std::uint16_t kDevBlockVersion = 1;
inline constexpr std::uint32_t kMaxDevBlockTxs = 10'000;      // protocol.md §3.3
inline constexpr std::uint32_t kMaxDevBlockTxBytes = 65'536;  // protocol.md §3.3

struct DevBlockHeader {
    std::uint16_t version = kDevBlockVersion;
    std::string chain_id;
    std::uint64_t height = 0;
    std::uint64_t time_ms = 0;
    Hash32 prev_block_hash{};  ///< genesis_hash for height 1
    Hash32 tx_root{};          ///< RFC 6962 root of the transactions (§8.2)
    Hash32 state_root{};       ///< state after executing this block
    crypto::PublicKey producer_pubkey{};

    bool operator==(const DevBlockHeader&) const = default;
    void encode_to(ycs::Writer& w) const;
    static DevBlockHeader decode_from(ycs::Reader& r);
};

struct DevBlock {
    DevBlockHeader header;
    std::vector<Bytes> txs;
    crypto::Signature signature{};  ///< Ed25519(producer, "YAPCHAIN/DEVBLOCK/V1\0" || YCS(header))

    bool operator==(const DevBlock&) const = default;
    void encode_to(ycs::Writer& w) const;
    static DevBlock decode_from(ycs::Reader& r);
};

/// block_hash = H_tag("YAP/devblock/v1", YCS(header))
[[nodiscard]] Hash32 block_hash(const DevBlockHeader& header);
[[nodiscard]] crypto::Signature sign_header(const DevBlockHeader& header, const crypto::SecretKey& producer);
[[nodiscard]] bool verify_header_signature(const DevBlock& block);

}  // namespace yap::dev
