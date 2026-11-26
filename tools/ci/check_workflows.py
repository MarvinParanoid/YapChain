#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Local checks of .github/workflows (no network, PyYAML only).

    check_workflows.py [repo root]

- every file parses as YAML and has `on`, `permissions` and `jobs`;
- workflow permissions are read-only (no `write` anywhere);
- every job has `timeout-minutes`; no `continue-on-error` anywhere;
- every `uses:` is pinned to a full 40-hex commit SHA;
- ci.yml (the mandatory CI, docs/ci.md): exactly the jobs in MANDATORY_JOBS; the build/test matrix
  expands (GitHub `exclude`/`include` semantics) to exactly 11 jobs — {x86_64, aarch64} x {gcc, clang}
  x {debug, release, asan} without aarch64/gcc/asan — each with the right runner and the long tests at
  full scale; 15 mandatory jobs in all; the fuzz job runs every fuzz target; the live jobs listed in
  REQUIRED_LIVE_JOBS check that their tests are registered; every compiling job (CXX_STD_CHECKED) checks
  the effective C++23 standard of its own build directory (tools/ci/check_cxx_standard.py, ADR-0030), and
  the build/test jobs run the whole CTest suite without filters;
- extended.yml is manual only and runs aarch64/gcc/asan (GCC 14, asan preset, arm64 runner);
- asan-arm64-diagnose.yml is kept and manual only;
- CMakePresets.json keeps the sanitizer presets (asan, tsan, fuzz).
Prints the expanded matrix and exits non-zero on any finding.
"""
import itertools
import json
import os
import re
import sys

try:
    import yaml
except ImportError:  # reported as "skipped" by CTest (SKIP_RETURN_CODE 77), never as a pass
    print("PyYAML is not installed: workflow checks skipped", file=sys.stderr)
    sys.exit(77)

FINDINGS = []
REQUIRED_LIVE_JOBS = ("live", "tsan")  # jobs that must check the registration of the live tests
# Jobs that compile first-party code, and the build directory whose compile commands each must check.
CXX_STD_CHECKED = {"build-test": "build/${{ matrix.preset }}", "fuzz": "build/fuzz", "live": "build/debug",
                   "tsan": "build/tsan"}
MANDATORY_JOBS = ("build-test", "vectors-reproducible", "fuzz", "live", "tsan")  # ci.yml, all of them
EXTENDED_ONLY = ("aarch64", "gcc", "asan")  # Tier 2 configuration moved to extended.yml (M2.6.1a)
SANITIZER_PRESETS = ("asan", "tsan", "fuzz")


def finding(msg):
    FINDINGS.append(msg)
    print(f"FINDING: {msg}", file=sys.stderr)


def expand(matrix):
    """GitHub Actions matrix expansion: the product of the axes minus every combination that matches
    an `exclude` entry on all of its keys; then each `include` entry is added to every combination
    whose ORIGINAL axis values it does not contradict (without overwriting them); an entry that
    matches no combination becomes a new one."""
    axes = {k: v for k, v in matrix.items() if k not in ("include", "exclude")}
    combos = [dict(zip(axes, vals)) for vals in itertools.product(*axes.values())]
    for exc in matrix.get("exclude", []):
        combos = [c for c in combos if not all(c.get(k) == v for k, v in exc.items())]
    originals = [dict(c) for c in combos]
    for inc in matrix.get("include", []):
        matched = False
        for c, orig in zip(combos, originals):
            if all(orig.get(k, v) == v for k, v in inc.items() if k in orig):
                matched = True
                for k, v in inc.items():
                    if k not in orig:
                        c[k] = v
        if not matched:
            combos.append(dict(inc))
            originals.append(dict(inc))
    return combos


def walk(node, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            yield f"{path}.{k}", k, v
            yield from walk(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk(v, f"{path}[{i}]")


def main(root):
    wdir = os.path.join(root, ".github", "workflows")
    files = sorted(f for f in os.listdir(wdir) if f.endswith((".yml", ".yaml")))
    docs = {}
    for f in files:
        with open(os.path.join(wdir, f)) as fh:
            doc = yaml.load(fh, Loader=yaml.BaseLoader)  # strings only: `on` stays a key
        docs[f] = doc
        for key in ("on", "permissions", "jobs"):
            if key not in doc:
                finding(f"{f}: missing top-level '{key}'")
        perms = doc.get("permissions", {})
        if perms != {"contents": "read"}:
            finding(f"{f}: workflow permissions {perms} (expected contents: read)")
        for path, k, v in walk(doc):
            if k == "continue-on-error":
                finding(f"{f}{path}: continue-on-error")
            if k == "permissions" and path != ".permissions" and "write" in str(v):
                finding(f"{f}{path}: write permission")
            if k == "uses" and not re.fullmatch(r"[\w.-]+/[\w.-]+@[0-9a-f]{40}", str(v)):
                finding(f"{f}{path}: '{v}' is not pinned to a full commit SHA")
        for name, job in doc.get("jobs", {}).items():
            if "timeout-minutes" not in job:
                finding(f"{f}: job '{name}' has no timeout-minutes")
        print(f"{f}: {len(doc.get('jobs', {}))} job(s), parsed")

    ci = docs.get("ci.yml")
    if ci is None:
        finding("ci.yml missing")
        return report()
    if sorted(ci["jobs"]) != sorted(MANDATORY_JOBS):
        finding(f"ci.yml jobs {sorted(ci['jobs'])}, expected {sorted(MANDATORY_JOBS)}")
    for name, job in ci["jobs"].items():
        if "if" in job:
            finding(f"ci.yml: job '{name}' is conditional (a skipped job is not a pass)")
    combos = expand(ci["jobs"]["build-test"]["strategy"]["matrix"])
    print(f"ci.yml build-test: {len(combos)} jobs")
    for c in combos:
        print("   ", {k: c[k] for k in sorted(c)})
    want = {(a, c, p) for a in ("x86_64", "aarch64") for c in ("gcc", "clang") for p in ("debug", "release", "asan")}
    want.discard(EXTENDED_ONLY)
    got = {(c.get("arch"), c.get("compiler"), c.get("preset")) for c in combos}
    if len(combos) != 11 or got != want:
        finding(f"build-test matrix: {len(combos)} jobs, missing {want - got}, unexpected {got - want}")
    runners = {"x86_64": "ubuntu-24.04", "aarch64": "ubuntu-24.04-arm"}
    for c in combos:
        if c.get("runner") != runners.get(c.get("arch")):
            finding(f"build-test {c}: runner {c.get('runner')}")
        if "long_test_scale" in c:
            finding(f"build-test {c}: the mandatory jobs run the long tests at full scale")
    bt = json.dumps(ci["jobs"]["build-test"])
    if "LONG_TEST_SCALE" in bt:
        finding("ci.yml build-test: the long-test scale must not be reduced in the mandatory CI")
    total = len(combos) + len(ci["jobs"]) - 1
    print(f"ci.yml: {total} mandatory jobs")
    if total != 15:
        finding(f"ci.yml: {total} mandatory jobs, expected 15")
    fuzz = " ".join(s.get("run", "") for s in ci["jobs"]["fuzz"]["steps"])
    for t in ("ycs", "bech32m", "tx", "devblock", "abci", "rpc"):
        if not re.search(rf"\b{t}\b", fuzz):
            finding(f"fuzz job does not run fuzz_{t}")
    for job, build_dir in CXX_STD_CHECKED.items():
        steps = [s.get("run", "") for s in ci["jobs"].get(job, {}).get("steps", [])]
        if not any(f"python3 -I tools/ci/check_cxx_standard.py {build_dir} ." in r for r in steps):
            finding(f"ci.yml {job}: the effective C++23 standard of {build_dir} is not checked")
    bt_steps = [s.get("run", "").strip() for s in ci["jobs"]["build-test"]["steps"]]
    if "ctest --preset ${{ matrix.preset }}" not in bt_steps or any(
            r.startswith("ctest") and r != "ctest --preset ${{ matrix.preset }}" for r in bt_steps):
        finding("ci.yml build-test: the test step must be exactly `ctest --preset ${{ matrix.preset }}` (whole suite)")
    for job in REQUIRED_LIVE_JOBS:
        if job not in ci["jobs"]:
            finding(f"ci.yml: no '{job}' job")
            continue
        steps = " ".join(s.get("run", "") for s in ci["jobs"][job]["steps"])
        if "test \"$n\" -eq 3" not in steps:
            finding(f"ci.yml {job}: the registration of the live tests is not checked")
    ext = docs.get("extended.yml")
    if ext is None:
        finding("extended.yml missing")
    else:
        on = ext.get("on", {})
        if set(on) != {"workflow_dispatch"}:
            finding(f"extended.yml must be manual only, triggers: {sorted(on)}")
        steps = " ".join(s.get("run", "") for s in ext["jobs"].get("extended", {}).get("steps", []))
        if 'test "$n" -eq' not in steps:
            finding("extended.yml: the registration of the selected tests is not checked")
        arm = ext["jobs"].get("arm64-gcc-asan")
        if arm is None:
            finding("extended.yml: no 'arm64-gcc-asan' job (aarch64/gcc/asan must run somewhere)")
        else:
            steps = " ".join(s.get("run", "") for s in arm.get("steps", []))
            if arm.get("runs-on") != "ubuntu-24.04-arm":
                finding(f"extended.yml arm64-gcc-asan: runner {arm.get('runs-on')}")
            for need in ("CXX=g++-14", "cmake --preset asan", "cmake --build --preset asan", "ctest --preset asan"):
                if need not in steps:
                    finding(f"extended.yml arm64-gcc-asan: missing '{need}'")
    diag = docs.get("asan-arm64-diagnose.yml")
    if diag is None:
        finding("asan-arm64-diagnose.yml missing")
    elif set(diag.get("on", {})) != {"workflow_dispatch"}:
        finding(f"asan-arm64-diagnose.yml must be manual only, triggers: {sorted(diag.get('on', {}))}")
    try:
        with open(os.path.join(root, "CMakePresets.json")) as fh:
            presets = {p["name"] for p in json.load(fh).get("configurePresets", [])}
    except (OSError, ValueError, KeyError) as e:
        finding(f"CMakePresets.json: {e}")
        presets = set()
    for p in SANITIZER_PRESETS:
        if p not in presets:
            finding(f"CMakePresets.json: no '{p}' configure preset")
    return report()


def report():
    if FINDINGS:
        print(f"{len(FINDINGS)} finding(s)", file=sys.stderr)
        return 1
    print("workflows: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")))
