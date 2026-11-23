// SPDX-License-Identifier: Apache-2.0
//
// LMDB-backed KvStore (ADR-0007, ADR-0022).
//
// - One LMDB environment per directory, one named database per Table.
// - Durability: default LMDB flags; MDB_NOSYNC / MDB_NOMETASYNC / MDB_MAPASYNC are never used,
//   so write() returns only after the transaction is synced to disk.
// - One writer at a time (LMDB serialises write transactions); any number of readers, each
//   reading from a consistent snapshot. Read operations use MDB_RDONLY transactions and never
//   modify the data file.
// - The environment must not be shared across fork(): open it after forking.
#pragma once

#include <filesystem>
#include <memory>

#include "yap/storage/store.hpp"

namespace yap::storage {

class LmdbStore final : public KvStore {
public:
    struct Options {
        std::filesystem::path dir;
        std::size_t map_size = std::size_t{1} << 30;  ///< maximum database size in bytes
        bool read_only = false;
        bool create = true;  ///< create the directory and tables if missing
    };

    /// Opens (or creates) the environment. Throws StorageError.
    explicit LmdbStore(const Options& options);
    ~LmdbStore() override;
    LmdbStore(const LmdbStore&) = delete;
    LmdbStore& operator=(const LmdbStore&) = delete;

    [[nodiscard]] std::optional<Bytes> get(Table t, ByteView key) const override;
    void for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const override;
    /// One LMDB write transaction per batch. Throws StorageFull on MDB_MAP_FULL and StorageError
    /// otherwise; in both cases the transaction is aborted and nothing is applied.
    void write(const WriteBatch& batch) override;
    /// Holds one MDB_RDONLY transaction for its lifetime (the environment uses MDB_NOTLS, so a thread
    /// may keep a snapshot open while doing other reads). Long-lived snapshots pin old pages.
    [[nodiscard]] std::unique_ptr<KvStore> snapshot() const override;

    [[nodiscard]] std::size_t map_size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace yap::storage
