#!/usr/bin/env python3
"""QA suite runner — discover, tag, provision-once, run, and report.

The orchestrator that ties the QA infrastructure together. It adds ONLY
orchestration; assertions, navigation, provisioning, and probe-recovery all live
in their existing homes (harness / provision / envprobe / the tests themselves),
which this calls rather than re-implements.

How tests are tagged
--------------------
Each test advertises a module-level ``TEST_META`` dict:

    TEST_META = {
        "tier": "L2",            # L0/L1/L2/L3/L4/tour/manual
        "subsystem": "boot",
        "envs": ["ENV-RG", "ENV-DOCS"],   # or ["ANY"]
        "build": "REMOTE_INPUT=1",        # firmware flags the test needs
        "observable": "swd",     # swd|fs|ocr|host|manual (how it asserts — for the report)
        "automated": True,
        "goal": [1],             # DESIGN.md goal numbers, for traceability
        "args": [],              # optional CLI args to pass through
    }

Pre-existing tests are tagged centrally in ``LEGACY_META`` (so the standing
green tests stay untouched); every NEW test carries its own ``TEST_META``.

Modes
-----
  --tier L0,L1     host/build gates only, no device (CI default)
  --adaptive       probe the bench device, run every automated test valid for the
                   flashed build, prove the invariant, report coverage
  --matrix ENV,... provision each named environment, run its tagged tests, report

Execution model: tests run as isolated SUBPROCESSES (each owns its single
OpenOCD session, so probe access stays serialized — one at a time). Each is
wall-clock bounded; a wedge is recovered (recover_probe) and reported, never
fatal to the run.

  python3 scripts/tests/run_suite.py --tier L0,L1
  python3 scripts/tests/run_suite.py --adaptive
  python3 scripts/tests/run_suite.py --matrix ENV-BARE,ENV-DOCS
"""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))   # scripts/ on path
from common import harness as h            # noqa: E402
from common.harness import resolve_symbol  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
TESTS_DIR = REPO / "scripts" / "tests"

# Build flag -> a symbol that only exists when that flag was compiled in. The
# runner skips (not fails) a device test whose required build isn't flashed.
BUILD_SENTINEL = {
    "ABI_SELFTEST": "g_abi_selftest_mod",
    "BOOT_BENCH":   "g_boot_bench",
    "CRASH_TEST":   "crash_test_check",
    # REMOTE_INPUT (default-on) and RTL_TEST have no clean sentinel; assumed present.
}

TIERS_NO_DEVICE = {"L0", "L1"}
EXCLUDE_FILES = {"run_suite.py", "press.py", "__init__.py"}

# Back-fill for the pre-existing tests (canonical place is each test's own
# TEST_META; these predate the convention and are tagged here to avoid churning
# standing green tests). Keyed by file stem.
LEGACY_META = {
    "chainloader_running_test": dict(tier="L2", subsystem="boot", envs=["ANY"],
        build=None, observable="swd", automated=True, goal=[1, 8]),
    "boot_selector_test": dict(tier="L3", subsystem="boot", envs=["ENV-DOCS", "ENV-OFW-RESIDENT"],
        build="REMOTE_INPUT=1", observable="swd", automated=True, goal=[1, 2, 15],
        args=["--target", "all"]),
    "retrogo_return_test": dict(tier="L3", subsystem="boot", envs=["ENV-RG", "ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="swd", automated=True, goal=[15]),
    "feature_menu_test": dict(tier="L2", subsystem="modules", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="swd", automated=True, goal=[16]),
    "installer_test": dict(tier="L3", subsystem="i18n", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="fs", automated=True, goal=[10, 13, 14],
        args=["--kind", "all"]),
    "fileops_test": dict(tier="L2", subsystem="fs", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="ocr", automated=True, goal=[7]),
    "ocr_nav_test": dict(tier="L2", subsystem="i18n", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="ocr", automated=True, goal=[10]),
    "theme_lang_test": dict(tier="L2", subsystem="theme", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="swd", automated=True, goal=[4, 10]),
    "test_abi_reject": dict(tier="L2", subsystem="modules", envs=["ENV-STALE-ABI"],
        build="ABI_SELFTEST=1", observable="swd", automated=True, goal=[14]),
    "test_remote_input": dict(tier="L2", subsystem="input", envs=["ANY"],
        build="REMOTE_INPUT=1", observable="swd", automated=True, goal=[1]),
    "sd_fonttest": dict(tier="L2", subsystem="sd", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="ocr", automated=True, goal=[13]),
    "i18n_switch": dict(tier="tour", subsystem="i18n", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="ocr", automated=False, goal=[10]),
    "i18n_screens": dict(tier="tour", subsystem="i18n", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="ocr", automated=False, goal=[10]),
    "i18n_demo": dict(tier="tour", subsystem="i18n", envs=["ENV-DOCS"],
        build="REMOTE_INPUT=1", observable="ocr", automated=False, goal=[10]),
    "verify_entry_translated_in_all_languages": dict(tier="L2", subsystem="i18n",
        envs=["ENV-DOCS"], build="REMOTE_INPUT=1", observable="ocr", automated=False,
        goal=[10]),
}

DEFAULT_META = dict(tier="L2", subsystem="?", envs=["ANY"], build="REMOTE_INPUT=1",
                    observable="?", automated=True, goal=[], args=[])


@dataclass
class TestCase:
    id: str
    path: Path
    tier: str
    subsystem: str
    envs: list
    build: str | None
    observable: str
    automated: bool
    goal: list
    args: list = field(default_factory=list)
    tagged: bool = True


@dataclass
class TestResult:
    tc: TestCase
    status: str           # PASS / FAIL / SKIP / TIMEOUT / MANUAL / TOUR / ERROR
    seconds: float = 0.0
    reason: str = ""
    tail: str = ""


# --------------------------------------------------------------------------
def _candidates():
    """Every plausible test file under scripts/tests/ (top level + subsystem dirs),
    excluding helpers, env fixtures, and the OCR corpus."""
    out = []
    for p in sorted(TESTS_DIR.rglob("*.py")):
        rel = p.relative_to(TESTS_DIR)
        if p.name in EXCLUDE_FILES:
            continue
        if rel.parts[0] in ("env", "__pycache__") or "corpus" in rel.parts:
            continue
        out.append(p)
    return out


def _load_meta(path: Path):
    """Import a test module just to read TEST_META (top-level only; main() is
    guarded). Returns (meta_dict|None, error|None)."""
    name = "qa_probe_" + path.stem
    try:
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return getattr(mod, "TEST_META", None), None
    except Exception as e:                                   # noqa: BLE001
        return None, f"{type(e).__name__}: {e}"


def discover():
    """Return (tests:list[TestCase], errors:list[(path,err)])."""
    tests, errors = [], []
    for p in _candidates():
        is_named_test = p.stem.endswith("_test") or p.stem.startswith("test_")
        meta, err = _load_meta(p)
        if err and (is_named_test or meta is None):
            # Only surface an import error for things that look like tests.
            if is_named_test:
                errors.append((p, err))
            continue
        if meta is None:
            meta = LEGACY_META.get(p.stem)
        if meta is None:
            if not is_named_test:
                continue                # a helper module, not a test
            meta = dict(DEFAULT_META)
            tagged = False
        else:
            tagged = True
        m = {**DEFAULT_META, **meta}
        tests.append(TestCase(
            id=p.stem, path=p, tier=m["tier"], subsystem=m["subsystem"],
            envs=list(m["envs"]), build=m["build"], observable=m["observable"],
            automated=bool(m["automated"]), goal=list(m["goal"]),
            args=list(m.get("args", [])), tagged=tagged))
    return tests, errors


def build_satisfied(build):
    """(ok, reason): is the firmware flashed with the flags this test needs?
    Checked by the presence of a sentinel symbol in build/app/app.elf."""
    if not build:
        return True, ""
    for flag in build.split():
        name = flag.split("=")[0]
        sentinel = BUILD_SENTINEL.get(name)
        if sentinel is None:
            continue                    # no detectable sentinel; assume present
        try:
            resolve_symbol(sentinel)
        except Exception:
            return False, f"needs the {name} build flashed (symbol {sentinel} absent)"
    return True, ""


# --------------------------------------------------------------------------
def run_one(tc: TestCase, timeout: float) -> TestResult:
    """Run a test as an isolated subprocess (it owns its probe session)."""
    print(f"\n>>> {tc.id}  [{tc.tier} {tc.subsystem} obs={tc.observable}]", flush=True)
    # Device tests contend for the single ST-Link; if another session is using the
    # programmer, wait for it to finish rather than colliding (host tiers don't care).
    if tc.tier not in TIERS_NO_DEVICE:
        h.wait_for_probe_free()
    t0 = time.time()
    cmd = [sys.executable, str(tc.path)] + tc.args
    try:
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        h.recover_probe()
        tail = (e.stdout or "")[-600:] if isinstance(e.stdout, str) else ""
        return TestResult(tc, "TIMEOUT", time.time() - t0,
                          reason=f"exceeded {timeout:.0f}s (probe recovered)", tail=tail)
    except Exception as e:                                  # noqa: BLE001
        return TestResult(tc, "ERROR", time.time() - t0, reason=str(e))
    dt = time.time() - t0
    # Stream the child's output through so a live run is still followable.
    sys.stdout.write(r.stdout)
    if r.stderr.strip():
        sys.stdout.write(r.stderr)
    status = "PASS" if r.returncode == 0 else "FAIL"
    tail = "\n".join(r.stdout.strip().splitlines()[-8:])
    return TestResult(tc, status, dt, tail=tail if status == "FAIL" else "")


def _select(tests, *, tier=None, env=None, adaptive=False):
    """Yield (tc, skip_reason|None) for the tests in scope for this mode."""
    for tc in tests:
        if tier is not None:
            if tc.tier not in tier:
                continue
            yield tc, None
            continue
        # device modes
        if not tc.automated:
            yield tc, "manual / tour (run by hand)"
            continue
        if tc.tier in TIERS_NO_DEVICE:
            continue                    # host gates run under --tier, not on device
        ok, why = build_satisfied(tc.build)
        if not ok:
            yield tc, why
            continue
        if env is not None and not ({env, "ANY"} & set(tc.envs)):
            continue                    # not tagged for this provisioned environment
        yield tc, None


def _update_html_report(results, env_snapshot, mode):
    """Merge this run's results into the persistent store (build/qa_results.json,
    latest result per test id) and re-render the single qa-report.html in the
    project root -- so the HTML is ONE file no matter how many suites/modes ran."""
    import datetime
    import json
    store_path = REPO / "build" / "qa_results.json"
    store = {"tests": {}, "env": "", "updated": "", "runs": []}
    if store_path.is_file():
        try:
            store = json.loads(store_path.read_text())
        except Exception:
            pass
    now = datetime.datetime.now().isoformat(timespec="seconds")
    for r in results:
        store["tests"][r.tc.id] = {
            "status": r.status, "tier": r.tc.tier, "subsystem": r.tc.subsystem,
            "observable": r.tc.observable, "goal": list(r.tc.goal),
            "seconds": round(r.seconds, 1) if r.seconds else None,
            "reason": r.reason, "mode": mode, "updated": now,
        }
    if env_snapshot:
        store["env"] = env_snapshot
    store["updated"] = now
    store.setdefault("runs", []).append({"mode": mode, "at": now, "n": len(results)})
    try:
        store_path.parent.mkdir(parents=True, exist_ok=True)
        store_path.write_text(json.dumps(store, indent=1))
        subprocess.run([sys.executable, str(REPO / "scripts" / "build" / "render_qa_report.py")],
                       capture_output=True, text=True, timeout=30)
    except Exception as e:                                  # noqa: BLE001
        print(f"  (HTML report not written: {e})")


def emit_report(results, env_snapshot, mode, errors, started):
    """The final phase: a tactful, informative report.

    A skip reads as "not applicable in this run", never a failure; failures are
    stated plainly with where to look; the run ends with a clear bottom line and
    concrete next steps.
    """
    bar = "=" * 72
    print(f"\n{bar}\nQA SUITE REPORT  ({mode})\n{bar}")
    if env_snapshot:
        print(env_snapshot + "\n")

    order = {"PASS": 0, "FAIL": 1, "TIMEOUT": 2, "ERROR": 3, "SKIP": 4, "MANUAL": 5, "TOUR": 6}
    counts = {}
    for r in results:
        counts[r.status] = counts.get(r.status, 0) + 1

    # Per-test results, grouped by status.
    print("Results")
    print("-" * 72)
    glyph = {"PASS": "PASS ", "FAIL": "FAIL ", "TIMEOUT": "TIME ", "ERROR": "ERR  ",
             "SKIP": "skip ", "MANUAL": "mant ", "TOUR": "tour "}
    for r in sorted(results, key=lambda r: (order.get(r.status, 9), r.tc.subsystem, r.tc.id)):
        line = f"  {glyph.get(r.status, r.status):5} {r.tc.id:42} {r.tc.tier:4} {r.tc.observable:4}"
        if r.seconds:
            line += f" {r.seconds:5.1f}s"
        if r.reason:
            line += f"  — {r.reason}"
        print(line)

    # Coverage traced to project goals.
    print("\nGoal coverage (DESIGN.md goals touched by this run)")
    print("-" * 72)
    by_goal = {}
    for r in results:
        for g in r.tc.goal:
            by_goal.setdefault(g, []).append(r)
    if by_goal:
        for g in sorted(by_goal):
            rs = by_goal[g]
            p = sum(1 for r in rs if r.status == "PASS")
            print(f"  goal {g:>2}: {p}/{len(rs)} passed  "
                  f"({', '.join(sorted({r.tc.id for r in rs}))})")
    else:
        print("  (no goal-tagged tests ran)")

    # Skips, grouped by reason and made actionable.
    skips = [r for r in results if r.status in ("SKIP", "MANUAL", "TOUR")]
    if skips:
        print("\nNot run in this pass (not failures)")
        print("-" * 72)
        by_reason = {}
        for r in skips:
            by_reason.setdefault(r.reason or "manual / tour", []).append(r.tc.id)
        for reason, ids in sorted(by_reason.items()):
            print(f"  {reason}:")
            for i in ids:
                print(f"      - {i}")

    if errors:
        print("\nDiscovery problems (could not import)")
        print("-" * 72)
        for p, e in errors:
            print(f"  {p.relative_to(REPO)}: {e}")

    # Failures, with where to look.
    fails = [r for r in results if r.status in ("FAIL", "TIMEOUT", "ERROR")]
    if fails:
        print("\nFailures (look here first)")
        print("-" * 72)
        for r in fails:
            print(f"  {r.tc.id} ({r.status}){(': ' + r.reason) if r.reason else ''}")
            if r.tail:
                for ln in r.tail.splitlines():
                    print(f"      | {ln}")

    # Bottom line — honest, non-alarmist.
    npass = counts.get("PASS", 0)
    nfail = counts.get("FAIL", 0) + counts.get("TIMEOUT", 0) + counts.get("ERROR", 0)
    nskip = counts.get("SKIP", 0) + counts.get("MANUAL", 0) + counts.get("TOUR", 0)
    dur = time.time() - started
    _update_html_report(results, env_snapshot, mode)

    print(f"\n{bar}")
    print(f"Bottom line: {npass} passed, {nfail} failed, {nskip} not applicable here "
          f"({len(results)} considered, {dur:.0f}s).")
    print(f"HTML report: qa-report.html")
    if nfail == 0 and npass:
        print("Everything that applied to this device/build passed. Skips above are "
              "tests that need a different environment or build, not problems.")
    elif nfail:
        print("The failures above are where to focus. Each lists its observable so "
              "you can confirm via SWD/filesystem before trusting the screen.")
    else:
        print("No automated tests applied to this run — check the mode/env and build.")
    print(bar)
    return 1 if nfail else 0


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=False)
    g.add_argument("--tier", help="comma list of tiers to run with no device (e.g. L0,L1)")
    g.add_argument("--adaptive", action="store_true",
                   help="probe the bench device, run every applicable automated test")
    g.add_argument("--auto", action="store_true",
                   help="gate: full bench run if a ready device is present (programmer "
                        "free + chainloader alive), else fall back to host tiers L0,L1")
    g.add_argument("--matrix", help="comma list of ENV-* to provision and sweep")
    ap.add_argument("--timeout", type=float, default=300.0, help="per-test wall-clock budget (s)")
    ap.add_argument("--list", action="store_true", help="list discovered tests and exit")
    ap.add_argument("--ids", help="comma list of test ids to restrict the run to "
                    "(e.g. a curated safe subset that avoids bank swaps)")
    args = ap.parse_args()
    if not (args.list or args.tier or args.adaptive or args.auto or args.matrix):
        ap.error("choose a mode: --tier, --adaptive, --auto, --matrix, or --list")

    started = time.time()
    tests, errors = discover()
    if args.ids:
        want = set(args.ids.split(","))
        tests = [tc for tc in tests if tc.id in want]

    if args.list:
        for tc in sorted(tests, key=lambda t: (t.tier, t.subsystem, t.id)):
            tag = "" if tc.tagged else "  (untagged: using default meta)"
            print(f"  {tc.tier:4} {tc.subsystem:10} {tc.id:42} "
                  f"envs={','.join(tc.envs)} build={tc.build}{tag}")
        print(f"\n{len(tests)} tests, {len(errors)} discovery errors")
        for p, e in errors:
            print(f"  ERROR {p.relative_to(REPO)}: {e}")
        return 0

    # Self-heal the OCR fonts (build/i18n/fonts, which `make clean` wipes) so any
    # run that reads the screen regenerates them via `make i18n` instead of silently
    # degrading to ASCII. No-op when present; non-fatal if cooking fails (SWD-only
    # tests don't need them).
    from common import ocr
    if not ocr.fonts_available():
        print("OCR fonts absent -> regenerating (make i18n)...")
        ocr.ensure_fonts()

    # --auto makes `make qa` USE THE DEVICE BY DEFAULT while still working without
    # one. First pgrep for an openocd/gnwmanager already holding the single ST-Link
    # (probe_in_use) so we never collide with another session; if the programmer is
    # free, run the full bench suite whenever a device answers at all -- we do NOT
    # gate on the chainloader being "alive", since the device tests report their own
    # state and the user can see it. Fall back to host tiers L0,L1 only when the
    # programmer is busy or no device is connected (envprobe.probe raises). For
    # deterministic behavior use QA_SCOPE=full / host-only. The probe is reused in
    # the adaptive branch (no double-connect).
    auto_env = None
    if args.auto:
        from common import envprobe
        busy = h.probe_in_use()
        if busy:
            print(f"=== auto: programmer in use ({busy}) -> host tiers L0,L1 ===")
            args.tier = "L0,L1"
        else:
            try:
                auto_env = envprobe.probe()              # free: is a device connected?
            except Exception as e:                       # no ST-Link / connect failure
                print(f"=== auto: no device reachable ({e}) -> host tiers L0,L1 ===")
                auto_env = None
            if auto_env is not None:
                args.adaptive = True                     # device present -> use it
            else:
                args.tier = "L0,L1"

    results = []
    env_snapshot = None

    if args.tier:
        tiers = set(args.tier.split(","))
        dev = " (on device)" if tiers - TIERS_NO_DEVICE else " (no device)"
        print(f"=== tiers {sorted(tiers)}{dev} ===")
        for tc, _ in _select(tests, tier=tiers):
            results.append(run_one(tc, args.timeout))

    elif args.adaptive:
        from common import envprobe
        if auto_env is not None:
            print("=== auto: device ready -> full bench run ===")
            env = auto_env
        else:
            print("=== adaptive: probing the bench device ===")
            env = envprobe.probe()
        env_snapshot = envprobe.summarize(env)
        print(env_snapshot)
        if not env.chainloader_alive:
            print("\n  WARNING: chainloader not confirmed live; device tests may all fail.")
        for tc, skip in _select(tests, adaptive=True):
            if skip:
                results.append(TestResult(tc, _skip_status(tc), reason=skip))
            else:
                results.append(run_one(tc, args.timeout))

    elif args.matrix:
        from common import envprobe, provision
        from tests import env as envpkg
        env_lines = []
        for name in args.matrix.split(","):
            name = name.strip()
            try:
                recipe = envpkg.get(name)
            except KeyError as e:
                print(f"  {e}")
                continue
            print(f"\n=== provisioning {name} ===")
            snap, failures = provision.apply(recipe)
            env_lines.append(envprobe.summarize(snap) if snap else f"{name}: (provision)")
            if failures:
                # Provision verification failed: record it, still try the invariant.
                tc0 = TestCase(id=f"provision::{name}", path=Path("(provision)"),
                               tier="L4", subsystem="provision", envs=[name], build=None,
                               observable="swd", automated=True, goal=[8])
                results.append(TestResult(tc0, "FAIL", reason="; ".join(failures)))
            for tc, skip in _select(tests, env=name):
                if skip:
                    results.append(TestResult(tc, _skip_status(tc), reason=skip))
                else:
                    results.append(run_one(tc, args.timeout))
        env_snapshot = "\n\n".join(env_lines)

    mode = ("tier " + args.tier if args.tier
            else "adaptive" if args.adaptive else "matrix " + args.matrix)
    if args.auto:
        mode = f"auto ({mode})"          # show what the gate resolved to
    return emit_report(results, env_snapshot, mode, errors, started)


def _skip_status(tc):
    if not tc.automated:
        return "TOUR" if tc.tier == "tour" else "MANUAL"
    return "SKIP"


if __name__ == "__main__":
    sys.exit(main())
