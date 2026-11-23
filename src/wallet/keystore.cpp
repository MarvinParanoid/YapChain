// SPDX-License-Identifier: Apache-2.0
#include "yap/wallet/keystore.hpp"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

#include "yap/common/hex.hpp"
#include "yap/crypto/sodium.hpp"

namespace yap::wallet {
namespace {

using nlohmann::json;

constexpr std::string_view kAadPrefix{"YAPCHAIN/KEYSTORE/V1\0", 21};
constexpr int kVersion = 1;
constexpr std::size_t kSaltBytes = crypto_pwhash_SALTBYTES;
constexpr std::size_t kNonceBytes = crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;
constexpr std::size_t kKeyBytes = crypto_aead_xchacha20poly1305_ietf_KEYBYTES;
constexpr std::size_t kTagBytes = crypto_aead_xchacha20poly1305_ietf_ABYTES;
static_assert(kSaltBytes == 16 && kNonceBytes == 24 && kKeyBytes == 32);

/// Guarded secret buffer (sodium_malloc: guard pages, mlock, zeroed on free).
class Secret {
public:
    explicit Secret(std::size_t n) : n_(n), p_(static_cast<unsigned char*>(sodium_malloc(n))) {
        if (p_ == nullptr) throw std::bad_alloc();
    }
    ~Secret() { sodium_free(p_); }
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    [[nodiscard]] unsigned char* data() noexcept { return p_; }
    [[nodiscard]] std::size_t size() const noexcept { return n_; }

private:
    std::size_t n_;
    unsigned char* p_;
};

struct Fields {
    KeyInfo info;
    KdfParams kdf;
    FixedBytes<kSaltBytes> salt{};
    FixedBytes<kNonceBytes> nonce{};
    FixedBytes<32 + kTagBytes> ciphertext{};
};

Bytes aad(KeyPurpose purpose, const KdfParams& kdf, ByteView salt, const crypto::PublicKey& pk) {
    Bytes a(kAadPrefix.begin(), kAadPrefix.end());
    a.push_back(static_cast<Byte>(purpose));
    for (int i = 0; i < 8; ++i) a.push_back(static_cast<Byte>(kdf.opslimit >> (8 * i)));
    for (int i = 0; i < 8; ++i) a.push_back(static_cast<Byte>(kdf.memlimit >> (8 * i)));
    a.insert(a.end(), salt.begin(), salt.end());
    a.insert(a.end(), pk.begin(), pk.end());
    return a;
}

bool kdf_in_range(const KdfParams& k) {
    return k.opslimit >= crypto_pwhash_OPSLIMIT_INTERACTIVE && k.opslimit <= crypto_pwhash_OPSLIMIT_SENSITIVE &&
           k.memlimit >= crypto_pwhash_MEMLIMIT_INTERACTIVE && k.memlimit <= crypto_pwhash_MEMLIMIT_SENSITIVE;
}

void derive_key(Secret& key, std::string_view password, ByteView salt, const KdfParams& kdf) {
    if (crypto_pwhash(key.data(), key.size(), password.data(), password.size(), salt.data(), kdf.opslimit,
                      static_cast<std::size_t>(kdf.memlimit), crypto_pwhash_ALG_ARGON2ID13) != 0) {
        throw std::runtime_error("Argon2id key derivation failed (out of memory?)");
    }
}

std::optional<KeyPurpose> purpose_from(std::string_view s) {
    if (s == "account") return KeyPurpose::Account;
    if (s == "dev-producer") return KeyPurpose::DevProducer;
    return std::nullopt;
}

std::optional<std::uint64_t> u64_from(const json& j) {
    if (!j.is_string()) return std::nullopt;
    const auto& s = j.get_ref<const std::string&>();
    if (s.empty() || s.size() > 20 || (s.size() > 1 && s[0] == '0')) return std::nullopt;
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        if (v > (UINT64_MAX - static_cast<std::uint64_t>(c - '0')) / 10) return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

template <std::size_t N>
std::optional<FixedBytes<N>> hex_field(const json& j) {
    if (!j.is_string()) return std::nullopt;
    return from_hex_fixed<N>(j.get<std::string>());
}

Result<Fields, KeystoreError> parse(std::string_view text) {
    std::vector<std::set<std::string>> scopes;
    bool duplicate = false;
    const json::parser_callback_t cb = [&](int, json::parse_event_t ev, json& parsed) {
        if (ev == json::parse_event_t::object_start) scopes.emplace_back();
        if (ev == json::parse_event_t::object_end && !scopes.empty()) scopes.pop_back();
        if (ev == json::parse_event_t::key && !scopes.empty() && !scopes.back().insert(parsed.get<std::string>()).second) {
            duplicate = true;
        }
        return true;
    };
    const json j = json::parse(text.begin(), text.end(), cb, false);
    if (j.is_discarded() || duplicate || !j.is_object()) return unexpected(KeystoreError::Malformed);
    static const std::set<std::string> names{"version", "purpose", "kdf",        "opslimit",   "memlimit",
                                             "salt",    "nonce",   "ciphertext", "public_key", "address"};
    if (j.size() != names.size()) return unexpected(KeystoreError::Malformed);
    for (const auto& [k, _] : j.items()) {
        if (!names.contains(k)) return unexpected(KeystoreError::Malformed);
    }
    if (!j.at("version").is_number_unsigned()) return unexpected(KeystoreError::Malformed);
    if (j.at("version").get<std::uint64_t>() != kVersion) return unexpected(KeystoreError::UnsupportedVersion);
    if (!j.at("kdf").is_string() || j.at("kdf").get<std::string>() != "argon2id") {
        return unexpected(KeystoreError::UnsupportedKdf);
    }
    Fields f;
    if (!j.at("purpose").is_string()) return unexpected(KeystoreError::Malformed);
    const auto purpose = purpose_from(j.at("purpose").get<std::string>());
    const auto ops = u64_from(j.at("opslimit"));
    const auto mem = u64_from(j.at("memlimit"));
    const auto salt = hex_field<kSaltBytes>(j.at("salt"));
    const auto nonce = hex_field<kNonceBytes>(j.at("nonce"));
    const auto ct = hex_field<32 + kTagBytes>(j.at("ciphertext"));
    const auto pk = hex_field<32>(j.at("public_key"));
    if (!purpose || !ops || !mem || !salt || !nonce || !ct || !pk || !j.at("address").is_string()) {
        return unexpected(KeystoreError::Malformed);
    }
    f.kdf = {*ops, *mem};
    if (!kdf_in_range(f.kdf)) return unexpected(KeystoreError::UnsupportedKdf);
    f.salt = *salt;
    f.nonce = *nonce;
    f.ciphertext = *ct;
    f.info.purpose = *purpose;
    f.info.public_key = *pk;
    f.info.account_id = crypto::account_id_from_public_key(*pk);
    f.info.address = j.at("address").get<std::string>();
    // The address is derived data: it must be the canonical address of the public key.
    if (f.info.address != crypto::encode_address(crypto::kTestnetHrp, f.info.account_id)) {
        return unexpected(KeystoreError::Inconsistent);
    }
    return f;
}

}  // namespace

std::string_view to_string(KeyPurpose p) noexcept {
    return p == KeyPurpose::Account ? "account" : "dev-producer";
}

std::string_view to_string(KeystoreError e) noexcept {
    switch (e) {
        case KeystoreError::Malformed: return "malformed keystore file";
        case KeystoreError::UnsupportedVersion: return "unsupported keystore version";
        case KeystoreError::UnsupportedKdf: return "unsupported or out-of-range KDF parameters";
        case KeystoreError::WrongPurpose: return "key has a different purpose";
        case KeystoreError::WrongPasswordOrCorrupted: return "wrong password or corrupted keystore";
        case KeystoreError::Inconsistent: return "keystore is internally inconsistent";
    }
    return "unknown keystore error";
}

KdfParams KdfParams::moderate() noexcept { return {crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE}; }
KdfParams KdfParams::interactive() noexcept {
    return {crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE};
}

NewKey create_keystore(std::string_view password, KeyPurpose purpose, const KdfParams& kdf) {
    crypto::ensure_initialized();
    if (!kdf_in_range(kdf)) throw std::invalid_argument("KDF parameters out of the accepted range");
    Secret seed(32);
    randombytes_buf(seed.data(), seed.size());  // OS CSPRNG
    const auto key = crypto::SecretKey::from_seed_bytes(seed.data());

    FixedBytes<kSaltBytes> salt{};
    FixedBytes<kNonceBytes> nonce{};
    randombytes_buf(salt.data(), salt.size());
    randombytes_buf(nonce.data(), nonce.size());
    Secret enc_key(kKeyBytes);
    derive_key(enc_key, password, salt, kdf);
    const Bytes ad = aad(purpose, kdf, salt, key.public_key());
    FixedBytes<32 + kTagBytes> ct{};
    unsigned long long ct_len = 0;
    crypto_aead_xchacha20poly1305_ietf_encrypt(ct.data(), &ct_len, seed.data(), seed.size(), ad.data(), ad.size(),
                                               nullptr, nonce.data(), enc_key.data());

    NewKey out;
    out.info.purpose = purpose;
    out.info.public_key = key.public_key();
    out.info.account_id = crypto::account_id_from_public_key(key.public_key());
    out.info.address = crypto::encode_address(crypto::kTestnetHrp, out.info.account_id);
    json j = {{"version", kVersion},
              {"purpose", std::string(to_string(purpose))},
              {"kdf", "argon2id"},
              {"opslimit", std::to_string(kdf.opslimit)},
              {"memlimit", std::to_string(kdf.memlimit)},
              {"salt", to_hex(salt)},
              {"nonce", to_hex(nonce)},
              {"ciphertext", to_hex(ct)},
              {"public_key", to_hex(out.info.public_key)},
              {"address", out.info.address}};
    out.json = j.dump(2) + "\n";
    return out;
}

Result<KeyInfo, KeystoreError> read_key_info(std::string_view json_text) {
    auto f = parse(json_text);
    if (!f) return unexpected(f.error());
    return f->info;
}

Result<crypto::SecretKey, KeystoreError> unlock_keystore(std::string_view json_text, std::string_view password,
                                                         KeyPurpose expected) {
    crypto::ensure_initialized();
    auto f = parse(json_text);
    if (!f) return unexpected(f.error());
    if (f->info.purpose != expected) return unexpected(KeystoreError::WrongPurpose);
    Secret enc_key(kKeyBytes);
    derive_key(enc_key, password, f->salt, f->kdf);
    const Bytes ad = aad(f->info.purpose, f->kdf, f->salt, f->info.public_key);
    Secret seed(32);
    unsigned long long seed_len = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(seed.data(), &seed_len, nullptr, f->ciphertext.data(),
                                                   f->ciphertext.size(), ad.data(), ad.size(), f->nonce.data(),
                                                   enc_key.data()) != 0 ||
        seed_len != 32) {
        return unexpected(KeystoreError::WrongPasswordOrCorrupted);
    }
    auto key = crypto::SecretKey::from_seed_bytes(seed.data());
    if (key.public_key() != f->info.public_key) return unexpected(KeystoreError::Inconsistent);
    return key;
}

// ------------------------------------------------------------------ files

bool valid_key_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 32) return false;
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

void write_private_file(const std::filesystem::path& path, std::string_view content) {
    const auto dir = path.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
    if (std::filesystem::exists(path)) throw std::runtime_error(path.string() + " already exists");

    const auto tmp = path.string() + ".tmp-" + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("cannot create " + tmp + ": " + std::strerror(errno));
    std::size_t done = 0;
    while (done < content.size()) {
        const auto n = ::write(fd, content.data() + done, content.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error("cannot write " + tmp);
        }
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
        ::unlink(tmp.c_str());
        throw std::runtime_error("cannot sync " + tmp);
    }
    // link() fails if the target exists: an existing key is never overwritten.
    if (::link(tmp.c_str(), path.c_str()) != 0) {
        const int err = errno;
        ::unlink(tmp.c_str());
        throw std::runtime_error("cannot create " + path.string() + ": " + std::strerror(err));
    }
    ::unlink(tmp.c_str());
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
}

std::string read_private_file(const std::filesystem::path& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) throw std::runtime_error(path.string() + ": no such key file");
    if ((st.st_mode & 077) != 0) {
        throw std::runtime_error(path.string() + ": permissions too open (must not be accessible by group/others)");
    }
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return ss.str();
}

}  // namespace yap::wallet
