// SPDX-License-Identifier: Apache-2.0
//
// ChainParams (docs/protocol.md §12).
#pragma once

#include <cstdint>
#include <string_view>

#include "yap/codec/ycs.hpp"

namespace yap::tx {

struct ChainParams {
    std::uint64_t min_fee = 0;
    std::uint16_t fee_burn_bps = 0;
    std::uint64_t block_reward = 0;
    std::uint16_t reward_validators_bps = 0;
    std::uint16_t reward_pool_bps = 0;
    std::uint16_t reward_treasury_bps = 0;
    std::uint64_t max_issuance = 0;
    std::uint32_t max_tx_bytes = 0;
    std::uint32_t max_memo_bytes = 0;
    std::uint64_t max_tx_ttl_blocks = 0;
    std::uint64_t epoch_length_blocks = 0;
    std::uint64_t max_attestation_validity_s = 0;
    std::uint64_t attestation_clock_skew_s = 0;
    std::uint16_t max_claims_per_subject_per_epoch = 0;

    bool operator==(const ChainParams&) const = default;
    void encode_to(ycs::Writer& w) const;
    static ChainParams decode_from(ycs::Reader& r);
};

inline constexpr std::uint32_t kBpsDenominator = 10000;

/// §12 validity rules. Returns an empty view if valid, otherwise a description of the first violation.
[[nodiscard]] std::string_view check_params(const ChainParams& p) noexcept;

/// The testnet values of docs/economics.md §8.
[[nodiscard]] ChainParams testnet_params() noexcept;

}  // namespace yap::tx
