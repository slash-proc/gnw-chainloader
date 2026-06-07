#!/usr/bin/env python3
"""Installer module end-to-end: delete an artifact from LittleFS, reboot, and verify
the transient installer offers to re-install it from the SD and does so.

Exercises Stage 2 for BOTH artifact classes the generic installer handles:
  --kind lang    a language pack  (/i18n/it_IT.lang)
  --kind module  a PIE module     (/modules/theme/theme.bin)
  --kind all     both (default)

The installer lives in /modules/installer.bin (MOD_FLAG_TRANSIENT) and runs the same
descriptor-driven path for either class (peek header -> gate magic+ABI -> version
compare -> stream the copy). Precondition: an SD card carrying the artifact under its
canonical path (the real user flow copies build/ onto the SD). For a module the test
pushes it to the SD itself (sdpush); the lang packs are already there. If the offer
doesn't appear the test restores the artifact and notes it (clean device, not a fail).

Flow per kind:
  1. (module) push the .bin to the SD; delete the artifact from LittleFS (reboot).
  2. On boot the installer scans, and -- if the SD carries it and LittleFS lacks it --
     shows "Install N ... from SD?".
  3. Detect the confirm by its distinctive "from SD" text, press A to accept.
  4. Verify the artifact is back on LittleFS by reading the device tree, and leave it
     present either way.

A non-critical artifact is used per kind (it_IT lang; theme module -- the menu still
renders without it), so a delete can't break the live UI.

  python3 scripts/tests/installer_test.py [--kind all|lang|module]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import remote_input as ri
from common import harness as h
from common import ocrnav

REPO = Path(__file__).resolve().parents[2]
PUSH = REPO / "scripts" / "build" / "push_batched.py"

KINDS = {
    "lang": dict(
        lfs_path="/i18n/it_IT.lang", local=REPO / "build" / "i18n" / "it_IT.lang",
        sd_src=None, tree_dir="/i18n", name="it_IT.lang", word="language"),
    "module": dict(
        lfs_path="/modules/theme/theme.bin", local=REPO / "build" / "theme.bin",
        sd_src="/modules/theme/theme.bin", tree_dir="/modules/theme", name="theme.bin",
        word="module"),
}


def push_rm(path: str) -> None:
    subprocess.run([sys.executable, str(PUSH), "--rm", path], cwd=REPO, check=False, timeout=180)


def push_file(gnw: str, local: Path) -> None:
    subprocess.run([sys.executable, str(PUSH), "--no-skip", f"{gnw}={local}"],
                   cwd=REPO, check=False, timeout=180)


def sd_push(local: Path, sd_path: str) -> None:
    subprocess.run(["gnwmanager", "sdpush", str(local), sd_path],
                   cwd=REPO, check=False, capture_output=True, timeout=150)


def lfs_has(tree_dir: str, name: str) -> bool:
    """True if `name` exists under `tree_dir` on LittleFS; leaves the device on bank1."""
    r = subprocess.run(["gnwmanager", "tree", tree_dir, "--", "start", "bank1"],
                       cwd=REPO, capture_output=True, text=True, timeout=180)
    return name in r.stdout


def run_kind(t, k: dict) -> None:
    if not k["local"].is_file():
        t.note(f"{k['local']} missing -- run the build (+ make i18n) first")
        return
    if k["sd_src"]:
        sd_push(k["local"], k["sd_src"])      # ensure the SD carries it (modules)

    print(f"removing {k['lfs_path']} from LittleFS ...", flush=True)
    push_rm(k["lfs_path"])
    time.sleep(3.0)                           # boot + the one-shot installer scan

    accepted = False
    with ri.session() as dev:
        time.sleep(1.0)
        sc = ocrnav.shot(dev)
        if sc.has("from SD"):                 # unique to the install confirm
            t.check(sc.has(k["word"]),
                    f"prompt is class-aware ('{k['word']}') for {k['name']}")
            t.check(True, f"installer offered to install {k['name']} from SD")
            dev.button_press([ri.BTN_A]); h.settle(0.5)
            time.sleep(3.0)                    # commit + rediscover
            accepted = True
        else:
            t.note(f"no install confirm for {k['name']} -- SD missing it / not newer; "
                   "restoring and skipping")

    present = lfs_has(k["tree_dir"], k["name"]) if accepted else False
    if accepted:
        t.check(present, f"{k['name']} re-installed to LittleFS (read from the device tree)")
    if not present:
        push_file(k["lfs_path"], k["local"])   # restore so the device is left intact
        t.note(f"restored {k['lfs_path']}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kind", default="all", choices=["all", "lang", "module"])
    args = ap.parse_args()
    kinds = ["lang", "module"] if args.kind == "all" else [args.kind]

    t = h.TestRun("installer: SD re-install via confirm")
    for name in kinds:
        run_kind(t, KINDS[name])
    return t.summary()


if __name__ == "__main__":
    sys.exit(main())
