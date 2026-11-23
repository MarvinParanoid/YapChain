// SPDX-License-Identifier: Apache-2.0
#include "yap/storage/lmdb_store.hpp"

#include <fcntl.h>
#include <lmdb.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <array>
#include <string>
#include <system_error>

namespace yap::storage {
namespace {

[[noreturn]] void throw_lmdb(int rc, const std::string& what) {
    const std::string msg = what + ": " + mdb_strerror(rc);
    if (rc == MDB_MAP_FULL) throw StorageFull(msg, rc);
    throw StorageError(msg, rc);
}

void check(int rc, const char* what) {
    if (rc != MDB_SUCCESS) throw_lmdb(rc, what);
}

MDB_val as_val(ByteView b) {
    // An empty span may have data() == nullptr; LMDB would then call memcpy(dst, nullptr, 0),
    // which is undefined behaviour (caught by UBSan). Point empty values at a valid dummy byte.
    static Byte empty_dummy = 0;
    Byte* p = b.empty() ? &empty_dummy : const_cast<Byte*>(b.data());  // LMDB does not modify inputs
    return MDB_val{b.size(), p};
}

Bytes to_bytes(const MDB_val& v) {
    const auto* p = static_cast<const Byte*>(v.mv_data);
    return Bytes(p, p + v.mv_size);
}

/// RAII transaction: aborted on destruction unless committed.
class Txn {
public:
    Txn(MDB_env* env, unsigned flags) {
        check(mdb_txn_begin(env, nullptr, flags, &txn_), "mdb_txn_begin");
    }
    ~Txn() {
        if (txn_ != nullptr) mdb_txn_abort(txn_);
    }
    Txn(const Txn&) = delete;
    Txn& operator=(const Txn&) = delete;

    [[nodiscard]] MDB_txn* get() const noexcept { return txn_; }
    void commit() {
        MDB_txn* t = txn_;
        txn_ = nullptr;  // mdb_txn_commit frees the handle even on failure
        check(mdb_txn_commit(t), "mdb_txn_commit");
    }

private:
    MDB_txn* txn_ = nullptr;
};

/// RAII cursor.
class Cursor {
public:
    Cursor(MDB_txn* txn, MDB_dbi dbi) { check(mdb_cursor_open(txn, dbi, &cur_), "mdb_cursor_open"); }
    ~Cursor() { mdb_cursor_close(cur_); }
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    [[nodiscard]] MDB_cursor* get() const noexcept { return cur_; }

private:
    MDB_cursor* cur_ = nullptr;
};

}  // namespace

struct LmdbStore::Impl {
    MDB_env* env = nullptr;
    std::array<MDB_dbi, kTableCount> dbi{};
    bool read_only = false;
    int lock_fd = -1;  ///< exclusive writer lock (flock) on <dir>/yap.lock

    ~Impl() {
        if (env != nullptr) mdb_env_close(env);  // closes all DBI handles
        if (lock_fd >= 0) ::close(lock_fd);      // releases the writer lock
    }
};

LmdbStore::LmdbStore(const Options& options) : impl_(std::make_unique<Impl>()) {
    impl_->read_only = options.read_only;
    if (options.create && !options.read_only) {
        std::error_code ec;
        std::filesystem::create_directories(options.dir, ec);
        if (ec) throw StorageError("cannot create " + options.dir.string() + ": " + ec.message());
    }
    if (!options.read_only) {
        // One writing process per database: LMDB serialises single transactions, but the engine's
        // read-modify-write of a whole block must not interleave with another writer. The lock is
        // per open file description, so it also prevents opening the same environment twice for
        // writing in one process (which LMDB forbids). It is released when the process dies.
        const auto lock_path = options.dir / "yap.lock";
        impl_->lock_fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (impl_->lock_fd < 0) {
            throw StorageError("cannot open " + lock_path.string() + ": " + std::strerror(errno));
        }
        if (::flock(impl_->lock_fd, LOCK_EX | LOCK_NB) != 0) {
            throw StorageError("database " + options.dir.string() + " is in use by another writer");
        }
    }
    check(mdb_env_create(&impl_->env), "mdb_env_create");
    check(mdb_env_set_maxdbs(impl_->env, kTableCount), "mdb_env_set_maxdbs");
    check(mdb_env_set_mapsize(impl_->env, options.map_size), "mdb_env_set_mapsize");
    // MDB_NOTLS: read transactions are not bound to a thread slot, so snapshots can coexist with
    // other reads in the same thread. It does not affect durability.
    const unsigned env_flags = (options.read_only ? MDB_RDONLY : 0U) | MDB_NOTLS;
    check(mdb_env_open(impl_->env, options.dir.c_str(), env_flags, 0600), "mdb_env_open");

    // Open (and, when allowed, create) one named database per table.
    Txn txn(impl_->env, options.read_only ? MDB_RDONLY : 0U);
    const unsigned db_flags = (options.create && !options.read_only) ? MDB_CREATE : 0U;
    for (std::size_t i = 0; i < kTableCount; ++i) {
        const std::string name(table_name(static_cast<Table>(i)));
        check(mdb_dbi_open(txn.get(), name.c_str(), db_flags, &impl_->dbi[i]), "mdb_dbi_open");
    }
    txn.commit();
}

LmdbStore::~LmdbStore() = default;

std::optional<Bytes> LmdbStore::get(Table t, ByteView key) const {
    Txn txn(impl_->env, MDB_RDONLY);
    MDB_val k = as_val(key);
    MDB_val v{};
    const int rc = mdb_get(txn.get(), impl_->dbi[static_cast<std::size_t>(t)], &k, &v);
    if (rc == MDB_NOTFOUND) return std::nullopt;
    check(rc, "mdb_get");
    return to_bytes(v);  // copied before the read transaction ends
}

void LmdbStore::for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const {
    Txn txn(impl_->env, MDB_RDONLY);
    Cursor cur(txn.get(), impl_->dbi[static_cast<std::size_t>(t)]);
    MDB_val k{};
    MDB_val v{};
    int rc = mdb_cursor_get(cur.get(), &k, &v, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        fn(to_bytes(k), to_bytes(v));
        rc = mdb_cursor_get(cur.get(), &k, &v, MDB_NEXT);
    }
    if (rc != MDB_NOTFOUND) throw_lmdb(rc, "mdb_cursor_get");
}

void LmdbStore::write(const WriteBatch& batch) {
    if (impl_->read_only) throw StorageError("store is read-only");
    fault_point("before_txn");
    Txn txn(impl_->env, 0);
    for (const auto& op : batch.ops()) {
        const MDB_dbi dbi = impl_->dbi[static_cast<std::size_t>(op.table)];
        switch (op.kind) {
            case WriteOp::Kind::Put: {
                MDB_val k = as_val(op.key);
                MDB_val v = as_val(op.value);
                check(mdb_put(txn.get(), dbi, &k, &v, 0), "mdb_put");
                break;
            }
            case WriteOp::Kind::Erase: {
                MDB_val k = as_val(op.key);
                const int rc = mdb_del(txn.get(), dbi, &k, nullptr);
                if (rc != MDB_NOTFOUND) check(rc, "mdb_del");
                break;
            }
            case WriteOp::Kind::Marker:
                fault_point(op.marker);
                break;
            case WriteOp::Kind::Expect: {
                MDB_val k = as_val(op.key);
                MDB_val v{};
                const int rc = mdb_get(txn.get(), dbi, &k, &v);
                if (rc != MDB_SUCCESS && rc != MDB_NOTFOUND) check(rc, "mdb_get");
                const bool present = rc == MDB_SUCCESS;
                const bool ok = op.expect_absent ? !present : (present && to_bytes(v) == op.value);
                if (!ok) throw StorageConflict("store changed by another writer (precondition failed)");
                break;
            }
        }
    }
    fault_point("before_commit");
    txn.commit();  // synced to disk before returning (no MDB_NOSYNC)
    fault_point("after_commit");
}

namespace {

/// Read-only KvStore bound to one MDB_RDONLY transaction.
class LmdbSnapshot final : public KvStore {
public:
    LmdbSnapshot(MDB_env* env, const std::array<MDB_dbi, kTableCount>& dbi) : txn_(env, MDB_RDONLY), dbi_(dbi) {}

    [[nodiscard]] std::optional<Bytes> get(Table t, ByteView key) const override {
        MDB_val k = as_val(key);
        MDB_val v{};
        const int rc = mdb_get(txn_.get(), dbi_[static_cast<std::size_t>(t)], &k, &v);
        if (rc == MDB_NOTFOUND) return std::nullopt;
        check(rc, "mdb_get");
        return to_bytes(v);
    }
    void for_each(Table t, const std::function<void(const Bytes&, const Bytes&)>& fn) const override {
        Cursor cur(txn_.get(), dbi_[static_cast<std::size_t>(t)]);
        MDB_val k{};
        MDB_val v{};
        int rc = mdb_cursor_get(cur.get(), &k, &v, MDB_FIRST);
        while (rc == MDB_SUCCESS) {
            fn(to_bytes(k), to_bytes(v));
            rc = mdb_cursor_get(cur.get(), &k, &v, MDB_NEXT);
        }
        if (rc != MDB_NOTFOUND) throw_lmdb(rc, "mdb_cursor_get");
    }
    void write(const WriteBatch&) override { throw StorageError("snapshot is read-only"); }
    [[nodiscard]] std::unique_ptr<KvStore> snapshot() const override {
        return std::make_unique<ReadOnlyView>(*this);  // same transaction, same point in time
    }

private:
    Txn txn_;
    std::array<MDB_dbi, kTableCount> dbi_;
};

}  // namespace

std::unique_ptr<KvStore> LmdbStore::snapshot() const { return std::make_unique<LmdbSnapshot>(impl_->env, impl_->dbi); }

std::size_t LmdbStore::map_size() const {
    MDB_envinfo info{};
    check(mdb_env_info(impl_->env, &info), "mdb_env_info");
    return info.me_mapsize;
}

}  // namespace yap::storage
