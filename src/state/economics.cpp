// SPDX-License-Identifier: Apache-2.0
#include "yap/state/economics.hpp"

#include <algorithm>
#include <set>

#include "yap/common/checked.hpp"
#include "yap/common/hex.hpp"

namespace yap::state {
namespace {

std::uint64_t must(std::optional<std::uint64_t> v, const char* what) {
    if (!v) throw FailStop(std::string("emission arithmetic failure: ") + what);
    return *v;
}

void must_credit(State& st, const crypto::AccountId& id, std::uint64_t amount) {
    if (!st.credit(id, amount)) throw FailStop("emission credit overflow");
}

}  // namespace

void validate_last_commit(const State& st, std::uint64_t height, std::span<const CommitVote> last_commit) {
    if (height == 1 && !last_commit.empty()) {
        throw FailStop("last_commit must be empty at height 1");
    }
    std::set<ConsensusAddress> seen;
    for (const auto& vote : last_commit) {
        if (vote.flag < static_cast<std::uint8_t>(BlockIdFlag::Absent) ||
            vote.flag > static_cast<std::uint8_t>(BlockIdFlag::Nil)) {
            throw FailStop("last_commit: invalid BlockIDFlag");
        }
        if (!seen.insert(vote.validator_address).second) {
            throw FailStop("last_commit: duplicate validator address");
        }
        const auto info = st.validator(vote.validator_address);
        if (!info) throw FailStop("last_commit: unknown validator " + to_hex(vote.validator_address));
        if (info->power != vote.power) throw FailStop("last_commit: power mismatch with ValidatorInfo");
    }
}

std::uint64_t apply_block_reward(State& st, std::span<const CommitVote> last_commit, std::vector<Event>& events) {
    const tx::ChainParams p = st.params();
    ChainState cs = st.chain_state();

    const std::uint64_t headroom = must(checked_sub(p.max_issuance, cs.total_minted), "total_minted > max_issuance");
    const std::uint64_t reward = std::min(p.block_reward, headroom);
    if (reward == 0) return 0;  // §9.5: the state is not modified

    const std::uint64_t v = must(mul_div_floor(reward, p.reward_validators_bps, tx::kBpsDenominator), "v");
    const std::uint64_t pool = must(mul_div_floor(reward, p.reward_pool_bps, tx::kBpsDenominator), "p");
    std::uint64_t t = must(checked_sub(must(checked_sub(reward, v), "reward - v"), pool), "reward - v - p");

    // S: signers (flag = Commit) in ascending address order.
    std::vector<const CommitVote*> signers;
    for (const auto& vote : last_commit) {
        if (vote.flag == static_cast<std::uint8_t>(BlockIdFlag::Commit)) signers.push_back(&vote);
    }
    std::sort(signers.begin(), signers.end(),
              [](const CommitVote* a, const CommitVote* b) { return a->validator_address < b->validator_address; });

    if (signers.empty()) {
        t = must(checked_add(t, v), "t + v");
    } else {
        std::uint64_t w = 0;
        std::vector<ValidatorInfo> infos;
        infos.reserve(signers.size());
        for (const auto* s : signers) {
            auto info = st.validator(s->validator_address);
            if (!info) throw FailStop("emission: signer is not a validator");
            w = must(checked_add(w, info->power), "W");
            infos.push_back(std::move(*info));
        }
        std::uint64_t distributed = 0;
        for (std::size_t i = 0; i < signers.size(); ++i) {
            const std::uint64_t share = must(mul_div_floor(v, infos[i].power, w), "share");
            must_credit(st, infos[i].reward_account, share);
            distributed = must(checked_add(distributed, share), "sum of shares");
            events.push_back({"validator_reward",
                              {{"validator", to_hex(signers[i]->validator_address)},
                               {"account", to_hex(infos[i].reward_account)},
                               {"amount", std::to_string(share)}}});
        }
        t = must(checked_add(t, must(checked_sub(v, distributed), "v - shares")), "t + remainder");
    }

    must_credit(st, crypto::system_account_id(crypto::kRewardsPoolAccountName), pool);
    must_credit(st, crypto::system_account_id(crypto::kTreasuryAccountName), t);

    cs = st.chain_state();  // reread: credits do not touch ChainState, but stay explicit
    cs.total_minted = must(checked_add(cs.total_minted, reward), "total_minted");
    cs.total_supply = must(checked_add(cs.total_supply, reward), "total_supply");
    st.put_chain_state(cs);
    events.push_back({"block_reward", {{"minted", std::to_string(reward)},
                                       {"validators", std::to_string(v)},
                                       {"rewards_pool", std::to_string(pool)},
                                       {"treasury", std::to_string(t)}}});
    return reward;
}

std::optional<ChainState> genesis_chain_state(std::span<const std::uint64_t> balances, std::uint64_t genesis_time_ms,
                                              std::uint64_t max_issuance) {
    std::uint64_t sum = 0;
    for (const auto b : balances) {
        const auto s = checked_add(sum, b);
        if (!s) return std::nullopt;
        sum = *s;
    }
    if (sum > max_issuance) return std::nullopt;
    return ChainState{0, genesis_time_ms, sum, sum, 0};
}

}  // namespace yap::state
