// SPDX-License-Identifier: Apache-2.0
#include "yap/abci/application.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

#include "yap/abci/convert.hpp"
#include "yap/common/fail_stop.hpp"
#include "yap/common/hex.hpp"
#include "yap/crypto/address.hpp"
#include "yap/crypto/hash.hpp"
#include "yap/dev/dev_chain.hpp"
#include "yap/state/state.hpp"

namespace yap::abci {

namespace pb = tendermint::abci;
using Json = nlohmann::ordered_json;

namespace {
constexpr const char* kQueryCodespace = "yap.query";

std::string to_string_bytes(const Hash32& h) { return std::string(h.begin(), h.end()); }

void set_events(google::protobuf::RepeatedPtrField<pb::Event>& out, const std::vector<state::Event>& events) {
    for (const auto& e : events) {
        auto* pe = out.Add();
        pe->set_type(e.type);
        for (const auto& [k, v] : e.attributes) {
            auto* a = pe->add_attributes();
            a->set_key(k);
            a->set_value(v);
            a->set_index(true);
        }
    }
}

void query_error(pb::ResponseQuery& r, QueryCode code, const std::string& log) {
    r.set_code(std::to_underlying(code));
    r.set_codespace(kQueryCodespace);
    r.set_log(log);
}

std::string u64s(std::uint64_t v) { return std::to_string(v); }  // u64 as decimal strings, like genesis JSON

std::string hex_bytes(const std::string& b) { return to_hex(ByteView(reinterpret_cast<const Byte*>(b.data()), b.size())); }

// Public consensus data only: validator addresses, powers and BlockIDFlags (1 absent, 2 commit, 3 nil).
template <class Commit>
std::string votes_summary(const Commit& c) {
    std::string flags;
    std::string list;
    for (const auto& v : c.votes()) {
        flags += std::to_string(static_cast<int>(v.block_id_flag()));
        if (!list.empty()) list += ',';
        list += hex_bytes(v.validator().address()) + ":" + std::to_string(v.validator().power()) + ":" +
                std::to_string(static_cast<int>(v.block_id_flag()));
    }
    return "votes=" + std::to_string(c.votes_size()) + " flags=" + flags + " commit=[" + list + "]";
}

std::string tx_hash_hex(const std::string& tx) {  // CometBFT's transaction hash: SHA-256 of the bytes
    return to_hex(crypto::sha256(ByteView(reinterpret_cast<const Byte*>(tx.data()), tx.size())));
}

std::string trace_line(const pb::Request& r) {
    switch (r.value_case()) {
        case pb::Request::kInfo: return "Info";
        case pb::Request::kInitChain:
            return "InitChain chain_id=" + r.init_chain().chain_id() +
                   " initial_height=" + std::to_string(r.init_chain().initial_height()) +
                   " validators=" + std::to_string(r.init_chain().validators_size()) +
                   " app_version=" + std::to_string(r.init_chain().consensus_params().version().app()) +
                   " vote_ext_height=" + std::to_string(r.init_chain().consensus_params().abci().vote_extensions_enable_height());
        case pb::Request::kQuery: return "Query path=" + r.query().path();
        case pb::Request::kCheckTx:
            return std::string("CheckTx type=") + (r.check_tx().type() == pb::RECHECK ? "RECHECK" : "NEW") +
                   " bytes=" + std::to_string(r.check_tx().tx().size()) + " hash=" + tx_hash_hex(r.check_tx().tx());
        case pb::Request::kPrepareProposal:
            return "PrepareProposal height=" + std::to_string(r.prepare_proposal().height()) +
                   " proposer=" + hex_bytes(r.prepare_proposal().proposer_address()) +
                   " txs=" + std::to_string(r.prepare_proposal().txs_size()) +
                   " max_tx_bytes=" + std::to_string(r.prepare_proposal().max_tx_bytes()) + " local_" +
                   votes_summary(r.prepare_proposal().local_last_commit());
        case pb::Request::kProcessProposal:
            return "ProcessProposal height=" + std::to_string(r.process_proposal().height()) +
                   " proposer=" + hex_bytes(r.process_proposal().proposer_address()) +
                   " txs=" + std::to_string(r.process_proposal().txs_size()) + " " +
                   votes_summary(r.process_proposal().proposed_last_commit());
        case pb::Request::kFinalizeBlock:
            return "FinalizeBlock height=" + std::to_string(r.finalize_block().height()) +
                   " proposer=" + hex_bytes(r.finalize_block().proposer_address()) +
                   " time=" + std::to_string(r.finalize_block().time().seconds()) + "." +
                   std::to_string(r.finalize_block().time().nanos()) +
                   " txs=" + std::to_string(r.finalize_block().txs_size()) + " " +
                   votes_summary(r.finalize_block().decided_last_commit()) +
                   " misbehavior=" + std::to_string(r.finalize_block().misbehavior_size());
        case pb::Request::kCommit: return "Commit";
        default: return "request field " + std::to_string(static_cast<int>(r.value_case()));
    }
}
}  // namespace

void require_consensus_store(const storage::KvStore& store) {
    if (store.get(storage::Table::Meta, dev::dev_config_key())) {
        throw std::invalid_argument("the database belongs to a dev chain (yap dev); yapd never drives it");
    }
}

Application::Application(chain::Engine& engine, ApplicationOptions options)
    : engine_(engine), opts_(std::move(options)) {
    require_consensus_store(engine_.store());
}

std::optional<std::string> Application::failure() const {
    const std::lock_guard lock(mu_);
    return failure_;
}

void Application::log(const std::string& line) const {
    if (opts_.log) opts_.log(line);
}

void Application::handle(const pb::Request& request, pb::Response& response) {
    const std::lock_guard lock(mu_);
    if (failure_) throw std::runtime_error("application stopped after a fatal error: " + *failure_);
    if (opts_.trace) log("ABCI " + trace_line(request));
    try {
        dispatch(request, response);
        if (opts_.trace) trace_result(request, response);
    } catch (const std::exception& e) {
        failure_ = e.what();
        log("FATAL: " + *failure_);
        throw;
    }
}

void Application::trace_result(const pb::Request& req, const pb::Response& resp) const {
    switch (req.value_case()) {
        case pb::Request::kCheckTx:
            log("ABCI CheckTx result code=" + std::to_string(resp.check_tx().code()));
            return;
        case pb::Request::kProcessProposal:
            log(std::string("ABCI ProcessProposal result ") +
                (resp.process_proposal().status() == pb::ResponseProcessProposal::ACCEPT ? "ACCEPT" : "REJECT"));
            return;
        default:
            return;
    }
}

void Application::dispatch(const pb::Request& req, pb::Response& resp) {
    switch (req.value_case()) {
        case pb::Request::kInfo:
            info(*resp.mutable_info());
            return;
        case pb::Request::kInitChain:
            init_chain(req.init_chain(), *resp.mutable_init_chain());
            return;
        case pb::Request::kQuery:
            query(req.query(), *resp.mutable_query());
            return;
        case pb::Request::kCheckTx:
            check_tx(req.check_tx(), *resp.mutable_check_tx());
            return;
        case pb::Request::kPrepareProposal:
            prepare_proposal(req.prepare_proposal(), *resp.mutable_prepare_proposal());
            return;
        case pb::Request::kProcessProposal:
            process_proposal(req.process_proposal(), *resp.mutable_process_proposal());
            return;
        case pb::Request::kFinalizeBlock:
            finalize_block(req.finalize_block(), *resp.mutable_finalize_block());
            return;
        case pb::Request::kCommit:
            commit(*resp.mutable_commit());
            return;

        // State sync is not supported (ADR-0006, consensus.md §7): explicit refusals, no fake snapshots.
        case pb::Request::kListSnapshots:
            resp.mutable_list_snapshots();  // empty list
            return;
        case pb::Request::kOfferSnapshot:
            resp.mutable_offer_snapshot()->set_result(pb::ResponseOfferSnapshot::REJECT);
            return;
        case pb::Request::kLoadSnapshotChunk:
            resp.mutable_load_snapshot_chunk();  // empty chunk
            return;
        case pb::Request::kApplySnapshotChunk:
            resp.mutable_apply_snapshot_chunk()->set_result(pb::ResponseApplySnapshotChunk::ABORT);
            return;

        // Vote extensions are disabled (InitChain refuses vote_extensions_enable_height != 0). Should
        // CometBFT still ask, the only extension we produce or accept is the empty one.
        case pb::Request::kExtendVote:
            resp.mutable_extend_vote();  // empty extension
            return;
        case pb::Request::kVerifyVoteExtension:
            resp.mutable_verify_vote_extension()->set_status(req.verify_vote_extension().vote_extension().empty()
                                                                 ? pb::ResponseVerifyVoteExtension::ACCEPT
                                                                 : pb::ResponseVerifyVoteExtension::REJECT);
            return;

        // The application-side mempool (mempool.type = "app") is not supported: yapd runs with "flood".
        case pb::Request::kInsertTx:
        case pb::Request::kReapTxs:
            throw std::runtime_error("InsertTx/ReapTxs: mempool.type = \"app\" is not supported, use \"flood\"");

        case pb::Request::kEcho:
        case pb::Request::kFlush:
        case pb::Request::VALUE_NOT_SET:
            break;
    }
    throw std::runtime_error("unexpected ABCI request (field " + std::to_string(static_cast<int>(req.value_case())) + ")");
}

void Application::info(pb::ResponseInfo& r) const {
    r.set_data("yapchain");
    r.set_version(opts_.software_version);
    r.set_app_version(kAppVersion);
    // Only durably committed data (Engine::info reads the committed metadata). Height 0 — not
    // initialised, or initialised without blocks — makes CometBFT send InitChain.
    if (engine_.initialized() && engine_.info().last_height > 0) {
        const auto i = engine_.info();
        r.set_last_block_height(static_cast<std::int64_t>(i.last_height));
        r.set_last_block_app_hash(to_string_bytes(i.last_state_root));
    } else {
        r.set_last_block_height(0);
    }
}

void Application::init_chain(const pb::RequestInitChain& req, pb::ResponseInitChain& r) {
    // protocol.md §11.4: everything must match, otherwise fail-stop.
    auto parsed = chain::parse_genesis_json(req.app_state_bytes());
    if (!parsed) throw std::invalid_argument("InitChain: invalid app_state genesis: " + parsed.error());
    const chain::Genesis& g = *parsed;
    if (req.chain_id() != g.chain_id) {
        throw FailStop("InitChain: chain_id '" + req.chain_id() + "' differs from genesis '" + g.chain_id + "'");
    }
    if (req.initial_height() != 1 || g.initial_height != 1) throw FailStop("InitChain: initial_height must be 1");
    if (to_time_ms(req.time()) != g.genesis_time_ms) {
        throw FailStop("InitChain: genesis time " + std::to_string(to_time_ms(req.time())) +
                       " ms differs from genesis_time_ms " + std::to_string(g.genesis_time_ms));
    }
    using Entry = std::pair<crypto::PublicKey, std::uint64_t>;
    std::vector<Entry> got;
    for (const auto& v : req.validators()) {
        if (v.pub_key().sum_case() != tendermint::crypto::PublicKey::kEd25519 || v.pub_key().ed25519().size() != 32) {
            throw FailStop("InitChain: validator key is not a 32-byte Ed25519 key");
        }
        if (v.power() <= 0) throw FailStop("InitChain: validator power must be positive");
        crypto::PublicKey pk{};
        std::copy(v.pub_key().ed25519().begin(), v.pub_key().ed25519().end(), pk.begin());
        got.emplace_back(pk, static_cast<std::uint64_t>(v.power()));
    }
    std::vector<Entry> want;
    for (const auto& v : g.validators) want.emplace_back(v.consensus_pubkey, v.power);
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    if (got != want) throw FailStop("InitChain: CometBFT validator set differs from the genesis validators");
    if (req.has_consensus_params()) {
        const auto& cp = req.consensus_params();
        if (cp.has_abci() && cp.abci().vote_extensions_enable_height() != 0) {
            throw FailStop("InitChain: vote extensions are not supported (vote_extensions_enable_height must be 0)");
        }
        if (!cp.has_version() || cp.version().app() != kAppVersion) {
            throw FailStop("InitChain: consensus_params.version.app must be " + std::to_string(kAppVersion));
        }
    } else {
        throw FailStop("InitChain: consensus_params are missing");
    }

    Hash32 root{};
    if (engine_.initialized()) {
        // CometBFT sends InitChain again whenever Info reports height 0 (e.g. it crashed before
        // block 1). The same genesis yields the stored root; anything else is a different chain.
        const auto& m = engine_.meta();
        if (m.last_height != 0) throw FailStop("InitChain on a chain that already has blocks");
        if (m.genesis_hash != chain::genesis_hash(g)) throw FailStop("InitChain: the store holds a different genesis");
        root = m.last_state_root;
        log("InitChain: already initialised with this genesis, app_hash " + to_hex(root));
    } else {
        root = engine_.init_chain(g);  // durable before we answer
        log("InitChain: genesis " + to_hex(chain::genesis_hash(g)) + ", app_hash " + to_hex(root));
    }
    r.set_app_hash(to_string_bytes(root));
    // validators left empty: CometBFT's set (checked equal above) is accepted.
}

void Application::query(const pb::RequestQuery& req, pb::ResponseQuery& r) const {
    r.set_key(req.path());
    if (!engine_.initialized()) return query_error(r, QueryCode::NotInitialized, "chain is not initialised");
    const auto info = engine_.info();
    r.set_height(static_cast<std::int64_t>(info.last_height));
    if (req.prove()) return query_error(r, QueryCode::ProofUnsupported, "proofs are not supported");
    if (req.height() != 0 && req.height() != static_cast<std::int64_t>(info.last_height)) {
        return query_error(r, QueryCode::HeightUnavailable,
                           "only the latest committed height " + std::to_string(info.last_height) + " is available");
    }
    state::Overlay view(&engine_.committed());  // committed data only, never the pending block
    const state::State st(view);
    Json j;
    const std::string& path = req.path();
    static constexpr std::string_view kAccount = "/account/";
    if (path == "/chain") {
        const auto cs = st.chain_state();
        j["chain_id"] = info.chain_id;
        j["height"] = u64s(info.last_height);
        j["state_root"] = to_hex(info.last_state_root);
        j["genesis_hash"] = to_hex(engine_.meta().genesis_hash);
        j["last_time_ms"] = u64s(cs.last_time_ms);
        j["total_supply"] = u64s(cs.total_supply);
        j["total_minted"] = u64s(cs.total_minted);
        j["total_burned"] = u64s(cs.total_burned);
    } else if (path == "/params") {
        const auto p = st.params();
        j["min_fee"] = u64s(p.min_fee);
        j["fee_burn_bps"] = p.fee_burn_bps;
        j["block_reward"] = u64s(p.block_reward);
        j["reward_validators_bps"] = p.reward_validators_bps;
        j["reward_pool_bps"] = p.reward_pool_bps;
        j["reward_treasury_bps"] = p.reward_treasury_bps;
        j["max_issuance"] = u64s(p.max_issuance);
        j["max_tx_bytes"] = p.max_tx_bytes;
        j["max_memo_bytes"] = p.max_memo_bytes;
        j["max_tx_ttl_blocks"] = u64s(p.max_tx_ttl_blocks);
        j["epoch_length_blocks"] = u64s(p.epoch_length_blocks);
    } else if (path.starts_with(kAccount)) {
        const std::string text = path.substr(kAccount.size());
        const auto id = crypto::decode_address(text, chain::hrp_for_chain(info.chain_id));
        if (!id) {
            return query_error(r, QueryCode::BadRequest,
                               "invalid address: " + std::string(crypto::to_string(id.error())));
        }
        const auto a = st.account(*id);
        j["address"] = text;
        j["account_id"] = to_hex(*id);
        j["exists"] = a.has_value();
        j["balance"] = u64s(a ? a->balance : 0);
        j["nonce"] = u64s(a ? a->nonce : 0);
    } else {
        return query_error(r, QueryCode::UnknownPath, "unknown path '" + path + "' (use /chain, /params, /account/<addr>)");
    }
    r.set_code(std::to_underlying(QueryCode::Ok));
    r.set_value(j.dump());
}

void Application::check_tx(const pb::RequestCheckTx& req, pb::ResponseCheckTx& r) {
    // NEW and RECHECK are the same operation: the check-state is reset by every Commit, and CometBFT
    // re-sends the remaining mempool transactions in order, so they are re-validated sequentially.
    const auto& tx = req.tx();
    const auto res = engine_.check_tx(ByteView(reinterpret_cast<const Byte*>(tx.data()), tx.size()));
    r.set_code(std::to_underlying(res.code));
    r.set_log(res.log.empty() ? std::string(tx::to_string(res.code)) : res.log);
}

void Application::prepare_proposal(const pb::RequestPrepareProposal& req, pb::ResponsePrepareProposal& r) const {
    if (req.max_tx_bytes() < 0) throw FailStop("PrepareProposal: negative max_tx_bytes");
    const auto ctx = block_context(engine_.meta().chain_id, req);
    const auto txs = engine_.prepare_proposal(ctx, ctx.txs, static_cast<std::uint64_t>(req.max_tx_bytes()));
    for (const auto& tx : txs) r.add_txs(std::string(tx.begin(), tx.end()));
}

void Application::process_proposal(const pb::RequestProcessProposal& req, pb::ResponseProcessProposal& r) const {
    const auto ctx = block_context(engine_.meta().chain_id, req);
    const bool accept = engine_.process_proposal(ctx);
    r.set_status(accept ? pb::ResponseProcessProposal::ACCEPT : pb::ResponseProcessProposal::REJECT);
    if (!accept) {
        log("ProcessProposal: REJECT height " + std::to_string(req.height()) + " from proposer " +
            hex_bytes(req.proposer_address()) + " (a transaction is invalid)");
    }
}

void Application::finalize_block(const pb::RequestFinalizeBlock& req, pb::ResponseFinalizeBlock& r) {
    const auto ctx = block_context(engine_.meta().chain_id, req);
    if (engine_.has_pending()) {
        // A finalized block that was never committed (CometBFT restarted before Commit). Nothing of it
        // was written; executing the decided block again is deterministic.
        log("FinalizeBlock: discarding an uncommitted block, re-executing height " + std::to_string(ctx.height));
        engine_.abort_pending();
    }
    const auto result = engine_.finalize_block(ctx);
    if (result.tx_results.size() != ctx.txs.size()) throw FailStop("FinalizeBlock: result count differs from tx count");
    for (const auto& t : result.tx_results) {
        auto* e = r.add_tx_results();
        e->set_code(std::to_underlying(t.code));
        e->set_log(t.log);
        set_events(*e->mutable_events(), t.events);
    }
    set_events(*r.mutable_events(), result.events);
    r.set_app_hash(to_string_bytes(result.state_root));
}

void Application::commit(pb::ResponseCommit& r) {
    if (!engine_.has_pending()) throw std::logic_error("Commit without FinalizeBlock");
    const auto root = engine_.commit();  // returns only after the height is durable
    r.set_retain_height(0);              // never prune
    log("Commit: height " + std::to_string(engine_.info().last_height) + ", app_hash " + to_hex(root));
}

}  // namespace yap::abci
