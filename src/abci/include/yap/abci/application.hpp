// SPDX-License-Identifier: Apache-2.0
//
// ABCI++ application: connects CometBFT (through yap::abci::Server) to the deterministic
// chain::Engine (docs/cometbft-compatibility.md, protocol.md §9–§11).
//
// Synchronisation: CometBFT calls in parallel on four connections (consensus, mempool, query,
// snapshot). Every request is handled under ONE mutex, so the Engine is never used concurrently
// and the order of calls is the order in which they acquire the lock.
//
// Failure model: transaction errors are result codes; a malformed query is an error code. Anything
// else — inconsistent consensus input, storage failure, a method we do not support — throws, the
// transport answers with ResponseException and the node stops. The failure is sticky: after it,
// every further request throws, so no response after a fail-stop can look like success.
#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include "tendermint/abci/types.pb.h"
#include "yap/abci/server.hpp"
#include "yap/chain/engine.hpp"

namespace yap::abci {

struct ApplicationOptions {
    std::string software_version = "0.1.0";
    /// Diagnostics (non-consensus): commits, re-executions, fatal errors.
    std::function<void(const std::string&)> log;
    /// Also log one line per request (method, height, sizes) — for checking real CometBFT traffic.
    bool trace = false;
};

class Application final : public Handler {
public:
    /// `engine` must outlive the application. Throws std::invalid_argument if the store belongs to a
    /// dev chain (ADR-0025): dev-chain and ABCI mode never share a database.
    explicit Application(chain::Engine& engine, ApplicationOptions options = {});

    void handle(const tendermint::abci::Request& request, tendermint::abci::Response& response) override;

    /// The first fatal error, if any.
    [[nodiscard]] std::optional<std::string> failure() const;

private:
    void dispatch(const tendermint::abci::Request& req, tendermint::abci::Response& resp);
    void info(tendermint::abci::ResponseInfo& r) const;
    void init_chain(const tendermint::abci::RequestInitChain& req, tendermint::abci::ResponseInitChain& r);
    void query(const tendermint::abci::RequestQuery& req, tendermint::abci::ResponseQuery& r) const;
    void check_tx(const tendermint::abci::RequestCheckTx& req, tendermint::abci::ResponseCheckTx& r);
    void prepare_proposal(const tendermint::abci::RequestPrepareProposal& req,
                          tendermint::abci::ResponsePrepareProposal& r) const;
    void process_proposal(const tendermint::abci::RequestProcessProposal& req,
                          tendermint::abci::ResponseProcessProposal& r) const;
    void finalize_block(const tendermint::abci::RequestFinalizeBlock& req, tendermint::abci::ResponseFinalizeBlock& r);
    void commit(tendermint::abci::ResponseCommit& r);
    void log(const std::string& line) const;

    mutable std::mutex mu_;
    chain::Engine& engine_;
    ApplicationOptions opts_;
    std::optional<std::string> failure_;
};

/// Query result codes (non-consensus; codespace "yap.query").
enum class QueryCode : std::uint32_t {
    Ok = 0,
    UnknownPath = 1,
    BadRequest = 2,
    HeightUnavailable = 3,
    ProofUnsupported = 4,
    NotInitialized = 5,
};

/// Throws std::invalid_argument if `store` holds a dev chain (it must not be driven by CometBFT).
void require_consensus_store(const storage::KvStore& store);

}  // namespace yap::abci
