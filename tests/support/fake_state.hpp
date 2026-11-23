// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>

#include "yap/state/validation.hpp"

namespace yap::test {

/// Minimal in-memory StateReader for validation tests.
class FakeState final : public state::StateReader {
public:
    tx::ChainParams p = tx::testnet_params();
    std::map<crypto::AccountId, state::AccountState> accounts;

    [[nodiscard]] std::optional<state::AccountState> account(const crypto::AccountId& id) const override {
        auto it = accounts.find(id);
        if (it == accounts.end()) return std::nullopt;
        return it->second;
    }
    [[nodiscard]] tx::ChainParams params() const override { return p; }
};

}  // namespace yap::test
