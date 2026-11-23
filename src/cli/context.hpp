// SPDX-License-Identifier: Apache-2.0
//
// Shared infrastructure of the `yap` command-line tool.
#pragma once

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#include "yap/common/bytes.hpp"

namespace yap::cli {

/// Process exit codes (stable, documented in `yap --help`).
enum ExitCode : int {
    kExitOk = 0,
    kExitUsage = 1,      ///< bad command line
    kExitRejected = 2,   ///< invalid input, rejected transaction or block, validation failure
    kExitStorage = 3,    ///< storage failure or corruption
    kExitAuth = 4,       ///< wrong password or unusable keystore
    kExitInternal = 5,
};

/// An error reported to the user with a specific exit code.
class CliError : public std::runtime_error {
public:
    CliError(int code, const std::string& msg) : std::runtime_error(msg), code_(code) {}
    [[nodiscard]] int code() const noexcept { return code_; }

private:
    int code_;
};

struct Context {
    std::filesystem::path home;           ///< --home, $YAP_HOME or ~/.yapchain
    std::filesystem::path password_file;  ///< --password-file (optional)
    bool force = false;                   ///< --force: allow overwriting output files

    [[nodiscard]] std::filesystem::path keys_dir() const { return home / "keys"; }
    [[nodiscard]] std::filesystem::path dev_dir() const { return home / "dev"; }
    [[nodiscard]] std::filesystem::path data_dir() const { return home / "data"; }
    [[nodiscard]] std::filesystem::path producer_key_path() const { return dev_dir() / "producer.json"; }
    [[nodiscard]] std::filesystem::path key_path(std::string_view name) const;
};

[[nodiscard]] std::filesystem::path default_home();

/// Reads a password: from --password-file, else from the terminal with echo disabled, else one
/// line from stdin. Passwords are never taken from the command line itself.
[[nodiscard]] std::string read_password(const Context& ctx, std::string_view prompt, bool confirm);

/// Overwrites a password held in a std::string before it is released.
void wipe(std::string& secret) noexcept;

/// One "key: value" line on stdout (stable, machine-friendly output).
void out(std::string_view key, std::string_view value);
void out(std::string_view key, std::uint64_t value);

[[nodiscard]] Bytes read_file_bytes(const std::filesystem::path& path);
/// Creates an output file. Refuses to replace an existing file (or follow a symlink) unless
/// `overwrite` is set; I/O failures are storage errors (exit 3).
void write_file_bytes(const std::filesystem::path& path, ByteView data, bool overwrite = false);
/// Reads a file that holds hex (whitespace ignored); throws CliError on bad hex.
[[nodiscard]] Bytes read_hex_file(const std::filesystem::path& path);

}  // namespace yap::cli
