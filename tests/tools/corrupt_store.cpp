// SPDX-License-Identifier: Apache-2.0
//
// Test-only tool that damages a local database on purpose, so CLI tests can check that `yap`
// detects it. Not installed, not part of the product.
//   yap_test_corrupt <data_dir> balance <account_id_hex>   add 1 yip to an account behind the engine
//   yap_test_corrupt <data_dir> schema                      set the schema version to 2
//   yap_test_corrupt <data_dir> smt-node                    flip a byte of one stored SMT node
#include <algorithm>
#include <cstdio>
#include <string>

#include "yap/chain/store_layout.hpp"
#include "yap/common/hex.hpp"
#include "yap/state/types.hpp"
#include "yap/storage/lmdb_store.hpp"

using namespace yap;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <data_dir> balance <account_id_hex> | schema | smt-node\n", argv[0]);
        return 1;
    }
    storage::LmdbStore::Options o{argv[1]};
    o.create = std::string(argv[2]) == "empty-db";  // empty-db: create the tables and nothing else
    storage::LmdbStore store(o);
    const std::string mode = argv[2];
    storage::WriteBatch b;
    if (mode == "balance" && argc == 4) {
        const auto id = from_hex_fixed<32>(argv[3]);
        if (!id) return 1;
        const auto key = state::key::account(*id);
        auto acct = ycs::decode<state::AccountState>(store.get(storage::Table::State, key).value()).value();
        acct.balance += 1;
        b.put(storage::Table::State, key, ycs::encode(acct));
    } else if (mode == "schema") {
        auto raw = store.get(storage::Table::Meta, chain::meta_key()).value();
        raw[0] = 2;
        b.put(storage::Table::Meta, chain::meta_key(), raw);
    } else if (mode == "empty-db") {
        return 0;  // opening with create=false already required the tables; nothing to write
    } else if (mode == "smt-nonroot") {
        const auto meta = ycs::decode<chain::StoreMeta>(store.get(storage::Table::Meta, chain::meta_key()).value()).value();
        store.for_each(storage::Table::Smt, [&](const Bytes& k, const Bytes& v) {
            if (Hash32 h{}; std::equal(k.begin(), k.end(), meta.last_state_root.begin()) && k.size() == h.size()) return;
            Bytes damaged = v;
            damaged[40] ^= 1;
            b.put(storage::Table::Smt, k, damaged);
        });
    } else if (mode == "smt-node") {
        Bytes k, v;
        store.for_each(storage::Table::Smt, [&](const Bytes& kk, const Bytes& vv) {
            if (k.empty()) {
                k = kk;
                v = vv;
            }
        });
        v[10] ^= 1;
        b.put(storage::Table::Smt, k, v);
    } else {
        return 1;
    }
    store.write(b);
    return 0;
}
