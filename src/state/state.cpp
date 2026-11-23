// SPDX-License-Identifier: Apache-2.0
#include "yap/state/state.hpp"

#include "yap/common/checked.hpp"

namespace yap::state {

std::optional<AccountState> State::account(const crypto::AccountId& id) const {
    return read_value<AccountState>(*kv_, key::account(id));
}

tx::ChainParams State::params() const {
    auto p = read_value<tx::ChainParams>(*kv_, key::params());
    if (!p) throw FailStop("state has no ChainParams");
    return *p;
}

ChainState State::chain_state() const {
    auto cs = read_value<ChainState>(*kv_, key::chain_state());
    if (!cs) throw FailStop("state has no ChainState");
    return *cs;
}

std::optional<ValidatorInfo> State::validator(const ConsensusAddress& address) const {
    return read_value<ValidatorInfo>(*kv_, key::validator(address));
}

void State::put_account(const crypto::AccountId& id, const AccountState& account) {
    kv_->put(key::account(id), ycs::encode(account));
}
void State::put_chain_state(const ChainState& cs) { kv_->put(key::chain_state(), ycs::encode(cs)); }
void State::put_params(const tx::ChainParams& params) { kv_->put(key::params(), ycs::encode(params)); }
void State::put_validator(const ConsensusAddress& address, const ValidatorInfo& info) {
    kv_->put(key::validator(address), ycs::encode(info));
}

bool State::credit(const crypto::AccountId& id, std::uint64_t amount) {
    if (amount == 0) return true;  // ADR-0021
    AccountState a = account(id).value_or(AccountState{});
    const auto b = checked_add(a.balance, amount);
    if (!b) return false;
    a.balance = *b;
    put_account(id, a);
    return true;
}

bool State::debit(const crypto::AccountId& id, std::uint64_t amount) {
    if (amount == 0) return true;
    auto a = account(id);
    if (!a) return false;
    const auto b = checked_sub(a->balance, amount);
    if (!b) return false;
    a->balance = *b;
    put_account(id, *a);
    return true;
}

void check_invariants(const KvReader& kv) {
    const auto cs = read_value<ChainState>(kv, key::chain_state());
    const auto params = read_value<tx::ChainParams>(kv, key::params());
    if (!cs || !params) throw FailStop("invariant 4: ChainState and ChainParams must exist");

    std::uint64_t sum = 0;
    bool overflow = false;
    kv.for_each([&](const Bytes& k, const Bytes& v) {
        // Invariant 4 in full: exactly one key in the 0x00 and 0x02 namespaces, no unknown namespaces.
        if (k.empty() || k[0] > ns::kNullifier) throw FailStop("invariant 4: key outside the known namespaces");
        if ((k[0] == ns::kChainState || k[0] == ns::kParams) && k.size() != 1) {
            throw FailStop("invariant 4: extra key in a singleton namespace");
        }
        if (k[0] != ns::kAccount) return;
        auto a = ycs::decode<AccountState>(v);
        if (!a) throw FailStop("corrupted account value");
        const auto s = checked_add(sum, a->balance);
        if (!s) {
            overflow = true;
            return;
        }
        sum = *s;
    });
    if (overflow) throw FailStop("invariant 1: sum of balances overflows u64");
    if (sum != cs->total_supply) throw FailStop("invariant 1: sum of balances != total_supply");
    const auto diff = checked_sub(cs->total_minted, cs->total_burned);
    if (!diff || *diff != cs->total_supply) throw FailStop("invariant 2: total_supply != minted - burned");
    if (cs->total_minted > params->max_issuance) throw FailStop("invariant 3: total_minted > max_issuance");
}

}  // namespace yap::state
