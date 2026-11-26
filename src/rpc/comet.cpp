// SPDX-License-Identifier: Apache-2.0
#include "yap/rpc/comet.hpp"

#include <nlohmann/json.hpp>

#include <limits>

#include "yap/common/hex.hpp"

namespace yap::rpc {
namespace {

using Json = nlohmann::json;

[[noreturn]] void bad(const std::string& what) { throw MalformedResponse("malformed RPC response: " + what); }

Json parse_json(const std::string& text) {
    // No exceptions from the parser itself; reject duplicates implicitly by strict shape checks.
    Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) bad("not JSON");
    return j;
}

const Json& field(const Json& o, const char* name) {
    if (!o.is_object()) bad(std::string("expected an object around '") + name + "'");
    const auto it = o.find(name);
    if (it == o.end()) bad(std::string("missing '") + name + "'");
    return *it;
}

const Json& object(const Json& o, const char* name) {
    const Json& v = field(o, name);
    if (!v.is_object()) bad(std::string("'") + name + "' is not an object");
    return v;
}

std::string str(const Json& o, const char* name) {
    const Json& v = field(o, name);
    if (!v.is_string()) bad(std::string("'") + name + "' is not a string");
    return v.get<std::string>();
}

/// Optional string field: absent or null = empty (CometBFT omits empty fields in some answers).
std::string opt_str(const Json& o, const char* name) {
    if (!o.is_object()) bad("expected an object");
    const auto it = o.find(name);
    if (it == o.end() || it->is_null()) return {};
    if (!it->is_string()) bad(std::string("'") + name + "' is not a string");
    return it->get<std::string>();
}

bool boolean(const Json& o, const char* name) {
    const Json& v = field(o, name);
    if (!v.is_boolean()) bad(std::string("'") + name + "' is not a boolean");
    return v.get<bool>();
}

/// int64 encoded as a decimal string (CometBFT JSON). Non-negative, digits only, no leading zeros.
std::uint64_t dec(const Json& o, const char* name) {
    const std::string s = str(o, name);
    if (s.empty() || s.size() > 19 || (s.size() > 1 && s[0] == '0')) bad(std::string("'") + name + "' is not a decimal");
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') bad(std::string("'") + name + "' is not a decimal");
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    if (v > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) bad(std::string("'") + name + "' out of range");
    return v;
}

/// uint32 encoded as a JSON number; absent = 0 (protobuf omits zero values).
std::uint32_t u32_opt(const Json& o, const char* name) {
    if (!o.is_object()) bad("expected an object");
    const auto it = o.find(name);
    if (it == o.end() || it->is_null()) return 0;
    if (!it->is_number_unsigned() && !(it->is_number_integer() && it->get<std::int64_t>() >= 0)) {
        bad(std::string("'") + name + "' is not an unsigned integer");
    }
    const auto v = it->get<std::uint64_t>();
    if (v > std::numeric_limits<std::uint32_t>::max()) bad(std::string("'") + name + "' out of range");
    return static_cast<std::uint32_t>(v);
}

/// Upper-case hex of `bytes` length (an empty string is allowed when `allow_empty`).
std::string hexfield(const Json& o, const char* name, std::size_t bytes, bool allow_empty) {
    const std::string s = str(o, name);
    if (s.empty() && allow_empty) return s;
    if (s.size() != bytes * 2) bad(std::string("'") + name + "' has the wrong length");
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) bad(std::string("'") + name + "' is not upper-case hex");
    }
    return s;
}

Bytes b64field(const Json& o, const char* name, bool optional) {
    if (!o.is_object()) bad("expected an object");
    const auto it = o.find(name);
    if (it == o.end() || it->is_null()) {
        if (optional) return {};
        bad(std::string("missing '") + name + "'");
    }
    if (!it->is_string()) bad(std::string("'") + name + "' is not a string");
    auto v = base64_decode(it->get<std::string>());
    if (!v) bad(std::string("'") + name + "' is not base64");
    return std::move(*v);
}

std::string upper_hex(ByteView b) {
    std::string h = to_hex(b);
    for (auto& c : h) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return h;
}

constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

std::optional<Bytes> base64_decode(std::string_view in) {
    if (in.size() % 4 != 0) return std::nullopt;
    Bytes out;
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + static_cast<std::size_t>(k)];
            if (c == '=') {
                if (!last || k < 2) return std::nullopt;
                v[k] = 0;
                ++pad;
            } else {
                if (pad != 0) return std::nullopt;  // data after padding
                v[k] = b64val(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const std::uint32_t n = (static_cast<std::uint32_t>(v[0]) << 18) | (static_cast<std::uint32_t>(v[1]) << 12) |
                                (static_cast<std::uint32_t>(v[2]) << 6) | static_cast<std::uint32_t>(v[3]);
        if (pad == 2 && (n & 0xFFFF) != 0) return std::nullopt;  // non-canonical trailing bits
        if (pad == 1 && (n & 0xFF) != 0) return std::nullopt;
        out.push_back(static_cast<Byte>(n >> 16));
        if (pad < 2) out.push_back(static_cast<Byte>(n >> 8));
        if (pad < 1) out.push_back(static_cast<Byte>(n));
    }
    return out;
}

std::string base64_encode(ByteView in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const std::size_t rem = in.size() - i;
        const std::uint32_t n = (static_cast<std::uint32_t>(in[i]) << 16) |
                                (rem > 1 ? static_cast<std::uint32_t>(in[i + 1]) << 8 : 0) |
                                (rem > 2 ? static_cast<std::uint32_t>(in[i + 2]) : 0);
        out.push_back(kB64[(n >> 18) & 63]);
        out.push_back(kB64[(n >> 12) & 63]);
        out.push_back(rem > 1 ? kB64[(n >> 6) & 63] : '=');
        out.push_back(rem > 2 ? kB64[n & 63] : '=');
    }
    return out;
}

std::string parse_envelope(const std::string& body, std::uint64_t expected_id) {
    const Json j = parse_json(body);
    if (!j.is_object()) bad("the envelope is not an object");
    if (str(j, "jsonrpc") != "2.0") bad("jsonrpc is not \"2.0\"");
    const Json& id = field(j, "id");
    if (!id.is_number_integer() || id.get<std::int64_t>() < 0 || id.get<std::uint64_t>() != expected_id) {
        bad("response id does not match the request");
    }
    const bool has_result = j.contains("result");
    const bool has_error = j.contains("error");
    if (has_result == has_error) bad("exactly one of result / error expected");
    if (has_error) {
        const Json& e = j["error"];
        if (!e.is_object()) bad("error is not an object");
        const Json& code = field(e, "code");
        if (!code.is_number_integer()) bad("error code is not an integer");
        throw RpcError(code.get<long>(), str(e, "message"), opt_str(e, "data"));
    }
    if (!j["result"].is_object()) bad("result is not an object");
    return j["result"].dump();
}

Status parse_status(const std::string& result_json) {
    const Json r = parse_json(result_json);
    const Json& ni = object(r, "node_info");
    const Json& si = object(r, "sync_info");
    Status s;
    s.chain_id = str(ni, "network");
    s.node_id = str(ni, "id");
    s.moniker = opt_str(ni, "moniker");
    s.version = opt_str(ni, "version");
    s.latest_height = dec(si, "latest_block_height");
    s.latest_block_hash = hexfield(si, "latest_block_hash", 32, true);
    s.latest_app_hash = hexfield(si, "latest_app_hash", 32, true);
    s.latest_block_time = str(si, "latest_block_time");
    s.earliest_height = dec(si, "earliest_block_height");
    s.catching_up = boolean(si, "catching_up");
    if (r.contains("validator_info")) s.voting_power = dec(object(r, "validator_info"), "voting_power");
    if (s.chain_id.empty() || s.chain_id.size() > 50) bad("node_info.network is empty or too long");
    return s;
}

NetInfo parse_net_info(const std::string& result_json) {
    const Json r = parse_json(result_json);
    NetInfo n;
    n.listening = boolean(r, "listening");
    n.n_peers = dec(r, "n_peers");
    const Json& peers = field(r, "peers");
    if (!peers.is_array()) bad("peers is not an array");
    for (const auto& p : peers) {
        const Json& ni = object(p, "node_info");
        n.peers.push_back({str(ni, "id"), opt_str(ni, "moniker"), opt_str(p, "remote_ip"),
                           p.contains("is_outbound") && boolean(p, "is_outbound")});
    }
    if (n.peers.size() != n.n_peers) bad("n_peers differs from the peer list");
    return n;
}

AbciQuery parse_abci_query(const std::string& result_json) {
    const Json r = parse_json(result_json);
    const Json& resp = object(r, "response");
    AbciQuery q;
    q.code = u32_opt(resp, "code");
    q.codespace = opt_str(resp, "codespace");
    q.log = opt_str(resp, "log");
    q.height = resp.contains("height") ? dec(resp, "height") : 0;
    q.value = b64field(resp, "value", true);
    return q;
}

BlockInfo parse_block(const std::string& result_json) {
    const Json r = parse_json(result_json);
    BlockInfo b;
    b.block_hash = hexfield(object(r, "block_id"), "hash", 32, false);
    const Json& blk = object(r, "block");
    const Json& h = object(blk, "header");
    b.chain_id = str(h, "chain_id");
    b.height = dec(h, "height");
    b.time = str(h, "time");
    b.app_hash = hexfield(h, "app_hash", 32, true);
    b.proposer_address = hexfield(h, "proposer_address", 20, false);
    const Json& data = object(blk, "data");
    if (data.contains("txs") && !data["txs"].is_null()) {
        const Json& txs = data["txs"];
        if (!txs.is_array()) bad("txs is not an array");
        for (const auto& t : txs) {
            if (!t.is_string()) bad("a tx is not a string");
            auto v = base64_decode(t.get<std::string>());
            if (!v) bad("a tx is not base64");
            b.txs.push_back(std::move(*v));
        }
    }
    const Json& lc = object(blk, "last_commit");
    const Json& sigs = field(lc, "signatures");
    if (!sigs.is_array()) bad("signatures is not an array");
    b.last_commit_signatures = sigs.size();
    return b;
}

TxResult parse_tx(const std::string& result_json) {
    const Json r = parse_json(result_json);
    TxResult t;
    t.hash = hexfield(r, "hash", 32, false);
    t.height = dec(r, "height");
    const Json& idx = field(r, "index");
    if (!idx.is_number_unsigned() && !(idx.is_number_integer() && idx.get<std::int64_t>() >= 0)) bad("index");
    if (idx.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max()) bad("index out of range");
    t.index = static_cast<std::uint32_t>(idx.get<std::uint64_t>());
    const Json& res = object(r, "tx_result");
    t.code = u32_opt(res, "code");
    t.codespace = opt_str(res, "codespace");
    t.log = opt_str(res, "log");
    t.tx = b64field(r, "tx", false);
    return t;
}

BroadcastResult parse_broadcast(const std::string& result_json) {
    const Json r = parse_json(result_json);
    BroadcastResult b;
    b.code = u32_opt(r, "code");
    b.codespace = opt_str(r, "codespace");
    b.log = opt_str(r, "log");
    b.hash = hexfield(r, "hash", 32, false);
    return b;
}

Client::Client(std::string endpoint, HttpOptions opts) : endpoint_(std::move(endpoint)), opts_(std::move(opts)) {
    if (const auto err = check_endpoint(endpoint_); !err.empty()) {
        throw TransportError(TransportErrorKind::InvalidUrl, false, err);
    }
}

std::string Client::call(const std::string& method, const std::string& params_json) {
    const std::uint64_t id = next_id_++;
    const std::string body = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + method +
                             R"(","params":)" + params_json + "}";
    const HttpResponse resp = http_post(endpoint_, body, opts_);
    try {
        return parse_envelope(resp.body, id);
    } catch (const MalformedResponse& e) {
        if (resp.status != 200) {
            throw MalformedResponse("HTTP " + std::to_string(resp.status) + " without a JSON-RPC answer");
        }
        throw;
    }
}

Status Client::status() { return parse_status(call("status", "{}")); }

NetInfo Client::net_info() { return parse_net_info(call("net_info", "{}")); }

AbciQuery Client::abci_query(const std::string& path) {
    Json p;
    p["path"] = path;
    p["data"] = "";
    p["height"] = "0";
    p["prove"] = false;
    return parse_abci_query(call("abci_query", p.dump()));
}

std::optional<BlockInfo> Client::block(std::optional<std::uint64_t> height) {
    Json p = Json::object();
    if (height) p["height"] = std::to_string(*height);
    try {
        return parse_block(call("block", p.dump()));
    } catch (const RpcError& e) {
        // "height H must be less than or equal to the current blockchain height T" /
        // "height H is not available, lowest height is L"
        if (e.data().contains("must be less than or equal to the current blockchain height") ||
            e.data().contains("is not available")) {
            return std::nullopt;
        }
        throw;
    }
}

std::optional<TxResult> Client::tx(const Hash32& hash) {
    Json p;
    p["hash"] = base64_encode(hash);
    p["prove"] = false;
    try {
        auto t = parse_tx(call("tx", p.dump()));
        if (t.hash != upper_hex(hash)) bad("tx answer is for a different hash");
        return t;
    } catch (const RpcError& e) {
        const std::string want = "tx (" + upper_hex(hash) + ") not found";
        if (e.data() == want) return std::nullopt;
        throw;
    }
}

BroadcastResult Client::broadcast_tx_sync(ByteView tx) {
    Json p;
    p["tx"] = base64_encode(tx);
    return parse_broadcast(call("broadcast_tx_sync", p.dump()));
}

}  // namespace yap::rpc
