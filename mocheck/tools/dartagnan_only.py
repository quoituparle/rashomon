#!/usr/bin/env python3
"""Run only the second judge (Dartagnan) on every scenario of a spec, without GenMC.

For regression checks on the second judge itself: `verify --second-judge` never runs
Dartagnan on code GenMC already FAILs, so the defences that matter only for Dartagnan
(the C++20 release-sequence model, the signal-fence rewrite) cannot be pinned through it.

usage: dartagnan_only.py IMPL --spec NAME [--timeout S]
exit:  0 every scenario PASS, 1 some scenario FAIL, 3 otherwise (same codes as verify)
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mocheck as M  # noqa: E402


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("impl")
    p.add_argument("--spec", required=True)
    p.add_argument("--timeout", type=float, default=300.0)
    a = p.parse_args()
    impl, spec = Path(a.impl).resolve(), M.load_spec(a.spec)
    verdicts = []
    for scen in spec["scenarios"]:
        r = M.run_scenario_dartagnan(impl, spec, scen, a.timeout)
        sc = " ".join(f"{k}={v}" for k, v in scen.items())
        print(f"  [{r.verdict.upper():>12}] {sc:<44} {r.note or r.error}  ({r.seconds:.1f}s)")
        verdicts.append(r.verdict)
        if r.verdict == "fail":
            break
    return 1 if "fail" in verdicts else 0 if all(v == "pass" for v in verdicts) else 3


if __name__ == "__main__":
    sys.exit(main())
