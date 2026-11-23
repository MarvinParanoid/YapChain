// SPDX-License-Identifier: Apache-2.0
#include "yap/state/state_root.hpp"

#include "yap/merkle/merkle.hpp"

namespace yap::state {

Hash32 state_root(const KvReader& kv) {
    std::vector<merkle::SmtLeaf> leaves;
    kv.for_each([&leaves](const Bytes& k, const Bytes& v) {
        leaves.push_back({merkle::smt_key(k), merkle::smt_value_hash(v)});
    });
    return merkle::smt_root(std::move(leaves));
}

}  // namespace yap::state
