// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>

namespace yap {

/// Raised for conditions where the node must stop instead of risking state divergence
/// (docs/protocol.md §1.1): arithmetic failure outside a transaction, invariant violation,
/// corrupted stored state, inconsistent consensus input. Never raised for bad transaction data.
class FailStop : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace yap
