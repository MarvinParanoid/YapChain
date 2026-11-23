// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

#include "yap/codec/ycs.hpp"

namespace yap::state {

/// docs/protocol.md §7.2.
struct AccountState {
    std::uint64_t balance = 0;
    std::uint64_t nonce = 0;

    bool operator==(const AccountState&) const = default;
    void encode_to(ycs::Writer& w) const {
        w.u64(balance);
        w.u64(nonce);
    }
    static AccountState decode_from(ycs::Reader& r) {
        AccountState a;
        a.balance = r.u64();
        a.nonce = r.u64();
        return a;
    }
};

}  // namespace yap::state
