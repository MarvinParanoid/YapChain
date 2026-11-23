// SPDX-License-Identifier: Apache-2.0
//
// Deterministic transaction execution (docs/protocol.md §9.2 step 3, §9.3, §7.5).
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "yap/state/state.hpp"
#include "yap/tx/codes.hpp"

namespace yap::state {

/// Non-consensus annotation of a result (protocol.md §9.4): only `code` is consensus-critical.
struct Event {
    std::string type;
    std::vector<std::pair<std::string, std::string>> attributes;
    bool operator==(const Event&) const = default;
};

struct TxResult {
    tx::TxCode code = tx::TxCode::ErrDecode;
    std::string log;
    std::vector<Event> events;
};

/// Validates and applies one transaction on top of `block_state` at `height`.
/// The transaction runs in its own overlay: on any error nothing is written (no fee, no nonce).
TxResult execute_tx(Overlay& block_state, ByteView tx_bytes, std::uint64_t height, std::string_view chain_id);

/// Fee settlement (§9.3): floor(fee * fee_burn_bps / 10000) is burned, the rest goes to Treasury.
/// Returns false on arithmetic failure (the caller rolls the transaction back).
[[nodiscard]] bool settle_fee(State& state, std::uint64_t fee, std::vector<Event>& events);

}  // namespace yap::state
