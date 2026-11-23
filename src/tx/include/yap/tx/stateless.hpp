// SPDX-License-Identifier: Apache-2.0
//
// Stateless validation S1..S11 (docs/protocol.md §6.2). Rules are evaluated strictly in
// order and the code of the first violated rule is returned (ADR-0019); the signature
// check is last.
#pragma once

#include <optional>
#include <string_view>

#include "yap/tx/codes.hpp"
#include "yap/tx/params.hpp"
#include "yap/tx/transaction.hpp"

namespace yap::tx {

struct StatelessResult {
    TxCode code = TxCode::ErrDecode;
    /// Present whenever decoding succeeded (S2 passed), even if a later rule failed.
    std::optional<SignedTx> tx;
};

[[nodiscard]] StatelessResult check_stateless(ByteView tx_bytes, std::string_view chain_id,
                                              const ChainParams& params);

/// S10: attestations.md §4.1 rules for a decoded ClaimAchievement.
[[nodiscard]] bool attestation_stateless_ok(const TxBody& body, const SignedAttestation& claim,
                                            const ChainParams& params);

}  // namespace yap::tx
