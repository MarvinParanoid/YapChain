#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Negative cases of tools/ci/check_cxx_standard.py: every way a first-party TU can miss C++23 is
reported, third-party and C sources are ignored, and an empty selection is a failure."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools", "ci"))
import check_cxx_standard as c  # noqa: E402

SRC = "/repo"
BUILD = "/repo/build/release"


def tu(path, *flags, use_arguments=False):
    if use_arguments:
        return {"directory": BUILD, "file": path, "arguments": ["c++", *flags, "-c", path]}
    return {"directory": BUILD, "file": path, "command": " ".join(["c++", *flags, "-c", path])}


def expect(name, entries, n_checked, n_bad):
    checked, bad = c.check(entries, BUILD, SRC)
    ok = checked == n_checked and len(bad) == n_bad
    print(f"{'ok  ' if ok else 'FAIL'} {name}: checked={checked} bad={bad}")
    return ok


def main():
    good = [tu("/repo/src/a.cpp", "-O3", "-std=c++23"),
            tu("/repo/tests/unit/b_test.cpp", "-std=c++2b", use_arguments=True),
            tu("/repo/build/release/generated/abci-proto/x.pb.cc", "-std=c++23"),
            tu("/repo/build/release/_deps/catch2-src/src/c.cpp", "-std=c++20"),  # third party
            tu("/repo/build/release/_deps/lmdb-src/mdb.c", "-std=c11"),           # C
            tu("/repo/src/d.c")]                                                   # C, no -std
    results = [
        expect("baseline", good, 3, 0),
        expect("C++20 first-party TU", good + [tu("/repo/src/e.cpp", "-std=c++20")], 4, 1),
        expect("GNU dialect", good + [tu("/repo/src/e.cpp", "-std=gnu++23")], 4, 1),
        expect("no -std flag", good + [tu("/repo/tests/f.cpp", "-O2")], 4, 1),
        expect("two -std flags (later one wins)", good + [tu("/repo/src/g.cpp", "-std=c++23", "-std=c++20")], 4, 1),
        expect("generated code at C++20", good + [tu("/repo/build/release/generated/y.pb.cc", "-std=c++20")], 4, 1),
        expect("no first-party TUs", good[3:], 0, 1),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
