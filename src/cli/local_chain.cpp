// SPDX-License-Identifier: Apache-2.0
#include "local_chain.hpp"

#include "yap/state/state.hpp"

namespace yap::cli {

state::ChainState LocalChain::chain_state() const {
    state::Overlay view(&engine->committed());
    return state::State(view).chain_state();
}

tx::ChainParams LocalChain::params() const {
    state::Overlay view(&engine->committed());
    return state::State(view).params();
}

std::optional<state::AccountState> LocalChain::account(const crypto::AccountId& id) const {
    state::Overlay view(&engine->committed());
    return state::State(view).account(id);
}

std::optional<LocalChain> open_local_chain(const Context& ctx, bool read_only, bool required) {
    const auto dir = ctx.data_dir();
    if (!std::filesystem::exists(dir / "data.mdb")) {
        if (required) throw CliError(kExitRejected, "no local chain at " + dir.string() + " (run `yap dev init`)");
        return std::nullopt;
    }
    storage::LmdbStore::Options o{dir};
    o.read_only = read_only;
    o.create = false;
    LocalChain lc;
    lc.store = std::make_unique<storage::LmdbStore>(o);
    lc.engine = std::make_unique<chain::Engine>(*lc.store);
    if (!lc.engine->initialized()) {
        if (required) throw CliError(kExitRejected, "local chain at " + dir.string() + " is not initialised");
        return std::nullopt;
    }
    return lc;
}

}  // namespace yap::cli
