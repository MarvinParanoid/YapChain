// SPDX-License-Identifier: Apache-2.0
#include "yap/state/types.hpp"

#include <algorithm>

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

void Issuer::encode_to(ycs::Writer& w) const {
    w.u32(issuer_id);
    w.string(name);
    w.u8(status);
    w.u64(epoch_budget);
    w.length(keys.size());
    for (const auto& k : keys) {
        w.u8(k.key_id);
        w.fixed(k.pubkey);
        w.u8(k.status);
    }
}

Issuer Issuer::decode_from(ycs::Reader& r) {
    Issuer i;
    i.issuer_id = r.u32();
    i.name = r.ascii_printable(kMaxNameBytes);
    i.status = r.u8();
    i.epoch_budget = r.u64();
    const std::uint32_t n = r.vec_length(kMaxIssuerKeys, 1 + 32 + 1);
    for (std::uint32_t k = 0; k < n && r.ok(); ++k) {
        IssuerKey key;
        key.key_id = r.u8();
        key.pubkey = r.fixed<32>();
        key.status = r.u8();
        i.keys.push_back(key);
    }
    return i;
}

void Achievement::encode_to(ycs::Writer& w) const {
    w.u32(issuer_id);
    w.u32(achievement_id);
    w.u64(reward);
    w.u8(policy);
    w.u16(max_per_epoch);
    w.u8(status);
}

Achievement Achievement::decode_from(ycs::Reader& r) {
    Achievement a;
    a.issuer_id = r.u32();
    a.achievement_id = r.u32();
    a.reward = r.u64();
    a.policy = r.u8();
    a.max_per_epoch = r.u16();
    a.status = r.u8();
    return a;
}

ConsensusAddress consensus_address(const crypto::PublicKey& pubkey) {
    const Hash32 h = crypto::sha256(pubkey);
    ConsensusAddress a{};
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = h[i];
    return a;
}

namespace key {
namespace {
// Sized once, then filled: the reserve + push_back form made GCC 14 at -O3 report a false
// -Wfree-nonheap-object on the (unreachable) reallocation path.
Bytes with_ns(Byte ns, ByteView id) {
    Bytes k(1 + id.size());
    k[0] = ns;
    std::copy(id.begin(), id.end(), k.begin() + 1);
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
