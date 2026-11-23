// SPDX-License-Identifier: Apache-2.0
// yap tx build|sign|inspect|transfer|burn — offline transaction tooling (never broadcasts).
#include <CLI11.hpp>

#include <unistd.h>

#include <algorithm>
#include <memory>

#include "commands.hpp"
#include "local_chain.hpp"
#include "yap/common/hex.hpp"
#include "yap/tx/chain_id.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/tx/stateless.hpp"
#include "yap/wallet/amount.hpp"

namespace yap::cli {
namespace {

constexpr std::uint64_t kDefaultExpiryWindow = 100;

struct BuildArgs {
    std::string type = "transfer";
    std::string from;          // key name
    std::string from_pubkey;   // or a raw public key (hex)
    std::string to;
    std::string amount;
    std::string fee;
    std::string nonce;    // strict decimal; empty = from the local chain
    std::string expires;  // strict decimal; empty = from the local chain
    std::string memo;
    std::string chain_id;
    std::string out;
    bool yes = false;
};

std::uint64_t amount_or_fail(const std::string& text, const char* what) {
    const auto v = wallet::parse_amount(text);
    if (!v) {
        throw CliError(kExitRejected, std::string("invalid ") + what + " '" + text +
                                          "' (use an integer number of yip, or e.g. 1.5YAP)");
    }
    return *v;
}

crypto::AccountId address_or_fail(const std::string& text) {
    auto id = crypto::decode_address(text, crypto::kTestnetHrp);
    if (!id) throw CliError(kExitRejected, "invalid address '" + text + "': " + std::string(crypto::to_string(id.error())));
    return *id;
}

std::optional<std::uint64_t> decimal_opt(const std::string& text, const char* what) {
    if (text.empty()) return std::nullopt;
    const auto v = wallet::parse_decimal_u64(text);
    if (!v) throw CliError(kExitRejected, std::string("invalid ") + what + " '" + text + "' (decimal digits only)");
    return v;
}

tx::TxBody build_body(const Context& ctx, const BuildArgs& a) {
    struct Parsed {
        std::optional<std::uint64_t> nonce, expires;
    };
    const Parsed n{decimal_opt(a.nonce, "--nonce"), decimal_opt(a.expires, "--expires")};
    tx::TxBody b;
    if (!a.from.empty() == !a.from_pubkey.empty()) throw CliError(kExitUsage, "give exactly one of --from / --from-pubkey");
    if (!a.from.empty()) {
        b.sender_pubkey = load_key_info(ctx.key_path(a.from), wallet::KeyPurpose::Account).public_key;
    } else {
        auto pk = from_hex_fixed<32>(a.from_pubkey);
        if (!pk) throw CliError(kExitRejected, "--from-pubkey must be 64 lowercase hex characters");
        b.sender_pubkey = *pk;
    }

    // Missing chain id / nonce / expiry / fee come from the local chain, if there is one.
    const bool need_local = a.chain_id.empty() || !n.nonce || !n.expires;
    std::optional<LocalChain> local = open_local_chain(ctx, /*read_only=*/true, /*required=*/false);
    if (need_local && !local) {
        throw CliError(kExitRejected, "no local chain: pass --chain-id, --nonce and --expires explicitly");
    }
    if (!a.chain_id.empty() && !tx::is_valid_chain_id(a.chain_id)) {
        throw CliError(kExitRejected, "invalid --chain-id (allowed: [a-z0-9-], 1..32, no leading/trailing '-')");
    }
    b.chain_id = a.chain_id.empty() ? local->engine->info().chain_id : a.chain_id;
    if (n.nonce) {
        b.nonce = *n.nonce;
    } else {
        b.nonce = local->account(b.sender()).value_or(state::AccountState{}).nonce;
    }
    if (n.expires) {
        b.expires_at_height = *n.expires;
    } else {  // within the chain's TTL limit (T2), never more than the default window
        b.expires_at_height = local->engine->info().last_height +
                              std::min<std::uint64_t>(kDefaultExpiryWindow, local->params().max_tx_ttl_blocks);
    }
    if (!a.fee.empty()) {
        b.fee = amount_or_fail(a.fee, "fee");
    } else {
        b.fee = local ? local->params().min_fee : tx::testnet_params().min_fee;
    }
    if (a.memo.size() > tx::kMemoCeiling) throw CliError(kExitRejected, "memo longer than 1024 bytes");
    b.memo.assign(a.memo.begin(), a.memo.end());

    if (a.type == "transfer") {
        if (a.to.empty() || a.amount.empty()) throw CliError(kExitUsage, "transfer needs --to and --amount");
        b.payload = tx::Transfer{address_or_fail(a.to), amount_or_fail(a.amount, "amount")};
    } else if (a.type == "burn") {
        if (!a.to.empty()) throw CliError(kExitUsage, "burn takes no --to");
        if (a.amount.empty()) throw CliError(kExitUsage, "burn needs --amount");
        b.payload = tx::Burn{amount_or_fail(a.amount, "amount")};
    } else {
        throw CliError(kExitUsage, "--type must be transfer or burn");
    }
    // Catch obvious mistakes before signing (the chain re-checks everything anyway).
    if (auto* t = std::get_if<tx::Transfer>(&b.payload); t && (t->amount == 0 || t->to == b.sender())) {
        throw CliError(kExitRejected, "transfer amount must be >= 1 yip and the recipient must differ from the sender");
    }
    if (auto* bn = std::get_if<tx::Burn>(&b.payload); bn && bn->amount == 0) {
        throw CliError(kExitRejected, "burn amount must be >= 1 yip");
    }
    return b;
}

void print_body(const tx::TxBody& b) {
    out("type", tx::payload_tag(b.payload) == tx::PayloadTag::Transfer ? "transfer"
                : tx::payload_tag(b.payload) == tx::PayloadTag::Burn  ? "burn"
                                                                       : "claim_achievement");
    out("chain_id", b.chain_id);
    out("version", b.version);
    out("sender", crypto::encode_address(crypto::kTestnetHrp, b.sender()));
    out("sender_pubkey", to_hex(b.sender_pubkey));
    out("nonce", b.nonce);
    out("fee", wallet::format_amount(b.fee));
    out("expires_at_height", b.expires_at_height);
    if (const auto* t = std::get_if<tx::Transfer>(&b.payload)) {
        out("to", crypto::encode_address(crypto::kTestnetHrp, t->to));
        out("amount", wallet::format_amount(t->amount));
    } else if (const auto* bn = std::get_if<tx::Burn>(&b.payload)) {
        out("amount", wallet::format_amount(bn->amount));
    } else {  // every signed field of the attestation is shown before signing
        const auto& c = std::get<tx::ClaimAchievement>(b.payload).attestation;
        const auto& a = c.attestation;
        out("attestation_version", a.version);
        out("attestation_chain_id", a.chain_id);
        out("issuer_id", a.issuer_id);
        out("key_id", a.key_id);
        out("achievement_id", a.achievement_id);
        out("subject", crypto::encode_address(crypto::kTestnetHrp, a.subject));
        out("claim_id", to_hex(a.claim_id));
        out("evidence_hash", to_hex(a.evidence_hash));
        out("issued_at_s", a.issued_at_s);
        out("expires_at_s", a.expires_at_s);
        out("issuer_signature", to_hex(c.issuer_signature));
    }
    out("memo_hex", to_hex(b.memo));
}

void write_hex(const Context& ctx, const std::string& path, ByteView data) {
    const std::string h = to_hex(data) + "\n";
    write_file_bytes(path, as_bytes(h), ctx.force);
}

tx::SignedTx sign_body(const Context& ctx, const tx::TxBody& body, const std::string& key_name, bool yes) {
    if (!yes && ::isatty(STDIN_FILENO) != 0 && ctx.password_file.empty()) {
        std::cerr << "Sign this transaction? [y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y" && answer != "yes") throw CliError(kExitRejected, "not signed");
    }
    const auto key = unlock_key(ctx, ctx.key_path(key_name), wallet::KeyPurpose::Account);
    if (key.public_key() != body.sender_pubkey) {
        throw CliError(kExitRejected, "key '" + key_name + "' is not the sender of this transaction");
    }
    return tx::sign_tx(body, key);
}

void add_build_options(CLI::App* c, BuildArgs& a, bool with_type) {
    if (with_type) c->add_option("--type", a.type, "transfer | burn")->capture_default_str();
    c->add_option("--from", a.from, "Sender key name");
    c->add_option("--from-pubkey", a.from_pubkey, "Sender public key (hex), instead of --from");
    if (a.type == "transfer" || with_type) c->add_option("--to", a.to, "Recipient address (tyap1...)");
    c->add_option("--amount", a.amount, "Amount: integer yip, or decimal YAP like 1.5YAP")->required();
    c->add_option("--fee", a.fee, "Fee (default: min_fee of the local chain, else 1000 yip)");
    c->add_option("--nonce", a.nonce, "Sender nonce (default: from the local chain)");
    c->add_option("--expires", a.expires, "Last valid height (default: local height + 100)");
    c->add_option("--memo", a.memo, "Memo text (stored as bytes)");
    c->add_option("--chain-id", a.chain_id, "Chain id (default: from the local chain)");
    c->add_option("--out", a.out, "Output file (hex)")->required();
}

}  // namespace

void register_tx(CLI::App& app, Context& ctx) {
    auto* txc = app.add_subcommand("tx", "Build, sign and inspect transactions (offline, never broadcast)");
    txc->require_subcommand(1);

    auto build = std::make_shared<BuildArgs>();
    auto* b = txc->add_subcommand("build", "Build an unsigned transaction body");
    add_build_options(b, *build, true);
    b->callback([&ctx, build] {
        const auto body = build_body(ctx, *build);
        write_hex(ctx, build->out, ycs::encode(body));
        print_body(body);
        out("body_file", build->out);
    });

    auto sign = std::make_shared<std::tuple<std::string, std::string, std::string, bool>>();
    auto* s = txc->add_subcommand("sign", "Show an unsigned body and sign it with an account key");
    s->add_option("--key", std::get<0>(*sign), "Key name")->required();
    s->add_option("--in", std::get<1>(*sign), "Unsigned body file (hex)")->required();
    s->add_option("--out", std::get<2>(*sign), "Signed transaction file (hex)")->required();
    s->add_flag("--yes", std::get<3>(*sign), "Do not ask for confirmation on a terminal");
    s->callback([&ctx, sign] {
        auto body = ycs::decode<tx::TxBody>(read_hex_file(std::get<1>(*sign)));
        if (!body) throw CliError(kExitRejected, "not a transaction body: " + std::string(ycs::to_string(body.error())));
        print_body(*body);  // shown before signing
        const auto stx = sign_body(ctx, *body, std::get<0>(*sign), std::get<3>(*sign));
        const Bytes bytes = ycs::encode(stx);
        write_hex(ctx, std::get<2>(*sign), bytes);
        out("tx_id", to_hex(tx::tx_id(bytes)));
        out("tx_file", std::get<2>(*sign));
    });

    auto inspect_file = std::make_shared<std::string>();
    auto* i = txc->add_subcommand("inspect", "Decode a signed transaction or an unsigned body");
    i->add_option("file", *inspect_file, "File with hex")->required();
    i->callback([&ctx, inspect_file] {
        const Bytes bytes = read_hex_file(*inspect_file);
        if (auto stx = tx::decode_signed_tx(bytes)) {
            out("kind", "signed_transaction");
            print_body(stx->body);
            out("tx_id", to_hex(tx::tx_id(bytes)));
            const bool sig_ok = crypto::verify_prefixed(stx->body.sender_pubkey, crypto::sign_prefix::kTx,
                                                         ycs::encode(stx->body), stx->signature);
            out("signature", sig_ok ? "valid" : "INVALID");
            const auto local = open_local_chain(ctx, true, false);
            if (local) {
                const auto r = tx::check_stateless(bytes, local->engine->info().chain_id, local->params());
                out("stateless_check", tx::to_string(r.code));
            }
            if (!sig_ok) throw CliError(kExitRejected, "the transaction signature is invalid");
            return;
        }
        if (auto body = ycs::decode<tx::TxBody>(bytes)) {
            out("kind", "unsigned_body");
            print_body(*body);
            return;
        }
        throw CliError(kExitRejected, "neither a signed transaction nor a transaction body");
    });

    for (const char* type : {"transfer", "burn"}) {
        auto args = std::make_shared<BuildArgs>();
        args->type = type;
        auto* c = txc->add_subcommand(type, std::string("Build and sign a ") + type + " in one step");
        add_build_options(c, *args, false);
        c->add_flag("--yes", args->yes, "Do not ask for confirmation on a terminal");
        c->callback([&ctx, args] {
            if (args->from.empty()) throw CliError(kExitUsage, "--from <key name> is required to sign");
            const auto body = build_body(ctx, *args);
            print_body(body);
            const auto stx = sign_body(ctx, body, args->from, args->yes);
            const Bytes bytes = ycs::encode(stx);
            write_hex(ctx, args->out, bytes);
            out("tx_id", to_hex(tx::tx_id(bytes)));
            out("tx_file", args->out);
        });
    }
}

}  // namespace yap::cli
