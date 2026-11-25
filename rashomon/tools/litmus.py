#!/usr/bin/env python3
"""Calibrate the judges on litmus tests whose answer is known.

Each litmus/*.cpp is a complete program with a header:
    // expect: PASS | FAIL [assert|race|liveness] | INCONCLUSIVE
    // expect-<judge>: ...           optional per-judge override (genmc, dartagnan)
    // known-wrong: <judge>: reason  a documented blind spot of that judge
    // liveness: genmc               ask for the liveness check (default: off)
    // why: ...
It is staged exactly like an implementation under test (stage_impl, same flags, same
model), so this calibrates the whole pipeline, not just the checker binaries.

Both directions matter. An allowed weak behaviour (expect FAIL) that a judge PASSes means
its model is too strong: the dangerous direction, a false PASS. A forbidden one (expect
PASS) that it FAILs means its model is too weak.

usage: litmus.py [--judges genmc,dartagnan] [--jobs N] [-k SUBSTR] [--quiet]
exit:  0 every result matches (known blind spots allowed), 1 otherwise
"""
import argparse
import re
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import rashomon as M  # noqa: E402

LITMUS = ROOT / "litmus"
VERDICT = {"pass": "PASS", "fail": "FAIL", "inconclusive": "INCONCLUSIVE", "compile-error": "COMPILE-ERROR"}


def fail_kind(judge: str, r) -> str:
    """Why a judge failed: assert, race or liveness ('?' if unrecognised)."""
    e = r.error
    if judge == "genmc":
        if "race" in e.lower():
            return "race"
        if "Liveness" in e or "no execution terminates" in e:
            return "liveness"
        if "Assertion" in e or "Safety violation" in e:
            return "assert"
    else:
        if "CAT specification" in e:
            return "race"   # the only flag in _rc11_cpp20.cat is 'racy'
        if "Program specification" in e:
            return "assert"
        if "ermination" in e or "iveness" in e:
            return "liveness"
    return "?"


def parse(path: Path) -> dict:
    meta = {"expect": None, "known": {}, "liveness": "termination", "why": "", "override": {}}
    for line in path.read_text().splitlines():
        m = re.match(r"//\s*([\w-]+):\s*(.*)$", line)
        if not m:
            if line.strip() and not line.startswith("//"):
                break
            continue
        key, val = m.group(1), m.group(2).strip()
        if key == "expect":
            meta["expect"] = val.split()
        elif key.startswith("expect-"):
            meta["override"][key[len("expect-"):]] = val.split()
        elif key == "known-wrong":
            judge, _, reason = val.partition(":")
            meta["known"][judge.strip()] = reason.strip()
        elif key == "liveness":
            meta["liveness"] = val
        elif key == "why":
            meta["why"] = val
    if not meta["expect"]:
        raise SystemExit(f"{path.name}: missing '// expect:' header")
    return meta


def run(judge: str, path: Path, meta: dict, timeout: float):
    spec = {"harness": LITMUS / "_harness.cpp", "_name": "litmus", "_dir": LITMUS,
            "liveness": meta["liveness"], "scenarios": [{}]}
    fn = M.run_scenario if judge == "genmc" else M.run_scenario_dartagnan
    return fn(path, spec, {}, timeout)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--judges", default=None, help="comma list (default: genmc, plus dartagnan if installed)")
    p.add_argument("--jobs", type=int, default=3)
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("-k", default="", help="only tests whose name contains this")
    p.add_argument("--quiet", action="store_true", help="print only problems and the summary")
    a = p.parse_args()
    judges = a.judges.split(",") if a.judges else (
        ["genmc", "dartagnan"] if Path(M.DARTAGNAN_JAR).exists() else ["genmc"])
    tests = sorted(t for t in LITMUS.glob("*.cpp") if not t.name.startswith("_") and a.k in t.stem)
    metas = {t: parse(t) for t in tests}
    lock = threading.Lock()
    counts = {"ok": 0, "known": 0, "WRONG": 0, "FIXED?": 0}

    def one(job):
        t, judge = job
        meta = metas[t]
        exp = meta["override"].get(judge, meta["expect"])
        r = run(judge, t, meta, a.timeout)
        got = VERDICT[r.verdict]
        kind = fail_kind(judge, r) if r.verdict == "fail" else ""
        match = got == exp[0] and (got != "FAIL" or len(exp) < 2 or kind == exp[1])
        known = meta["known"].get(judge)
        status = ("FIXED?" if known else "ok") if match else ("known" if known else "WRONG")
        detail = "" if match and not known else (known if status == "known" else r.error[:160])
        line = (f"{status:<7} {t.stem:<22} {judge:<10} expected {' '.join(exp):<17} "
                f"got {(got + ' ' + kind).strip():<17} {r.note:<9} {detail}")
        with lock:
            counts[status] += 1
            if not a.quiet or status not in ("ok",):
                print(line, flush=True)

    jobs = [(t, j) for t in tests for j in judges]
    with ThreadPoolExecutor(a.jobs) as ex:
        list(ex.map(one, jobs))
    print(f"litmus: {len(tests)} tests x {len(judges)} judges: ok {counts['ok']}, known blind spots "
          f"{counts['known']}, WRONG {counts['WRONG']}, known blind spot now passing {counts['FIXED?']}")
    return 1 if counts["WRONG"] or counts["FIXED?"] else 0


if __name__ == "__main__":
    sys.exit(main())
