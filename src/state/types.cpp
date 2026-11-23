// SPDX-License-Identifier: Apache-2.0
#include "yap/state/types.hpp"

#include "yap/crypto/hash.hpp"

namespace yap::state {

void ChainState::encode_to(ycs::Writer& w) const {
    w.u64(height);
    w.u64(last_time_ms);
    w.u64(total_supply);
    w.u64(total_minted);
    w.u64(total_burned);
}

ChainState ChainState::decode_from(ycs::Reader& r) {
    ChainState s;
    s.height = r.u64();
    s.last_time_ms = r.u64();
    s.total_supply = r.u64();
    s.total_minted = r.u64();
    s.total_burned = r.u64();
    return s;
}

void ValidatorInfo::encode_to(ycs::Writer& w) const {
    w.string(name);
    w.fixed(consensus_pubkey);
    w.u64(power);
    w.fixed(reward_account);
}

ValidatorInfo ValidatorInfo::decode_from(ycs::Reader& r) {
    ValidatorInfo v;
    v.name = r.ascii_printable(kMaxNameBytes);
    v.consensus_pubkey = r.fixed<32>();
    v.power = r.u64();
    v.reward_account = r.fixed<32>();
    return v;
}

ConsensusAddress consensus_address(const crypto::PublicKey& pubkey) {
    const Hash32 h = crypto::sha256(pubkey);
    ConsensusAddress a{};
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = h[i];
    return a;
}

namespace key {
namespace {
Bytes with_ns(Byte ns, ByteView id) {
    Bytes k;
    k.reserve(1 + id.size());
    k.push_back(ns);
    k.insert(k.end(), id.begin(), id.end());
    return k;
}
}  // namespace

Bytes chain_state() { return {ns::kChainState}; }
Bytes params() { return {ns::kParams}; }
Bytes account(const crypto::AccountId& id) { return with_ns(ns::kAccount, id); }
Bytes validator(const ConsensusAddress& address) { return with_ns(ns::kValidator, address); }

Bytes issuer(std::uint32_t issuer_id) {
    ycs::Writer w;
    w.u8(ns::kIssuer);
    w.u32(issuer_id);
    return std::move(w).take();
}

Bytes achievement(std::uint32_t issuer_id, std::uint32_t achievement_id) {
    ycs::Writer w;
    w.u8(ns::kAchievement);
    w.u32(issuer_id);
    w.u32(achievement_id);
    return std::move(w).take();
}
}  // namespace key

}  // namespace yap::state
