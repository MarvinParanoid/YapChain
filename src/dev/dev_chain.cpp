// SPDX-License-Identifier: Apache-2.0
#include "yap/dev/dev_chain.hpp"

#include "yap/common/hex.hpp"
#include "yap/merkle/merkle.hpp"
#include "yap/state/state.hpp"

namespace yap::dev {

using storage::Table;

std::string_view to_string(Reject r) noexcept {
    switch (r) {
        case Reject::BadVersion: return "unsupported block version";
        case Reject::WrongChain: return "wrong chain id";
        case Reject::AlreadyHave: return "height already committed";
        case Reject::HeightGap: return "height gap";
        case Reject::WrongParent: return "previous block hash mismatch";
        case Reject::BadTime: return "invalid block time";
        case Reject::WrongProducer: return "block not produced by the configured dev key";
        case Reject::BadSignature: return "invalid producer signature";
        case Reject::BadTxRoot: return "tx_root mismatch";
        case Reject::InvalidTransaction: return "block contains an invalid transaction";
        case Reject::BadStateRoot: return "state_root mismatch";
        case Reject::Conflict: return "a different block is stored at this height";
    }
    return "rejected";
}

void DevConfig::encode_to(ycs::Writer& w) const {
    w.u32(version);
    w.fixed(producer);
}

DevConfig DevConfig::decode_from(ycs::Reader& r) {
    DevConfig c;
    c.version = r.u32();
    if (r.ok() && c.version != 1) r.fail(ycs::DecodeError::InvalidContent);
    c.producer = r.fixed<32>();
    return c;
}

Bytes dev_config_key() {
    const ByteView k = as_bytes("yapchain/dev");
    return Bytes(k.begin(), k.end());
}

Bytes block_key(std::uint64_t height) { return chain::results_key(height); }

bool DevChain::init(storage::KvStore& store, const chain::Genesis& genesis, const crypto::PublicKey& producer) {
    chain::Engine e(store);
    if (e.initialized()) {
        const auto raw = store.get(Table::Meta, dev_config_key());
        if (!raw) throw std::invalid_argument("store holds a chain that is not a dev chain");
        const auto cfg = ycs::decode<DevConfig>(*raw);
        if (!cfg || cfg->producer != producer || e.meta().genesis_hash != chain::genesis_hash(genesis)) {
            throw std::invalid_argument("store already holds a different chain (genesis or producer key differs)");
        }
        return false;  // re-init never touches an existing chain
    }
    storage::WriteBatch extra;
    extra.put(Table::Meta, dev_config_key(), ycs::encode(DevConfig{1, producer}));
    e.init_chain(genesis, extra);
    return true;
}

DevChain::DevChain(storage::KvStore& store) : store_(&store), engine_(store) {
    if (!engine_.initialized()) throw std::logic_error("dev chain is not initialised");
    const auto raw = store_->get(Table::Meta, dev_config_key());
    if (!raw) throw chain::StoreCorruption("dev-chain configuration is missing");
    auto cfg = ycs::decode<DevConfig>(*raw);
    if (!cfg) throw chain::StoreCorruption("dev-chain configuration is corrupted");
    config_ = *cfg;
    // The block at the committed height must exist (it is written in the same transaction).
    const auto h = engine_.info().last_height;
    if (h > 0 && !block(h)) throw chain::StoreCorruption("dev block of the committed height is missing");
}

std::optional<DevBlock> DevChain::block(std::uint64_t height) const {
    const auto raw = store_->get(Table::Blocks, block_key(height));
    if (!raw) return std::nullopt;
    auto b = ycs::decode<DevBlock>(*raw);
    if (!b) throw chain::StoreCorruption("stored dev block " + std::to_string(height) + " cannot be decoded");
    return std::move(b).value();
}

Hash32 DevChain::tip_hash() const {
    const auto h = engine_.info().last_height;
    if (h == 0) return engine_.meta().genesis_hash;
    return block_hash(block(h)->header);
}

state::BlockContext DevChain::context_for(const DevBlock& b) const {
    state::BlockContext ctx;
    ctx.chain_id = b.header.chain_id;
    ctx.height = b.header.height;
    ctx.time_ms = b.header.time_ms;
    ctx.txs = b.txs;
    std::vector<std::pair<state::ConsensusAddress, std::uint64_t>> validators;
    engine_.committed().for_each([&](const Bytes& k, const Bytes& v) {
        if (k.size() == 21 && k[0] == state::ns::kValidator) {
            auto info = ycs::decode<state::ValidatorInfo>(v);
            if (!info) throw chain::StoreCorruption("corrupted ValidatorInfo");
            state::ConsensusAddress a{};
            std::copy(k.begin() + 1, k.end(), a.begin());
            validators.emplace_back(a, info->power);
        }
    });
    if (!validators.empty()) ctx.proposer_address = validators.front().first;  // ascending key order
    if (b.header.height > 1) {
        for (const auto& [addr, power] : validators) {
            ctx.last_commit.push_back({addr, power, static_cast<std::uint8_t>(state::BlockIdFlag::Commit)});
        }
    }
    return ctx;
}

state::BlockResult DevChain::import(const DevBlock& b) {
    const auto& h = b.header;
    const auto info = engine_.info();
    auto reject = [](Reject r, const std::string& d) { return BlockRejected(r, d); };

    if (h.version != kDevBlockVersion) throw reject(Reject::BadVersion, std::to_string(h.version));
    if (h.chain_id != info.chain_id) throw reject(Reject::WrongChain, h.chain_id);
    if (h.height <= info.last_height) throw reject(Reject::AlreadyHave, "height " + std::to_string(h.height));
    if (h.height != info.last_height + 1) {
        throw reject(Reject::HeightGap, "expected " + std::to_string(info.last_height + 1) + ", got " +
                                            std::to_string(h.height));
    }
    if (h.prev_block_hash != tip_hash()) throw reject(Reject::WrongParent, to_hex(h.prev_block_hash));
    state::Overlay view(&engine_.committed());
    const auto last_time = state::State(view).chain_state().last_time_ms;  // genesis time at height 0
    if (h.height == 1 ? h.time_ms < last_time : h.time_ms <= last_time) {
        throw reject(Reject::BadTime, std::to_string(h.time_ms) + " vs previous " + std::to_string(last_time));
    }
    if (h.producer_pubkey != config_.producer) throw reject(Reject::WrongProducer, to_hex(h.producer_pubkey));
    if (!verify_header_signature(b)) throw reject(Reject::BadSignature, "height " + std::to_string(h.height));
    if (merkle::rfc6962_root(b.txs) != h.tx_root) throw reject(Reject::BadTxRoot, to_hex(h.tx_root));

    const auto result = engine_.finalize_block(context_for(b));
    try {
        for (std::size_t i = 0; i < result.tx_results.size(); ++i) {
            if (result.tx_results[i].code != tx::TxCode::Ok) {
                throw reject(Reject::InvalidTransaction,
                             "tx " + std::to_string(i) + ": " + std::string(tx::to_string(result.tx_results[i].code)));
            }
        }
        if (result.state_root != h.state_root) {
            throw reject(Reject::BadStateRoot,
                         "computed " + to_hex(result.state_root) + ", header " + to_hex(h.state_root));
        }
        storage::WriteBatch extra;
        extra.put(Table::Blocks, block_key(h.height), ycs::encode(b));
        engine_.commit(extra);  // block and state in one transaction
    } catch (...) {
        // Rejections and failed writes (e.g. StorageFull) leave the chain usable for the next attempt.
        engine_.abort_pending();
        throw;
    }
    return result;
}

DevBlock DevChain::produce(const crypto::SecretKey& producer, const std::vector<Bytes>& candidates,
                           std::optional<std::uint64_t> time_ms, std::vector<tx::TxCode>* codes) {
    if (producer.public_key() != config_.producer) {
        throw BlockRejected(Reject::WrongProducer, "this key is not the configured dev producer");
    }
    const auto info = engine_.info();
    state::Overlay view(&engine_.committed());
    const auto last_time = state::State(view).chain_state().last_time_ms;

    DevBlock b;
    b.header.chain_id = info.chain_id;
    b.header.height = info.last_height + 1;
    b.header.time_ms = time_ms ? *time_ms : last_time + 2'000;
    b.header.prev_block_hash = tip_hash();
    b.header.producer_pubkey = producer.public_key();

    // Select the valid candidates (bounded by the dev-block limits), then execute to get the root.
    if (candidates.size() > kMaxDevBlockTxs) {
        throw BlockRejected(Reject::InvalidTransaction, "more than 10000 candidate transactions");
    }
    for (const auto& t : candidates) {
        if (t.size() > kMaxDevBlockTxBytes) throw BlockRejected(Reject::InvalidTransaction, "transaction above 65536 bytes");
    }
    auto ctx = context_for(b);
    b.txs = engine_.prepare_proposal(ctx, candidates, UINT64_MAX, codes);
    ctx.txs = b.txs;
    const auto result = engine_.finalize_block(ctx);
    engine_.abort_pending();
    b.header.tx_root = merkle::rfc6962_root(b.txs);
    b.header.state_root = result.state_root;
    b.signature = sign_header(b.header, producer);
    import(b);  // the producer's own block passes exactly the same validation
    return b;
}

ReplayStats DevChain::replay(std::span<const DevBlock> blocks) {
    ReplayStats st;
    for (const auto& b : blocks) {
        if (b.header.height <= engine_.info().last_height) {
            const auto stored = block(b.header.height);
            if (!stored || block_hash(stored->header) != block_hash(b.header)) {
                throw BlockRejected(Reject::Conflict, "height " + std::to_string(b.header.height));
            }
            ++st.skipped;
            continue;
        }
        import(b);
        ++st.imported;
    }
    return st;
}

}  // namespace yap::dev
