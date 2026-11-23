// SPDX-License-Identifier: Apache-2.0
//
// Block execution (docs/protocol.md §9.1, §9.2).
#pragma once

#include <string>
#include <vector>

#include "yap/state/economics.hpp"
#include "yap/state/ledger.hpp"

namespace yap::state {

/// Evidence reported by consensus (non-consensus-critical here: it only produces an event).
struct Misbehavior {
    std::uint8_t type = 0;
    ConsensusAddress validator_address{};
    std::uint64_t height = 0;
    bool operator==(const Misbehavior&) const = default;
};

struct BlockContext {
    std::string chain_id;
    std::uint64_t height = 0;
    std::uint64_t time_ms = 0;
    ConsensusAddress proposer_address{};
    std::vector<CommitVote> last_commit;
    std::vector<Misbehavior> misbehavior;
    std::vector<Bytes> txs;
};

struct BlockResult {
    std::vector<TxResult> tx_results;  ///< exactly one per transaction, in block order
    std::vector<Event> events;         ///< block-level events (evidence, rewards)
    Hash32 state_root{};
};

/// Executes §9.2 steps 1-6 into `block_state` (an overlay over the state of height H-1).
/// `chain_id` is the network's chain id from genesis. Context violations and arithmetic
/// failures outside transactions throw FailStop; transaction errors only produce result codes.
BlockResult execute_block(Overlay& block_state, std::string_view chain_id, const BlockContext& ctx);

}  // namespace yap::state
