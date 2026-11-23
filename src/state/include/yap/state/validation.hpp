// SPDX-License-Identifier: Apache-2.0
//
// Stateful validation T1..T7 (docs/protocol.md §6.3) through a read-only state interface,
// and the combined stateless + stateful pipeline.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "yap/crypto/address.hpp"
#include "yap/state/account.hpp"
#include "yap/tx/codes.hpp"
#include "yap/tx/params.hpp"
#include "yap/tx/transaction.hpp"

namespace yap::state {

/// Read-only view of the state needed to validate a transaction.
class StateReader {
public:
    virtual ~StateReader() = default;
    /// nullopt if the account does not exist.
    [[nodiscard]] virtual std::optional<AccountState> account(const crypto::AccountId& id) const = 0;
    [[nodiscard]] virtual tx::ChainParams params() const = 0;
};

/// T1..T7 for a transaction that already passed S1..S11, at block height `height`.
/// ClaimAchievement returns ERR_UNSUPPORTED at T7 until M3 (protocol.md §15).
[[nodiscard]] tx::TxCode check_stateful(const tx::SignedTx& tx, std::uint64_t height, const StateReader& state);

struct ValidationResult {
    tx::TxCode code = tx::TxCode::ErrDecode;
    std::optional<tx::SignedTx> tx;  ///< present if decoding succeeded
};

/// Full pipeline: S1..S11 then T1..T7; the first violated rule wins.
[[nodiscard]] ValidationResult validate_tx(ByteView tx_bytes, std::uint64_t height, std::string_view chain_id,
                                           const StateReader& state);

}  // namespace yap::state
