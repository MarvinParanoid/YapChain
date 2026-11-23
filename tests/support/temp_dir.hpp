// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdlib.h>  // mkdtemp (POSIX)

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace yap::test {

/// Unique temporary directory removed on destruction.
class TempDir {
public:
    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "yapchain-test-XXXXXX").string();
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        if (mkdtemp(buf.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
        path_ = buf.data();
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace yap::test
