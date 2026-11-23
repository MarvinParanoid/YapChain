// SPDX-License-Identifier: Apache-2.0
// yap dev init|produce|import|export|block|replay|state-root, yap verify-state
#include <CLI11.hpp>

#include <memory>

#include "commands.hpp"
#include "local_chain.hpp"
#include "yap/chain/recovery.hpp"
#include "yap/common/hex.hpp"
#include "yap/dev/dev_chain.hpp"
#include "yap/wallet/amount.hpp"

namespace yap::cli {
namespace {

std::unique_ptr<storage::LmdbStore> open_store(const Context& ctx, bool read_only, bool create) {
    if (!create && !std::filesystem::exists(ctx.data_dir() / "data.mdb")) {
        throw CliError(kExitRejected, "no local chain at " + ctx.data_dir().string() + " (run `yap dev init`)");
    }
    storage::LmdbStore::Options o{ctx.data_dir()};
    o.read_only = read_only;
    o.create = create;
    return std::make_unique<storage::LmdbStore>(o);
}

void print_block(const dev::DevBlock& b) {
    out("height", b.header.height);
    out("block_hash", to_hex(dev::block_hash(b.header)));
    out("prev_block_hash", to_hex(b.header.prev_block_hash));
    out("time_ms", b.header.time_ms);
    out("txs", b.txs.size());
    out("tx_root", to_hex(b.header.tx_root));
    out("state_root", to_hex(b.header.state_root));
    out("producer", to_hex(b.header.producer_pubkey));
}

Bytes encode_journal(const std::vector<dev::DevBlock>& blocks) {
    ycs::Writer w;
    w.length(blocks.size());
    for (const auto& b : blocks) b.encode_to(w);
    return std::move(w).take();
}

std::vector<dev::DevBlock> decode_journal(ByteView data) {
    ycs::Reader r(data);
    const auto n = r.vec_length(10'000'000, 1);
    std::vector<dev::DevBlock> blocks;
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) blocks.push_back(dev::DevBlock::decode_from(r));
    if (!r.finish()) throw CliError(kExitRejected, "malformed journal");
    return blocks;
}

std::uint64_t decimal_or_fail(const std::string& text, const char* what) {
    const auto v = wallet::parse_decimal_u64(text);
    if (!v) throw CliError(kExitRejected, std::string("invalid ") + what + " '" + text + "' (decimal digits only)");
    return *v;
}

std::string reject_message(const dev::BlockRejected& e) { return std::string("block rejected: ") + e.what(); }

/// The dev chain of an opened store; an uninitialised database is a user error (exit 2).
std::unique_ptr<dev::DevChain> open_dev_chain(storage::LmdbStore& store) {
    {
        chain::Engine probe(store);
        if (!probe.initialized()) throw CliError(kExitRejected, "the local chain is not initialised (run `yap dev init`)");
    }
    return std::make_unique<dev::DevChain>(store);
}

}  // namespace

void register_dev(CLI::App& app, Context& ctx) {
    auto* dv = app.add_subcommand("dev", "Local development chain (single producer; not a network)");
    dv->require_subcommand(1);

    // init
    auto init = std::make_shared<std::pair<std::string, std::string>>();
    auto* i = dv->add_subcommand("init", "Initialise <home>/data from a genesis file (never overwrites a chain)");
    i->add_option("--genesis", init->first, "Genesis JSON")->required();
    i->add_option("--producer-pubkey", init->second,
                  "Import-only node: trust this producer key (hex) instead of creating/using <home>/dev/producer.json");
    i->callback([&ctx, init] {
        const auto g = load_genesis(init->first);
        crypto::PublicKey producer{};
        // An existing chain is never re-initialised, and never gets a fresh producer key.
        if (std::filesystem::exists(ctx.data_dir() / "data.mdb")) {
            auto existing = open_store(ctx, true, false);
            chain::Engine probe(*existing);
            if (probe.initialized()) {
                const auto raw = existing->get(storage::Table::Meta, dev::dev_config_key());
                const auto cfg = raw ? ycs::decode<dev::DevConfig>(*raw) : Result<dev::DevConfig, ycs::DecodeError>(
                                                                            unexpected(ycs::DecodeError::Truncated));
                if (!cfg || probe.meta().genesis_hash != chain::genesis_hash(g)) {
                    throw CliError(kExitRejected, "the local database already holds a different chain");
                }
                if (init->second.empty() && !std::filesystem::exists(ctx.producer_key_path())) {
                    // Import-only node, or the key was moved away: do not invent a new producer.
                    init->second = to_hex(cfg->producer);
                }
            }
        }
        if (!init->second.empty()) {
            auto pk = from_hex_fixed<32>(init->second);
            if (!pk) throw CliError(kExitRejected, "--producer-pubkey must be 64 lowercase hex characters");
            producer = *pk;
        } else if (std::filesystem::exists(ctx.producer_key_path())) {
            producer = load_key_info(ctx.producer_key_path(), wallet::KeyPurpose::DevProducer).public_key;
        } else {
            producer = create_key_file(ctx, ctx.producer_key_path(), wallet::KeyPurpose::DevProducer).public_key;
        }
        auto store = open_store(ctx, false, true);
        bool created = false;
        try {
            created = dev::DevChain::init(*store, g, producer);
        } catch (const std::invalid_argument& e) {
            throw CliError(kExitRejected, e.what());
        }
        dev::DevChain chain(*store);
        out("created", created ? "yes" : "no (already initialised with this genesis and producer)");
        out("chain_id", g.chain_id);
        out("genesis_hash", to_hex(chain.engine().meta().genesis_hash));
        out("height", chain.engine().info().last_height);
        out("state_root", to_hex(chain.engine().info().last_state_root));
        out("producer", to_hex(producer));
    });

    // produce
    struct ProduceArgs {
        std::vector<std::string> txs;
        std::string time_ms;
        bool require_all = false;
    };
    auto pa = std::make_shared<ProduceArgs>();
    auto* p = dv->add_subcommand("produce", "Produce and commit the next block from signed transaction files");
    p->add_option("--tx", pa->txs, "Signed transaction file (hex), repeatable");
    p->add_option("--time-ms", pa->time_ms, "Block time (default: previous + 2000 ms)");
    p->add_flag("--require-all", pa->require_all, "Fail instead of leaving out invalid transactions");
    p->callback([&ctx, pa] {
        std::optional<std::uint64_t> time_ms;
        if (!pa->time_ms.empty()) time_ms = decimal_or_fail(pa->time_ms, "--time-ms");
        std::vector<Bytes> candidates;
        for (const auto& f : pa->txs) candidates.push_back(read_hex_file(f));
        auto store = open_store(ctx, false, false);
        auto chain_ptr = open_dev_chain(*store);
        auto& chain = *chain_ptr;
        if (pa->require_all) {  // dry run with codes before asking for the password
            std::vector<tx::TxCode> codes;
            auto probe = chain.context_for(dev::DevBlock{});
            probe.height = chain.engine().info().last_height + 1;
            (void)chain.engine().prepare_proposal(probe, candidates, UINT64_MAX, &codes);
            for (std::size_t k = 0; k < codes.size(); ++k) {
                if (codes[k] != tx::TxCode::Ok) {
                    throw CliError(kExitRejected, pa->txs[k] + ": " + std::string(tx::to_string(codes[k])) +
                                                      " (nothing produced, --require-all)");
                }
            }
        }
        const auto key = unlock_key(ctx, ctx.producer_key_path(), wallet::KeyPurpose::DevProducer);
        std::vector<tx::TxCode> codes;
        dev::DevBlock b;
        try {
            b = chain.produce(key, candidates, time_ms, &codes);
        } catch (const dev::BlockRejected& e) {
            throw CliError(kExitRejected, reject_message(e));
        }
        print_block(b);
        for (std::size_t k = 0; k < codes.size(); ++k) {
            const auto id = to_hex(tx::tx_id(candidates[k]));
            if (codes[k] == tx::TxCode::Ok) {
                out("included", id);
            } else {
                std::cout << "excluded: " << id << " " << tx::to_string(codes[k]) << " (" << pa->txs[k] << ")\n";
            }
        }
    });

    // import
    auto imp = std::make_shared<std::string>();
    auto* im = dv->add_subcommand("import", "Fully validate and import one dev block (hex file)");
    im->add_option("file", *imp, "Block file")->required();
    im->callback([&ctx, imp] {
        auto b = ycs::decode<dev::DevBlock>(read_hex_file(*imp));
        if (!b) throw CliError(kExitRejected, "not a dev block: " + std::string(ycs::to_string(b.error())));
        auto store = open_store(ctx, false, false);
        auto chain_ptr = open_dev_chain(*store);
        auto& chain = *chain_ptr;
        try {
            chain.import(*b);
        } catch (const dev::BlockRejected& e) {
            throw CliError(kExitRejected, reject_message(e));
        }
        out("imported", "yes");
        print_block(*b);
    });

    // block
    auto bh = std::make_shared<std::pair<std::string, std::string>>();
    auto* bl = dv->add_subcommand("block", "Show a stored block and optionally write it to a file");
    bl->add_option("--height", bh->first, "Height")->required();
    bl->add_option("--out", bh->second, "Write the block (hex) to this file");
    bl->callback([&ctx, bh] {
        auto store = open_store(ctx, true, false);
        auto chain_ptr = open_dev_chain(*store);
        const auto height = decimal_or_fail(bh->first, "--height");
        const auto b = chain_ptr->block(height);
        if (!b) throw CliError(kExitRejected, "no block at height " + std::to_string(height));
        print_block(*b);
        if (!bh->second.empty()) write_file_bytes(bh->second, as_bytes(to_hex(ycs::encode(*b)) + "\n"), ctx.force);
    });

    // export
    auto exp = std::make_shared<std::string>();
    auto* ex = dv->add_subcommand("export", "Write all blocks to a journal file (hex)");
    ex->add_option("--out", *exp, "Journal file")->required();
    ex->callback([&ctx, exp] {
        auto store = open_store(ctx, true, false);
        auto chain_ptr = open_dev_chain(*store);
        std::vector<dev::DevBlock> blocks;
        for (std::uint64_t h = 1; h <= chain_ptr->engine().info().last_height; ++h) {
            auto b = chain_ptr->block(h);
            if (!b) throw chain::StoreCorruption("dev block " + std::to_string(h) + " is missing");
            blocks.push_back(std::move(*b));
        }
        write_file_bytes(*exp, as_bytes(to_hex(encode_journal(blocks)) + "\n"), ctx.force);
        out("blocks", blocks.size());
        out("journal", *exp);
    });

    // replay
    auto rp = std::make_shared<std::string>();
    auto* re = dv->add_subcommand("replay", "Import a journal; identical stored blocks are skipped");
    re->add_option("--journal", *rp, "Journal file")->required();
    re->callback([&ctx, rp] {
        const auto blocks = decode_journal(read_hex_file(*rp));
        auto store = open_store(ctx, false, false);
        auto chain_ptr = open_dev_chain(*store);
        auto& chain = *chain_ptr;
        dev::ReplayStats st;
        try {
            st = chain.replay(blocks);
        } catch (const dev::BlockRejected& e) {
            throw CliError(kExitRejected, reject_message(e));
        }
        out("imported", st.imported);
        out("skipped", st.skipped);
        out("height", chain.engine().info().last_height);
        out("state_root", to_hex(chain.engine().info().last_state_root));
    });

    // state-root
    auto sr = std::make_shared<std::string>();
    auto* s = dv->add_subcommand("state-root", "State root of the last (or a given) committed height");
    s->add_option("--height", *sr, "Height (0 = genesis)");
    s->callback([&ctx, sr] {
        auto store = open_store(ctx, true, false);
        chain::Engine e(*store);
        if (!e.initialized()) throw CliError(kExitRejected, "local chain is not initialised");
        const std::uint64_t h = sr->empty() ? e.info().last_height : decimal_or_fail(*sr, "--height");
        const auto rec = e.block_record(h);
        if (!rec) throw CliError(kExitRejected, "no committed height " + std::to_string(h));
        out("height", h);
        out("state_root", to_hex(rec->state_root));
    });

    // verify-state (top level)
    auto* vs = app.add_subcommand("verify-state", "Fully verify the local database (read-only)");
    vs->callback([&ctx] {
        auto store = open_store(ctx, true, false);
        const auto rep = chain::verify_state(*store);
        out("status", "ok");
        out("height", rep.height);
        out("state_root", to_hex(rep.state_root));
        out("state_entries", rep.state_entries);
        out("smt_leaves", rep.smt_leaves);
        out("smt_internal_nodes", rep.smt_internal_nodes);
    });
}

}  // namespace yap::cli
