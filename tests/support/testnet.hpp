// SPDX-License-Identifier: Apache-2.0
//
// Deterministic test network: a genesis plus a reproducible journal of blocks with mostly valid
// and some deliberately invalid transactions. Also an external journal file format used by
// replay and crash tests (test-only; the real dev-block format is M1.4).
#pragma once

#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "ledger_fixture.hpp"
#include "yap/chain/engine.hpp"
#include "yap/chain/recovery.hpp"

namespace yap::test {

inline Bytes burn_tx_until(const crypto::SecretKey& from, std::uint64_t amount, std::uint64_t nonce,
                           std::uint64_t expires) {
    tx::TxBody b;
    b.chain_id = kTestChain;
    b.sender_pubkey = from.public_key();
    b.nonce = nonce;
    b.fee = 1000;
    b.expires_at_height = expires;
    b.payload = tx::Burn{amount};
    return ycs::encode(tx::sign_tx(b, from));
}

struct TestNet {
    chain::Genesis genesis;
    std::vector<crypto::SecretKey> users;

    static TestNet make(std::size_t n_users = 40) {
        TestNet t;
        t.genesis.chain_id = kTestChain;
        t.genesis.genesis_time_ms = 1'767'225'600'000;
        t.genesis.params = tx::testnet_params();
        for (std::size_t i = 0; i < n_users; ++i) {
            crypto::Seed s{};
            s.fill(static_cast<Byte>(i));
            s[31] = 0xA5;
            t.users.push_back(crypto::SecretKey::from_seed(s));
            t.genesis.accounts.push_back({id_of(t.users.back()), 1'000'000'000});
        }
        std::sort(t.genesis.accounts.begin(), t.genesis.accounts.end(),
                  [](const auto& a, const auto& b) { return a.account_id < b.account_id; });
        for (Byte i = 0; i < 4; ++i) {
            crypto::Seed s{};
            s.fill(static_cast<Byte>(200 + i));
            const auto k = crypto::SecretKey::from_seed(s);
            crypto::Seed r{};
            r.fill(static_cast<Byte>(210 + i));
            t.genesis.validators.push_back({k.public_key(), 10, id_of(crypto::SecretKey::from_seed(r)), "val"});
        }
        std::sort(t.genesis.validators.begin(), t.genesis.validators.end(), [](const auto& a, const auto& b) {
            return state::consensus_address(a.consensus_pubkey) < state::consensus_address(b.consensus_pubkey);
        });
        return t;
    }

    /// `n_blocks` blocks of ~`txs_per_block` transactions (about 10 % invalid: nonce gaps, replays,
    /// insufficient funds, low fees). Deterministic for a given seed.
    [[nodiscard]] std::vector<chain::ReplayBlock> journal(std::size_t n_blocks, std::size_t txs_per_block,
                                                          std::uint64_t seed) const {
        std::mt19937_64 rng(seed);
        std::map<crypto::AccountId, std::uint64_t> balance, nonce;
        for (const auto& a : genesis.accounts) balance[a.account_id] = a.balance;
        std::vector<chain::ReplayBlock> out;
        for (std::size_t b = 0; b < n_blocks; ++b) {
            chain::ReplayBlock rb;
            rb.ctx.chain_id = genesis.chain_id;
            rb.ctx.height = b + 1;
            rb.ctx.time_ms = genesis.genesis_time_ms + (b + 1) * 2'000;
            if (b > 0) {
                for (const auto& v : genesis.validators) {
                    const auto flag = static_cast<std::uint8_t>(1 + rng() % 3);  // Absent / Commit / Nil
                    rb.ctx.last_commit.push_back({state::consensus_address(v.consensus_pubkey), v.power, flag});
                }
            }
            for (std::size_t i = 0; i < txs_per_block; ++i) {
                const auto& from = users[rng() % users.size()];
                const auto& to = users[rng() % users.size()];
                const auto fid = id_of(from);
                const auto tid = id_of(to);
                if (fid == tid) continue;
                const auto roll = rng() % 20;
                const std::uint64_t n = nonce[fid];
                if (roll == 0) {  // nonce gap
                    rb.ctx.txs.push_back(transfer_tx(from, tid, 1, n + 2, 1000, rb.ctx.height + 100));
                } else if (roll == 1) {  // insufficient funds
                    rb.ctx.txs.push_back(transfer_tx(from, tid, balance[fid] + 1, n, 1000, rb.ctx.height + 100));
                } else if (roll == 2) {  // fee below minimum
                    rb.ctx.txs.push_back(transfer_tx(from, tid, 1, n, 999, rb.ctx.height + 100));
                } else if (roll == 3 && balance[fid] > 2'000) {  // burn
                    const std::uint64_t amount = 1 + rng() % 500;
                    rb.ctx.txs.push_back(burn_tx_until(from, amount, n, rb.ctx.height + 100));
                    balance[fid] -= amount + 1000;
                    ++nonce[fid];
                } else if (balance[fid] > 2'000) {  // valid transfer
                    const std::uint64_t amount = 1 + rng() % std::min<std::uint64_t>(balance[fid] - 1'000, 50'000);
                    rb.ctx.txs.push_back(transfer_tx(from, tid, amount, n, 1000, rb.ctx.height + 100));
                    balance[fid] -= amount + 1000;
                    balance[tid] += amount;
                    ++nonce[fid];
                }
            }
            out.push_back(std::move(rb));
        }
        return out;
    }
};

/// Runs `blocks` on a fresh in-memory engine and stores the resulting roots as expectations.
inline std::vector<Hash32> reference_roots(const TestNet& net, std::vector<chain::ReplayBlock>& blocks) {
    storage::MemoryStore store;
    chain::Engine e(store);
    e.init_chain(net.genesis);
    std::vector<Hash32> roots;
    chain::replay(e, blocks, [&](const chain::ReplayBlock&, const state::BlockResult& r) {
        roots.push_back(r.state_root);
    });
    for (std::size_t i = 0; i < blocks.size(); ++i) blocks[i].expected_root = roots[i];
    return roots;
}

// ------------------------------------------------------------------ external journal file

inline void encode_block(ycs::Writer& w, const chain::ReplayBlock& b) {
    w.string(b.ctx.chain_id);
    w.u64(b.ctx.height);
    w.u64(b.ctx.time_ms);
    w.length(b.ctx.last_commit.size());
    for (const auto& v : b.ctx.last_commit) {
        w.fixed(v.validator_address);
        w.u64(v.power);
        w.u8(v.flag);
    }
    w.length(b.ctx.txs.size());
    for (const auto& t : b.ctx.txs) w.bytes(t);
    w.option_tag(b.expected_root.has_value());
    if (b.expected_root) w.fixed(*b.expected_root);
}

inline void write_journal(const std::string& path, const std::vector<chain::ReplayBlock>& blocks) {
    ycs::Writer w;
    w.length(blocks.size());
    for (const auto& b : blocks) encode_block(w, b);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto& d = w.data();
    out.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    if (!out) throw std::runtime_error("cannot write journal " + path);
}

inline std::vector<chain::ReplayBlock> read_journal(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    const Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ycs::Reader r(data);
    std::vector<chain::ReplayBlock> blocks(r.vec_length(1'000'000, 1));
    for (auto& b : blocks) {
        b.ctx.chain_id = r.string(32);
        b.ctx.height = r.u64();
        b.ctx.time_ms = r.u64();
        b.ctx.last_commit.resize(r.vec_length(32, 29));
        for (auto& v : b.ctx.last_commit) {
            v.validator_address = r.fixed<20>();
            v.power = r.u64();
            v.flag = r.u8();
        }
        b.ctx.txs.resize(r.vec_length(100'000, 4));
        for (auto& t : b.ctx.txs) t = r.bytes(65'536);
        if (r.option_tag()) b.expected_root = r.fixed<32>();
    }
    if (!r.finish()) throw std::runtime_error("malformed journal " + path);
    return blocks;
}

}  // namespace yap::test
