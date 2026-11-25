// SPDX-License-Identifier: Apache-2.0
//
// CometBFT v0.40.0 JSON-RPC client (ADR-0029): JSON-RPC 2.0 over HTTP POST to the endpoint root.
// Every response is checked strictly (envelope, id, field types, decimal strings, hex/base64) and
// anything unexpected is a MalformedResponse. No call is retried; broadcast is never repeated.
//
// Trust: the answers are NOT verified (no light client, no state proofs). A remote RPC can hide data
// or lie about state and finality; signatures only protect the authorization of transactions.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "yap/common/bytes.hpp"
#include "yap/rpc/http.hpp"

namespace yap::rpc {

/// The node answered with a JSON-RPC error object.
class RpcError : public std::runtime_error {
public:
    RpcError(long code, std::string message, std::string data)
        : std::runtime_error("RPC error " + std::to_string(code) + ": " + message + (data.empty() ? "" : " (" + data + ")")),
          code_(code), message_(std::move(message)), data_(std::move(data)) {}
    [[nodiscard]] long code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::string& data() const noexcept { return data_; }

private:
    long code_;
    std::string message_, data_;
};

/// The response is not a well-formed answer of the expected shape.
class MalformedResponse : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Status {
    std::string chain_id;        ///< node_info.network
    std::string node_id;
    std::string moniker;
    std::string version;
    std::uint64_t latest_height = 0;
    std::string latest_block_hash;  ///< upper-case hex
    std::string latest_app_hash;    ///< upper-case hex (state root after latest_height - 1)
    std::string latest_block_time;
    std::uint64_t earliest_height = 0;
    bool catching_up = false;
    std::uint64_t voting_power = 0;  ///< of this node's validator key (0 for a full node)
};

struct Peer {
    std::string node_id, moniker, remote_ip;
    bool outbound = false;
};

struct NetInfo {
    bool listening = false;
    std::uint64_t n_peers = 0;
    std::vector<Peer> peers;
};

struct AbciQuery {
    std::uint32_t code = 0;
    std::string codespace;
    std::string log;
    std::uint64_t height = 0;  ///< the committed height the answer reflects (latest only, see ADR-0028)
    Bytes value;
};

struct BlockInfo {
    std::string block_hash;  ///< upper-case hex
    std::string chain_id;
    std::uint64_t height = 0;
    std::string time;
    std::string app_hash;          ///< state root after height - 1
    std::string proposer_address;
    std::vector<Bytes> txs;
    std::size_t last_commit_signatures = 0;
};

struct TxResult {
    std::string hash;  ///< upper-case hex (SHA-256 of the transaction bytes)
    std::uint64_t height = 0;
    std::uint32_t index = 0;
    std::uint32_t code = 0;
    std::string codespace;
    std::string log;
    Bytes tx;
};

struct BroadcastResult {
    std::uint32_t code = 0;  ///< CheckTx code: 0 = accepted into this node's mempool
    std::string codespace;
    std::string log;
    std::string hash;  ///< upper-case hex
};

class Client {
public:
    Client(std::string endpoint, HttpOptions opts);

    [[nodiscard]] Status status();
    [[nodiscard]] NetInfo net_info();
    /// ABCI Query at the latest committed height (the application answers latest only).
    [[nodiscard]] AbciQuery abci_query(const std::string& path);
    /// nullopt: the node has no block at this height (above its tip or pruned).
    [[nodiscard]] std::optional<BlockInfo> block(std::optional<std::uint64_t> height);
    /// nullopt: the node's transaction index has no such transaction. Throws RpcError if the node's
    /// indexer is disabled (tx_index.indexer = "null").
    [[nodiscard]] std::optional<TxResult> tx(const Hash32& hash);
    /// broadcast_tx_sync: the node's CheckTx answer. Never retried. A TransportError with
    /// request_sent() means the outcome is unknown.
    [[nodiscard]] BroadcastResult broadcast_tx_sync(ByteView tx);

    [[nodiscard]] const std::string& endpoint() const noexcept { return endpoint_; }

private:
    std::string call(const std::string& method, const std::string& params_json);
    std::string endpoint_;
    HttpOptions opts_;
    std::uint64_t next_id_ = 1;
};

// Strict parsers of the `result` objects (exposed for tests and fuzzing). They throw
// MalformedResponse on any deviation.
[[nodiscard]] std::string parse_envelope(const std::string& body, std::uint64_t expected_id);
[[nodiscard]] Status parse_status(const std::string& result_json);
[[nodiscard]] NetInfo parse_net_info(const std::string& result_json);
[[nodiscard]] AbciQuery parse_abci_query(const std::string& result_json);
[[nodiscard]] BlockInfo parse_block(const std::string& result_json);
[[nodiscard]] TxResult parse_tx(const std::string& result_json);
[[nodiscard]] BroadcastResult parse_broadcast(const std::string& result_json);

/// Strict standard base64 (RFC 4648, padding required); nullopt on any deviation.
[[nodiscard]] std::optional<Bytes> base64_decode(std::string_view in);
[[nodiscard]] std::string base64_encode(ByteView in);

}  // namespace yap::rpc
