// SPDX-License-Identifier: Apache-2.0
//
// Conversions from CometBFT ABCI++ messages to the core's block context (protocol.md §9.1).
// Every malformed consensus input throws FailStop: CometBFT has already validated the block, so a
// value we cannot represent means CometBFT and the application disagree — never something to skip.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tendermint/abci/types.pb.h"
#include "yap/state/block.hpp"

namespace yap::abci {

/// Application protocol version reported in Info and required in genesis consensus_params.version.app.
inline constexpr std::uint64_t kAppVersion = 1;

/// §9.1: 0 <= seconds <= 9 223 372 036 854 775, 0 <= nanos <= 999 999 999;
/// time_ms = seconds * 1000 + floor(nanos / 1e6).
[[nodiscard]] std::uint64_t to_time_ms(const google::protobuf::Timestamp& t);

/// A block height (>= 1).
[[nodiscard]] std::uint64_t to_block_height(std::int64_t h);

/// A 20-byte consensus address (SHA-256(pubkey)[0..20]).
[[nodiscard]] state::ConsensusAddress to_address(const std::string& bytes, std::string_view what);

/// Votes for H-1, in CometBFT's order (validation and sorting are the core's job, §9.1/§9.5).
[[nodiscard]] std::vector<state::CommitVote> to_last_commit(const tendermint::abci::CommitInfo& c);
[[nodiscard]] std::vector<state::CommitVote> to_last_commit(const tendermint::abci::ExtendedCommitInfo& c);

[[nodiscard]] std::vector<state::Misbehavior> to_misbehavior(
    const google::protobuf::RepeatedPtrField<tendermint::abci::Misbehavior>& m);

/// BlockContext of a PrepareProposal / ProcessProposal / FinalizeBlock request.
[[nodiscard]] state::BlockContext block_context(const std::string& chain_id,
                                                const tendermint::abci::RequestPrepareProposal& r);
[[nodiscard]] state::BlockContext block_context(const std::string& chain_id,
                                                const tendermint::abci::RequestProcessProposal& r);
[[nodiscard]] state::BlockContext block_context(const std::string& chain_id,
                                                const tendermint::abci::RequestFinalizeBlock& r);

}  // namespace yap::abci
