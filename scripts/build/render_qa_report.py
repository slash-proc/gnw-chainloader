#!/usr/bin/env python3
"""Render the QA results store into a single self-contained HTML report.

The QA suite runner (scripts/tests/run_suite.py) accumulates every test it runs
into one results store (build/qa_results.json), latest-result-per-test. This
renders that store into ONE file -- qa-report.html in the project root -- no
matter how many suites/modes were run. Self-contained (inline CSS, no assets).

  python3 scripts/build/render_qa_report.py            # render from the store
  make qa-report                                       # same, via the Makefile

Used both by run_suite (auto after each run) and standalone (re-render anytime).
"""
import html
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
STORE = REPO / "build" / "qa_results.json"
OUT = REPO / "qa-report.html"

BADGE = {
    "PASS": ("#1a7f37", "PASS"), "FAIL": ("#cf222e", "FAIL"),
    "TIMEOUT": ("#cf222e", "TIMEOUT"), "ERROR": ("#cf222e", "ERROR"),
    "SKIP": ("#6e7781", "skip"), "MANUAL": ("#9a6700", "manual"),
    "TOUR": ("#6e7781", "tour"),
}


def load_store() -> dict:
    if STORE.is_file():
        try:
            return json.loads(STORE.read_text())
        except Exception:
            pass
    return {"tests": {}, "env": "", "updated": "", "runs": []}


def _esc(s) -> str:
    return html.escape(str(s if s is not None else ""))


def render(store: dict) -> str:
    tests = store.get("tests", {})
    counts = {}
    for r in tests.values():
        s = r.get("status", "?")
        counts[s] = counts.get(s, 0) + 1
    npass = counts.get("PASS", 0)
    nfail = sum(counts.get(k, 0) for k in ("FAIL", "TIMEOUT", "ERROR"))
    nskip = sum(counts.get(k, 0) for k in ("SKIP", "MANUAL", "TOUR"))
    order = {"FAIL": 0, "TIMEOUT": 1, "ERROR": 2, "PASS": 3, "SKIP": 4, "MANUAL": 5, "TOUR": 6}

    rows = []
    for tid, r in sorted(tests.items(),
                         key=lambda kv: (order.get(kv[1].get("status"), 9),
                                         kv[1].get("subsystem", ""), kv[0])):
        colour, label = BADGE.get(r.get("status", ""), ("#6e7781", r.get("status", "?")))
        secs = r.get("seconds")
        rows.append(
            "<tr>"
            f"<td><span class=badge style='background:{colour}'>{_esc(label)}</span></td>"
            f"<td class=mono>{_esc(tid)}</td>"
            f"<td>{_esc(r.get('tier'))}</td>"
            f"<td>{_esc(r.get('subsystem'))}</td>"
            f"<td>{_esc(r.get('observable'))}</td>"
            f"<td>{_esc(','.join(str(g) for g in r.get('goal', [])))}</td>"
            f"<td class=num>{('%.1fs' % secs) if secs else ''}</td>"
            f"<td class=reason>{_esc(r.get('reason'))}</td>"
            "</tr>")

    # Goal coverage.
    by_goal = {}
    for tid, r in tests.items():
        for g in r.get("goal", []):
            by_goal.setdefault(g, []).append(r)
    goal_rows = []
    for g in sorted(by_goal):
        rs = by_goal[g]
        p = sum(1 for r in rs if r.get("status") == "PASS")
        goal_rows.append(f"<tr><td class=num>{g}</td><td class=num>{p}/{len(rs)}</td>"
                         f"<td>{_esc(', '.join(sorted({k for k, v in tests.items() if g in v.get('goal', [])})))}</td></tr>")

    env = store.get("env", "")
    return f"""<!doctype html><html lang=en><head><meta charset=utf-8>
<title>GNW Chainloader QA Report</title>
<style>
 body{{font:14px/1.5 -apple-system,Segoe UI,Roboto,sans-serif;margin:0;background:#f6f8fa;color:#1f2328}}
 .wrap{{max-width:1000px;margin:0 auto;padding:24px}}
 h1{{font-size:22px;margin:0 0 4px}} h2{{font-size:16px;margin:28px 0 8px}}
 .sub{{color:#6e7781;font-size:13px}}
 .cards{{display:flex;gap:12px;margin:16px 0}}
 .card{{flex:1;background:#fff;border:1px solid #d0d7de;border-radius:8px;padding:14px;text-align:center}}
 .card .n{{font-size:28px;font-weight:600}} .card.p .n{{color:#1a7f37}} .card.f .n{{color:#cf222e}} .card.s .n{{color:#6e7781}}
 table{{width:100%;border-collapse:collapse;background:#fff;border:1px solid #d0d7de;border-radius:8px;overflow:hidden}}
 th,td{{text-align:left;padding:7px 10px;border-bottom:1px solid #eaeef2;font-size:13px}}
 th{{background:#f6f8fa;font-weight:600}}
 .badge{{color:#fff;padding:2px 8px;border-radius:12px;font-size:11px;font-weight:600}}
 .mono{{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}}
 .num{{text-align:right;font-variant-numeric:tabular-nums}} .reason{{color:#6e7781;font-size:12px}}
 pre{{background:#fff;border:1px solid #d0d7de;border-radius:8px;padding:12px;overflow:auto;font-size:12px;white-space:pre-wrap}}
 .foot{{color:#6e7781;font-size:12px;margin-top:24px}}
</style></head><body><div class=wrap>
 <h1>GNW Chainloader QA Report</h1>
 <div class=sub>generated {_esc(store.get('updated'))} &middot; {len(tests)} tests across {len(store.get('runs', []))} run(s)</div>
 <div class=cards>
  <div class="card p"><div class=n>{npass}</div><div>passed</div></div>
  <div class="card f"><div class=n>{nfail}</div><div>failed</div></div>
  <div class="card s"><div class=n>{nskip}</div><div>not applicable</div></div>
 </div>
 <h2>Results</h2>
 <table><tr><th>Status</th><th>Test</th><th>Tier</th><th>Subsystem</th><th>Obs</th><th>Goals</th><th>Time</th><th>Note</th></tr>
 {''.join(rows) or '<tr><td colspan=8 class=sub>no results yet</td></tr>'}</table>
 <h2>Goal coverage</h2>
 <table><tr><th>Goal</th><th>Passed</th><th>Tests</th></tr>
 {''.join(goal_rows) or '<tr><td colspan=3 class=sub>none</td></tr>'}</table>
 <h2>Device environment (last device run)</h2>
 <pre>{_esc(env) or 'n/a (host-only run)'}</pre>
 <div class=foot>Each suite run merges into build/qa_results.json (latest result per test); this is the single rendered view.</div>
</div></body></html>"""


def main() -> int:
    store = load_store()
    OUT.write_text(render(store))
    print(f"wrote {OUT.relative_to(REPO)} ({len(store.get('tests', {}))} tests)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
