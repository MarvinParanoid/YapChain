// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <string>

#include "yap/state/ledger.hpp"

namespace yap::test {

inline const std::string kTestChain = "yapchain-testnet-1";

inline crypto::SecretKey key_of(Byte b) {
    crypto::Seed s{};
    s.fill(b);
    return crypto::SecretKey::from_seed(s);
}

inline crypto::AccountId id_of(const crypto::SecretKey& k) { return crypto::account_id_from_public_key(k.public_key()); }

/// Committed state with params, a consistent ChainState and the given balances.
inline void seed_state(state::MemoryKvStore& store, const std::map<crypto::AccountId, std::uint64_t>& balances,
                       const tx::ChainParams& params = tx::testnet_params()) {
    state::Overlay o(&store);
    state::State st(o);
    st.put_params(params);
    std::uint64_t total = 0;
    for (const auto& [id, bal] : balances) {
        st.put_account(id, {bal, 0});
        total += bal;
    }
    st.put_chain_state({0, 0, total, total, 0});
    o.apply_to(store);
}

inline Bytes transfer_tx(const crypto::SecretKey& from, const crypto::AccountId& to, std::uint64_t amount,
                         std::uint64_t nonce, std::uint64_t fee = 1000, std::uint64_t expires = 100) {
    tx::TxBody b;
    b.chain_id = kTestChain;
    b.sender_pubkey = from.public_key();
    b.nonce = nonce;
    b.fee = fee;
    b.expires_at_height = expires;
    b.payload = tx::Transfer{to, amount};
    return ycs::encode(tx::sign_tx(b, from));
}

inline Bytes burn_tx(const crypto::SecretKey& from, std::uint64_t amount, std::uint64_t nonce, std::uint64_t fee = 1000) {
    tx::TxBody b;
    b.chain_id = kTestChain;
    b.sender_pubkey = from.public_key();
    b.nonce = nonce;
    b.fee = fee;
    b.expires_at_height = 100;
    b.payload = tx::Burn{amount};
    return ycs::encode(tx::sign_tx(b, from));
}

/// Full dump of a view, for byte-exact comparisons.
inline std::map<Bytes, Bytes> dump(const state::KvReader& kv) {
    std::map<Bytes, Bytes> out;
    kv.for_each([&out](const Bytes& k, const Bytes& v) { out.emplace(k, v); });
    return out;
}

}  // namespace yap::test
