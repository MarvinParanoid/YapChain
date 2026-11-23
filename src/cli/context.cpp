// SPDX-License-Identifier: Apache-2.0
#include "context.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "yap/common/hex.hpp"
#include "yap/wallet/keystore.hpp"

namespace yap::cli {

std::filesystem::path Context::key_path(std::string_view name) const {
    if (!wallet::valid_key_name(name)) {
        throw CliError(kExitRejected, "invalid key name '" + std::string(name) + "' (allowed: [a-z0-9_-]{1,32})");
    }
    return keys_dir() / (std::string(name) + ".json");
}

std::filesystem::path default_home() {
    if (const char* h = std::getenv("YAP_HOME"); h != nullptr && *h != '\0') return h;
    if (const char* h = std::getenv("HOME"); h != nullptr && *h != '\0') return std::filesystem::path(h) / ".yapchain";
    throw CliError(kExitUsage, "cannot determine the home directory: pass --home");
}

namespace {

std::string strip_newline(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

std::string read_tty_line(std::string_view prompt) {
    std::cerr << prompt << std::flush;
    termios old{};
    const bool tty = ::tcgetattr(STDIN_FILENO, &old) == 0;
    if (tty) {
        termios silent = old;
        silent.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &silent);
    }
    std::string line;
    std::getline(std::cin, line);
    if (tty) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        std::cerr << "\n";
    }
    return strip_newline(line);
}

}  // namespace

std::string read_password(const Context& ctx, std::string_view prompt, bool confirm) {
    std::string pw;
    if (!ctx.password_file.empty()) {
        struct stat st {};
        if (::stat(ctx.password_file.c_str(), &st) != 0) {
            throw CliError(kExitUsage, "password file " + ctx.password_file.string() + " does not exist");
        }
        if ((st.st_mode & 077) != 0) {
            throw CliError(kExitUsage, "password file " + ctx.password_file.string() +
                                           " is accessible by group/others (chmod 600)");
        }
        std::ifstream in(ctx.password_file);
        std::getline(in, pw);
        pw = strip_newline(pw);
    } else if (::isatty(STDIN_FILENO) != 0) {
        pw = read_tty_line(prompt);
        if (confirm && read_tty_line("Repeat password: ") != pw) throw CliError(kExitAuth, "passwords do not match");
    } else {
        std::getline(std::cin, pw);
        pw = strip_newline(pw);
    }
    if (pw.empty()) throw CliError(kExitAuth, "empty password");
    return pw;
}

void out(std::string_view key, std::string_view value) { std::cout << key << ": " << value << "\n"; }
void out(std::string_view key, std::uint64_t value) { std::cout << key << ": " << value << "\n"; }

Bytes read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw CliError(kExitUsage, "cannot read " + path.string());
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file_bytes(const std::filesystem::path& path, ByteView data, bool overwrite) {
    const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (overwrite ? O_TRUNC : O_EXCL);
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) {
        if (errno == EEXIST) throw CliError(kExitRejected, path.string() + " already exists (use --force to overwrite)");
        throw CliError(kExitStorage, "cannot create " + path.string() + ": " + std::strerror(errno));
    }
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            throw CliError(kExitStorage, "cannot write " + path.string() + ": " + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) throw CliError(kExitStorage, "cannot write " + path.string());
}

Bytes read_hex_file(const std::filesystem::path& path) {
    const Bytes raw = read_file_bytes(path);
    std::string hex;
    for (const Byte b : raw) {
        const char c = static_cast<char>(b);
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t') hex.push_back(c);
    }
    auto bytes = from_hex(hex);
    if (!bytes) throw CliError(kExitRejected, path.string() + " does not contain lowercase hex");
    return *bytes;
}

}  // namespace yap::cli
