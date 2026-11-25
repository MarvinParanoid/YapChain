// SPDX-License-Identifier: Apache-2.0
//
// yap network (alias: yap net) — the CLI's network mode (M2.6): talks to a CometBFT node over its RPC.
// Local dev mode (yap dev / query / tx) is untouched and never contacts a node.
//
// Trust: answers of the RPC node are not verified (no light client, no state proofs). Signatures
// protect the authorization of transactions; a node can still hide data or lie about state and
// finality. The chain id signed for is the user's expected one, never the node's.
#include <CLI11.hpp>

#include <unistd.h>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

#include "commands.hpp"
#include "network_config.hpp"
#include "yap/common/hex.hpp"
#include "yap/crypto/address.hpp"
#include "yap/crypto/hash.hpp"
#include "yap/rpc/comet.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/crypto/ed25519.hpp"
#include "yap/tx/codes.hpp"
#include "yap/tx/transaction.hpp"
#include "yap/wallet/amount.hpp"

namespace yap::cli {
namespace {

using Json = nlohmann::ordered_json;

/// Human ("key: value" lines, like the rest of the CLI) or one stable JSON object. In JSON, every
/// u64 is a decimal string (heights, amounts in yip, nonces): no floating point anywhere.
class Report {
public:
    explicit Report(bool json) : json_(json) {}
    void str(const std::string& k, const std::string& v) {
        j_[k] = v;
        if (!json_) out(k, v);
    }
    void u64(const std::string& k, std::uint64_t v) {
        j_[k] = std::to_string(v);
        if (!json_) out(k, v);
    }
    void flag(const std::string& k, bool v) {
        j_[k] = v;
        if (!json_) out(k, v ? "yes" : "no");
    }
    void raw(const std::string& k, Json v, const std::string& human) {
        j_[k] = std::move(v);
        if (!json_) out(k, human);
    }
    void finish() const {
        if (json_) std::cout << j_.dump(2) << "\n";
    }
    [[nodiscard]] bool json() const { return json_; }

private:
    bool json_;
    Json j_ = Json::object();
};

struct NetState {
    NetworkFlags flags;
};

/// Runs a network command: maps transport/RPC failures to exit codes and, with --json, also reports
/// the error as JSON on stdout (the message always goes to stderr).
/// `sending`: a command that broadcasts. Its broadcast step converts its own failures; any RPC
/// failure that reaches this point happened before the broadcast, so nothing was sent.
template <class F>
void guarded(const NetState& st, F&& f, bool sending = false) {
    const std::string note = sending ? " (the transaction was not sent)" : "";
    try {
        try {
            f();
        } catch (const rpc::TransportError& e) {
            throw CliError(kExitNetwork, std::string(e.what()) + note);
        } catch (const rpc::MalformedResponse& e) {
            throw CliError(kExitNetwork, std::string(e.what()) + note);
        } catch (const rpc::RpcError& e) {
            throw CliError(kExitNetwork, std::string(e.what()) + note);
        }
    } catch (const ReportedError&) {
        throw;
    } catch (const CliError& e) {
        if (st.flags.json) {
            Json j;
            j["error"] = e.what();
            j["exit_code"] = e.code();
            std::cout << j.dump(2) << "\n";
        }
        throw;
    }
}

struct Session {
    NetworkConfig cfg;
    std::unique_ptr<rpc::Client> client;
};

Session open_session(const Context& ctx, const NetState& st) {
    Session s;
    s.cfg = resolve_network_config(ctx, st.flags);
    s.client = std::make_unique<rpc::Client>(s.cfg.rpc.value, s.cfg.http());
    return s;
}

/// Latest-committed-height JSON answer of the application's Query path (ADR-0028: latest only).
std::pair<std::uint64_t, Json> app_query(rpc::Client& c, const std::string& path) {
    const auto q = c.abci_query(path);
    if (q.code != 0) {
        throw CliError(q.code == 2 ? static_cast<int>(kExitRejected) : static_cast<int>(kExitNetwork),
                       "query " + path + " failed: code " + std::to_string(q.code) + " " + q.log);
    }
    Json j = Json::parse(q.value.begin(), q.value.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw CliError(kExitNetwork, "query " + path + ": the answer is not a JSON object");
    return {q.height, std::move(j)};
}

std::string jstr(const Json& j, const char* k) {
    if (!j.contains(k) || !j[k].is_string()) throw CliError(kExitNetwork, std::string("query answer lacks '") + k + "'");
    return j[k].get<std::string>();
}

std::uint64_t ju64(const Json& j, const char* k) {
    const auto v = wallet::parse_decimal_u64(jstr(j, k));
    if (!v) throw CliError(kExitNetwork, std::string("query answer: '") + k + "' is not a decimal");
    return *v;
}

/// Chain identity (ADR-0029 item 7): the node's CometBFT network and the application's chain id must
/// both equal the expected chain id. `require` = an expected chain id must be configured (signing,
/// sending); otherwise read-only commands proceed with a warning.
rpc::Status check_identity(Session& s, bool require) {
    const auto status = s.client->status();
    if (!s.cfg.has_chain_id()) {
        if (require) {
            throw CliError(kExitUsage, "no expected chain id: pass --chain-id or run `yap network config set --chain-id <id>` "
                                       "(the chain id is never taken from the RPC node)");
        }
        std::cerr << "warning: chain identity not checked (no expected chain id configured); the node says '"
                  << status.chain_id << "'\n";
        return status;
    }
    if (status.chain_id != s.cfg.chain_id.value) {
        throw CliError(kExitChainMismatch, "the node at " + s.cfg.rpc.value + " is on chain '" + status.chain_id +
                                               "', expected '" + s.cfg.chain_id.value + "'");
    }
    const auto [h, chain] = app_query(*s.client, "/chain");
    if (jstr(chain, "chain_id") != s.cfg.chain_id.value) {
        throw CliError(kExitChainMismatch, "the application at " + s.cfg.rpc.value + " reports chain '" +
                                               jstr(chain, "chain_id") + "', expected '" + s.cfg.chain_id.value + "'");
    }
    return status;
}

void add_network_flags(CLI::App* app, NetState& st) {
    app->add_option("--rpc", st.flags.rpc, "CometBFT RPC endpoint, http(s)://host:port (default: config, $YAP_RPC, " +
                                               std::string(kDefaultRpc) + ")");
    app->add_option("--chain-id", st.flags.chain_id, "Expected chain id (default: config, $YAP_CHAIN_ID)");
    app->add_option("--connect-timeout-ms", st.flags.connect_timeout_ms, "TCP/TLS connect timeout");
    app->add_option("--read-timeout-ms", st.flags.read_timeout_ms, "Abort when no data arrives for this long");
    app->add_option("--timeout-ms", st.flags.timeout_ms, "Timeout of one RPC request");
    app->add_option("--max-response-bytes", st.flags.max_response_bytes, "Largest accepted RPC response");
    app->add_option("--ca-file", st.flags.ca_file, "Extra trusted CA certificate (PEM) for https endpoints");
    app->add_flag("--json", st.flags.json, "Print one JSON object (u64 values as decimal strings)");
}

void cmd_config_show(const Context& ctx, const NetState& st) {
    const auto c = resolve_network_config(ctx, st.flags);
    Report r(st.flags.json);
    r.str("config_file", network_config_path(ctx).string());
    r.str("rpc", c.rpc.value);
    r.str("rpc_source", c.rpc.source);
    r.str("chain_id", c.chain_id.value.empty() ? "(unset)" : c.chain_id.value);
    r.str("chain_id_source", c.chain_id.source);
    r.str("address_prefix", c.hrp());
    r.u64("connect_timeout_ms", static_cast<std::uint64_t>(c.connect_timeout_ms.value));
    r.u64("read_timeout_ms", static_cast<std::uint64_t>(c.read_timeout_ms.value));
    r.u64("timeout_ms", static_cast<std::uint64_t>(c.timeout_ms.value));
    r.u64("max_response_bytes", c.max_response_bytes.value);
    r.str("ca_file", c.ca_file.value.empty() ? "(system CA store)" : c.ca_file.value);
    r.finish();
}

void cmd_status(const Context& ctx, const NetState& st) {
    auto s = open_session(ctx, st);
    const auto status = check_identity(s, false);
    Report r(st.flags.json);
    r.str("rpc", s.cfg.rpc.value);
    r.str("chain_id", status.chain_id);
    r.str("expected_chain_id", s.cfg.has_chain_id() ? s.cfg.chain_id.value : "(unset)");
    r.flag("chain_id_verified", s.cfg.has_chain_id());
    r.str("node_id", status.node_id);
    r.str("moniker", status.moniker);
    r.str("version", status.version);
    r.u64("latest_height", status.latest_height);
    r.str("latest_block_hash", status.latest_block_hash);
    r.str("latest_app_hash", status.latest_app_hash);  // state root after latest_height - 1
    r.str("latest_block_time", status.latest_block_time);
    r.u64("earliest_height", status.earliest_height);
    r.flag("catching_up", status.catching_up);
    r.u64("voting_power", status.voting_power);
    r.str("trust", "unverified RPC answer (no light client)");
    r.finish();
}

void cmd_info(const Context& ctx, const NetState& st) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto ni = s.client->net_info();
    Report r(st.flags.json);
    r.flag("listening", ni.listening);
    r.u64("peers", ni.n_peers);
    Json peers = Json::array();
    for (const auto& p : ni.peers) {
        peers.push_back({{"node_id", p.node_id}, {"moniker", p.moniker}, {"remote_ip", p.remote_ip}, {"outbound", p.outbound}});
        if (!r.json()) out("peer", p.node_id + " " + p.moniker + " " + p.remote_ip + (p.outbound ? " out" : " in"));
    }
    if (r.json()) r.raw("peer_list", peers, "");
    r.finish();
}


// ------------------------------------------------------------------ remote queries (M2.6c)

crypto::AccountId parse_address(const NetworkConfig& cfg, const std::string& text) {
    auto id = crypto::decode_address(text, cfg.hrp());
    if (!id) throw CliError(kExitRejected, "invalid address '" + text + "': " + std::string(crypto::to_string(id.error())));
    return *id;
}

Hash32 parse_hash(const std::string& text) {
    std::string lower = text;
    if (lower.rfind("0x", 0) == 0 || lower.rfind("0X", 0) == 0) lower = lower.substr(2);
    for (auto& c : lower) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    auto h = from_hex_fixed<32>(lower);
    if (!h) throw CliError(kExitRejected, "invalid transaction hash '" + text + "' (64 hex characters)");
    return *h;
}

std::string upper_hex(ByteView b) {
    std::string s = to_hex(b);
    for (auto& c : s) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

void amount(Report& r, const std::string& k, std::uint64_t yip) {
    r.u64(k + "_yip", yip);
    r.str(k, wallet::format_amount(yip));
}

void latest_only(Report& r, std::uint64_t h) {
    r.u64("height", h);
    r.str("state", "latest committed state at this height (no historical queries; unverified RPC answer)");
}

void cmd_query_chain(const Context& ctx, const NetState& st) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto [h, j] = app_query(*s.client, "/chain");
    Report r(st.flags.json);
    r.str("chain_id", jstr(j, "chain_id"));
    latest_only(r, h);
    r.str("state_root", jstr(j, "state_root"));
    r.str("genesis_hash", jstr(j, "genesis_hash"));
    r.u64("last_time_ms", ju64(j, "last_time_ms"));
    amount(r, "total_supply", ju64(j, "total_supply"));
    amount(r, "total_minted", ju64(j, "total_minted"));
    amount(r, "total_burned", ju64(j, "total_burned"));
    r.finish();
}

void cmd_query_params(const Context& ctx, const NetState& st) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto [h, j] = app_query(*s.client, "/params");
    Report r(st.flags.json);
    latest_only(r, h);
    amount(r, "min_fee", ju64(j, "min_fee"));
    amount(r, "block_reward", ju64(j, "block_reward"));
    amount(r, "max_issuance", ju64(j, "max_issuance"));
    r.u64("max_tx_ttl_blocks", ju64(j, "max_tx_ttl_blocks"));
    r.u64("epoch_length_blocks", ju64(j, "epoch_length_blocks"));
    for (const char* k : {"fee_burn_bps", "reward_validators_bps", "reward_pool_bps", "reward_treasury_bps",
                          "max_tx_bytes", "max_memo_bytes"}) {
        if (!j.contains(k) || !j[k].is_number_unsigned()) throw CliError(kExitNetwork, std::string("query answer lacks '") + k + "'");
        r.u64(k, j[k].get<std::uint64_t>());
    }
    r.finish();
}

struct RemoteAccount {
    std::uint64_t height = 0;
    bool exists = false;
    std::uint64_t balance = 0;
    std::uint64_t nonce = 0;
    std::string account_id;
};

RemoteAccount remote_account(Session& s, const std::string& address) {
    (void)parse_address(s.cfg, address);
    const auto [h, j] = app_query(*s.client, "/account/" + address);
    if (jstr(j, "address") != address) throw CliError(kExitNetwork, "the node answered for another address");
    if (!j.contains("exists") || !j["exists"].is_boolean()) throw CliError(kExitNetwork, "query answer lacks 'exists'");
    return {h, j["exists"].get<bool>(), ju64(j, "balance"), ju64(j, "nonce"), jstr(j, "account_id")};
}

void cmd_query_account(const Context& ctx, const NetState& st, const std::string& address, bool full) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto a = remote_account(s, address);
    Report r(st.flags.json);
    r.str("address", address);
    if (full) r.str("account_id", a.account_id);
    r.flag("exists", a.exists);  // an absent account is a valid answer: balance 0, nonce 0
    amount(r, "balance", a.balance);
    if (full) r.u64("nonce", a.nonce);
    latest_only(r, a.height);
    r.finish();
}

void cmd_query_block(const Context& ctx, const NetState& st, std::optional<std::uint64_t> height) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto b = s.client->block(height);
    if (!b) throw CliError(kExitNotFound, "the node has no block at height " + std::to_string(height.value_or(0)));
    if (s.cfg.has_chain_id() && b->chain_id != s.cfg.chain_id.value) {
        throw CliError(kExitChainMismatch, "block of chain '" + b->chain_id + "', expected '" + s.cfg.chain_id.value + "'");
    }
    Report r(st.flags.json);
    r.u64("height", b->height);
    r.str("block_hash", b->block_hash);
    r.str("chain_id", b->chain_id);
    r.str("time", b->time);
    r.str("app_hash", b->app_hash);
    r.str("app_hash_meaning", "state root after height " + std::to_string(b->height - 1));
    r.str("proposer_address", b->proposer_address);
    r.u64("last_commit_signatures", b->last_commit_signatures);
    r.u64("txs", b->txs.size());
    Json hashes = Json::array();
    for (const auto& t : b->txs) {
        const auto h = upper_hex(crypto::sha256(t));
        hashes.push_back(h);
        if (!r.json()) out("tx_hash", h);
    }
    if (r.json()) r.raw("tx_hashes", hashes, "");
    r.finish();
}

/// The transaction fields worth showing, if the bytes decode as a YapChain transaction.
void describe_tx(Report& r, const NetworkConfig& cfg, ByteView bytes) {
    const auto stx = tx::decode_signed_tx(bytes);
    if (!stx) {
        r.str("decoded", "no");
        return;
    }
    const auto& b = stx->body;
    const auto tag = tx::payload_tag(b.payload);
    r.str("type", tag == tx::PayloadTag::Transfer ? "transfer" : tag == tx::PayloadTag::Burn ? "burn" : "claim_achievement");
    r.str("tx_chain_id", b.chain_id);
    r.str("sender", crypto::encode_address(cfg.hrp(), b.sender()));
    r.u64("nonce", b.nonce);
    amount(r, "fee", b.fee);
    r.u64("expires_at_height", b.expires_at_height);
    if (const auto* t = std::get_if<tx::Transfer>(&b.payload)) {
        r.str("to", crypto::encode_address(cfg.hrp(), t->to));
        amount(r, "amount", t->amount);
    } else if (const auto* bn = std::get_if<tx::Burn>(&b.payload)) {
        amount(r, "amount", bn->amount);
    }
    r.str("tx_id", to_hex(tx::tx_id(bytes)));
}

/// Finalized result of a transaction (exit 0 when executed OK, 9 when executed with an error code).
void report_finalized(Report& r, const NetworkConfig& cfg, const rpc::TxResult& t) {
    r.str("status", t.code == 0 ? "finalized" : "finalized_failed");
    r.str("hash", t.hash);
    r.u64("height", t.height);
    r.u64("index", t.index);
    r.u64("code", t.code);
    r.str("result", t.code == 0 ? "OK" : std::string(tx::to_string(static_cast<tx::TxCode>(t.code))));
    if (!t.log.empty()) r.str("log", t.log);
    describe_tx(r, cfg, t.tx);
    r.str("trust", "unverified RPC answer (no light client)");
}

void cmd_query_tx(const Context& ctx, const NetState& st, const std::string& hash_text) {
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto hash = parse_hash(hash_text);
    const auto t = s.client->tx(hash);
    if (!t) {
        throw CliError(kExitNotFound, "transaction " + upper_hex(hash) + " is not in this node's transaction index; "
                                      "that is not proof either way: it may not be executed yet, never be executed, "
                                      "or the index may lag, be incomplete or belong to another node behind this endpoint");
    }
    Report r(st.flags.json);
    report_finalized(r, s.cfg, *t);
    r.finish();
    if (t->code != 0) throw CliError(kExitTxFailed, "the transaction was finalized with execution code " + std::to_string(t->code));
}

// ------------------------------------------------------------------ broadcast (M2.6d)

struct SendOptions {
    bool wait = false;
    long wait_timeout_s = 60;
    long poll_ms = 1'000;
    bool skip_local_checks = false;
};

/// Checks a signed transaction before it leaves this machine: it decodes, it is for the expected
/// chain (never skippable) and its signature is valid (skippable only for testing a node's checks).
tx::SignedTx check_local(const NetworkConfig& cfg, ByteView bytes, bool skip_signature) {
    auto stx = tx::decode_signed_tx(bytes);
    if (!stx) throw CliError(kExitRejected, "not a signed transaction: " + std::string(ycs::to_string(stx.error())));
    if (stx->body.chain_id != cfg.chain_id.value) {
        throw CliError(kExitChainMismatch, "the transaction is signed for chain '" + stx->body.chain_id + "', expected '" +
                                               cfg.chain_id.value + "'; not sent");
    }
    const bool sig_ok = crypto::verify_prefixed(stx->body.sender_pubkey, crypto::sign_prefix::kTx,
                                                ycs::encode(stx->body), stx->signature);
    if (!sig_ok && !skip_signature) {
        throw CliError(kExitRejected, "the transaction signature is invalid; not sent");
    }
    return std::move(*stx);
}

void wait_final(Session& s, Report& r, const Hash32& hash, std::optional<std::uint64_t> expires, const SendOptions& o);

/// Sends once with broadcast_tx_sync and reports what is known. Never re-sends.
void broadcast_and_report(Session& s, Report& r, ByteView bytes, std::optional<std::uint64_t> expires,
                          const SendOptions& o) {
    const Hash32 hash = crypto::sha256(bytes);
    const std::string hx = upper_hex(hash);
    r.str("hash", hx);
    r.str("tx_id", to_hex(tx::tx_id(bytes)));
    const std::string recheck = " — not sent again; check it with `yap network tx " + hx + "`";
    rpc::BroadcastResult br;
    try {
        br = s.client->broadcast_tx_sync(bytes);
    } catch (const rpc::TransportError& e) {
        if (!e.request_sent()) throw CliError(kExitNetwork, std::string(e.what()) + " (the transaction was not sent)");
        r.str("status", "unknown");
        r.finish();
        throw ReportedError(kExitUnknown, std::string(e.what()) + ": the node may or may not have accepted it" + recheck);
    } catch (const rpc::MalformedResponse& e) {
        r.str("status", "unknown");
        r.finish();
        throw ReportedError(kExitUnknown, std::string(e.what()) + recheck);
    } catch (const rpc::RpcError& e) {
        if (e.data().find("tx already exists in cache") == std::string::npos) {
            r.str("status", "not_accepted");
            r.str("node_error", e.data().empty() ? e.message() : e.data());
            r.finish();
            throw ReportedError(kExitRejected, "the node did not accept the transaction: " + std::string(e.what()));
        }
        // The node has seen these exact bytes before (executed, or still in a mempool somewhere).
        r.str("status", "already_known");
        if (o.wait) return wait_final(s, r, hash, expires, o);
        if (const auto t = s.client->tx(hash)) {
            report_finalized(r, s.cfg, *t);
            r.finish();
            if (t->code != 0) throw ReportedError(kExitTxFailed, "finalized with execution code " + std::to_string(t->code));
            return;
        }
        r.finish();
        throw ReportedError(kExitUnknown, "the node already received this transaction earlier and it is not final yet" + recheck);
    }
    if (br.hash != hx) {
        r.str("status", "unknown");
        r.finish();
        throw ReportedError(kExitUnknown, "the node answered for another hash " + br.hash + recheck);
    }
    if (br.code == static_cast<std::uint32_t>(tx::TxCode::ErrNonce)) {
        // The sender's nonce is already used — possibly by this very transaction. Never report that as
        // "not executed": look it up, and say so when it cannot be established.
        std::optional<rpc::TxResult> t;
        try {
            t = s.client->tx(hash);
        } catch (const std::exception&) {  // the lookup is best effort; the rejection stands either way
        }
        if (t) {
            r.str("broadcast", "rejected by CheckTx with ERR_NONCE: this transaction was already executed");
            report_finalized(r, s.cfg, *t);
            r.finish();
            if (t->code != 0) throw ReportedError(kExitTxFailed, "finalized with execution code " + std::to_string(t->code));
            return;
        }
        r.str("execution", "unknown: ERR_NONCE means the sender's nonce is already used, possibly by this very "
                           "transaction (not in this node's index); check `yap network tx " + hx + "` before re-sending");
    }
    if (br.code != 0) {
        r.str("status", "rejected");
        r.u64("check_tx_code", br.code);
        r.str("result", std::string(tx::to_string(static_cast<tx::TxCode>(br.code))));
        if (!br.log.empty()) r.str("log", br.log);
        r.finish();
        throw ReportedError(kExitRejected, "rejected by CheckTx: " + std::string(tx::to_string(static_cast<tx::TxCode>(br.code))));
    }
    r.str("status", "accepted_mempool");
    if (!o.wait) {
        r.str("note", "accepted into the mempool of " + s.cfg.rpc.value + "; NOT final — confirm with `yap network wait " + hx + "`");
        r.finish();
        return;
    }
    wait_final(s, r, hash, expires, o);
}

struct NewTxArgs {
    std::string from;
    std::string to;
    std::string amount;
    std::string fee;
    std::string nonce;
    std::string expires;
    std::uint64_t ttl = 100;
    std::string memo;
    std::string out;
    bool yes = false;
};

std::optional<std::uint64_t> opt_decimal(const std::string& text, const char* what) {
    if (text.empty()) return std::nullopt;
    const auto v = wallet::parse_decimal_u64(text);
    if (!v) throw CliError(kExitUsage, std::string("invalid ") + what + " '" + text + "' (decimal digits only)");
    return v;
}

std::uint64_t parse_yip(const std::string& text, const char* what) {
    const auto v = wallet::parse_amount(text);
    if (!v) throw CliError(kExitUsage, std::string("invalid ") + what + " '" + text + "' (integer yip, or e.g. 1.5YAP)");
    return *v;
}

/// Asks before signing on a terminal; elsewhere --yes is required (documented automation mode).
void confirm(const NewTxArgs& a, const std::string& summary) {
    std::cerr << summary;
    if (a.yes) return;
    if (::isatty(STDIN_FILENO) == 0) {
        throw CliError(kExitUsage, "confirmation needed: run on a terminal, or pass --yes for non-interactive use");
    }
    std::cerr << "Sign and send this transaction? [y/N] " << std::flush;
    std::string answer;
    std::getline(std::cin, answer);
    if (answer != "y" && answer != "Y" && answer != "yes") throw CliError(kExitRejected, "not signed");
}

void check_wait_options(const SendOptions& o);

void cmd_new_tx(const Context& ctx, const NetState& st, const std::string& kind, const NewTxArgs& a, const SendOptions& o) {
    check_wait_options(o);
    auto s = open_session(ctx, st);
    const auto status = check_identity(s, /*require=*/true);
    const auto [ph, params] = app_query(*s.client, "/params");
    const std::uint64_t min_fee = ju64(params, "min_fee");
    const std::uint64_t max_ttl = ju64(params, "max_tx_ttl_blocks");

    tx::TxBody b;
    b.chain_id = s.cfg.chain_id.value;  // the expected chain id, never the node's answer
    b.sender_pubkey = load_key_info(ctx.key_path(a.from), wallet::KeyPurpose::Account).public_key;
    const std::string sender = crypto::encode_address(s.cfg.hrp(), b.sender());
    const auto acc = remote_account(s, sender);
    const auto nonce = opt_decimal(a.nonce, "--nonce");
    b.nonce = nonce.value_or(acc.nonce);
    const auto expires = opt_decimal(a.expires, "--expires");
    if (a.ttl == 0) throw CliError(kExitUsage, "--ttl must be >= 1");
    b.expires_at_height = expires.value_or(std::max(acc.height, status.latest_height) + std::min(a.ttl, max_ttl));
    b.fee = a.fee.empty() ? min_fee : parse_yip(a.fee, "--fee");
    if (a.memo.size() > tx::kMemoCeiling) throw CliError(kExitUsage, "memo longer than 1024 bytes");
    b.memo.assign(a.memo.begin(), a.memo.end());
    const std::uint64_t amt = parse_yip(a.amount, "--amount");
    if (amt == 0) throw CliError(kExitRejected, "amount must be >= 1 yip");
    std::string summary = "chain_id: " + b.chain_id + "\nfrom: " + sender + "\n";
    if (kind == "transfer") {
        const auto to = parse_address(s.cfg, a.to);
        if (to == b.sender()) throw CliError(kExitRejected, "the recipient must differ from the sender");
        b.payload = tx::Transfer{to, amt};
        summary += "to: " + a.to + "\n";
    } else {
        b.payload = tx::Burn{amt};
    }
    summary += kind + " amount: " + wallet::format_amount(amt) + "\nfee: " + wallet::format_amount(b.fee) +
               "\nnonce: " + std::to_string(b.nonce) + (nonce ? " (given)" : " (from the node)") +
               "\nexpires_at_height: " + std::to_string(b.expires_at_height) + "\nrpc: " + s.cfg.rpc.value + "\n";
    confirm(a, summary);

    const auto key = unlock_key(ctx, ctx.key_path(a.from), wallet::KeyPurpose::Account);  // stays local
    if (key.public_key() != b.sender_pubkey) throw CliError(kExitRejected, "key '" + a.from + "' is not the sender");
    const Bytes bytes = ycs::encode(tx::sign_tx(b, key));
    (void)check_local(s.cfg, bytes, false);
    if (!a.out.empty()) write_file_bytes(a.out, as_bytes(to_hex(bytes) + "\n"), ctx.force);

    Report r(st.flags.json);
    r.str("type", kind);
    r.str("from", sender);
    if (kind == "transfer") r.str("to", a.to);
    amount(r, "amount", amt);
    amount(r, "fee", b.fee);
    r.u64("nonce", b.nonce);
    r.u64("expires_at_height", b.expires_at_height);
    if (!a.out.empty()) r.str("tx_file", a.out);
    broadcast_and_report(s, r, bytes, b.expires_at_height, o);
}

void check_wait_options(const SendOptions& o);

void cmd_send(const Context& ctx, const NetState& st, const std::string& file, const SendOptions& o) {
    check_wait_options(o);
    auto s = open_session(ctx, st);
    if (!s.cfg.has_chain_id()) (void)check_identity(s, true);  // explains how to set it
    const Bytes bytes = read_hex_file(file);
    const auto stx = check_local(s.cfg, bytes, o.skip_local_checks);
    (void)check_identity(s, true);
    Report r(st.flags.json);
    describe_tx(r, s.cfg, bytes);
    broadcast_and_report(s, r, bytes, stx.body.expires_at_height, o);
}

// ------------------------------------------------------------------ confirmation (M2.6e)

/// Polls the node's transaction index until the transaction is found or the time is up. It never
/// concludes that a transaction was NOT executed: a CometBFT index is written asynchronously after the
/// block store (IndexerService), can miss heights (indexer enabled later, reset) and can differ between
/// nodes behind one endpoint, so "not in the index" proves nothing about the chain.
/// What the node's height does prove: once it is above expires_at_height the transaction can no longer
/// be INCLUDED (protocol T1; heights only grow). The index is then re-checked (immediately, closing the
/// race between tx() and status(), and for a short grace period for a lagging indexer) and the result
/// is "unknown" with both facts. The mempool is never relied upon; nothing is ever re-sent.
void wait_final(Session& s, Report& r, const Hash32& hash, std::optional<std::uint64_t> expires, const SendOptions& o) {
    using Clock = std::chrono::steady_clock;
    const std::string hx = upper_hex(hash);
    const auto deadline = Clock::now() + std::chrono::seconds(o.wait_timeout_s);
    std::optional<Clock::time_point> grace_end;  // set once the inclusion window is seen closed
    std::uint64_t closed_at = 0;                  // node height that proved the window closed
    std::uint64_t last_height = 0;
    auto last_change = Clock::now();
    bool stall_noted = false;
    std::size_t errors = 0;
    std::string last_error;
    const auto finalized = [&](const rpc::TxResult& t) {
        report_finalized(r, s.cfg, t);
        r.finish();
        if (t.code != 0) {
            throw ReportedError(kExitTxFailed, "finalized at height " + std::to_string(t.height) +
                                                   " with execution code " + std::to_string(t.code));
        }
    };
    for (;;) {
        try {
            if (const auto t = s.client->tx(hash)) return finalized(*t);
            const auto st = s.client->status();
            if (st.latest_height != last_height) {
                last_height = st.latest_height;
                last_change = Clock::now();
            } else if (!stall_noted && Clock::now() - last_change > std::chrono::seconds(10)) {
                std::cerr << "note: no new block on the node for 10 s (consensus may be stalled); still waiting\n";
                stall_noted = true;
            }
            if (expires && st.latest_height > *expires && !grace_end) {
                closed_at = st.latest_height;
                // Executed between the tx() and status() calls above? Ask again now.
                if (const auto t = s.client->tx(hash)) return finalized(*t);
                const auto grace = std::max(std::chrono::milliseconds(3 * o.poll_ms), std::chrono::milliseconds(3'000));
                grace_end = std::min(deadline, Clock::now() + grace);
            }
        } catch (const rpc::RpcError& e) {
            if (e.data().find("transaction indexing is disabled") != std::string::npos) {
                throw CliError(kExitNetwork, "the node does not index transactions (tx_index.indexer = \"null\"); "
                                             "confirmation needs a node with the \"kv\" indexer");
            }
            ++errors;
            last_error = e.what();
        } catch (const rpc::TransportError& e) {
            ++errors;
            last_error = e.what();
        } catch (const rpc::MalformedResponse& e) {
            ++errors;
            last_error = e.what();
        }
        const auto now = Clock::now();
        if (now >= deadline || (grace_end && now >= *grace_end)) {
            r.str("status", "unknown");
            r.str("execution", "unknown");
            if (grace_end) {
                r.str("inclusion_window", "closed");
                r.u64("node_height", closed_at);
                r.u64("expires_at_height", *expires);
                r.str("note", "it can no longer be included (the node is past expires_at_height), but whether it was "
                              "executed earlier is unknown: it is not in this node's transaction index, which can lag, "
                              "be incomplete or differ between nodes. This is NOT proof that it was not executed — check "
                              "`yap network tx " + hx + "` later or on another node before sending anything again");
            } else {
                r.u64("waited_s", static_cast<std::uint64_t>(o.wait_timeout_s));
                if (last_height != 0) r.u64("node_height", last_height);
                r.str("note", "not final within the timeout; this does NOT mean rejected — check again with "
                              "`yap network tx " + hx + "` or `yap network wait " + hx + "`");
            }
            if (errors != 0) r.str("last_rpc_error", last_error);
            r.finish();
            throw ReportedError(kExitUnknown, grace_end ? "outcome unknown: the inclusion window is closed (height " +
                                                              std::to_string(closed_at) + " > expires_at_height " +
                                                              std::to_string(*expires) + "), past execution not established"
                                                        : "not finalized within " + std::to_string(o.wait_timeout_s) +
                                                              " s (outcome unknown)");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(o.poll_ms));
    }
}

void check_wait_options(const SendOptions& o) {
    if (o.wait_timeout_s < 1 || o.wait_timeout_s > 3600) throw CliError(kExitUsage, "the wait timeout must be 1..3600 s");
    if (o.poll_ms < 100 || o.poll_ms > 60'000) throw CliError(kExitUsage, "the poll interval must be 100..60000 ms");
}

void cmd_wait(const Context& ctx, const NetState& st, const std::string& hash_text, const std::string& expires_text,
              const SendOptions& o) {
    check_wait_options(o);
    auto s = open_session(ctx, st);
    (void)check_identity(s, false);
    const auto hash = parse_hash(hash_text);
    Report r(st.flags.json);
    r.str("hash", upper_hex(hash));
    wait_final(s, r, hash, opt_decimal(expires_text, "--expires"), o);
}

void fallthrough_all(CLI::App* app) {
    for (auto* sub : app->get_subcommands({})) {
        sub->fallthrough();
        fallthrough_all(sub);
    }
}

}  // namespace

void register_network(CLI::App& app, Context& ctx) {
    auto st = std::make_shared<NetState>();
    auto* net = app.add_subcommand("network", "Talk to a YapChain network through a CometBFT RPC endpoint "
                                              "(exit codes 6 network, 7 not found, 8 unknown outcome, "
                                              "9 execution failed, 10 chain mismatch)");
    net->alias("net");
    net->require_subcommand(1);
    add_network_flags(net, *st);

    auto* cfg = net->add_subcommand("config", "Show or set the network configuration (<home>/network.json)");
    cfg->require_subcommand(1);
    auto* show = cfg->add_subcommand("show", "Effective settings and where each comes from");
    show->callback([&ctx, st] { guarded(*st, [&] { cmd_config_show(ctx, *st); }); });
    auto* set = cfg->add_subcommand("set", "Store --rpc / --chain-id / timeouts / --ca-file in the config file");
    set->callback([&ctx, st] {
        guarded(*st, [&] {
            save_network_config(ctx, st->flags);
            NetState shown;  // show the stored file values, not the flags
            shown.flags.json = st->flags.json;
            cmd_config_show(ctx, shown);
        });
    });

    net->add_subcommand("status", "Node status: chain id, height, hashes, catching_up")->callback([&ctx, st] {
        guarded(*st, [&] { cmd_status(ctx, *st); });
    });
    net->add_subcommand("info", "Node network information: peers")->callback([&ctx, st] {
        guarded(*st, [&] { cmd_info(ctx, *st); });
    });

    auto* q = net->add_subcommand("query", "Query the latest committed state through the node (unverified)");
    q->require_subcommand(1);
    q->add_subcommand("chain", "Chain summary: height, state root, supply")->callback([&ctx, st] {
        guarded(*st, [&] { cmd_query_chain(ctx, *st); });
    });
    q->add_subcommand("params", "Chain parameters: fees, emission, limits")->callback([&ctx, st] {
        guarded(*st, [&] { cmd_query_params(ctx, *st); });
    });
    auto qaddr = std::make_shared<std::string>();
    auto* qb = q->add_subcommand("balance", "Balance of an address");
    qb->add_option("address", *qaddr, "Address")->required();
    qb->callback([&ctx, st, qaddr] { guarded(*st, [&] { cmd_query_account(ctx, *st, *qaddr, false); }); });
    auto qaddr2 = std::make_shared<std::string>();
    auto* qa = q->add_subcommand("account", "Account: balance and nonce");
    qa->add_option("address", *qaddr2, "Address")->required();
    qa->callback([&ctx, st, qaddr2] { guarded(*st, [&] { cmd_query_account(ctx, *st, *qaddr2, true); }); });
    auto qheight = std::make_shared<std::optional<std::uint64_t>>();
    auto* qbl = q->add_subcommand("block", "A block by height (default: the latest)");
    qbl->add_option("height", *qheight, "Height");
    qbl->callback([&ctx, st, qheight] { guarded(*st, [&] { cmd_query_block(ctx, *st, *qheight); }); });
    auto qhash = std::make_shared<std::string>();
    auto* qt = q->add_subcommand("tx", "A finalized transaction by hash (needs the node's tx index)");
    qt->add_option("hash", *qhash, "Transaction hash (CometBFT: SHA-256 of the bytes, hex)")->required();
    qt->callback([&ctx, st, qhash] { guarded(*st, [&] { cmd_query_tx(ctx, *st, *qhash); }); });
    auto thash = std::make_shared<std::string>();
    auto* tq = net->add_subcommand("tx", "Same as `query tx`: result of a transaction by hash");
    tq->add_option("hash", *thash, "Transaction hash")->required();
    tq->callback([&ctx, st, thash] { guarded(*st, [&] { cmd_query_tx(ctx, *st, *thash); }); });

    auto sfile = std::make_shared<std::string>();
    auto sopts = std::make_shared<SendOptions>();
    auto* snd = net->add_subcommand("send", "Broadcast a signed transaction file (hex) once; never re-sent automatically");
    snd->add_option("file", *sfile, "Signed transaction (hex), e.g. from `yap tx transfer --out`")->required();
    snd->add_flag("--skip-local-checks", sopts->skip_local_checks,
                  "Send even if the signature does not verify locally (to test a node; the chain id check stays)");
    const auto add_wait = [](CLI::App* c, SendOptions& o) {
        c->add_flag("--wait", o.wait, "Wait until the transaction is final (bounded by --wait-timeout)");
        c->add_option("--wait-timeout", o.wait_timeout_s, "Seconds to wait for finalization (1..3600)")->capture_default_str();
        c->add_option("--poll-ms", o.poll_ms, "Polling interval while waiting (100..60000 ms)")->capture_default_str();
    };
    add_wait(snd, *sopts);
    snd->callback([&ctx, st, sfile, sopts] { guarded(*st, [&] { cmd_send(ctx, *st, *sfile, *sopts); }, true); });
    for (const char* kind : {"transfer", "burn"}) {
        auto a = std::make_shared<NewTxArgs>();
        auto o = std::make_shared<SendOptions>();
        auto* c = net->add_subcommand(kind, std::string("Build a ") + kind +
                                                " (nonce, fee, expiry from the node), sign it with a local key, send it");
        c->add_option("--from", a->from, "Sender key name (local keystore; the key never leaves this machine)")->required();
        if (std::string(kind) == "transfer") c->add_option("--to", a->to, "Recipient address")->required();
        c->add_option("--amount", a->amount, "Amount: integer yip, or decimal YAP like 1.5YAP")->required();
        c->add_option("--fee", a->fee, "Fee (default: min_fee from the node)");
        c->add_option("--nonce", a->nonce, "Nonce (default: the sender's committed nonce from the node)");
        c->add_option("--expires", a->expires, "Last valid height (default: node height + --ttl)");
        c->add_option("--ttl", a->ttl, "Blocks of validity when --expires is not given (capped by max_tx_ttl_blocks)")
            ->capture_default_str();
        c->add_option("--memo", a->memo, "Memo text");
        c->add_option("--out", a->out, "Also write the signed transaction (hex) to this file");
        c->add_flag("--yes", a->yes, "Do not ask for confirmation (required when not on a terminal)");
        add_wait(c, *o);
        const std::string k = kind;
        c->callback([&ctx, st, a, o, k] { guarded(*st, [&] { cmd_new_tx(ctx, *st, k, *a, *o); }, true); });
    }

    auto whash = std::make_shared<std::string>();
    auto wexp = std::make_shared<std::string>();
    auto wopts = std::make_shared<SendOptions>();
    auto* w = net->add_subcommand("wait", "Wait until a transaction is final (by hash; nothing is sent). A timeout "
                                          "means unknown, not rejected (exit 8)");
    w->add_option("hash", *whash, "Transaction hash")->required();
    w->add_option("--expires", *wexp, "Its expires_at_height: once the node is past it, the wait stops early and reports "
                                      "that it can no longer be included (execution stays unknown unless found)");
    w->add_option("--timeout", wopts->wait_timeout_s, "Seconds to wait (1..3600)")->capture_default_str();
    w->add_option("--poll-ms", wopts->poll_ms, "Polling interval (100..60000 ms)")->capture_default_str();
    w->callback([&ctx, st, whash, wexp, wopts] { guarded(*st, [&] { cmd_wait(ctx, *st, *whash, *wexp, *wopts); }); });

    fallthrough_all(net);  // network options may also follow the subcommand
}

}  // namespace yap::cli
