// SPDX-License-Identifier: Apache-2.0
#include "yap/tx/params.hpp"

#include "yap/tx/transaction.hpp"

namespace yap::tx {

void ChainParams::encode_to(ycs::Writer& w) const {
    w.u64(min_fee);
    w.u16(fee_burn_bps);
    w.u64(block_reward);
    w.u16(reward_validators_bps);
    w.u16(reward_pool_bps);
    w.u16(reward_treasury_bps);
    w.u64(max_issuance);
    w.u32(max_tx_bytes);
    w.u32(max_memo_bytes);
    w.u64(max_tx_ttl_blocks);
    w.u64(epoch_length_blocks);
    w.u64(max_attestation_validity_s);
    w.u64(attestation_clock_skew_s);
    w.u16(max_claims_per_subject_per_epoch);
}

ChainParams ChainParams::decode_from(ycs::Reader& r) {
    ChainParams p;
    p.min_fee = r.u64();
    p.fee_burn_bps = r.u16();
    p.block_reward = r.u64();
    p.reward_validators_bps = r.u16();
    p.reward_pool_bps = r.u16();
    p.reward_treasury_bps = r.u16();
    p.max_issuance = r.u64();
    p.max_tx_bytes = r.u32();
    p.max_memo_bytes = r.u32();
    p.max_tx_ttl_blocks = r.u64();
    p.epoch_length_blocks = r.u64();
    p.max_attestation_validity_s = r.u64();
    p.attestation_clock_skew_s = r.u64();
    p.max_claims_per_subject_per_epoch = r.u16();
    return p;
}

std::string_view check_params(const ChainParams& p) noexcept {
    if (p.min_fee < 1) return "min_fee must be >= 1";
    if (p.fee_burn_bps > kBpsDenominator) return "fee_burn_bps must be <= 10000";
    // u16 + u16 + u16 cannot overflow uint32_t.
    if (static_cast<std::uint32_t>(p.reward_validators_bps) + p.reward_pool_bps + p.reward_treasury_bps !=
        kBpsDenominator) {
        return "reward_*_bps must sum to 10000";
    }
    if (p.max_issuance < 1) return "max_issuance must be >= 1";
    if (p.max_tx_bytes < 256 || p.max_tx_bytes > 65536) return "max_tx_bytes must be in 256..65536";
    if (p.max_memo_bytes > kMemoCeiling) return "max_memo_bytes must be <= 1024";
    if (p.max_tx_ttl_blocks < 1) return "max_tx_ttl_blocks must be >= 1";
    if (p.epoch_length_blocks < 1) return "epoch_length_blocks must be >= 1";
    if (p.max_attestation_validity_s < 1) return "max_attestation_validity_s must be >= 1";
    if (p.max_claims_per_subject_per_epoch < 1) return "max_claims_per_subject_per_epoch must be >= 1";
    return {};
}

ChainParams testnet_params() noexcept {
    ChainParams p;
    p.min_fee = 1'000;
    p.fee_burn_bps = 5'000;
    p.block_reward = 2'000'000;
    p.reward_validators_bps = 6'000;
    p.reward_pool_bps = 3'000;
    p.reward_treasury_bps = 1'000;
    p.max_issuance = 1'000'000'000'000'000;
    p.max_tx_bytes = 4'096;
    p.max_memo_bytes = 128;
    p.max_tx_ttl_blocks = 1'800;
    p.epoch_length_blocks = 43'200;
    p.max_attestation_validity_s = 604'800;
    p.attestation_clock_skew_s = 300;
    p.max_claims_per_subject_per_epoch = 20;
    return p;
}

}  // namespace yap::tx
