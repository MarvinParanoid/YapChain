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
template <class F>
void guarded(const NetState& st, F&& f) {
    try {
        try {
            f();
        } catch (const rpc::TransportError& e) {
            throw CliError(kExitNetwork, std::string(e.what()));
        } catch (const rpc::MalformedResponse& e) {
            throw CliError(kExitNetwork, std::string(e.what()));
        } catch (const rpc::RpcError& e) {
            throw CliError(kExitNetwork, std::string(e.what()));
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
        throw CliError(kExitNotFound, "transaction " + upper_hex(hash) + " is not in the node's transaction index: "
                                      "not executed (yet), never accepted, or the node does not index it");
    }
    Report r(st.flags.json);
    report_finalized(r, s.cfg, *t);
    r.finish();
    if (t->code != 0) throw CliError(kExitTxFailed, "the transaction was finalized with execution code " + std::to_string(t->code));
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

    fallthrough_all(net);  // network options may also follow the subcommand
}

}  // namespace yap::cli
