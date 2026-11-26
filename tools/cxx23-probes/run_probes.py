#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C++23 compatibility probes (ADR-0030).

Compiles, links and runs every probes/*.cpp with each given toolchain in C++23 mode and prints a
Markdown table, then the language and library feature-test macros each toolchain reports. A probe
passes only if it compiles without warnings (-Wall -Wextra -Werror: an ignored attribute is not
support), links and exits 0: the verdict comes from the toolchain itself, not from
its version number. Language support (the compiler) and library support (the standard library it is
paired with) are separate columns of the same row: e.g. Clang 18 + libstdc++ 14.

Usage:
  run_probes.py --tc "GCC 14 / libstdc++ 14=g++-14" \
                --tc "Clang 18 / libstdc++ 13=clang++-18 --gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/13"
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "probes")

# Feature-test macros reported per toolchain (from <version> plus the predefined language macros).
MACROS = [
    "__cplusplus",
    "__cpp_if_consteval", "__cpp_explicit_this_parameter", "__cpp_static_call_operator",
    "__cpp_multidimensional_subscript", "__cpp_size_t_suffix", "__cpp_auto_cast",
    "__cpp_concepts", "__cpp_constexpr",
    "__cpp_lib_expected", "__cpp_lib_byteswap", "__cpp_lib_to_underlying", "__cpp_lib_unreachable",
    "__cpp_lib_string_contains", "__cpp_lib_move_only_function", "__cpp_lib_ranges_to_container",
    "__cpp_lib_print", "__cpp_lib_constexpr_memory", "__cpp_lib_constexpr_bitset",
    "__cpp_lib_constexpr_cmath", "__cpp_lib_ranges_contains", "__cpp_lib_ranges_starts_ends_with",
    "__cpp_lib_ranges_fold", "__cpp_lib_ranges_zip", "__cpp_lib_ranges_enumerate",
    "__cpp_lib_ranges_chunk", "__cpp_lib_ranges_slide", "__cpp_lib_ranges_stride",
    "__cpp_lib_optional", "__cpp_lib_string_resize_and_overwrite", "__cpp_lib_forward_like",
    "__cpp_lib_out_ptr", "__cpp_lib_flat_map", "__cpp_lib_mdspan", "__cpp_lib_generator",
    "__cpp_lib_stacktrace", "__cpp_lib_spanstream",
]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=300, **kw)


def probe(cxx, src, work):
    exe = os.path.join(work, os.path.basename(src) + ".out")
    c = run(cxx + ["-std=c++23", "-O1", "-Wall", "-Wextra", "-Werror", src, "-o", exe])
    if c.returncode != 0:
        first = next((l for l in c.stderr.splitlines() if "error" in l), c.stderr.strip()[:200])
        return "compile-fail", first.strip()
    r = run([exe])
    return ("ok" if r.returncode == 0 else "run-fail"), ""


def macros(cxx):
    src = "#include <version>\n"
    r = run(cxx + ["-std=c++23", "-x", "c++", "-E", "-dM", "-"], input=src)
    defs = {}
    for line in r.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[0] == "#define":
            defs[parts[1]] = parts[2]
    return {m: defs.get(m, "—") for m in MACROS}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tc", action="append", required=True, help="'label=compiler [args]'")
    ap.add_argument("--json", help="also write the raw results here")
    a = ap.parse_args()
    tcs = []
    for spec in a.tc:
        label, _, cmd = spec.partition("=")
        tcs.append((label.strip(), shlex.split(cmd)))
    srcs = sorted(os.path.join(PROBES, f) for f in os.listdir(PROBES) if f.endswith(".cpp"))
    results = {}
    with tempfile.TemporaryDirectory() as work:
        for label, cxx in tcs:
            v = run(cxx + ["--version"]).stdout.splitlines()
            results[label] = {"version": v[0] if v else "?", "macros": macros(cxx), "probes": {}}
            for s in srcs:
                results[label]["probes"][os.path.basename(s)[:-4]] = probe(cxx, s, work)
    labels = [l for l, _ in tcs]
    print("| probe | " + " | ".join(labels) + " |")
    print("|---|" + "---|" * len(labels))
    for s in srcs:
        name = os.path.basename(s)[:-4]
        cells = []
        for l in labels:
            st, _ = results[l]["probes"][name]
            cells.append({"ok": "yes", "compile-fail": "**no**", "run-fail": "**run fails**"}[st])
        print(f"| `{name}` | " + " | ".join(cells) + " |")
    print()
    print("| macro | " + " | ".join(labels) + " |")
    print("|---|" + "---|" * len(labels))
    for m in MACROS:
        print(f"| `{m}` | " + " | ".join(results[l]["macros"][m] for l in labels) + " |")
    print()
    for l in labels:
        print(f"- {l}: `{results[l]['version']}`")
        for name, (st, why) in results[l]["probes"].items():
            if st != "ok":
                print(f"  - {name}: {st}: {why}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
