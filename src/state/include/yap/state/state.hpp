// SPDX-License-Identifier: Apache-2.0
//
// Typed access to the state stored in an Overlay. Stored values that fail to decode are
// treated as corruption (FailStop), never as transaction errors.
#pragma once

#include <optional>

#include "yap/common/fail_stop.hpp"
#include "yap/state/kv.hpp"
#include "yap/state/types.hpp"
#include "yap/state/validation.hpp"

namespace yap::state {

template <class T>
[[nodiscard]] std::optional<T> read_value(const KvReader& kv, ByteView key) {
    auto raw = kv.get(key);
    if (!raw) return std::nullopt;
    auto decoded = ycs::decode<T>(*raw);
    if (!decoded) {
        throw FailStop("corrupted state value: " + std::string(ycs::to_string(decoded.error())));
    }
    return std::move(decoded).value();
}

class State final : public StateReader {
public:
    explicit State(Overlay& kv) : kv_(&kv) {}

    [[nodiscard]] std::optional<AccountState> account(const crypto::AccountId& id) const override;
    [[nodiscard]] tx::ChainParams params() const override;
    [[nodiscard]] ChainState chain_state() const;
    [[nodiscard]] std::optional<ValidatorInfo> validator(const ConsensusAddress& address) const;

    void put_account(const crypto::AccountId& id, const AccountState& account);
    void put_chain_state(const ChainState& cs);
    void put_params(const tx::ChainParams& params);
    void put_validator(const ConsensusAddress& address, const ValidatorInfo& info);

    /// Adds `amount` to the balance. Zero is a no-op that never creates an account
    /// (protocol.md §9.3, ADR-0021). Returns false on overflow (nothing written).
    [[nodiscard]] bool credit(const crypto::AccountId& id, std::uint64_t amount);
    /// Subtracts `amount`. Returns false if the balance is insufficient (nothing written).
    [[nodiscard]] bool debit(const crypto::AccountId& id, std::uint64_t amount);

    [[nodiscard]] Overlay& kv() noexcept { return *kv_; }

private:
    Overlay* kv_;
};

/// docs/protocol.md §7.3. Throws FailStop on any violation.
void check_invariants(const KvReader& kv);

}  // namespace yap::state
