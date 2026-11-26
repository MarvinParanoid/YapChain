#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Checks that every first-party C++ translation unit of a build is compiled as C++23 (ADR-0030).

Reads <build-dir>/compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS is always on). First-party
code is every C++ source under <source-dir>/src, <source-dir>/tests and <build-dir>/generated (the
ABCI bindings generated from the vendored schemas). Each of them must carry exactly one -std= flag,
and it must be -std=c++23 (or its pre-standard spelling -std=c++2b): a target that sets its own
CXX_STANDARD, a stray -std=c++20 in compile options, or a GNU dialect is a failure, and so is a
build with no first-party C++ sources at all. Third-party sources (_deps, C files) are not checked;
the separately built protobuf/abseil stay at C++20 by design (cmake/Protobuf.cmake).

Usage: check_cxx_standard.py <build-dir> <source-dir>
Exit codes: 0 ok, 1 violation, 77 no compile_commands.json (skipped).
"""
import json
import os
import shlex
import sys

ACCEPTED = {"-std=c++23", "-std=c++2b"}
CXX_EXT = (".cpp", ".cc", ".cxx")


def first_party_roots(build_dir, source_dir):
    return [os.path.join(os.path.realpath(source_dir), "src"),
            os.path.join(os.path.realpath(source_dir), "tests"),
            os.path.join(os.path.realpath(build_dir), "generated")]


def args_of(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def check(entries, build_dir, source_dir):
    """Returns (number of first-party TUs checked, list of violations)."""
    roots = first_party_roots(build_dir, source_dir)
    checked, bad = 0, []
    for e in entries:
        path = os.path.realpath(os.path.join(e.get("directory", ""), e["file"]))
        if not path.endswith(CXX_EXT) or not any(path.startswith(r + os.sep) for r in roots):
            continue
        checked += 1
        stds = [a for a in args_of(e) if a.startswith("-std=")]
        if len(stds) != 1 or stds[0] not in ACCEPTED:
            bad.append(f"{os.path.relpath(path, os.path.realpath(source_dir))}: {stds or 'no -std flag'}")
    if checked == 0:
        bad.append("no first-party C++ translation units in compile_commands.json")
    return checked, bad


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 1
    build_dir, source_dir = argv[1], argv[2]
    db = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(db):
        print(f"SKIP: {db} not found")
        return 77
    with open(db) as f:
        entries = json.load(f)
    checked, bad = check(entries, build_dir, source_dir)
    for b in bad:
        print(f"NOT C++23: {b}")
    if bad:
        return 1
    print(f"cxx-standard: {checked} first-party C++ translation units, all -std=c++23")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
