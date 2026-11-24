// SPDX-License-Identifier: Apache-2.0
//
// Integration tests of the ABCI++ application (M2.2): requests are built exactly as CometBFT
// v0.40.0 sends them and go through yap::abci::Application (directly, or over the real socket
// transport), backed by a real chain::Engine on MemoryStore or LMDB.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "abci_client.hpp"
#include "temp_dir.hpp"
#include "testnet.hpp"
#include "yap/abci/application.hpp"
#include "yap/abci/convert.hpp"
#include "yap/common/fail_stop.hpp"
#include "yap/common/hex.hpp"
#include "yap/dev/dev_chain.hpp"
#include "yap/storage/lmdb_store.hpp"

using namespace yap;
namespace ta = ::tendermint::abci;

namespace {

std::string str(const Hash32& h) { return std::string(h.begin(), h.end()); }
std::string str(const Bytes& b) { return std::string(b.begin(), b.end()); }
template <std::size_t N>
std::string str(const FixedBytes<N>& b) {
    return std::string(b.begin(), b.end());
}

void set_time(google::protobuf::Timestamp* t, std::uint64_t ms) {
    t->set_seconds(static_cast<std::int64_t>(ms / 1000));
    t->set_nanos(static_cast<std::int32_t>((ms % 1000) * 1'000'000));
}

ta::Request init_chain_req(const chain::Genesis& g) {
    ta::Request r;
    auto* ic = r.mutable_init_chain();
    set_time(ic->mutable_time(), g.genesis_time_ms);
    ic->set_chain_id(g.chain_id);
    ic->set_initial_height(1);
    // CometBFT sends the set sorted by power, then address; the order must not matter.
    for (auto it = g.validators.rbegin(); it != g.validators.rend(); ++it) {
        auto* v = ic->add_validators();
        v->mutable_pub_key()->set_ed25519(str(it->consensus_pubkey));
        v->set_power(static_cast<std::int64_t>(it->power));
    }
    ic->set_app_state_bytes(chain::to_genesis_json(g));
    auto* cp = ic->mutable_consensus_params();
    cp->mutable_version()->set_app(abci::kAppVersion);
    cp->mutable_abci()->set_vote_extensions_enable_height(0);
    cp->mutable_block()->set_max_bytes(4'194'304);
    return r;
}

state::ConsensusAddress addr_of(const chain::GenesisValidator& v) { return state::consensus_address(v.consensus_pubkey); }

/// Votes for H-1 as CometBFT reports them: one per validator (equal powers here).
template <class Commit>
void fill_votes(Commit* c, const chain::Genesis& g, std::uint64_t height, int absent_index = -1) {
    if (height <= 1) return;  // empty at initial_height
    for (std::size_t i = 0; i < g.validators.size(); ++i) {
        auto* v = c->add_votes();
        v->mutable_validator()->set_address(str(addr_of(g.validators[i])));
        v->mutable_validator()->set_power(static_cast<std::int64_t>(g.validators[i].power));
        v->set_block_id_flag(static_cast<int>(i) == absent_index ? tendermint::types::BLOCK_ID_FLAG_ABSENT
                                                                 : tendermint::types::BLOCK_ID_FLAG_COMMIT);
    }
}

std::uint64_t block_time(const chain::Genesis& g, std::uint64_t h) { return g.genesis_time_ms + (h - 1) * 1'000; }

ta::Request finalize_req(const chain::Genesis& g, std::uint64_t h, const std::vector<Bytes>& txs, int absent = -1) {
    ta::Request r;
    auto* f = r.mutable_finalize_block();
    for (const auto& t : txs) f->add_txs(str(t));
    fill_votes(f->mutable_decided_last_commit(), g, h, absent);
    f->set_height(static_cast<std::int64_t>(h));
    set_time(f->mutable_time(), block_time(g, h));
    f->set_proposer_address(str(addr_of(g.validators[h % g.validators.size()])));
    f->set_hash(std::string(32, 'h'));
    return r;
}

ta::Request process_req(const chain::Genesis& g, std::uint64_t h, const std::vector<Bytes>& txs) {
    ta::Request r;
    auto* p = r.mutable_process_proposal();
    for (const auto& t : txs) p->add_txs(str(t));
    fill_votes(p->mutable_proposed_last_commit(), g, h);
    p->set_height(static_cast<std::int64_t>(h));
    set_time(p->mutable_time(), block_time(g, h));
    p->set_proposer_address(str(addr_of(g.validators[0])));
    return r;
}

ta::Request prepare_req(const chain::Genesis& g, std::uint64_t h, const std::vector<Bytes>& txs, std::int64_t max) {
    ta::Request r;
    auto* p = r.mutable_prepare_proposal();
    p->set_max_tx_bytes(max);
    for (const auto& t : txs) p->add_txs(str(t));
    fill_votes(p->mutable_local_last_commit(), g, h);
    p->set_height(static_cast<std::int64_t>(h));
    set_time(p->mutable_time(), block_time(g, h));
    p->set_proposer_address(str(addr_of(g.validators[0])));
    return r;
}

ta::Request check_req(const Bytes& tx, bool recheck = false) {
    ta::Request r;
    r.mutable_check_tx()->set_tx(str(tx));
    r.mutable_check_tx()->set_type(recheck ? ta::RECHECK : ta::NEW);
    return r;
}

ta::Request query_req(const std::string& path, std::int64_t height = 0, bool prove = false) {
    ta::Request r;
    r.mutable_query()->set_path(path);
    r.mutable_query()->set_height(height);
    r.mutable_query()->set_prove(prove);
    return r;
}

ta::Request simple(ta::Request::ValueCase c) {
    ta::Request r;
    switch (c) {
        case ta::Request::kInfo: r.mutable_info(); break;
        case ta::Request::kCommit: r.mutable_commit(); break;
        case ta::Request::kListSnapshots: r.mutable_list_snapshots(); break;
        case ta::Request::kOfferSnapshot: r.mutable_offer_snapshot(); break;
        case ta::Request::kLoadSnapshotChunk: r.mutable_load_snapshot_chunk(); break;
        case ta::Request::kApplySnapshotChunk: r.mutable_apply_snapshot_chunk(); break;
        case ta::Request::kExtendVote: r.mutable_extend_vote(); break;
        case ta::Request::kInsertTx: r.mutable_insert_tx(); break;
        case ta::Request::kReapTxs: r.mutable_reap_txs(); break;
        default: throw std::logic_error("simple(): unsupported");
    }
    return r;
}

struct Node {
    std::unique_ptr<storage::KvStore> owned;
    storage::KvStore* store;
    std::unique_ptr<chain::Engine> engine;
    std::unique_ptr<abci::Application> app;

    Node() : owned(std::make_unique<storage::MemoryStore>()), store(owned.get()) { attach(); }
    explicit Node(storage::KvStore& s) : store(&s) { attach(); }
    void attach() {
        engine = std::make_unique<chain::Engine>(*store);
        app = std::make_unique<abci::Application>(*engine);
    }
    ta::Response call(const ta::Request& r) {
        ta::Response resp;
        app->handle(r, resp);
        return resp;
    }
};

test::TestNet small_net() { return test::TestNet::make(4); }

}  // namespace

// ------------------------------------------------------------------ conversions (protocol.md §9.1)

TEST_CASE("convert: time, heights, addresses, votes", "[abci][convert]") {
    google::protobuf::Timestamp t;
    t.set_seconds(1'767'225'600);
    t.set_nanos(999'999'999);
    CHECK(abci::to_time_ms(t) == 1'767'225'600'999);  // floor(nanos / 1e6)
    t.set_nanos(0);
    t.set_seconds(9'223'372'036'854'775);
    CHECK(abci::to_time_ms(t) == 9'223'372'036'854'775'000ULL);
    t.set_seconds(9'223'372'036'854'776);
    CHECK_THROWS_AS(abci::to_time_ms(t), FailStop);
    t.set_seconds(-1);
    CHECK_THROWS_AS(abci::to_time_ms(t), FailStop);
    t.set_seconds(0);
    t.set_nanos(1'000'000'000);
    CHECK_THROWS_AS(abci::to_time_ms(t), FailStop);
    t.set_nanos(-1);
    CHECK_THROWS_AS(abci::to_time_ms(t), FailStop);

    CHECK(abci::to_block_height(1) == 1);
    CHECK_THROWS_AS(abci::to_block_height(0), FailStop);
    CHECK_THROWS_AS(abci::to_block_height(-5), FailStop);
    CHECK_THROWS_AS(abci::to_address(std::string(19, 'a'), "x"), FailStop);
    CHECK_THROWS_AS(abci::to_address(std::string(32, 'a'), "x"), FailStop);  // a pubkey is not an address

    ta::CommitInfo ci;
    auto* v = ci.add_votes();
    v->mutable_validator()->set_address(std::string(20, '\x07'));
    v->mutable_validator()->set_power(42);
    v->set_block_id_flag(tendermint::types::BLOCK_ID_FLAG_NIL);
    const auto votes = abci::to_last_commit(ci);
    REQUIRE(votes.size() == 1);
    state::ConsensusAddress sevens{};
    sevens.fill(0x07);
    CHECK(votes[0].validator_address == sevens);
    CHECK(votes[0].power == 42);
    CHECK(votes[0].flag == 3);
    v->mutable_validator()->set_power(-1);
    CHECK_THROWS_AS(abci::to_last_commit(ci), FailStop);
}

// ------------------------------------------------------------------ Info / InitChain (protocol.md §11.4)

TEST_CASE("app: Info before and after InitChain; InitChain answers the genesis state root", "[abci][app]") {
    const auto net = small_net();
    Node n;
    auto info = n.call(simple(ta::Request::kInfo)).info();
    CHECK(info.data() == "yapchain");
    CHECK(info.app_version() == abci::kAppVersion);
    CHECK(info.last_block_height() == 0);
    CHECK(info.last_block_app_hash().empty());

    const auto r = n.call(init_chain_req(net.genesis)).init_chain();
    CHECK(r.validators_size() == 0);  // CometBFT's set is accepted
    CHECK_FALSE(r.has_consensus_params());
    REQUIRE(n.engine->initialized());
    CHECK(r.app_hash() == str(n.engine->info().last_state_root));
    // Independent: a bare engine on the same genesis.
    storage::MemoryStore ref_store;
    chain::Engine ref(ref_store);
    CHECK(r.app_hash() == str(ref.init_chain(net.genesis)));

    info = n.call(simple(ta::Request::kInfo)).info();
    CHECK(info.last_block_height() == 0);  // no block yet: CometBFT sends InitChain again on restart

    // Repeated InitChain with the same genesis (CometBFT crashed before block 1): same answer.
    CHECK(n.call(init_chain_req(net.genesis)).init_chain().app_hash() == r.app_hash());
}

TEST_CASE("app: InitChain refuses every mismatch and fails stop without writing", "[abci][app]") {
    const auto net = small_net();
    using Mutate = std::function<void(ta::RequestInitChain&)>;
    struct Case {
        const char* name;
        Mutate mutate;
        const char* reason;  // expected in the fail-stop message
    };
    const std::vector<Case> cases = {
        {"chain_id", [](auto& r) { r.set_chain_id("yapchain-testnet-2"); }, "chain_id"},
        {"initial_height", [](auto& r) { r.set_initial_height(2); }, "initial_height"},
        {"time +1ms", [](auto& r) { r.mutable_time()->set_nanos(r.time().nanos() + 1'000'000); }, "genesis time"},
        {"missing validator", [](auto& r) { r.mutable_validators()->RemoveLast(); }, "validator set differs"},
        {"extra validator",
         [](auto& r) {
             auto* v = r.add_validators();
             *v = r.validators(0);
         },
         "validator set differs"},
        {"power", [](auto& r) { r.mutable_validators(1)->set_power(11); }, "validator set differs"},
        {"pubkey", [](auto& r) { r.mutable_validators(0)->mutable_pub_key()->set_ed25519(std::string(32, '\x01')); },
         "validator set differs"},
        {"secp256k1 key",
         [](auto& r) { r.mutable_validators(0)->mutable_pub_key()->set_secp256k1(std::string(33, '\x02')); }, "Ed25519"},
        {"zero power", [](auto& r) { r.mutable_validators(0)->set_power(0); }, "positive"},
        {"app_state not JSON", [](auto& r) { r.set_app_state_bytes("{"); }, "app_state"},
        {"app_state empty", [](auto& r) { r.set_app_state_bytes(""); }, "app_state"},
        {"vote extensions",
         [](auto& r) { r.mutable_consensus_params()->mutable_abci()->set_vote_extensions_enable_height(5); },
         "vote extensions"},
        {"app version", [](auto& r) { r.mutable_consensus_params()->mutable_version()->set_app(2); }, "version.app"},
        {"no consensus params", [](auto& r) { r.clear_consensus_params(); }, "consensus_params are missing"},
    };
    for (const auto& c : cases) {
        INFO(c.name);
        Node n;
        auto req = init_chain_req(net.genesis);
        c.mutate(*req.mutable_init_chain());
        CHECK_THROWS(n.call(req));
        CHECK_FALSE(n.engine->initialized());
        REQUIRE(n.app->failure().has_value());
        CHECK(n.app->failure()->find(c.reason) != std::string::npos);
        CHECK_THROWS(n.call(simple(ta::Request::kInfo)));  // sticky: nothing answers after a fail-stop
    }

    SECTION("a store initialised with another genesis") {
        Node n;
        n.call(init_chain_req(net.genesis));
        auto other = net.genesis;
        other.genesis_time_ms += 1000;
        CHECK_THROWS(n.call(init_chain_req(other)));
    }
}

// ------------------------------------------------------------------ one height end to end

TEST_CASE("app: CheckTx, PrepareProposal, ProcessProposal, FinalizeBlock, Commit", "[abci][app]") {
    const auto net = small_net();
    Node n;
    n.call(init_chain_req(net.genesis));
    const auto& alice = net.users[0];
    const auto bob = test::id_of(net.users[1]);
    const auto t0 = test::transfer_tx(alice, bob, 5'000, 0);
    const auto t1 = test::transfer_tx(alice, bob, 7'000, 1);
    const auto bad = test::transfer_tx(alice, bob, 1, 7);  // nonce gap
    const auto root0 = n.engine->info().last_state_root;

    // CheckTx: sequential mempool view; never touches the committed state.
    CHECK(n.call(check_req(t0)).check_tx().code() == 0);
    CHECK(n.call(check_req(t0)).check_tx().code() == static_cast<std::uint32_t>(tx::TxCode::ErrNonce));
    CHECK(n.call(check_req(t1)).check_tx().code() == 0);
    CHECK(n.call(check_req(bad)).check_tx().code() == static_cast<std::uint32_t>(tx::TxCode::ErrNonce));
    CHECK(n.call(check_req(Bytes{1, 2, 3})).check_tx().code() == static_cast<std::uint32_t>(tx::TxCode::ErrDecode));
    CHECK(n.engine->info().last_state_root == root0);
    CHECK(n.engine->info().last_height == 0);

    // PrepareProposal keeps valid transactions in order within the byte budget.
    auto prep = n.call(prepare_req(net.genesis, 1, {t0, bad, t1}, 1 << 20)).prepare_proposal();
    REQUIRE(prep.txs_size() == 2);
    CHECK(prep.txs(0) == str(t0));
    CHECK(prep.txs(1) == str(t1));
    const auto budget = static_cast<std::int64_t>(chain::proposal_tx_cost(t0.size()));
    prep = n.call(prepare_req(net.genesis, 1, {t0, t1}, budget)).prepare_proposal();
    CHECK(prep.txs_size() == 1);
    CHECK_THROWS(Node().call(prepare_req(net.genesis, 1, {}, -1)));  // negative budget (uninitialised node too)

    // ProcessProposal: ACCEPT only if every transaction is OK; never UNKNOWN; state untouched.
    CHECK(n.call(process_req(net.genesis, 1, {t0, t1})).process_proposal().status() == ta::ResponseProcessProposal::ACCEPT);
    CHECK(n.call(process_req(net.genesis, 1, {t0, bad})).process_proposal().status() == ta::ResponseProcessProposal::REJECT);
    CHECK(n.call(process_req(net.genesis, 1, {})).process_proposal().status() == ta::ResponseProcessProposal::ACCEPT);
    CHECK(n.engine->info().last_state_root == root0);
    CHECK_FALSE(n.engine->has_pending());

    // FinalizeBlock: one result per tx, exact codes, app_hash = state root; nothing durable yet.
    const auto fin = n.call(finalize_req(net.genesis, 1, {t0, bad, t1})).finalize_block();
    REQUIRE(fin.tx_results_size() == 3);
    CHECK(fin.tx_results(0).code() == 0);
    CHECK(fin.tx_results(1).code() == static_cast<std::uint32_t>(tx::TxCode::ErrNonce));
    CHECK(fin.tx_results(2).code() == 0);
    for (const auto& r : fin.tx_results()) {
        CHECK(r.data().empty());
        CHECK(r.gas_wanted() == 0);
        CHECK(r.gas_used() == 0);
    }
    CHECK(fin.validator_updates_size() == 0);
    CHECK_FALSE(fin.has_consensus_param_updates());
    CHECK(n.call(simple(ta::Request::kInfo)).info().last_block_height() == 0);  // Info: durable data only

    // Independent reference: the bare engine on the same BlockContext.
    storage::MemoryStore ref_store;
    chain::Engine ref(ref_store);
    ref.init_chain(net.genesis);
    state::BlockContext ctx;
    ctx.chain_id = net.genesis.chain_id;
    ctx.height = 1;
    ctx.time_ms = net.genesis.genesis_time_ms;
    ctx.proposer_address = addr_of(net.genesis.validators[1]);
    ctx.txs = {t0, bad, t1};
    const auto expect = ref.finalize_block(ctx);
    CHECK(fin.app_hash() == str(expect.state_root));

    const auto c = n.call(simple(ta::Request::kCommit)).commit();
    CHECK(c.retain_height() == 0);
    const auto info = n.call(simple(ta::Request::kInfo)).info();
    CHECK(info.last_block_height() == 1);
    CHECK(info.last_block_app_hash() == fin.app_hash());

    // Recheck after Commit: the included transactions are now stale.
    CHECK(n.call(check_req(t0, true)).check_tx().code() == static_cast<std::uint32_t>(tx::TxCode::ErrNonce));
    CHECK(n.call(check_req(test::transfer_tx(alice, bob, 1, 2), true)).check_tx().code() == 0);

    // Commit without FinalizeBlock is a protocol violation: fail-stop.
    CHECK_THROWS(n.call(simple(ta::Request::kCommit)));
    CHECK(n.app->failure().has_value());
}

TEST_CASE("app: decided_last_commit drives emission exactly like the core", "[abci][app]") {
    const auto net = small_net();
    Node n;
    n.call(init_chain_req(net.genesis));
    n.call(finalize_req(net.genesis, 1, {}));
    n.call(simple(ta::Request::kCommit));

    storage::MemoryStore ref_store;
    chain::Engine ref(ref_store);
    ref.init_chain(net.genesis);
    state::BlockContext c1;
    c1.chain_id = net.genesis.chain_id;
    c1.height = 1;
    c1.time_ms = block_time(net.genesis, 1);
    (void)ref.finalize_block(c1);
    ref.commit();

    // Height 2: validator #2 absent. The core sorts by address; CometBFT order must not matter.
    const auto fin = n.call(finalize_req(net.genesis, 2, {}, 2)).finalize_block();
    state::BlockContext c2 = c1;
    c2.height = 2;
    c2.time_ms = block_time(net.genesis, 2);
    for (std::size_t i = 0; i < net.genesis.validators.size(); ++i) {
        c2.last_commit.push_back({addr_of(net.genesis.validators[i]), net.genesis.validators[i].power,
                                  static_cast<std::uint8_t>(i == 2 ? 1 : 2)});
    }
    CHECK(fin.app_hash() == str(ref.finalize_block(c2).state_root));
    n.call(simple(ta::Request::kCommit));

    const auto account = [&](const crypto::AccountId& id) {
        const auto addr = crypto::encode_address(crypto::kTestnetHrp, id);
        const auto q = n.call(query_req("/account/" + addr)).query();
        REQUIRE(q.code() == 0);
        return nlohmann::json::parse(q.value());
    };
    // Signers got their share; the absent validator nothing (ADR-0015, §9.5).
    CHECK(account(net.genesis.validators[0].reward_account)["balance"] != "0");
    CHECK(account(net.genesis.validators[2].reward_account)["exists"] == false);
    const auto chain = nlohmann::json::parse(n.call(query_req("/chain")).query().value());
    CHECK(chain["height"] == "2");
    CHECK(chain["total_minted"] != chain["total_burned"]);
}

TEST_CASE("app: inconsistent consensus input fails stop", "[abci][app]") {
    const auto net = small_net();
    using Mutate = std::function<void(ta::RequestFinalizeBlock&)>;
    struct Case {
        const char* name;
        Mutate mutate;
        const char* reason;
    };
    const std::vector<Case> at_height_2 = {
        {"unknown validator",
         [](auto& r) { r.mutable_decided_last_commit()->mutable_votes(0)->mutable_validator()->set_address(std::string(20, 'z')); },
         "unknown validator"},
        {"wrong power", [](auto& r) { r.mutable_decided_last_commit()->mutable_votes(0)->mutable_validator()->set_power(9); },
         "power mismatch"},
        {"duplicate vote", [](auto& r) { *r.mutable_decided_last_commit()->add_votes() = r.decided_last_commit().votes(0); },
         "duplicate validator"},
        {"flag UNKNOWN",
         [](auto& r) { r.mutable_decided_last_commit()->mutable_votes(0)->set_block_id_flag(tendermint::types::BLOCK_ID_FLAG_UNKNOWN); },
         "invalid BlockIDFlag"},
        {"address 32 bytes",
         [](auto& r) { r.mutable_decided_last_commit()->mutable_votes(0)->mutable_validator()->set_address(std::string(32, 'a')); },
         "expected 20 bytes"},
        {"height skips", [](auto& r) { r.set_height(3); }, "last_height + 1"},
        {"time before last block", [](auto& r) { r.mutable_time()->set_seconds(r.time().seconds() - 10); }, "time goes backwards"},
        {"proposer address length", [](auto& r) { r.set_proposer_address("short"); }, "proposer_address"},
    };
    for (const auto& c : at_height_2) {
        INFO(c.name);
        Node n;
        n.call(init_chain_req(net.genesis));
        n.call(finalize_req(net.genesis, 1, {}));
        n.call(simple(ta::Request::kCommit));
        const auto root = n.engine->info().last_state_root;
        auto req = finalize_req(net.genesis, 2, {});
        c.mutate(*req.mutable_finalize_block());
        CHECK_THROWS(n.call(req));
        REQUIRE(n.app->failure().has_value());
        CAPTURE(*n.app->failure());
        CHECK(n.app->failure()->find(c.reason) != std::string::npos);
        CHECK(n.engine->info().last_height == 1);
        CHECK(n.engine->info().last_state_root == root);
        CHECK_THROWS(n.call(simple(ta::Request::kCommit)));  // nothing can be committed afterwards
        CHECK(n.engine->info().last_height == 1);
    }
    SECTION("votes at the initial height") {
        Node n;
        n.call(init_chain_req(net.genesis));
        auto req = finalize_req(net.genesis, 2, {});
        req.mutable_finalize_block()->set_height(1);
        set_time(req.mutable_finalize_block()->mutable_time(), net.genesis.genesis_time_ms);
        CHECK_THROWS(n.call(req));
    }
}

TEST_CASE("app: FinalizeBlock again after an uncommitted one re-executes deterministically", "[abci][app]") {
    const auto net = small_net();
    Node n;
    n.call(init_chain_req(net.genesis));
    const auto tx = test::transfer_tx(net.users[0], test::id_of(net.users[2]), 10, 0);
    const auto a = n.call(finalize_req(net.genesis, 1, {tx})).finalize_block().app_hash();
    const auto b = n.call(finalize_req(net.genesis, 1, {tx})).finalize_block().app_hash();
    CHECK(a == b);
    n.call(simple(ta::Request::kCommit));
    CHECK(n.engine->info().last_height == 1);
}

// ------------------------------------------------------------------ Query, unsupported methods

TEST_CASE("app: Query paths, heights and errors", "[abci][app]") {
    const auto net = small_net();
    Node n;
    CHECK(n.call(query_req("/chain")).query().code() == static_cast<std::uint32_t>(abci::QueryCode::NotInitialized));
    n.call(init_chain_req(net.genesis));
    const auto alice = crypto::encode_address(crypto::kTestnetHrp, test::id_of(net.users[0]));
    n.call(finalize_req(net.genesis, 1, {test::transfer_tx(net.users[0], test::id_of(net.users[1]), 1, 0)}));
    // The pending block is invisible to queries.
    auto acc = nlohmann::json::parse(n.call(query_req("/account/" + alice)).query().value());
    CHECK(acc["nonce"] == "0");
    n.call(simple(ta::Request::kCommit));
    const auto q = n.call(query_req("/account/" + alice)).query();
    CHECK(q.code() == 0);
    CHECK(q.height() == 1);
    acc = nlohmann::json::parse(q.value());
    CHECK(acc["nonce"] == "1");
    CHECK(acc["exists"] == true);
    CHECK(n.call(query_req("/account/" + alice, 1)).query().code() == 0);
    CHECK(n.call(query_req("/params")).query().code() == 0);
    CHECK(nlohmann::json::parse(n.call(query_req("/params")).query().value())["min_fee"] == "1000");

    const auto code = [&](const ta::Request& r) { return n.call(r).query().code(); };
    CHECK(code(query_req("/nope")) == static_cast<std::uint32_t>(abci::QueryCode::UnknownPath));
    CHECK(code(query_req("/account/tyap1invalid")) == static_cast<std::uint32_t>(abci::QueryCode::BadRequest));
    CHECK(code(query_req("/chain", 5)) == static_cast<std::uint32_t>(abci::QueryCode::HeightUnavailable));
    CHECK(code(query_req("/chain", 0, true)) == static_cast<std::uint32_t>(abci::QueryCode::ProofUnsupported));
    CHECK(n.call(query_req("/nope")).query().codespace() == "yap.query");
    CHECK_FALSE(n.app->failure().has_value());  // bad queries are not fatal
}

TEST_CASE("app: snapshots, vote extensions and the app mempool are explicitly unsupported", "[abci][app]") {
    Node n;
    CHECK(n.call(simple(ta::Request::kListSnapshots)).list_snapshots().snapshots_size() == 0);
    CHECK(n.call(simple(ta::Request::kOfferSnapshot)).offer_snapshot().result() == ta::ResponseOfferSnapshot::REJECT);
    CHECK(n.call(simple(ta::Request::kLoadSnapshotChunk)).load_snapshot_chunk().chunk().empty());
    CHECK(n.call(simple(ta::Request::kApplySnapshotChunk)).apply_snapshot_chunk().result() ==
          ta::ResponseApplySnapshotChunk::ABORT);
    CHECK(n.call(simple(ta::Request::kExtendVote)).extend_vote().vote_extension().empty());
    ta::Request v;
    v.mutable_verify_vote_extension();
    CHECK(n.call(v).verify_vote_extension().status() == ta::ResponseVerifyVoteExtension::ACCEPT);
    v.mutable_verify_vote_extension()->set_vote_extension("x");
    CHECK(n.call(v).verify_vote_extension().status() == ta::ResponseVerifyVoteExtension::REJECT);
    CHECK_FALSE(n.app->failure().has_value());
    CHECK_THROWS(n.call(simple(ta::Request::kInsertTx)));
    CHECK(n.app->failure().has_value());
    Node m;
    CHECK_THROWS(m.call(simple(ta::Request::kReapTxs)));
}

TEST_CASE("app: a dev-chain database is refused", "[abci][app]") {
    const auto net = small_net();
    storage::MemoryStore store;
    dev::DevChain::init(store, net.genesis, net.users[0].public_key());
    chain::Engine e(store);
    CHECK_THROWS_AS(abci::Application(e), std::invalid_argument);
}

// ------------------------------------------------------------------ durability (LMDB)

TEST_CASE("app: Commit is durable; a crash between FinalizeBlock and Commit loses only the pending block",
          "[abci][app][lmdb]") {
    const auto net = small_net();
    test::TempDir dir;
    const auto tx0 = test::transfer_tx(net.users[0], test::id_of(net.users[3]), 77, 0);
    const auto tx1 = test::transfer_tx(net.users[0], test::id_of(net.users[3]), 88, 1);
    std::string h1, h2;
    {
        storage::LmdbStore store({dir.path()});
        Node n(store);
        n.call(init_chain_req(net.genesis));
        h1 = n.call(finalize_req(net.genesis, 1, {tx0})).finalize_block().app_hash();
        n.call(simple(ta::Request::kCommit));
        h2 = n.call(finalize_req(net.genesis, 2, {tx1})).finalize_block().app_hash();
        // "crash": no Commit for height 2
    }
    {
        storage::LmdbStore store({dir.path()});
        Node n(store);
        auto info = n.call(simple(ta::Request::kInfo)).info();
        CHECK(info.last_block_height() == 1);
        CHECK(info.last_block_app_hash() == h1);
        // CometBFT replays height 2 with the real app: same result, applied once.
        CHECK(n.call(finalize_req(net.genesis, 2, {tx1})).finalize_block().app_hash() == h2);
        n.call(simple(ta::Request::kCommit));
    }
    {
        storage::LmdbStore store({dir.path(), std::size_t{1} << 30, true, false});
        chain::Engine e(store);
        CHECK(e.info().last_height == 2);
        CHECK(str(e.info().last_state_root) == h2);
        (void)chain::verify_state(store);
    }
}

// ------------------------------------------------------------------ concurrency over the real socket

TEST_CASE("app: four concurrent connections, one consistent engine", "[abci][app][socket]") {
    const auto net = small_net();
    test::TempDir dir;
    storage::MemoryStore store;
    chain::Engine engine(store);
    abci::Application app(engine);
    abci::ServerOptions o;
    o.socket_path = dir.path() / "app.sock";
    abci::Server server(o, app);
    server.start();
    const auto path = o.socket_path.string();

    test::AbciClient consensus(path);
    REQUIRE(consensus.call(init_chain_req(net.genesis)).has_init_chain());
    std::atomic<bool> done{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> side;
    side.emplace_back([&] {  // mempool connection
        test::AbciClient c(path);
        std::uint64_t i = 0;
        while (!done.load()) {
            const auto r = c.call(check_req(test::transfer_tx(net.users[1], test::id_of(net.users[2]), 1, i++ % 3)));
            if (!r.has_check_tx()) ++errors;
        }
    });
    side.emplace_back([&] {  // query connection
        test::AbciClient c(path);
        while (!done.load()) {
            if (!c.call(query_req("/chain")).has_query()) ++errors;
            if (!c.call(simple(ta::Request::kInfo)).has_info()) ++errors;
        }
    });
    side.emplace_back([&] {  // snapshot connection
        test::AbciClient c(path);
        while (!done.load()) {
            if (!c.call(simple(ta::Request::kListSnapshots)).has_list_snapshots()) ++errors;
        }
    });

    storage::MemoryStore ref_store;
    chain::Engine ref(ref_store);
    ref.init_chain(net.genesis);
    for (std::uint64_t h = 1; h <= 20; ++h) {
        const auto tx = test::transfer_tx(net.users[0], test::id_of(net.users[3]), h, h - 1);
        const auto proc = consensus.call(process_req(net.genesis, h, {tx}));
        CHECK(proc.process_proposal().status() == ta::ResponseProcessProposal::ACCEPT);
        const auto fin = consensus.call(finalize_req(net.genesis, h, {tx}));
        REQUIRE(fin.has_finalize_block());
        REQUIRE(consensus.call(simple(ta::Request::kCommit)).has_commit());

        state::BlockContext ctx;
        ctx.chain_id = net.genesis.chain_id;
        ctx.height = h;
        ctx.time_ms = block_time(net.genesis, h);
        ctx.proposer_address = addr_of(net.genesis.validators[h % net.genesis.validators.size()]);
        if (h > 1) {
            for (const auto& v : net.genesis.validators) ctx.last_commit.push_back({addr_of(v), v.power, 2});
        }
        ctx.txs = {tx};
        const auto expect = ref.finalize_block(ctx);
        ref.commit();
        CHECK(fin.finalize_block().app_hash() == str(expect.state_root));
    }
    done = true;
    for (auto& t : side) t.join();
    CHECK(errors.load() == 0);
    CHECK_FALSE(app.failure().has_value());
    CHECK(engine.info().last_height == 20);
    server.stop();
}

// ------------------------------------------------------------------ M2.4d: NIL from the exact CometBFT message

namespace {

/// A node that has committed height 1 (so height 2 carries a last commit).
Node node_at_height_1(const test::TestNet& net) {
    Node n;
    n.call(init_chain_req(net.genesis));
    n.call(finalize_req(net.genesis, 1, {}));
    n.call(simple(ta::Request::kCommit));
    return n;
}

}  // namespace

TEST_CASE("app: NIL and ABSENT in decided_last_commit — exact CometBFT message", "[abci][app][economics]") {
    const auto net = small_net();
    auto n = node_at_height_1(net);
    // RequestFinalizeBlock for height 2 as CometBFT v0.40.0 builds it (state/execution.go
    // buildLastCommitInfo): one VoteInfo per validator of height 1 in validator-set order, flags
    // COMMIT, NIL, ABSENT, COMMIT. Round-tripped through the wire encoding.
    ta::Request built = finalize_req(net.genesis, 2, {});
    auto* votes = built.mutable_finalize_block()->mutable_decided_last_commit()->mutable_votes();
    REQUIRE(votes->size() == 4);
    votes->Mutable(1)->set_block_id_flag(tendermint::types::BLOCK_ID_FLAG_NIL);
    votes->Mutable(2)->set_block_id_flag(tendermint::types::BLOCK_ID_FLAG_ABSENT);
    const std::string wire = built.SerializeAsString();
    ta::Request req;
    REQUIRE(req.ParseFromString(wire));
    const auto& v1 = req.finalize_block().decided_last_commit().votes(1);
    CHECK(v1.block_id_flag() == 3);  // BLOCK_ID_FLAG_NIL = 3 on the wire (VoteInfo field 3)
    CHECK(v1.SerializeAsString().find(std::string("\x18\x03", 2)) != std::string::npos);
    CHECK(req.finalize_block().decided_last_commit().votes(2).block_id_flag() == 1);

    const auto fin = n.call(req).finalize_block();
    n.call(simple(ta::Request::kCommit));
    // Only COMMIT signers share the validators' part: 1.2 YAP over 2 x power 10 -> 600 000 yip each.
    std::map<std::string, std::uint64_t> paid;
    std::uint64_t treasury_part = 0;
    for (const auto& e : fin.events()) {
        std::map<std::string, std::string> a;
        for (const auto& kv : e.attributes()) a[kv.key()] = kv.value();
        if (e.type() == "validator_reward") paid[a["validator"]] = std::stoull(a["amount"]);
        if (e.type() == "block_reward") treasury_part = std::stoull(a["treasury"]);
    }
    const auto hex_addr = [&](std::size_t i) { return to_hex(addr_of(net.genesis.validators[i])); };
    CHECK(paid == std::map<std::string, std::uint64_t>{{hex_addr(0), 600'000}, {hex_addr(3), 600'000}});
    CHECK(treasury_part == 200'000);  // t plus the (zero) remainder
    const auto balance = [&](const crypto::AccountId& id) {
        const auto q = n.call(query_req("/account/" + crypto::encode_address(crypto::kTestnetHrp, id))).query();
        REQUIRE(q.code() == 0);
        return std::stoull(nlohmann::json::parse(q.value())["balance"].get<std::string>());
    };
    CHECK(balance(net.genesis.validators[0].reward_account) == 600'000);
    CHECK(balance(net.genesis.validators[1].reward_account) == 0);  // NIL: no reward
    CHECK(balance(net.genesis.validators[2].reward_account) == 0);  // ABSENT: no reward
    CHECK(balance(net.genesis.validators[3].reward_account) == 600'000);
    // Height 1 (no votes): the validators' share went to the treasury; height 2: t only.
    CHECK(balance(crypto::system_account_id(crypto::kTreasuryAccountName)) == 1'400'000 + 200'000);
    CHECK(balance(crypto::system_account_id(crypto::kRewardsPoolAccountName)) == 2 * 600'000);
}
