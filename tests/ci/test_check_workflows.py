#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Negative tests of tools/ci/check_workflows.py: the unchanged repository passes, and every mutation
that would weaken the mandatory CI or drop the extended/diagnostic configurations is reported.

    test_check_workflows.py <repo root>

Each case copies .github/workflows and CMakePresets.json into a temporary root, applies one textual
change and expects exit code 1 with the given finding. Exit 77 (skip) without PyYAML.
"""
import os
import shutil
import subprocess
import sys
import tempfile

try:
    import yaml  # noqa: F401
except ImportError:
    print("PyYAML is not installed: workflow checker tests skipped", file=sys.stderr)
    sys.exit(77)

FAILS = []
EXCLUDE = "        exclude:\n          - { arch: aarch64, compiler: gcc, preset: asan }\n"
ARM_INCLUDE = "          - { arch: aarch64, runner: ubuntu-24.04-arm }\n"


def check(cond, msg):
    if not cond:
        FAILS.append(msg)
        print(f"FAIL: {msg}", file=sys.stderr)


def drop_job(text, name):
    """Remove a top-level job (two-space indented key) and its body from a workflow file."""
    out, skipping = [], False
    for line in text.splitlines(keepends=True):
        if line.startswith(f"  {name}:"):
            skipping = True
            continue
        if skipping and line.startswith("  ") and not line.startswith("   ") and line.strip():
            skipping = False
        if skipping and (line.startswith("   ") or not line.strip() or line.lstrip().startswith("#")):
            continue
        skipping = False
        out.append(line)
    return "".join(out)


def replace_once(old, new):
    def f(text):
        if old not in text:
            raise AssertionError(f"mutation anchor not found: {old!r}")
        return text.replace(old, new, 1)
    return f


CASES = [
    # (description, file, mutation (None = delete the file), expected finding)
    ("aarch64/gcc/asan back in the mandatory matrix", "ci.yml", replace_once(EXCLUDE, ""),
     "build-test matrix: 12 jobs"),
    ("a reduced scale in the mandatory matrix", "ci.yml",
     replace_once(ARM_INCLUDE, ARM_INCLUDE + "          - { arch: aarch64, compiler: clang, preset: asan, long_test_scale: 25 }\n"),
     "full scale"),
    ("a reduced scale on the configure line", "ci.yml",
     replace_once("run: cmake --preset ${{ matrix.preset }}\n",
                  "run: cmake --preset ${{ matrix.preset }} -DYAP_LONG_TEST_SCALE_PERCENT=25\n"),
     "must not be reduced"),
    ("another x86_64 configuration dropped", "ci.yml",
     replace_once(EXCLUDE, EXCLUDE + "          - { arch: x86_64, compiler: gcc, preset: asan }\n"),
     "build-test matrix: 10 jobs"),
    ("continue-on-error", "ci.yml",
     replace_once("    timeout-minutes: 75\n", "    timeout-minutes: 75\n    continue-on-error: true\n"),
     "continue-on-error"),
    ("a conditional mandatory job", "ci.yml",
     replace_once("    runs-on: ubuntu-24.04\n    timeout-minutes: 10\n",
                  "    runs-on: ubuntu-24.04\n    if: github.event_name == 'push'\n    timeout-minutes: 10\n"),
     "is conditional"),
    ("the live job removed", "ci.yml", lambda t: drop_job(t, "live"), "ci.yml jobs"),
    ("a fuzz target dropped", "ci.yml",
     replace_once("for t in ycs bech32m tx devblock abci rpc;", "for t in ycs bech32m tx devblock abci;"),
     "fuzz_rpc"),
    ("an unpinned action", "ci.yml",
     replace_once("actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1", "actions/checkout@v7"),
     "not pinned"),
    ("aarch64/gcc/asan removed from extended", "extended.yml", lambda t: drop_job(t, "arm64-gcc-asan"),
     "no 'arm64-gcc-asan' job"),
    ("extended aarch64 job on another runner", "extended.yml",
     replace_once("    runs-on: ubuntu-24.04-arm\n", "    runs-on: ubuntu-24.04\n"),
     "arm64-gcc-asan: runner"),
    ("extended aarch64 job with Clang", "extended.yml",
     lambda t: 'echo "CXX=clang++-18"'.join(t.rsplit('echo "CXX=g++-14"', 1)),  # the last job's
     "missing 'CXX=g++-14'"),
    ("a schedule on extended", "extended.yml",
     replace_once("on:\n  workflow_dispatch:\n", "on:\n  schedule:\n    - cron: \"0 3 * * 1\"\n  workflow_dispatch:\n"),
     "extended.yml must be manual only"),
    ("the diagnostic workflow removed", "asan-arm64-diagnose.yml", None, "asan-arm64-diagnose.yml missing"),
    ("a push trigger on the diagnostic workflow", "asan-arm64-diagnose.yml",
     replace_once("on:\n  workflow_dispatch:\n", "on:\n  push:\n  workflow_dispatch:\n"),
     "asan-arm64-diagnose.yml must be manual only"),
    ("the tsan preset removed", "CMakePresets.json",
     replace_once('"name": "tsan"', '"name": "tsan-renamed"'), "no 'tsan' configure preset"),
]


def make_root(src, tmp):
    shutil.copytree(os.path.join(src, ".github", "workflows"), os.path.join(tmp, ".github", "workflows"))
    shutil.copy(os.path.join(src, "CMakePresets.json"), tmp)


def path_of(root, name):
    return os.path.join(root, name) if name == "CMakePresets.json" else os.path.join(root, ".github", "workflows", name)


def run(checker, root):
    return subprocess.run([sys.executable, "-I", checker, root], capture_output=True, text=True, timeout=60)


def main(src):
    checker = os.path.join(src, "tools", "ci", "check_workflows.py")
    with tempfile.TemporaryDirectory() as tmp:
        make_root(src, tmp)
        r = run(checker, tmp)
        check(r.returncode == 0 and "15 mandatory jobs" in r.stdout, f"baseline: {r.returncode} {r.stderr}")
    for desc, name, mutate, expect in CASES:
        with tempfile.TemporaryDirectory() as tmp:
            make_root(src, tmp)
            p = path_of(tmp, name)
            try:
                if mutate is None:
                    os.remove(p)
                else:
                    with open(p) as fh:
                        text = fh.read()
                    changed = mutate(text)
                    check(changed != text, f"{desc}: the mutation changed nothing")
                    with open(p, "w") as fh:
                        fh.write(changed)
            except AssertionError as e:
                check(False, f"{desc}: {e}")
                continue
            r = run(checker, tmp)
            check(r.returncode == 1 and expect in r.stderr,
                  f"{desc}: exit {r.returncode}, expected finding '{expect}', got: {r.stderr.strip()}")
    if FAILS:
        print(f"{len(FAILS)} failure(s)", file=sys.stderr)
        return 1
    print(f"workflow checker: {len(CASES)} mutations reported")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")))
