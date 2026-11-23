// SPDX-License-Identifier: Apache-2.0
//
// Local development chain on top of the Engine (protocol.md §14, ADR-0025): a single trusted
// producer signs blocks; every block — produced locally or imported — passes the same full
// validation, and is committed together with the resulting state in ONE store transaction.
#pragma once

#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include "yap/chain/engine.hpp"
#include "yap/dev/dev_block.hpp"

namespace yap::dev {

/// Why a dev block was rejected. The state is unchanged whenever this is thrown.
enum class Reject : std::uint8_t {
    BadVersion = 1,
    WrongChain,
    AlreadyHave,       ///< height already committed (re-import): never applied twice
    HeightGap,
    WrongParent,
    BadTime,
    WrongProducer,
    BadSignature,
    BadTxRoot,
    InvalidTransaction,
    BadStateRoot,
    Conflict,          ///< replay: a different block is already stored at this height
    LimitExceeded,     ///< more than 10 000 transactions or a transaction above 65 536 bytes (§3.3)
};
[[nodiscard]] std::string_view to_string(Reject r) noexcept;

class BlockRejected : public std::runtime_error {
public:
    BlockRejected(Reject reason, const std::string& detail)
        : std::runtime_error(std::string(to_string(reason)) + ": " + detail), reason_(reason) {}
    [[nodiscard]] Reject reason() const noexcept { return reason_; }

private:
    Reject reason_;
};

struct DevConfig {
    std::uint32_t version = 1;
    crypto::PublicKey producer{};
    void encode_to(ycs::Writer& w) const;
    static DevConfig decode_from(ycs::Reader& r);
};

struct ReplayStats {
    std::size_t imported = 0;
    std::size_t skipped = 0;  ///< identical blocks already stored
};

class DevChain {
public:
    /// Initialises a dev chain (genesis state + producer key) atomically. On an already initialised
    /// store this is a no-op returning false if genesis and producer match, and an error otherwise.
    static bool init(storage::KvStore& store, const chain::Genesis& genesis, const crypto::PublicKey& producer);

    /// Attaches to an initialised dev chain. Throws chain::StoreCorruption if the dev config is missing.
    explicit DevChain(storage::KvStore& store);

    /// Builds, signs and commits the next block from `candidates` (invalid ones are left out).
    /// Time defaults to last time + 2000 ms (deterministic, never the wall clock).
    /// `codes` (optional) receives the execution code of every candidate (OK = included).
    DevBlock produce(const crypto::SecretKey& producer, const std::vector<Bytes>& candidates,
                     std::optional<std::uint64_t> time_ms = std::nullopt, std::vector<tx::TxCode>* codes = nullptr);

    /// Fully validates and commits `block` (protocol.md §14). Throws BlockRejected.
    state::BlockResult import(const DevBlock& block);

    /// Imports blocks in order; identical stored blocks are skipped, different ones are a Conflict.
    ReplayStats replay(std::span<const DevBlock> blocks);

    [[nodiscard]] std::optional<DevBlock> block(std::uint64_t height) const;
    [[nodiscard]] Hash32 tip_hash() const;
    [[nodiscard]] const DevConfig& config() const noexcept { return config_; }
    [[nodiscard]] chain::Engine& engine() noexcept { return engine_; }

    /// The BlockContext a dev block maps to (last_commit: empty at height 1, else every
    /// validator with flag Commit; proposer: the first validator) — protocol.md §14.
    [[nodiscard]] state::BlockContext context_for(const DevBlock& block) const;

private:
    storage::KvStore* store_;
    chain::Engine engine_;
    DevConfig config_;
};

[[nodiscard]] Bytes dev_config_key();
[[nodiscard]] Bytes block_key(std::uint64_t height);

}  // namespace yap::dev
