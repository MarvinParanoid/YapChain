// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "temp_dir.hpp"
#include "testnet.hpp"
#include "yap/dev/dev_chain.hpp"
#include "yap/merkle/merkle.hpp"
#include "yap/storage/lmdb_store.hpp"

using namespace yap;
using namespace yap::test;
using dev::Reject;

namespace {

std::map<Bytes, Bytes> dump_all(const storage::KvStore& s) {
    std::map<Bytes, Bytes> out;
    for (std::size_t t = 0; t < storage::kTableCount; ++t) {
        s.for_each(static_cast<storage::Table>(t), [&](const Bytes& k, const Bytes& v) {
            Bytes key{static_cast<Byte>(t)};
            key.insert(key.end(), k.begin(), k.end());
            out.emplace(key, v);
        });
    }
    return out;
}

crypto::SecretKey producer_key(Byte b = 0x77) {
    crypto::Seed s{};
    s.fill(b);
    return crypto::SecretKey::from_seed(s);
}

/// A chain with `n` produced blocks; returns the blocks.
std::vector<dev::DevBlock> produce_chain(storage::KvStore& store, const TestNet& net, std::size_t n) {
    const auto pk = producer_key();
    REQUIRE(dev::DevChain::init(store, net.genesis, pk.public_key()));
    dev::DevChain chain(store);
    const auto journal = net.journal(n, 6, 5);
    std::vector<dev::DevBlock> blocks;
    for (const auto& j : journal) blocks.push_back(chain.produce(pk, j.ctx.txs));
    return blocks;
}

Reject reject_reason(dev::DevChain& c, const dev::DevBlock& b) {
    try {
        c.import(b);
    } catch (const dev::BlockRejected& e) {
        return e.reason();
    }
    FAIL("block was accepted");
    return Reject::Conflict;
}

dev::DevBlock resign(dev::DevBlock b) {
    b.signature = dev::sign_header(b.header, producer_key());
    return b;
}

}  // namespace

TEST_CASE("produced dev blocks link, persist and import into a second node", "[dev]") {
    const auto net = TestNet::make(10);
    test::TempDir dir;
    std::vector<dev::DevBlock> blocks;
    Hash32 root{};
    {
        storage::LmdbStore store({dir.path()});
        blocks = produce_chain(store, net, 8);
        dev::DevChain c(store);
        root = c.engine().info().last_state_root;
        CHECK(c.engine().info().last_height == 8);
    }
    // Chain structure (protocol.md §14).
    CHECK(blocks[0].header.prev_block_hash == chain::genesis_hash(net.genesis));
    for (std::size_t i = 1; i < blocks.size(); ++i) {
        CHECK(blocks[i].header.prev_block_hash == dev::block_hash(blocks[i - 1].header));
        CHECK(blocks[i].header.time_ms > blocks[i - 1].header.time_ms);
        CHECK(blocks[i].header.tx_root == merkle::rfc6962_root(blocks[i].txs));
        CHECK(dev::verify_header_signature(blocks[i]));
    }
    // Reopen: the tip and the stored blocks survive.
    storage::LmdbStore store({dir.path()});
    dev::DevChain reopened(store);
    CHECK(reopened.tip_hash() == dev::block_hash(blocks.back().header));
    CHECK(reopened.block(3) == blocks[2]);
    CHECK_NOTHROW((void)chain::verify_state(store));

    // A second node (import-only: knows the producer's public key) replays the chain.
    storage::MemoryStore other;
    REQUIRE(dev::DevChain::init(other, net.genesis, producer_key().public_key()));
    dev::DevChain node(other);
    const auto st = node.replay(blocks);
    CHECK(st.imported == 8);
    CHECK(node.engine().info().last_state_root == root);
    // Replaying again skips identical blocks; re-importing one is rejected without emission.
    const auto minted_before = [&] {
        state::Overlay v(&node.engine().committed());
        return state::State(v).chain_state().total_minted;
    }();
    CHECK(node.replay(blocks).skipped == 8);
    CHECK(reject_reason(node, blocks[4]) == Reject::AlreadyHave);
    state::Overlay v(&node.engine().committed());
    CHECK(state::State(v).chain_state().total_minted == minted_before);
}

TEST_CASE("every malformed dev block is rejected without touching the state", "[dev]") {
    const auto net = TestNet::make(10);
    storage::MemoryStore source;
    const auto blocks = produce_chain(source, net, 3);

    storage::MemoryStore store;
    REQUIRE(dev::DevChain::init(store, net.genesis, producer_key().public_key()));
    dev::DevChain c(store);
    c.import(blocks[0]);
    const auto before = dump_all(store);
    const auto good = blocks[1];

    auto check = [&](const dev::DevBlock& bad, Reject expected) {
        CHECK(reject_reason(c, bad) == expected);
        CHECK(dump_all(store) == before);
        CHECK_FALSE(c.engine().has_pending());
    };
    auto b = good;
    b.header.version = 2;
    check(resign(b), Reject::BadVersion);
    b = good;
    b.header.chain_id = "other-chain";
    check(resign(b), Reject::WrongChain);
    check(blocks[0], Reject::AlreadyHave);
    check(blocks[2], Reject::HeightGap);
    b = good;
    b.header.prev_block_hash[0] ^= 1;
    check(resign(b), Reject::WrongParent);
    b = good;
    b.header.time_ms = blocks[0].header.time_ms;  // must be strictly later than the previous block
    check(resign(b), Reject::BadTime);
    b = good;
    b.header.producer_pubkey = producer_key(0x11).public_key();
    b.signature = dev::sign_header(b.header, producer_key(0x11));  // validly signed by a stranger
    check(b, Reject::WrongProducer);
    b = good;
    b.signature[5] ^= 1;
    check(b, Reject::BadSignature);
    b = good;
    b.header.state_root[0] ^= 1;  // signature over the original header no longer matches
    check(b, Reject::BadSignature);
    b = good;
    REQUIRE_FALSE(b.txs.empty());
    b.txs.pop_back();  // header still commits to the original tx_root
    check(b, Reject::BadTxRoot);
    b = good;
    b.header.state_root[0] ^= 1;
    check(resign(b), Reject::BadStateRoot);
    // A block that commits to an invalid transaction (consistent tx_root, valid signature).
    b = good;
    b.txs.push_back(Bytes{1, 2, 3});
    b.header.tx_root = merkle::rfc6962_root(b.txs);
    check(resign(b), Reject::InvalidTransaction);

    CHECK_NOTHROW(c.import(good));  // the genuine block is still accepted afterwards
}

TEST_CASE("dev chain init is idempotent and never overwrites a different chain", "[dev]") {
    const auto net = TestNet::make(5);
    storage::MemoryStore store;
    CHECK(dev::DevChain::init(store, net.genesis, producer_key().public_key()));
    dev::DevChain c(store);
    c.produce(producer_key(), {});
    const auto before = dump_all(store);
    CHECK_FALSE(dev::DevChain::init(store, net.genesis, producer_key().public_key()));  // no-op
    CHECK_THROWS_AS(dev::DevChain::init(store, net.genesis, producer_key(0x12).public_key()), std::invalid_argument);
    auto other = net.genesis;
    other.genesis_time_ms += 1;
    CHECK_THROWS_AS(dev::DevChain::init(store, other, producer_key().public_key()), std::invalid_argument);
    CHECK(dump_all(store) == before);
    CHECK_THROWS_AS(c.produce(producer_key(0x12), {}), dev::BlockRejected);  // wrong producer key
}

TEST_CASE("produce leaves out invalid candidates and uses deterministic time", "[dev]") {
    const auto net = TestNet::make(5);
    storage::MemoryStore store;
    dev::DevChain::init(store, net.genesis, producer_key().public_key());
    dev::DevChain c(store);
    const auto good = transfer_tx(net.users[0], id_of(net.users[1]), 10, 0, 1000, 50);
    const auto b = c.produce(producer_key(), {Bytes{9, 9}, good, good});
    CHECK(b.txs == std::vector<Bytes>{good});
    CHECK(b.header.time_ms == net.genesis.genesis_time_ms + 2'000);
}

TEST_CASE("dev block decoding enforces the protocol limits", "[dev]") {
    dev::DevBlock b;
    b.header.chain_id = "yapchain-testnet-1";
    b.txs.assign(dev::kMaxDevBlockTxs, Bytes{1});
    CHECK(ycs::decode<dev::DevBlock>(ycs::encode(b)).has_value());
    b.txs.push_back(Bytes{1});
    CHECK(ycs::decode<dev::DevBlock>(ycs::encode(b)).error() == ycs::DecodeError::LengthLimit);
    b.txs = {Bytes(dev::kMaxDevBlockTxBytes + 1, 0)};
    CHECK(ycs::decode<dev::DevBlock>(ycs::encode(b)).error() == ycs::DecodeError::LengthLimit);
}

TEST_CASE("a failed dev-block commit leaves the chain usable", "[dev][audit]") {
    const auto net = TestNet::make(5);
    storage::MemoryStore store;
    dev::DevChain::init(store, net.genesis, producer_key().public_key());
    dev::DevChain c(store);
    storage::set_fault_hook([](std::string_view p) {
        if (p == "before_commit") throw storage::StorageFull("injected MAP_FULL");
    });
    CHECK_THROWS_AS(c.produce(producer_key(), {}), storage::StorageFull);
    storage::set_fault_hook({});
    CHECK_FALSE(c.engine().has_pending());  // previously every later produce/import failed
    CHECK(c.engine().info().last_height == 0);
    CHECK_NOTHROW(c.produce(producer_key(), {}));
    CHECK(c.engine().info().last_height == 1);
}

TEST_CASE("dev import enforces the protocol size limits before committing", "[dev][audit]") {
    const auto net = TestNet::make(5);
    storage::MemoryStore store;
    dev::DevChain::init(store, net.genesis, producer_key().public_key());
    dev::DevChain c(store);
    dev::DevBlock b;
    b.header.chain_id = net.genesis.chain_id;
    b.header.height = 1;
    b.header.time_ms = net.genesis.genesis_time_ms + 1;
    b.header.prev_block_hash = c.tip_hash();
    b.header.producer_pubkey = producer_key().public_key();
    b.txs.assign(dev::kMaxDevBlockTxs + 1, Bytes{1});
    b.header.tx_root = merkle::rfc6962_root(b.txs);
    b.signature = dev::sign_header(b.header, producer_key());
    CHECK(reject_reason(c, b) == Reject::LimitExceeded);
    b.txs = {Bytes(dev::kMaxDevBlockTxBytes + 1, 0)};
    b.header.tx_root = merkle::rfc6962_root(b.txs);
    b.signature = dev::sign_header(b.header, producer_key());
    CHECK(reject_reason(c, b) == Reject::LimitExceeded);
    CHECK(c.engine().info().last_height == 0);
}
