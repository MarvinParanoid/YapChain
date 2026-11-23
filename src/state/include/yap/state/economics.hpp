// SPDX-License-Identifier: Apache-2.0
//
// Block-level economics (docs/protocol.md §9.1, §9.5, §11.4; economics.md §4).
// Any failure here is outside a transaction and therefore FailStop (§1.1).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "yap/state/ledger.hpp"

namespace yap::state {

/// CometBFT BlockIDFlag values (protocol.md §9.1).
enum class BlockIdFlag : std::uint8_t { Absent = 1, Commit = 2, Nil = 3 };

struct CommitVote {
    ConsensusAddress validator_address{};
    std::uint64_t power = 0;
    std::uint8_t flag = 0;  ///< raw value; validated by validate_last_commit()
    bool operator==(const CommitVote&) const = default;
};

/// §9.1 checks: empty at height 1; flag in {1,2,3}; every address is a known validator,
/// appears once and carries the stored power. Throws FailStop otherwise.
void validate_last_commit(const State& state, std::uint64_t height, std::span<const CommitVote> last_commit);

/// §9.5 emission and distribution. `last_commit` must already be validated.
/// Returns the minted amount (0 once max_issuance is reached; then the state is untouched).
std::uint64_t apply_block_reward(State& state, std::span<const CommitVote> last_commit, std::vector<Event>& events);

/// Initial supply accounting from genesis balances (§11.4 item 3): minted = supply = Σ balance.
/// Returns nullopt if the sum overflows or exceeds max_issuance (genesis invalid, §11.2 item 3).
[[nodiscard]] std::optional<ChainState> genesis_chain_state(std::span<const std::uint64_t> balances,
                                                            std::uint64_t genesis_time_ms,
                                                            std::uint64_t max_issuance);

}  // namespace yap::state
