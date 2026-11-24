// SPDX-License-Identifier: Apache-2.0
#include "yap/abci/convert.hpp"

#include <limits>

#include "yap/common/fail_stop.hpp"

namespace yap::abci {

namespace {
constexpr std::int64_t kMaxSeconds = 9'223'372'036'854'775;

std::uint64_t to_power(std::int64_t p) {
    if (p < 0) throw FailStop("negative voting power " + std::to_string(p));
    return static_cast<std::uint64_t>(p);
}

std::uint8_t to_flag(int flag) {
    // The raw value is kept; validate_last_commit() accepts only 1..3. Anything that does not even
    // fit a byte cannot be represented and is rejected here.
    if (flag < 0 || flag > std::numeric_limits<std::uint8_t>::max()) {
        throw FailStop("block_id_flag out of range: " + std::to_string(flag));
    }
    return static_cast<std::uint8_t>(flag);
}

template <class Votes>
std::vector<state::CommitVote> convert_votes(const Votes& votes) {
    std::vector<state::CommitVote> out;
    out.reserve(static_cast<std::size_t>(votes.size()));
    for (const auto& v : votes) {
        out.push_back({to_address(v.validator().address(), "vote validator address"), to_power(v.validator().power()),
                       to_flag(static_cast<int>(v.block_id_flag()))});
    }
    return out;
}

template <class Req>
state::BlockContext common_context(const std::string& chain_id, const Req& r) {
    state::BlockContext ctx;
    ctx.chain_id = chain_id;
    ctx.height = to_block_height(r.height());
    ctx.time_ms = to_time_ms(r.time());
    ctx.proposer_address = to_address(r.proposer_address(), "proposer_address");
    ctx.misbehavior = to_misbehavior(r.misbehavior());
    ctx.txs.reserve(static_cast<std::size_t>(r.txs().size()));
    for (const auto& tx : r.txs()) ctx.txs.emplace_back(tx.begin(), tx.end());
    return ctx;
}
}  // namespace

std::uint64_t to_time_ms(const google::protobuf::Timestamp& t) {
    if (t.seconds() < 0 || t.seconds() > kMaxSeconds) throw FailStop("block time seconds out of range");
    if (t.nanos() < 0 || t.nanos() > 999'999'999) throw FailStop("block time nanos out of range");
    return static_cast<std::uint64_t>(t.seconds()) * 1000 + static_cast<std::uint64_t>(t.nanos()) / 1'000'000;
}

std::uint64_t to_block_height(std::int64_t h) {
    if (h < 1) throw FailStop("block height must be >= 1, got " + std::to_string(h));
    return static_cast<std::uint64_t>(h);
}

state::ConsensusAddress to_address(const std::string& bytes, std::string_view what) {
    state::ConsensusAddress a{};
    if (bytes.size() != a.size()) {
        throw FailStop(std::string(what) + ": expected 20 bytes, got " + std::to_string(bytes.size()));
    }
    for (std::size_t i = 0; i < a.size(); ++i) a[i] = static_cast<Byte>(bytes[i]);
    return a;
}

std::vector<state::CommitVote> to_last_commit(const tendermint::abci::CommitInfo& c) { return convert_votes(c.votes()); }

std::vector<state::CommitVote> to_last_commit(const tendermint::abci::ExtendedCommitInfo& c) {
    return convert_votes(c.votes());
}

std::vector<state::Misbehavior> to_misbehavior(
    const google::protobuf::RepeatedPtrField<tendermint::abci::Misbehavior>& m) {
    std::vector<state::Misbehavior> out;
    out.reserve(static_cast<std::size_t>(m.size()));
    for (const auto& e : m) {
        const int type = static_cast<int>(e.type());
        if (type < 0 || type > std::numeric_limits<std::uint8_t>::max()) throw FailStop("misbehavior type out of range");
        if (e.height() < 0) throw FailStop("misbehavior height is negative");
        out.push_back({static_cast<std::uint8_t>(type), to_address(e.validator().address(), "misbehavior validator"),
                       static_cast<std::uint64_t>(e.height())});
    }
    return out;
}

state::BlockContext block_context(const std::string& chain_id, const tendermint::abci::RequestPrepareProposal& r) {
    auto ctx = common_context(chain_id, r);
    ctx.last_commit = to_last_commit(r.local_last_commit());
    return ctx;
}

state::BlockContext block_context(const std::string& chain_id, const tendermint::abci::RequestProcessProposal& r) {
    auto ctx = common_context(chain_id, r);
    ctx.last_commit = to_last_commit(r.proposed_last_commit());
    return ctx;
}

state::BlockContext block_context(const std::string& chain_id, const tendermint::abci::RequestFinalizeBlock& r) {
    auto ctx = common_context(chain_id, r);
    ctx.last_commit = to_last_commit(r.decided_last_commit());
    return ctx;
}

}  // namespace yap::abci
