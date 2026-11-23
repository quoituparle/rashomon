#!/usr/bin/env python3
"""mocheck — a memory-order judge for (AI-written) C++ concurrent code.

    mocheck lint     impl.hpp                 Rust-style: every atomic op must state its ordering
    mocheck verify   impl.hpp --spec SPEC     tool-owned scenarios, exhaustively checked under RC11
    mocheck minimize impl.hpp --spec SPEC     find the weakest orderings that still verify

The human reads the SPEC (see specs/), never the implementation.
The implementation only gets to supply code; scenario sizes, the harness and
the assertions are owned by this tool and cannot be chosen by the author.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPECS = HERE / "specs"
GENMC = os.environ.get("GENMC", "/home/claude/genmc/build/bin/genmc")
GENMC_INCLUDE = os.environ.get("GENMC_INCLUDE", str(Path(GENMC).resolve().parents[2] / "lli" / "runtime-include" / "c"))
MODEL = "--rc11"
GENMC_COMPAT = HERE / "specs" / "_genmc_compat.h"  # declarations libstdc++ needs under GenMC
GENMC_SHIMS = HERE / "specs" / "_genmc_shims"      # light <memory>/<functional> for GenMC builds

# ----------------------------------------------------------------------------
# Orderings
# ----------------------------------------------------------------------------
ORDERS = ["relaxed", "consume", "acquire", "release", "acq_rel", "seq_cst"]
# strictly-weaker relation (consume is treated as acquire)
_WEAKER = {
    "relaxed": set(),
    "acquire": {"relaxed"},
    "consume": {"relaxed"},
    "release": {"relaxed"},
    "acq_rel": {"relaxed", "acquire", "release"},
    "seq_cst": {"relaxed", "acquire", "release", "acq_rel"},
}
VALID = {
    "load": ["relaxed", "acquire", "seq_cst"],
    "store": ["relaxed", "release", "seq_cst"],
    "rmw": ["relaxed", "acquire", "release", "acq_rel", "seq_cst"],
    "cas_success": ["relaxed", "acquire", "release", "acq_rel", "seq_cst"],
    "cas_failure": ["relaxed", "acquire", "seq_cst"],
    "fence": ["relaxed", "acquire", "release", "acq_rel", "seq_cst"],  # relaxed fence == no-op
}
ORDER_RE = re.compile(r"memory_order(?:_|::)(relaxed|consume|acquire|release|acq_rel|seq_cst)\b")

RMW_NAMES = {"exchange", "fetch_add", "fetch_sub", "fetch_and", "fetch_or", "fetch_xor",
             "fetch_max", "fetch_min", "test_and_set"}
LOAD_NAMES = {"load", "wait", "test"}
STORE_NAMES = {"store", "clear"}
CAS_NAMES = {"compare_exchange_weak", "compare_exchange_strong"}
NO_ORDER_NAMES = {"notify_one", "notify_all", "is_lock_free"}
# C-style free functions without _explicit always mean seq_cst
FREE_IMPLICIT = re.compile(r"^atomic_(load|store|exchange|fetch_\w+|compare_exchange_\w+|flag_test_and_set|flag_clear|wait)$")
FREE_EXPLICIT = re.compile(r"^atomic_(load|store|exchange|fetch_\w+|compare_exchange_\w+|flag_test_and_set|flag_clear|wait)_explicit$")


@dataclass
class Site:
    """One explicit memory-order literal in the source that the tool may rewrite."""
    id: int
    line: int
    col: int
    start: int  # byte offsets of the order expression
    end: int
    op: str     # load/store/rmw/cas_success/cas_failure/fence
    call: str   # e.g. "head.store"
    order: str


@dataclass
class Finding:
    line: int
    col: int
    severity: str  # error / warning
    rule: str
    message: str


# ----------------------------------------------------------------------------
# Front end: clang's JSON AST of the *instantiated* code
# ----------------------------------------------------------------------------
CLANGXX = os.environ.get("CLANGXX", shutil.which("clang++-19") or shutil.which("clang++") or "clang++")
MAIN_NAME = "mocheck_frontend.cpp"


def _loc_offset(l):
    if not isinstance(l, dict):
        return None
    l = l.get("expansionLoc", l)
    return l.get("offset")


def _range(n):
    r = n.get("range") or {}
    b, e = r.get("begin", {}), r.get("end", {})
    bo, eo = _loc_offset(b), _loc_offset(e)
    if bo is None or eo is None:
        return None
    tl = e.get("expansionLoc", e).get("tokLen", 0)
    return bo, eo + tl


def _callee_name(n):
    """Name of the function an expression refers to (DeclRefExpr / MemberExpr / dependent member)."""
    k = n.get("kind")
    if k == "MemberExpr":
        return n.get("name", "")
    if k == "CXXDependentScopeMemberExpr":
        return n.get("member", "")
    if k == "DeclRefExpr":
        return (n.get("referencedDecl") or {}).get("name", "")
    for ch in n.get("inner", []):
        r = _callee_name(ch)
        if r:
            return r
    return ""


def _type(n):
    return ((n.get("type") or {}).get("qualType", "")) if n else ""


def _is_atomic_type(t: str) -> bool:
    return bool(re.search(r"\batomic(_ref|_flag)?\b|__atomic_base|__atomic_float|__atomic_ref", t))


def _prelude_args(work: Path, prelude: str | None) -> list[str]:
    """Declarations the harness provides to the implementation (e.g. a free() callback).
    Passed with -include so the implementation's byte offsets do not move."""
    if not prelude:
        return []
    f = work / "mocheck_prelude.h"
    f.write_text(prelude + "\n")
    return ["-include", str(f)]


def analyze(path: Path, instantiate: str | None = None, prelude: str | None = None):
    """Return (sites, findings, parse_errors) for the implementation in `path`.

    The file is analysed together with an explicit instantiation (from the spec),
    so atomics reached through template-dependent types are resolved too.
    """
    src = path.read_bytes()
    work = Path(tempfile.mkdtemp(prefix="mocheck-fe-"))
    main = work / MAIN_NAME
    main.write_bytes(src + b"\n" + (instantiate or "").encode() + b"\n")
    try:
        p = subprocess.run([CLANGXX, "-x", "c++", "-std=c++20", "-fsyntax-only", f"-I{path.parent}",
                            *_prelude_args(work, prelude), "-Xclang", "-ast-dump=json", str(main)],
                           capture_output=True, text=True, timeout=300)
    finally:
        pass
    parse_errors = [l for l in p.stderr.splitlines() if "error:" in l]
    if parse_errors or not p.stdout.strip():
        shutil.rmtree(work, ignore_errors=True)
        return [], [], parse_errors or ["clang produced no AST"]
    ast = json.loads(p.stdout)
    shutil.rmtree(work, ignore_errors=True)

    # line/col from byte offset
    line_starts = [0] + [i + 1 for i, b in enumerate(src) if b == 0x0A]

    def lc(off):
        import bisect
        i = bisect.bisect_right(line_starts, off) - 1
        return i + 1, off - line_starts[i] + 1

    sites: dict[tuple, Site] = {}
    findings: dict[tuple, Finding] = {}
    unresolved: dict[int, str] = {}
    resolved_calls: set[int] = set()

    def add_finding(off, sev, rule, msg):
        l, c = lc(off)
        findings.setdefault((off, rule), Finding(l, c, sev, rule, msg))

    def text(rg):
        return src[rg[0]:rg[1]].decode(errors="replace")

    def order_of(arg):
        if arg.get("kind") == "CXXDefaultArgExpr":
            return None, True
        rg = _range(arg)
        if rg is None:
            return "?", False
        m = ORDER_RE.search(text(rg))
        return (m.group(1) if m else "?"), False

    def add_site(arg, op, callname, order):
        rg = _range(arg)
        if rg is None or rg[1] > len(src):
            return
        if rg in [(x.start, x.end) for x in sites.values()]:
            return
        l, c = lc(rg[0])
        s = Site(len(sites), l, c, rg[0], rg[1], op, callname, order)
        sites[rg] = s
        if order not in VALID[op] and not (order == "consume" and op in ("load", "cas_failure")):
            add_finding(rg[0], "error", "invalid-order",
                        f"{callname}: memory_order_{order} is not valid for a {op.replace('_', ' ')} "
                        f"(C++: undefined behaviour; Rust: panic)")

    def member_call(n, callee, args):
        name = callee.get("name", "")
        base = (callee.get("inner") or [None])[0]
        if not _is_atomic_type(_type(base)) or name in NO_ORDER_NAMES:
            return
        rg = _range(n)
        off = rg[0]
        resolved_calls.add(off)
        obj = text(_range(callee))[: -len(name)] if _range(callee) else ""
        callname = (obj + name) if not name.startswith("operator") else name
        if name.startswith("operator"):
            what = f"implicit conversion ({name})" if re.match(r"operator [A-Za-z_]", name) else name
            add_finding(off, "error", "implicit-seq-cst",
                        f"{text(rg)!s:.40}: {what} on an atomic is an implicit seq_cst access — "
                        f"call .load/.store/.fetch_* with an explicit ordering")
        elif name in CAS_NAMES:
            if name == "compare_exchange_weak" and not in_loop[0]:
                add_finding(off, "error", "weak-cas-outside-loop",
                            f"{callname}: a single-shot weak CAS can fail spuriously (nothing changed, it just "
                            f"failed) — use compare_exchange_strong, or retry it in a loop")
            orders = args[2:]
            explicit = [a for a in orders if a.get("kind") != "CXXDefaultArgExpr"]
            if not explicit:
                add_finding(off, "error", "implicit-seq-cst", f"{callname}: no ordering given (defaults to seq_cst)")
            elif len(explicit) == 1:
                add_finding(off, "error", "implicit-failure-order",
                            f"{callname}: failure ordering is derived implicitly — "
                            f"state both success and failure orderings (as Rust requires)")
            for k, a in enumerate(explicit):
                o, _ = order_of(a)
                if o and o != "?":
                    add_site(a, "cas_success" if k == 0 else "cas_failure", callname, o)
        else:
            op = ("load" if name in LOAD_NAMES else "store" if name in STORE_NAMES
                  else "rmw" if name in RMW_NAMES else None)
            if not op:
                return
            oa = args[-1] if args else None
            o, defaulted = order_of(oa) if oa is not None else (None, True)
            if defaulted:
                add_finding(off, "error", "implicit-seq-cst", f"{callname}: no ordering given (defaults to seq_cst)")
            elif o == "?":
                add_finding(off, "warning", "opaque-order",
                            f"{callname}: ordering is not a literal; the tool cannot tune it")
            else:
                add_site(oa, op, callname, o)

    cur_file = [None]
    in_loop = [False]  # inside a loop of the current function (for weak-CAS spuriousness)
    LOOPS = {"WhileStmt", "ForStmt", "DoStmt", "CXXForRangeStmt"}
    FUNCS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl", "LambdaExpr"}

    def track(n):
        for l in (n.get("loc"), (n.get("range") or {}).get("begin"), (n.get("range") or {}).get("end")):
            if isinstance(l, dict):
                for sub in (l.get("spellingLoc"), l.get("expansionLoc"), l):
                    if isinstance(sub, dict) and "file" in sub:
                        cur_file[0] = sub["file"]

    def visit(n):
        saved = in_loop[0]
        if n.get("kind") in FUNCS:
            in_loop[0] = False
        elif n.get("kind") in LOOPS:
            in_loop[0] = True
        try:
            _visit(n)
        finally:
            in_loop[0] = saved

    def _visit(n):
        track(n)
        in_main = cur_file[0] is not None and cur_file[0].endswith(MAIN_NAME)
        k = n.get("kind")
        inner = n.get("inner", [])
        if in_main and k == "ClassTemplateDecl" and _range(n) and _range(n)[0] < len(src):
            if not any(ch.get("kind") == "ClassTemplateSpecializationDecl" for ch in inner):
                add_finding(_range(n)[0], "warning", "uninstantiated-template",
                            f"template '{n.get('name')}' is never instantiated, so atomics on dependent types "
                            f"(and implicit operators on them) cannot be checked — run with --spec")
        if in_main and _range(n) and _range(n)[0] < len(src):
            if k == "CXXMemberCallExpr" and inner and inner[0].get("kind") == "MemberExpr":
                member_call(n, inner[0], inner[1:])
            elif k == "CXXOperatorCallExpr" and len(inner) >= 2 and _is_atomic_type(_type(inner[1])):
                off = _range(n)[0]
                resolved_calls.add(off)
                add_finding(off, "error", "implicit-seq-cst",
                            f"{text(_range(n))!s:.40}: {_callee_name(inner[0])} on an atomic is an implicit "
                            f"seq_cst access — call .load/.store/.fetch_* with an explicit ordering")
            elif k in ("CallExpr", "CXXMemberCallExpr") and inner:
                fname = _callee_name(inner[0])
                off = _range(n)[0]
                if inner[0].get("kind") == "CXXDependentScopeMemberExpr" and (
                        fname in LOAD_NAMES | STORE_NAMES | RMW_NAMES | CAS_NAMES):
                    unresolved[off] = fname
                elif fname == "atomic_signal_fence":
                    add_finding(off, "warning", "signal-fence",
                                "atomic_signal_fence only orders against a signal handler on the same thread; it gives "
                                "no inter-thread ordering and is ignored by the judge (it would be enough only on x86-TSO)")
                elif fname == "atomic_thread_fence" and len(inner) > 1:
                    o, _ = order_of(inner[1])
                    if o and o != "?":
                        add_site(inner[1], "fence", fname, o)
                elif FREE_IMPLICIT.match(fname or ""):
                    add_finding(off, "error", "implicit-seq-cst", f"{fname}: the non-_explicit form is always seq_cst")
        for ch in inner:
            visit(ch)

    visit(ast)
    for off, fname in unresolved.items():
        if off not in resolved_calls:
            add_finding(off, "warning", "unresolved-template",
                        f".{fname}(...) inside an uninstantiated template could not be checked — "
                        f"run with --spec so the tool can instantiate it")
    site_list = sorted(sites.values(), key=lambda s: s.start)
    for i, s in enumerate(site_list):
        s.id = i
    flist = sorted(findings.values(), key=lambda f: (f.line, f.col))
    return site_list, flist, []


def rewrite(src: bytes, sites: list[Site], orders: dict[int, str]) -> bytes:
    out = src
    for s in sorted(sites, key=lambda s: s.start, reverse=True):
        new = orders.get(s.id, s.order)
        if new != s.order:
            out = out[:s.start] + f"std::memory_order_{new}".encode() + out[s.end:]
    return out


# ----------------------------------------------------------------------------
# Specs and the judge
# ----------------------------------------------------------------------------
def load_spec(name: str) -> dict:
    p = SPECS / name / "spec.json"
    if not p.exists():
        avail = ", ".join(sorted(d.name for d in SPECS.iterdir() if d.is_dir()))
        sys.exit(f"unknown spec '{name}'. available: {avail}")
    spec = json.loads(p.read_text())
    spec["harness"] = (SPECS / name / spec.get("harness", "harness.cpp")).resolve()
    spec["_name"], spec["_dir"] = name, SPECS / name
    return spec


# ----------------------------------------------------------------------------
# Spec admission: a spec is usable only after it has proven it can catch bugs
# ----------------------------------------------------------------------------
def spec_digest(spec: dict) -> str:
    """Hash of everything an admission depends on: spec.json, harness, reference
    implementations, and the judge itself (this file) — a changed judge must re-earn trust."""
    h = hashlib.sha256()
    h.update(Path(__file__).resolve().read_bytes())
    h.update(GENMC_COMPAT.read_bytes())
    for f in sorted(GENMC_SHIMS.iterdir()):
        h.update(f.name.encode() + f.read_bytes())
    h.update(_genmc_version().encode())
    h.update((spec["_dir"] / "spec.json").read_bytes())
    h.update(spec["harness"].read_bytes())
    for r in sorted(spec.get("references", [])):
        h.update(r.encode())
        h.update((spec["_dir"] / r).read_bytes())
    return h.hexdigest()


def admission_status(spec: dict) -> tuple[bool, str]:
    f = spec["_dir"] / "admission.json"
    if not f.exists():
        return False, f"spec '{spec['_name']}' has never been admitted (run: mocheck admit {spec['_name']})"
    rec = json.loads(f.read_text())
    if rec.get("verdict") != "ADMITTED":
        return False, f"spec '{spec['_name']}' was rejected at admission"
    if rec.get("spec_digest") != spec_digest(spec):
        return False, (f"spec '{spec['_name']}' changed since it was admitted "
                       f"(spec.json, harness, a reference or the judge itself differs) — re-run: mocheck admit {spec['_name']}")
    return True, f"admitted {rec.get('admitted_at', '?')}"


def _require_admission(spec: dict, allow: bool) -> bool:
    ok, msg = admission_status(spec)
    if ok:
        return True
    if allow:
        print(f"WARNING: {msg}; continuing because --allow-unadmitted was given", file=sys.stderr)
        return True
    print(f"refused: {msg}")
    print("an unadmitted spec has not shown that it can catch bugs, so its PASS would mean nothing.")
    return False


@dataclass
class RunResult:
    scenario: dict
    verdict: str  # pass / fail / inconclusive / compile-error
    error: str = ""
    trace: str = ""
    executions: int = 0
    seconds: float = 0.0
    blocked: int = 0


def _genmc_version() -> str:
    try:
        out = subprocess.run([GENMC, "--version"], capture_output=True, text=True, timeout=30).stdout
        v = re.search(r"GenMC v[\d.]+(?: \(commit #\w+\))?", out)
        l = re.search(r"LLVM [\d.]+", out)
        return " / ".join(x.group(0) for x in (v, l) if x) or out.strip().splitlines()[0]
    except Exception as e:  # pragma: no cover
        return f"unknown ({e})"


def run_scenario(impl: Path, spec: dict, scen: dict, timeout: float) -> RunResult:
    defs = [f"-D{k}={v}" for k, v in {**spec.get("defines", {}), **scen}.items()]
    stage = Path(tempfile.mkdtemp(prefix="mocheck-run-"))
    # Weak CAS inside a retry loop is modelled as strong: a spurious failure only
    # makes the loop go round again with the same expected value (a stutter), and
    # exploring unboundedly many stutters is what makes GenMC never terminate.
    # Weak CAS outside a loop is rejected by lint, so nothing is lost there.
    text = re.sub(r"\bcompare_exchange_weak\b", "compare_exchange_strong", impl.read_text(errors="replace"))
    # exceptions cannot be modelled by GenMC (see specs/_genmc_compat.h)
    text = re.sub(r"\bthrow\s+(?!\s*\))([^;{}]*);", "__mocheck_throw_unreachable();", text)
    # GenMC ignores syncscope("singlethread") and treats atomic_signal_fence as a full
    # thread fence, which would make code relying on it look correct. Between threads a
    # signal fence guarantees nothing, so removing it is exactly the C++ semantics.
    text = re.sub(r"(?:std::)?\batomic_signal_fence\s*\([^;]*\)\s*;", "/* signal fence: no inter-thread effect */;", text)
    # `T member[N] = {};` zero-fills with memset, which GenMC rejects on atomics. Harness
    # objects have static storage (already zeroed before construction), so dropping the
    # empty initializer does not change what the object holds.
    text = re.sub(r"(\[[^\]\n]+\])\s*=\s*\{\s*\}\s*;", r"\1;", text)
    # `std::atomic<T> x{}, y{};` value-initialises through memset as well, and so does `: x()` in
    # a constructor's initializer list: construct from T{} instead (same value, no memset).
    atomic_names = []
    def _value_init(m):
        ty, decls = m.group(1), m.group(2)
        out = []
        for d in decls.split(","):
            nm = re.match(r"\s*(\w+)", d)
            if nm:
                atomic_names.append((ty, nm.group(1)))
            out.append(re.sub(r"^(\s*\w+)\s*\{\s*\}\s*$", rf"\1{{{ty}{{}}}}", d))
        return f"std::atomic<{ty}>{','.join(out)};"
    text = re.sub(r"std::atomic<((?:[^<>]|<[^<>]*>)*)>((?:\s+\w+\s*(?:\{[^{};]*\})?\s*,?)+);", _value_init, text)
    for ty, name in set(atomic_names):
        text = re.sub(rf"([:,]\s*){name}\s*\(\s*\)", rf"\g<1>{name}({ty}{{}})", text)
    # Empty-template inline asm (`asm("" : "+r"(x))`) is a compiler barrier that emits no
    # instruction: semantically the identity. GenMC's interpreter cannot run inline asm and
    # yields 0 for its outputs, which silently corrupts the operand (false FAILs). Erase it.
    text = re.sub(r'\b(?:__)?asm(?:__)?\s*(?:(?:__)?volatile(?:__)?\s*)?\(\s*""\s*(?::[^;\n]*)?\)', "((void)0)", text)
    # CPU spin hints (x86 pause) are semantically no-ops and unknown to GenMC
    text = re.sub(r"\b(?:_mm_pause|__builtin_ia32_pause)\s*\(\s*\)", "((void)0)", text)
    (stage / "mocheck_impl.hpp").write_text(text)
    live = ["--check-liveness"] if spec.get("liveness", "genmc") == "genmc" else []
    # Inline asm that survives preprocessing is real code GenMC would silently evaluate as 0:
    # refuse to judge it. Checked on the IR so asm in inactive #if branches (other CPUs) is fine.
    flags = ["-std=c++20", "-include", str(GENMC_COMPAT), f"-I{GENMC_SHIMS}", f"-I{stage}", f"-I{impl.parent}",
             f"-I{GENMC_INCLUDE}", *defs]
    ir = subprocess.run([CLANGXX, *flags, "-S", "-emit-llvm", "-O0", "-o", "-", str(spec["harness"])],
                        capture_output=True, text=True)
    asm = re.search(r'call [^\n]*\basm (?:sideeffect )?(?:inteldialect )?"([^"]*)"', ir.stdout) if ir.returncode == 0 else None
    if asm:
        shutil.rmtree(stage, ignore_errors=True)
        return RunResult(scen, "inconclusive", f'inline assembly "{asm.group(1)[:40]}" cannot be model-checked '
                         "(GenMC would evaluate it as 0); give it a portable C++ fallback")
    cmd = [GENMC, MODEL, *live, "--disable-estimation", "--", "-std=c++20", "-include", str(GENMC_COMPAT),
           f"-I{GENMC_SHIMS}", f"-I{stage}", f"-I{impl.parent}", *defs, str(spec["harness"])]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return RunResult(scen, "inconclusive", f"timed out after {timeout:.0f}s", seconds=time.time() - t0)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    out = p.stdout + p.stderr
    dt = time.time() - t0
    m = re.search(r"complete executions explored: (\d+)", out)
    ex = int(m.group(1)) if m else 0
    bm = re.search(r"blocked executions seen: (\d+)", out)
    blocked = int(bm.group(1)) if bm else 0
    if "No errors were detected" in out:
        if ex == 0:
            return RunResult(scen, "fail", "no execution terminates (every interleaving gets stuck)",
                             seconds=dt, blocked=blocked)
        return RunResult(scen, "pass", executions=ex, seconds=dt, blocked=blocked)
    em = re.search(r"^Error: (.*)$", out, re.M)
    if em:
        trace = out[em.start():]
        trace = trace.split("*** Verification")[0].rstrip()
        err = em.group(1).strip()
        am = re.search(r"^Assertion violation: .*?\"(.*)\"\s*$", out, re.M)
        if am:
            err += " " + am.group(1)
        hist = list(dict.fromkeys(l[len("MOCHECK| "):] for l in out.splitlines() if l.startswith("MOCHECK| ")))
        if hist:
            trace = "history:\n  " + "\n  ".join(hist) + "\n\nexecution graph:\n" + trace
        return RunResult(scen, "fail", err, trace, ex, dt)
    if "error:" in out:
        msg = "\n".join(l for l in out.splitlines() if "error:" in l)[:2000]
        return RunResult(scen, "compile-error", msg, seconds=dt)
    return RunResult(scen, "inconclusive", out.strip().splitlines()[-1] if out.strip() else "no output", seconds=dt)


NATIVE_RUNS = 3


def run_native(impl: Path, spec: dict, scen: dict, timeout: float) -> RunResult:
    """Memory safety / UB pass: GenMC explores interleavings but does not check, e.g., an
    out-of-bounds write inside a global object. Compile the same harness natively with
    ASan + UBSan and run it a few times on real threads."""
    tag = {**scen, "check": "asan+ubsan"}
    defs = [f"-D{k}={v}" for k, v in {**spec.get("defines", {}), **scen}.items()]
    stage = Path(tempfile.mkdtemp(prefix="mocheck-native-"))
    t0 = time.time()
    try:
        shutil.copy(impl, stage / "mocheck_impl.hpp")
        exe = stage / "harness"
        c = subprocess.run([CLANGXX, "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-pthread",
                            f"-I{stage}", f"-I{impl.parent}", *defs, str(spec["harness"]), "-o", str(exe)],
                           capture_output=True, text=True, timeout=300)
        if c.returncode != 0:
            msg = "\n".join(l for l in c.stderr.splitlines() if "error:" in l)[:2000]
            return RunResult(tag, "compile-error", msg or c.stderr[:2000], seconds=time.time() - t0)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0", UBSAN_OPTIONS="print_stacktrace=1")
        for _ in range(NATIVE_RUNS):
            try:
                r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout, env=env)
            except subprocess.TimeoutExpired:
                return RunResult(tag, "inconclusive", "native run timed out", seconds=time.time() - t0)
            if r.returncode != 0:
                out = r.stderr + r.stdout
                m = (re.search(r"ERROR: AddressSanitizer: ([^\n]*)", out)
                     or re.search(r"runtime error: ([^\n]*)", out)
                     or re.search(r"Assertion [`'](.*?)' failed", out))
                err = m.group(0) if m else f"exit status {r.returncode}"
                return RunResult(tag, "fail", err.strip(), out[:6000], seconds=time.time() - t0)
        return RunResult(tag, "pass", executions=NATIVE_RUNS, seconds=time.time() - t0)
    finally:
        shutil.rmtree(stage, ignore_errors=True)


LLVM_PROFDATA = os.environ.get("LLVM_PROFDATA", shutil.which("llvm-profdata-19") or "llvm-profdata")
LLVM_COV = os.environ.get("LLVM_COV", shutil.which("llvm-cov-19") or "llvm-cov")


def line_profile(impl: Path, spec: dict, runs: int = 3, timeout: float = 30.0) -> dict[int, int]:
    """How often each line of the implementation executes on the spec's own scenarios
    (native build with coverage instrumentation). Used to weight hardware cost: a fence on
    a path that almost never runs costs almost nothing. Returns {} if profiling fails."""
    counts: dict[int, int] = {}
    stage = Path(tempfile.mkdtemp(prefix="mocheck-prof-"))
    try:
        shutil.copy(impl, stage / "mocheck_impl.hpp")
        for si, scen in enumerate(spec["scenarios"]):
            defs = [f"-D{k}={v}" for k, v in {**spec.get("defines", {}), **scen}.items()]
            exe = stage / f"prof{si}"
            c = subprocess.run([CLANGXX, "-std=c++20", "-O0", "-pthread", "-fprofile-instr-generate",
                                "-fcoverage-mapping", f"-I{stage}", f"-I{impl.parent}", *defs,
                                str(spec["harness"]), "-o", str(exe)], capture_output=True, text=True, timeout=300)
            if c.returncode != 0:
                return {}
            raws = []
            for r in range(runs):
                raw = stage / f"p{si}_{r}.profraw"
                env = dict(os.environ, LLVM_PROFILE_FILE=str(raw))
                subprocess.run([str(exe)], capture_output=True, timeout=timeout, env=env)
                if raw.exists():
                    raws.append(str(raw))
            if not raws:
                continue
            prof = stage / f"p{si}.profdata"
            subprocess.run([LLVM_PROFDATA, "merge", "-o", str(prof), *raws], capture_output=True, timeout=120)
            show = subprocess.run([LLVM_COV, "show", str(exe), f"-instr-profile={prof}",
                                   "-show-instantiations=false", str(stage / "mocheck_impl.hpp")],
                                  capture_output=True, text=True, timeout=120).stdout
            for l in show.splitlines():
                m = re.match(r"\s*(\d+)\|\s*([\d.]+)([kM]?)\|", l)
                if m:
                    n = float(m.group(2)) * {"": 1, "k": 1e3, "M": 1e6}[m.group(3)]
                    counts[int(m.group(1))] = counts.get(int(m.group(1)), 0) + int(n)
        return counts
    except Exception:
        return {}
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def judge(impl: Path, spec: dict, timeout: float, stop_on_fail=True) -> list[RunResult]:
    results = judge_genmc(impl, spec, timeout, stop_on_fail)
    if all(r.verdict == "pass" for r in results):
        for scen in spec["scenarios"]:
            r = run_native(impl, spec, scen, min(timeout, 30.0))
            results.append(r)
            if stop_on_fail and r.verdict in ("fail", "compile-error"):
                break
    return results


def judge_genmc(impl: Path, spec: dict, timeout: float, stop_on_fail=True) -> list[RunResult]:
    results = []
    # smallest scenarios first: bugs usually surface there, and cheaply
    for scen in spec["scenarios"]:
        r = run_scenario(impl, spec, scen, timeout)
        results.append(r)
        # A FAIL is definitive and ends judging. INCONCLUSIVE is not: a later
        # scenario may still produce a definitive FAIL, so keep going.
        if stop_on_fail and r.verdict in ("fail", "compile-error"):
            break
    return results


def overall(results: list[RunResult]) -> str:
    if any(r.verdict == "fail" for r in results):
        return "FAIL"
    if any(r.verdict == "compile-error" for r in results):
        return "COMPILE-ERROR"
    if any(r.verdict == "inconclusive" for r in results):
        return "INCONCLUSIVE"
    return "PASS"


def certificate(impl: Path, spec_name: str, spec: dict, results, verdict: str) -> dict:
    h = hashlib.sha256()
    h.update(impl.read_bytes())
    return {
        "verdict": verdict,
        "impl_sha256": h.hexdigest(),
        "spec": spec_name,
        "spec_digest": spec_digest(spec),
        "spec_admitted": admission_status(spec)[0],
        "memory_model": MODEL.lstrip("-").upper(),
        "checker": _genmc_version(),
        "guarantee": "exhaustive over all executions allowed by the memory model, "
                     "for the bounded scenarios listed (not an unbounded proof); "
                     "side-effect-free spin iterations are pruned by GenMC's spin-assume, and weak CAS "
                     "inside retry loops is modelled as strong (a spurious failure is a stutter); both are "
                     "part of the trusted base",
        "liveness": spec.get("liveness", "genmc"),
        "shrunk_scenarios_skipped": bool(spec.get("_shrink_skipped")),
        "lint_errors_ignored": spec.get("_lint_errors_ignored", 0),
        "scenarios": [{**r.scenario, "verdict": r.verdict, "executions": r.executions,
                       "pruned_by_spin_assume": r.blocked, "seconds": round(r.seconds, 3)}
                      for r in results],
    }


# ----------------------------------------------------------------------------
# Commands
# ----------------------------------------------------------------------------
def fmt_findings(path, findings):
    lines = []
    for f in findings:
        lines.append(f"{path}:{f.line}:{f.col}: {f.severity}: [{f.rule}] {f.message}")
    return "\n".join(lines)


def cmd_lint(a) -> int:
    path = Path(a.impl).resolve()
    sp = load_spec(a.spec) if a.spec else {}
    sites, findings, perr = analyze(path, sp.get("instantiate"), sp.get("prelude"))
    if perr:
        print("\n".join(perr))
        return 2
    if a.spec:
        findings = shrink_findings(path, load_spec(a.spec)) + findings
    if a.json:
        print(json.dumps({"findings": [asdict(f) for f in findings], "sites": [asdict(s) for s in sites]}, indent=2))
    else:
        if findings:
            print(fmt_findings(a.impl, findings))
        errs = sum(f.severity == "error" for f in findings)
        print(f"lint: {errs} error(s), {len(findings) - errs} warning(s), {len(sites)} tunable ordering site(s)")
    return 1 if any(f.severity == "error" for f in findings) else 0


def shrink_findings(path: Path, spec: dict) -> list[Finding]:
    """A shrunk scenario only means something if the implementation actually uses the
    shrinkable parameter (e.g. wraps its counter at 2^BITS). An implementation that
    ignores it runs at full width in the 'small' scenario, and the scenario tests nothing."""
    out = []
    src = path.read_text(errors="replace")
    code = re.sub(r"//[^\n]*|/\*.*?\*/", "", src, flags=re.S)
    for p in spec.get("shrink_params", []):
        uses = len(re.findall(rf"\b{re.escape(p)}\b", code))
        if uses < 2:  # the template declaration itself plus at least one real use
            out.append(Finding(1, 1, "error", "unused-shrink-parameter",
                               f"template parameter '{p}' is declared but never used: the spec shrinks it to "
                               f"expose wrap-around bugs, so the implementation must honour it"))
    return out


def without_shrink(spec: dict) -> dict:
    """For auditing third-party code that has no shrinkable parameters: drop the scenarios
    that shrink them (a parameter's production value is its largest value in the scenarios)."""
    params = spec.get("shrink_params", [])
    if not params:
        return spec

    def full(sc_list):
        prod = {p: max(sc.get(f"MO_{p}", 0) for sc in spec["scenarios"]) for p in params}
        return [sc for sc in sc_list if all(sc.get(f"MO_{p}", prod[p]) >= prod[p] for p in params)]

    out = dict(spec, scenarios=full(spec["scenarios"]),
               challenge_scenarios=full(spec.get("challenge_scenarios", [])), shrink_params=[])
    out["_shrink_skipped"] = True
    return out


def _lint_gate(path: Path, spec: dict, warn_only: bool = False) -> tuple[list[Site], bool]:
    sites, findings, perr = analyze(path, spec.get("instantiate"), spec.get("prelude"))
    if perr:
        print("\n".join(perr))
        return sites, False
    findings = shrink_findings(path, spec) + findings
    errs = [f for f in findings if f.severity == "error"]
    if errs and warn_only:
        print(fmt_findings(path.name, errs).replace(": error: ", ": warning (--lint-warn): "))
        spec["_lint_errors_ignored"] = len(errs)
        return sites, True
    if errs:
        print(fmt_findings(path.name, errs))
        print("lint failed: fix these before the judge will look at the code (same rule as Rust: say what you mean).")
        return sites, False
    return sites, True


def cmd_verify(a) -> int:
    path = Path(a.impl).resolve()
    spec = load_spec(a.spec)
    if not _require_admission(spec, a.allow_unadmitted):
        return 4
    if a.no_shrink:
        spec = without_shrink(spec)
    _, ok = _lint_gate(path, spec, a.lint_warn)
    if not ok:
        return 2
    results = judge(path, spec, a.timeout)
    v = overall(results)
    cert = certificate(path, a.spec, spec, results, v)
    if a.json:
        out = dict(cert)
        fails = [r for r in results if r.verdict != "pass"]
        if fails:
            out["counterexample"] = {"error": fails[0].error, "trace": fails[0].trace}
        print(json.dumps(out, indent=2, ensure_ascii=False))
    else:
        print(f"spec: {a.spec} — {spec['statement']}")
        for r in results:
            sc = " ".join(f"{k}={v}" for k, v in r.scenario.items())
            extra = f"{r.executions} executions" if r.verdict == "pass" else r.error
            print(f"  [{r.verdict.upper():>12}] {sc:<20} {extra}  ({r.seconds:.2f}s)")
        print(f"VERDICT: {v}")
        bad = next((r for r in results if r.verdict == "fail"), None)
        if bad and a.trace:
            print("\ncounterexample:\n" + bad.trace)
    if a.cert:
        Path(a.cert).write_text(json.dumps(cert, indent=2))
    return {"PASS": 0, "FAIL": 1}.get(v, 3)


# ----------------------------------------------------------------------------
# Hardware cost model (rough relative units per executed operation)
# ----------------------------------------------------------------------------
# x86-64 (TSO): plain loads/stores already give acquire/release; RMWs are locked
#   instructions whatever the ordering; a seq_cst store or fence needs a full barrier.
# AArch64 (ARMv8.0, LL/SC): ldar/stlr cost a little more than ldr/str; ldaxr/stlxr
#   loops for RMWs; dmb ishld for an acquire fence, dmb ish for the others.
# POWER: acquire = load + ctrl dep + isync; release = lwsync before; seq_cst = sync;
#   RMWs are larx/stcx loops with those barriers around them.
# Deliberately coarse: they rank candidates, they do not predict cycles.
_ALL = ["relaxed", "consume", "acquire", "release", "acq_rel", "seq_cst"]
COST = {
    "x86-64": {
        "load": dict.fromkeys(_ALL, 0),
        "store": {**dict.fromkeys(_ALL, 0), "seq_cst": 20},
        "rmw": dict.fromkeys(_ALL, 10),
        "cas_success": dict.fromkeys(_ALL, 10),
        "cas_failure": dict.fromkeys(_ALL, 0),
        "fence": {**dict.fromkeys(_ALL, 0), "seq_cst": 20},
    },
    "aarch64": {
        "load": {**dict.fromkeys(_ALL, 1), "relaxed": 0},
        "store": {**dict.fromkeys(_ALL, 1), "relaxed": 0},
        "rmw": {"relaxed": 2, "consume": 3, "acquire": 3, "release": 3, "acq_rel": 4, "seq_cst": 4},
        "cas_success": {"relaxed": 2, "consume": 3, "acquire": 3, "release": 3, "acq_rel": 4, "seq_cst": 4},
        "cas_failure": {**dict.fromkeys(_ALL, 1), "relaxed": 0},
        "fence": {"relaxed": 0, "consume": 4, "acquire": 4, "release": 6, "acq_rel": 6, "seq_cst": 6},
    },
    "power": {
        "load": {"relaxed": 0, "consume": 2, "acquire": 2, "release": 2, "acq_rel": 2, "seq_cst": 10},
        "store": {"relaxed": 0, "consume": 3, "acquire": 3, "release": 3, "acq_rel": 3, "seq_cst": 8},
        "rmw": {"relaxed": 3, "consume": 5, "acquire": 5, "release": 6, "acq_rel": 8, "seq_cst": 11},
        "cas_success": {"relaxed": 3, "consume": 5, "acquire": 5, "release": 6, "acq_rel": 8, "seq_cst": 11},
        "cas_failure": {"relaxed": 0, "consume": 2, "acquire": 2, "release": 2, "acq_rel": 2, "seq_cst": 2},
        "fence": {"relaxed": 0, "consume": 3, "acquire": 3, "release": 3, "acq_rel": 3, "seq_cst": 8},
    },
}
TARGETS = tuple(COST)


def hw_cost(sites: list[Site], orders: dict[int, str], weights: dict[int, int], arch: str) -> float:
    """Sum over sites of (cost of its ordering on arch) x (how often its line ran)."""
    return sum(COST[arch][s.op][orders.get(s.id, s.order)] * weights.get(s.line, 1) for s in sites)


FENCE_MARK = "/*mocheck-fence*/"


def fenceize(path: Path, spec: dict, sites: list[Site]) -> bytes | None:
    """Make a fence-based solution reachable. A fence only pays off when one fence does
    the work of several ordered accesses, so look for runs of >= 2 consecutive plain
    statements (same block) that each hold non-relaxed loads (or each hold non-relaxed
    stores):
      loads : all become relaxed, one fence of the same strength goes after the run;
      stores: all but the last become relaxed, one fence goes before the run. The last
              store keeps its ordering: a fence before the run orders what came before
              the run, not the run's own stores among themselves.
    The judge verifies the result; this is only a starting point for the search."""
    src = path.read_bytes()
    work = Path(tempfile.mkdtemp(prefix="mocheck-fz-"))
    try:
        main_f = work / MAIN_NAME
        main_f.write_bytes(src + b"\n" + (spec.get("instantiate") or "").encode() + b"\n")
        p = subprocess.run([CLANGXX, "-x", "c++", "-std=c++20", "-fsyntax-only", f"-I{path.parent}",
                            *_prelude_args(work, spec.get("prelude")), "-Xclang", "-ast-dump=json", str(main_f)],
                           capture_output=True, text=True, timeout=300)
        ast = json.loads(p.stdout)
    finally:
        shutil.rmtree(work, ignore_errors=True)

    SIMPLE = {"DeclStmt", "BinaryOperator", "CompoundAssignOperator", "CXXMemberCallExpr", "CallExpr",
              "ExprWithCleanups", "CXXOperatorCallExpr"}
    blocks: dict[tuple, list] = {}  # compound-stmt span -> [(stmt_start, stmt_end) or None, ...]
    cur_file = [None]

    def track(n):
        for l in (n.get("loc"), (n.get("range") or {}).get("begin"), (n.get("range") or {}).get("end")):
            if isinstance(l, dict):
                for sub in (l.get("spellingLoc"), l.get("expansionLoc"), l):
                    if isinstance(sub, dict) and "file" in sub:
                        cur_file[0] = sub["file"]

    def visit(n):
        track(n)
        if n.get("kind") == "CompoundStmt" and cur_file[0] and cur_file[0].endswith(MAIN_NAME):
            key = _range(n)
            stmts = []
            for ch in n.get("inner", []):
                track(ch)
                rg = _range(ch)
                span = None
                if ch.get("kind") in SIMPLE and rg and rg[1] <= len(src):
                    semi = src.find(b";", rg[1] - 1)
                    if semi != -1 and src[rg[1]:semi].strip() == b"":
                        span = (rg[0], semi + 1)
                stmts.append(span)
            if key and key not in blocks:
                blocks[key] = stmts
        for ch in n.get("inner", []):
            visit(ch)

    visit(ast)

    def sites_in(span):
        return [st for st in sites if span[0] <= st.start and st.end <= span[1]]

    edits = []  # (position, delete_len, text)

    def indent_at(pos):
        ls = src.rfind(b"\n", 0, pos) + 1
        ind = src[ls:pos].decode()
        return ind if ind.strip() == "" else ""

    for stmts in blocks.values():
        runs, cur_kind, cur = [], None, []
        for span in stmts + [None]:
            kind = None
            if span:
                ss = sites_in(span)
                strong = [x for x in ss if x.order != "relaxed"]
                if ss and all(x.op == "load" for x in ss) and strong:
                    kind = "load"
                elif ss and all(x.op == "store" for x in ss) and strong:
                    kind = "store"
            if kind and kind == cur_kind:
                cur.append(span)
            else:
                if cur_kind and len([x for sp in cur for x in sites_in(sp) if x.order != "relaxed"]) >= 2:
                    runs.append((cur_kind, cur))
                cur_kind, cur = kind, ([span] if kind else [])
        for kind, run in runs:
            rs = [x for sp in run for x in sites_in(sp) if x.order != "relaxed"]
            if kind == "store":
                rs = rs[:-1]  # the last store keeps its own ordering
            if not rs:
                continue
            strong = "seq_cst" if any(x.order == "seq_cst" for x in rs) else ("acquire" if kind == "load" else "release")
            fence = f"std::atomic_thread_fence(std::memory_order_{strong}); {FENCE_MARK}"
            for x in rs:
                edits.append((x.start, x.end - x.start, "std::memory_order_relaxed"))
            if kind == "load":
                edits.append((run[-1][1], 0, "\n" + indent_at(run[-1][0]) + fence))
            else:
                edits.append((run[0][0], 0, fence + "\n" + indent_at(run[0][0])))
    if not edits:
        return None
    out = src
    for pos, dl, text in sorted(edits, key=lambda e: (e[0], -e[1]), reverse=True):
        out = out[:pos] + text.encode() + out[pos + dl:]
    return out


def clean_fences(src: bytes) -> bytes:
    """Drop inserted fences that the search relaxed away, and the markers of the rest."""
    s = src.decode()
    s = re.sub(r"\n[ \t]*std::atomic_thread_fence\(std::memory_order_relaxed\); " + re.escape(FENCE_MARK), "", s)
    s = re.sub(r"[ \t]*std::atomic_thread_fence\(std::memory_order_relaxed\); " + re.escape(FENCE_MARK) + r"\n", "", s)
    return s.replace(" " + FENCE_MARK, "").encode()


def search_weakest(src: bytes, inc_dir: Path, name: str, spec: dict, sites: list[Site], prefer: str,
                   timeout: float, cand_timeout: float, log) -> tuple[dict | None, dict, int]:
    """Greedy weakest-ordering search. prefer: 'lattice' (source order), 'accesses'
    (try to weaken fences first, pushing synchronization onto the accesses) or 'fences'
    (weaken accesses first, keep synchronization in fences)."""
    work = Path(tempfile.mkdtemp(prefix="mocheck-"))
    for f in inc_dir.iterdir():
        if f.is_file() and f.suffix in (".h", ".hpp", ".hh", ".hxx", ".inc"):
            shutil.copy(f, work / f.name)
    cand = work / name
    runs = 0

    def verdict(orders, t):
        nonlocal runs
        runs += 1
        cand.write_bytes(rewrite(src, sites, orders))
        return overall(judge(cand, spec, t))

    current = {s.id: s.order for s in sites}
    unsure: dict[int, list[str]] = {}
    try:
        if verdict(current, timeout) != "PASS":
            return None, unsure, runs

        def group_of(s):
            obj = s.call.rsplit(".", 1)[0] if "." in s.call else s.call
            return re.sub(r"\[[^\]]*\]", "", re.split(r"\.|->", obj)[-1])

        fence_rank = {"lattice": lambda s: 0, "accesses": lambda s: 0 if s.op == "fence" else 1,
                      "fences": lambda s: 1 if s.op == "fence" else 0}[prefer]
        ordered = sorted(sites, key=lambda s: (fence_rank(s), s.start))
        groups: dict[str, list[Site]] = {}
        for st in ordered:
            groups.setdefault(group_of(st), []).append(st)
        AR = {"load": "acquire", "store": "release", "rmw": "acq_rel", "cas_success": "acq_rel",
              "cas_failure": "acquire", "fence": "acq_rel"}

        def site_pass(subset):
            changed = False
            for st in subset:
                cur = current[st.id]
                for o in [o for o in VALID[st.op] if o in _WEAKER.get(cur, set())]:  # weakest first
                    trial = dict(current)
                    trial[st.id] = o
                    t1 = time.time()
                    v = verdict(trial, cand_timeout)
                    log(f"  try line {st.line:<4} {st.call:<28} {cur:>8} -> {o:<8} {v:<12} {time.time() - t1:5.1f}s")
                    if v == "PASS":
                        current[st.id] = o
                        unsure.pop(st.id, None)
                        changed = True
                        break
                    if v == "INCONCLUSIVE":
                        unsure.setdefault(st.id, []).append(o)
            return changed

        def group_pass(only=None, relaxed_only=False):
            changed, stuck = False, set()
            moves = (("relaxed", lambda x: "relaxed"),) if relaxed_only else \
                (("relaxed", lambda x: "relaxed"), ("acq/rel", lambda x: AR[x.op]))
            for g, members in groups.items():
                if only is not None and g not in only:
                    continue
                for label, target in moves:
                    trial = dict(current)
                    for st in members:
                        t = target(st)
                        if t in _WEAKER.get(current[st.id], set()):
                            trial[st.id] = t
                    if trial == current:
                        continue
                    t1 = time.time()
                    v = verdict(trial, cand_timeout)
                    log(f"  try group '{g}' ({len(members)} sites) -> all {label:<8} {v:<12} {time.time() - t1:5.1f}s")
                    if v == "PASS":
                        current.update(trial)
                        for st in members:
                            unsure.pop(st.id, None)
                        changed = True
                        break
                    if v == "INCONCLUSIVE":
                        stuck.add(g)
            return changed, stuck

        def converge(subset, relaxed_only=False):
            names = {group_of(st) for st in subset}
            stuck = set()
            while True:
                g_changed, stuck = group_pass(names, relaxed_only)
                s_changed = site_pass([st for st in subset if group_of(st) not in stuck])
                if not (g_changed or s_changed):
                    break
            return stuck

        fences = [st for st in ordered if st.op == "fence"]
        accesses = [st for st in ordered if st.op != "fence"]
        if prefer == "fences":
            # weaken every access as far as it goes while the fences still carry the
            # synchronization (whole-variable moves only to relaxed: an acq_rel group
            # move would make the fences redundant), and only then weaken the fences
            converge(accesses, relaxed_only=True)
            converge(fences)
        elif prefer == "accesses":
            converge(fences)  # drop or weaken fences first, then pay for it on the accesses
            converge(accesses)
        stuck = converge(ordered)  # final pass over everything: local minimality
        if stuck:
            site_pass([st for st in ordered if group_of(st) in stuck])
        return current, unsure, runs
    finally:
        shutil.rmtree(work, ignore_errors=True)


def cmd_minimize(a) -> int:
    path = Path(a.impl).resolve()
    spec = load_spec(a.spec)
    if not _require_admission(spec, a.allow_unadmitted):
        return 4
    if a.no_shrink:
        spec = without_shrink(spec)
    sites, ok = _lint_gate(path, spec, a.lint_warn)
    if not ok:
        return 2
    src = path.read_bytes()

    def log(msg):
        if not a.quiet:
            print(msg, file=sys.stderr, flush=True)

    t0 = time.time()
    out = Path(a.out) if a.out else path.with_name(path.stem + ".min" + path.suffix)
    plans = {"lattice": ["lattice"], "accesses": ["accesses"], "fences": ["fences"],
             "auto": ["accesses", "fences"]}[a.prefer]
    results = []  # (label, final_src, sites, orders, unsure, runs)
    bsites_src = {}  # label -> source the sites/orders refer to (fence-rewritten or original)
    for prefer in plans:
        base_src, base_sites = src, sites
        if prefer == "fences":
            fz = fenceize(path, spec, sites)
            if fz is not None:
                tmp = Path(tempfile.mkdtemp(prefix="mocheck-fzs-")) / path.name
                tmp.write_bytes(fz)
                fsites, ffind, perr = analyze(tmp, spec.get("instantiate"), spec.get("prelude"))
                if perr or any(f.severity == "error" for f in ffind):
                    log("  fence rewriting produced code that does not pass lint; skipping the fence-style search")
                    continue
                base_src, base_sites = fz, fsites
        log(f"── search preferring {prefer}")
        orders, unsure, runs = search_weakest(base_src, path.parent, path.name, spec, base_sites, prefer,
                                              a.timeout, a.candidate_timeout, log)
        if orders is None:
            if prefer == "fences" and base_src is not src:
                log("  the fence-rewritten starting point does not verify; skipping the fence-style search")
                continue
            print("the original implementation does not verify; run `mocheck verify` first.")
            return 1
        bsites_src[prefer] = rewrite(base_src, base_sites, orders)
        final = clean_fences(bsites_src[prefer])
        if a.prefer != "lattice":  # a relaxed fence is a no-op: drop it from the output
            final = re.sub(rb"\n[ \t]*std::atomic_thread_fence\(std::memory_order_relaxed\);[ \t]*(?=\n)", b"", final)
        results.append((prefer, final, base_sites, orders, unsure, runs))

    if not results:
        print("no candidate could be produced")
        return 1

    # Weight hardware cost by how often each line runs. Profile the ORIGINAL code once and
    # map every candidate onto it: native timing (spin loops!) is noisy, and profiling each
    # candidate separately would let that noise decide between them.
    weights = line_profile(path, spec) if a.prefer != "lattice" or a.cost else {}

    def weight_of(base_src: bytes, line: int) -> float:
        lines = base_src.decode(errors="replace").split("\n")
        orig, mapping = 0, {}
        for i, l in enumerate(lines, 1):
            if FENCE_MARK in l:
                mapping[i] = None
            else:
                orig += 1
                mapping[i] = orig
        if not weights:
            return 1.0
        if mapping.get(line):
            return float(weights.get(mapping[line], 0))
        prev = next((mapping[j] for j in range(line - 1, 0, -1) if mapping.get(j)), None)
        nxt = next((mapping[j] for j in range(line + 1, len(lines) + 1) if mapping.get(j)), None)
        ws = [weights.get(x, 0) for x in (prev, nxt) if x]
        return float(sum(ws) / len(ws)) if ws else 1.0

    scored = []
    for prefer, final, bsites, orders, unsure, runs in results:
        base = bsites_src[prefer]
        costs = {arch: sum(COST[arch][st.op][orders[st.id]] * weight_of(base, st.line) for st in bsites)
                 for arch in TARGETS}
        if prefer == "accesses" or "orig_costs" not in locals():
            orig_costs = {arch: sum(COST[arch][st.op][st.order] * weight_of(base, st.line) for st in bsites)
                          for arch in TARGETS}
        scored.append((prefer, final, bsites, orders, unsure, runs, costs, bool(weights)))

    TIE = 0.05  # within 5%: call it a tie and keep the simpler, fence-free code

    def cheaper(arch):
        lo = min(r[6][arch] for r in scored)
        near = [r for r in scored if r[6][arch] <= lo * (1 + TIE) + 1e-9]
        return near, len(near) > 1

    print("   input cost: " + ", ".join(f"{k} {v:.0f}" for k, v in orig_costs.items()) + "\n")
    near, _ = cheaper(a.target)
    best = next((r for r in near if r[0] == "accesses"), near[0])
    dt = time.time() - t0
    for prefer, final, bsites, orders, unsure, runs, costs, weighted in scored:
        tag = "  ← chosen for " + a.target if (prefer is best[0] and len(scored) > 1) else ""
        print(f"── {prefer}-preferring result   judge runs: {runs}{tag}")
        print(f"{'line':>5}  {'call':<34} {'op':<12} {'before':<8} -> after")
        for st in bsites:
            mark = "" if orders[st.id] == st.order else "   weakened"
            if st.id in unsure:
                mark += f"   (untested: {'/'.join(unsure[st.id])} timed out)"
            after = orders[st.id] + ("  (fence removable)" if st.op == "fence" and orders[st.id] == "relaxed" else "")
            print(f"{st.line:>5}  {st.call:<34} {st.op:<12} {st.order:<8} -> {after}{mark}")
        cw = "execution-weighted" if weighted else "static"
        print(f"   estimated cost ({cw}): " + ", ".join(f"{k} {v:.0f}" for k, v in costs.items()) + "\n")

    if len(scored) > 1:
        wins = []
        for arch in TARGETS:
            near_a, tie = cheaper(arch)
            wins.append(f"{arch} → {'tie' if tie else near_a[0][0]}")
        print("cheapest per architecture: " + ", ".join(wins))
    out.write_bytes(best[1])
    print(f"written: {out}" + (f"   (preferring {best[0]}, cheapest on {a.target})" if len(scored) > 1 else ""))
    for prefer, final, *_ in scored:
        if prefer != best[0]:
            alt = out.with_name(out.stem + f".{prefer}" + out.suffix)
            alt.write_bytes(final)
            print(f"alternative: {alt}")
    if any(r[4] for r in scored):
        print("some weaker candidates timed out: those sites are minimal only among the candidates that finished.")
    else:
        print("each remaining ordering is locally minimal: weakening any single one makes some scenario fail.")
    print(f"time: {dt:.1f}s")
    return 0


@dataclass
class Mutant:
    line: int
    col: int
    kind: str
    desc: str
    start: int
    end: int
    repl: str
    verdict: str = ""
    note: str = ""

    @property
    def key(self) -> str:
        return f"L{self.line}:C{self.col} {self.kind} {self.desc}"


REL_SWAP = {"==": "!=", "!=": "==", "<": "<=", "<=": "<", ">": ">=", ">=": ">"}
ARITH_SWAP = {"+": "-", "-": "+"}


def mutation_points(path: Path, instantiate: str | None, sites: list[Site], prelude: str | None = None) -> list[Mutant]:
    """Small, single-point code mutations of the implementation (plus every single-site order weakening)."""
    src = path.read_bytes()
    work = Path(tempfile.mkdtemp(prefix="mocheck-mut-"))
    main_f = work / MAIN_NAME
    main_f.write_bytes(src + b"\n" + (instantiate or "").encode() + b"\n")
    p = subprocess.run([CLANGXX, "-x", "c++", "-std=c++20", "-fsyntax-only", f"-I{path.parent}",
                        *_prelude_args(work, prelude), "-Xclang", "-ast-dump=json", str(main_f)],
                       capture_output=True, text=True, timeout=300)
    shutil.rmtree(work, ignore_errors=True)
    ast = json.loads(p.stdout)
    line_starts = [0] + [i + 1 for i, b in enumerate(src) if b == 0x0A]

    def line_of(off):
        import bisect
        return bisect.bisect_right(line_starts, off)

    def col_of(off):
        return off - line_starts[line_of(off) - 1] + 1

    out: dict[tuple, Mutant] = {}
    site_ranges = [(s.start, s.end) for s in sites]

    def inside_site(a, b):
        return any(s <= a and b <= e for s, e in site_ranges)

    def add(kind, desc, a, b, repl):
        if b > len(src) or inside_site(a, b):
            return
        out.setdefault((a, b, repl), Mutant(line_of(a), col_of(a), kind, desc, a, b, repl))

    cur_file = [None]

    def track(n):
        for l in (n.get("loc"), (n.get("range") or {}).get("begin"), (n.get("range") or {}).get("end")):
            if isinstance(l, dict):
                for sub in (l.get("spellingLoc"), l.get("expansionLoc"), l):
                    if isinstance(sub, dict) and "file" in sub:
                        cur_file[0] = sub["file"]

    def visit(n):
        track(n)
        in_main = cur_file[0] is not None and cur_file[0].endswith(MAIN_NAME)
        k, inner, rg = n.get("kind"), n.get("inner", []), _range(n)
        if in_main and rg and rg[1] <= len(src):
            if k == "BinaryOperator" and len(inner) == 2 and n.get("opcode") in (REL_SWAP | ARITH_SWAP):
                l_rg, r_rg = _range(inner[0]), _range(inner[1])
                if l_rg and r_rg:
                    gap = src[l_rg[1]:r_rg[0]].decode(errors="replace")
                    op = n["opcode"]
                    i = gap.find(op)
                    if i >= 0:
                        a = l_rg[1] + i
                        new = REL_SWAP.get(op) or ARITH_SWAP.get(op)
                        add("operator", f"'{op}' -> '{new}'", a, a + len(op), new)
            elif k == "IntegerLiteral":
                v = int(n.get("value", "0"))
                txt = src[rg[0]:rg[1]].decode(errors="replace")
                if txt.isdigit():
                    add("constant", f"{v} -> {v + 1}", rg[0], rg[1], str(v + 1))
                    if v > 0:
                        add("constant", f"{v} -> {v - 1}", rg[0], rg[1], str(v - 1))
            elif k == "CXXBoolLiteralExpr":
                v = n.get("value")
                add("constant", f"{str(v).lower()} -> {str(not v).lower()}", rg[0], rg[1], str(not v).lower())
        for ch in inner:
            visit(ch)

    visit(ast)
    muts = sorted(out.values(), key=lambda m: (m.start, m.repl))
    for s in sites:
        for o in VALID[s.op]:
            if o in _WEAKER.get(s.order, set()):
                muts.append(Mutant(s.line, s.col, "ordering", f"{s.call}: {s.order} -> {o}",
                                   s.start, s.end, f"std::memory_order_{o}"))
    return muts


VERDICT_OF = {"FAIL": "killed", "PASS": "SURVIVED", "INCONCLUSIVE": "unknown", "COMPILE-ERROR": "stillborn"}


def run_mutants(path: Path, spec: dict, sites, cand_timeout: float, quiet: bool, max_n: int = 0) -> list[Mutant]:
    src = path.read_bytes()
    muts = mutation_points(path, spec.get("instantiate"), sites, spec.get("prelude"))
    if max_n and len(muts) > max_n:
        muts = muts[:max_n]
    work = Path(tempfile.mkdtemp(prefix="mocheck-"))
    cand = work / path.name
    for i, m in enumerate(muts):
        cand.write_bytes(src[:m.start] + m.repl.encode() + src[m.end:])
        _, _, perr = analyze(cand, spec.get("instantiate"), spec.get("prelude"))
        m.verdict = "stillborn" if perr else VERDICT_OF[overall(judge(cand, spec, cand_timeout))]
        if not quiet:
            print(f"  [{i + 1:>3}/{len(muts)}] {m.key:<56} {m.verdict}", file=sys.stderr, flush=True)
    shutil.rmtree(work, ignore_errors=True)
    return muts


def _score_line(muts):
    killed = sum(m.verdict in ("killed", "killed-by-challenge") for m in muts)
    surv = sum(m.verdict == "SURVIVED" for m in muts)
    unk = sum(m.verdict == "unknown" for m in muts)
    still = sum(m.verdict == "stillborn" for m in muts)
    scored = killed + surv
    return (f"killed {killed}, survived {surv}, unknown {unk}, stillborn {still}"
            + (f"   mutation score {killed / scored:.0%}" if scored else ""))


def cmd_mutate(a) -> int:
    path = Path(a.impl).resolve()
    spec = load_spec(a.spec)
    sites, ok = _lint_gate(path, spec)
    if not ok:
        return 2
    if overall(judge(path, spec, a.timeout)) != "PASS":
        print("the reference implementation does not verify; mutation testing needs a passing reference.")
        return 1
    t0 = time.time()
    muts = run_mutants(path, spec, sites, a.candidate_timeout, a.quiet, a.max)
    print(f"spec: {a.spec}   reference: {path.name}   mutants: {len(muts)}   time: {time.time() - t0:.0f}s")
    print(_score_line(muts))
    surv = [m for m in muts if m.verdict == "SURVIVED"]
    if surv:
        print("\nsurvivors (each is either an equivalent mutant or a hole in the spec):")
        for m in surv:
            print(f"  {m.key}")
    return 0


def cmd_admit(a) -> int:
    """Admit a spec only if, on each reference implementation, every single-point mutant is
    killed by the spec's scenarios, or is annotated by a human as equivalent/undecided.
    Mutants that survive the spec but die under the (bigger) challenge scenarios mean the
    spec's scenarios are too weak: the spec is rejected with the scenario to promote."""
    spec = load_spec(a.spec)
    refs = spec.get("references", [])
    adm_file = spec["_dir"] / "admission.json"
    if not refs:
        print(f"REJECTED: spec '{a.spec}' lists no reference implementations to test it against.")
        adm_file.unlink(missing_ok=True)
        return 1
    notes = {(n["ref"], n["mutant"]): n for n in spec.get("survivor_annotations", [])}
    challenge = dict(spec, scenarios=spec.get("challenge_scenarios", []))
    problems, report, used_notes = [], {}, set()
    t0 = time.time()
    for ref in refs:
        path = (spec["_dir"] / ref).resolve()
        print(f"── reference {ref}")
        sites, ok = _lint_gate(path, spec)
        if not ok:
            problems.append(f"{ref}: reference fails lint")
            continue
        base = overall(judge(path, spec, a.timeout))
        if base != "PASS":
            problems.append(f"{ref}: reference does not verify ({base})")
            continue
        muts = run_mutants(path, spec, sites, a.candidate_timeout, a.quiet)
        for m in muts:
            if m.verdict not in ("SURVIVED", "unknown"):
                continue
            if challenge["scenarios"]:
                src = path.read_bytes()
                work = Path(tempfile.mkdtemp(prefix="mocheck-ch-"))
                cand = work / path.name
                cand.write_bytes(src[:m.start] + m.repl.encode() + src[m.end:])
                rs = judge(cand, challenge, a.candidate_timeout)
                shutil.rmtree(work, ignore_errors=True)
                bad = next((r for r in rs if r.verdict == "fail"), None)
                if bad:
                    m.verdict, m.note = "killed-by-challenge", " ".join(f"{k}={v}" for k, v in bad.scenario.items())
                    problems.append(f"{ref}: {m.key} survives the spec's scenarios but is killed by challenge "
                                    f"scenario {{{m.note}}} — promote that scenario (or a cheaper one that kills it) "
                                    f"into \"scenarios\"")
                    continue
            n = notes.get((ref, m.key))
            want = "equivalent" if m.verdict == "SURVIVED" else "undecided"
            # 'outside-spec': behaviour does differ, but only in a way the spec deliberately
            # does not constrain (must be justified, and is shown in the admission record)
            # 'blind-spot': a real defect that none of the checkers can observe (documented, not hidden)
            allowed = (want, "outside-spec", "blind-spot") if want == "equivalent" else (want,)
            if n and n.get("status") in allowed and n.get("why"):
                used_notes.add((ref, m.key))
                m.note = f"{n['status']}: {n['why']}"
            else:
                problems.append(f"{ref}: {m.key} {'survives' if want == 'equivalent' else 'cannot be decided'} "
                                f"and has no '{want}' annotation with a reason in survivor_annotations")
        print("   " + _score_line(muts))
        for m in muts:
            if m.note:
                print(f"   {m.verdict:<20} {m.key}\n   {'':<20} └ {m.note}")
        report[ref] = {"mutants": len(muts), "summary": _score_line(muts),
                       "survivors": [{"mutant": m.key, "verdict": m.verdict, "note": m.note}
                                     for m in muts if m.note]}
    stale = [k for k in notes if k not in used_notes]
    verdict = "REJECTED" if problems else "ADMITTED"
    print(f"\n{verdict}: spec '{a.spec}'   ({time.time() - t0:.0f}s)")
    for p_ in problems:
        print(f"  ✗ {p_}")
    for r, k in stale:
        print(f"  ! stale annotation (mutant no longer survives or no longer exists): {r}: {k}")
    if verdict == "ADMITTED":
        adm_file.write_text(json.dumps({
            "verdict": verdict, "spec": a.spec, "spec_digest": spec_digest(spec),
            "admitted_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "checker": _genmc_version(),
            "memory_model": MODEL.lstrip("-").upper(), "references": report,
        }, indent=2, ensure_ascii=False))
        print(f"  admission record: {adm_file.relative_to(HERE)}")
    else:
        adm_file.unlink(missing_ok=True)
    return 0 if verdict == "ADMITTED" else 1


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="mocheck", description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("lint", help="reject implicit seq_cst and invalid orderings")
    pl.add_argument("impl")
    pl.add_argument("--json", action="store_true")
    pl.add_argument("--spec", help="instantiate templates the way this spec does, so dependent code is checked")
    pl.set_defaults(fn=cmd_lint)

    for name, fn, h in (("verify", cmd_verify, "judge an implementation against a spec"),
                        ("minimize", cmd_minimize, "search for the weakest orderings that still verify")):
        sp = sub.add_parser(name, help=h)
        sp.add_argument("impl")
        sp.add_argument("--spec", required=True)
        sp.add_argument("--timeout", type=float, default=60.0, help="seconds per scenario")
        sp.add_argument("--allow-unadmitted", action="store_true",
                        help="judge against a spec that has not passed `mocheck admit` (result is marked as such)")
        sp.add_argument("--no-shrink", action="store_true",
                        help="audit mode for third-party code without the spec's shrinkable parameters: skip the "
                             "shrunk scenarios (recorded in the certificate)")
        sp.add_argument("--lint-warn", action="store_true",
                        help="audit mode: report lint errors as warnings and judge anyway (recorded in the certificate)")
        if name == "verify":
            sp.add_argument("--json", action="store_true")
            sp.add_argument("--trace", action="store_true", help="print the counterexample execution")
            sp.add_argument("--cert", help="write a JSON certificate here")
        else:
            sp.add_argument("--out", help="where to write the minimized source")
            sp.add_argument("--candidate-timeout", type=float, default=30.0,
                            help="seconds per scenario when judging a weakened candidate")
            sp.add_argument("--quiet", action="store_true")
            sp.add_argument("--prefer", choices=["lattice", "accesses", "fences", "auto"], default="lattice",
                            help="search order: lattice (source order), accesses (drop fences first), fences "
                                 "(keep synchronization in fences, rewriting accesses if needed), auto (both, "
                                 "pick the cheaper on --target)")
            sp.add_argument("--target", choices=list(TARGETS), default="aarch64",
                            help="architecture whose cost model picks the result in --prefer auto")
            sp.add_argument("--cost", action="store_true", help="also report execution-weighted cost for lattice")
        sp.set_defaults(fn=fn)

    pm = sub.add_parser("mutate", help="score a spec: how many small breakages of a correct reference does it catch?")
    pm.add_argument("impl", help="a reference implementation that passes the spec")
    pm.add_argument("--spec", required=True)
    pm.add_argument("--timeout", type=float, default=60.0)
    pm.add_argument("--candidate-timeout", type=float, default=20.0)
    pm.add_argument("--max", type=int, default=0, help="cap the number of mutants")
    pm.add_argument("--quiet", action="store_true")
    pm.set_defaults(fn=cmd_mutate)

    def cmd_status(a) -> int:
        bad = 0
        for d in sorted(x for x in SPECS.iterdir() if (x / "spec.json").exists()):
            sp = load_spec(d.name)
            ok, msg = admission_status(sp)
            bad += not ok
            print(f"  {'✓' if ok else '✗'} {d.name:<22} {msg}")
        return 1 if bad else 0

    ps = sub.add_parser("status", help="show which specs are admitted and still unchanged")
    ps.set_defaults(fn=cmd_status)

    pa = sub.add_parser("admit", help="admit a spec: its scenarios must kill every non-equivalent mutant of its references")
    pa.add_argument("spec")
    pa.add_argument("--timeout", type=float, default=60.0)
    pa.add_argument("--candidate-timeout", type=float, default=20.0)
    pa.add_argument("--quiet", action="store_true")
    pa.set_defaults(fn=cmd_admit)

    a = p.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
