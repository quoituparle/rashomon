# Rashomon

**Rashomon checks the memory orderings in C++ concurrent code**, so you don't have to read that code yourself. You pick a *spec* (say, "a mutex"), someone or something (often an AI) writes the implementation, and Rashomon explores every thread interleaving and every weak-memory behaviour the C++ memory model allows. The answer is one of:

| verdict | meaning | exit code |
|---|---|---|
| **PASS** | no execution breaks the spec, in any of the spec's scenarios | 0 |
| **FAIL** | here is an execution that breaks it (data race, wrong result, a thread stuck forever) | 1 |
| *refused* | the code doesn't state its memory orderings explicitly; fix that first | 2 |
| **INCONCLUSIVE** | the checker could not decide (timeout, unsupported construct). Never treat it as PASS | 3 |
| *not admitted* | the spec hasn't been validated on this machine yet (`rashomon doctor` says how) | 4 |

*Why the name:* in Kurosawa's *Rashomon* every witness tells the same events in a different order, and none of them can be taken at their word. Threads under a weak memory model do the first; the checkers that judge them can do the second. So Rashomon can use two independent checkers and calibrates both on cases whose answer is known.

## A 2-minute example

Here is a spinlock. It looks fine and passes ordinary tests on x86:

```cpp
#include <atomic>
struct Mutex {
	std::atomic<bool> locked{false};
	void lock() {
		bool expected = false;
		while (!locked.compare_exchange_weak(expected, true,
		                                     std::memory_order_acquire, std::memory_order_relaxed))
			expected = false;
	}
	void unlock() { locked.store(false, std::memory_order_relaxed); }   // <- the bug
};
```

```text
$ rashomon verify my_mutex.hpp --spec mutex
spec: mutex — T threads each do K critical sections that increment a plain int; the final count is T*K, ...
  [        FAIL] MO_T=2 MO_K=1        Non-atomic race!  (0.40s)
VERDICT: FAIL
(run again with --trace to see the counterexample execution)
```

`unlock()` uses `relaxed`, so the next thread to take the lock may not see the writes made inside the critical section: two threads touch the protected counter without synchronization. `--trace` prints the exact execution. Change `relaxed` to `release` and check again, this time with the second, independent checker too:

```text
$ rashomon verify my_mutex.hpp --spec mutex --second-judge dartagnan
  [        PASS] MO_T=2 MO_K=1        2 executions  (0.25s)
  [        PASS] MO_T=2 MO_K=2        6 executions  (0.30s)
  [        PASS] MO_T=3 MO_K=1        6 executions  (0.21s)
  [        PASS] MO_T=2 MO_K=1 check=dartagnan bound 2  (2.62s)
  ...
  [        PASS] MO_T=3 MO_K=1 check=asan+ubsan 3 executions  (0.42s)
VERDICT: PASS
```

Code that doesn't say which ordering it means is refused before any checking:

```text
$ rashomon verify lazy.hpp --spec mutex
lazy.hpp:4:23: error: [implicit-seq-cst] locked.exchange: no ordering given (defaults to seq_cst)
lazy.hpp:5:18: error: [implicit-seq-cst] locked = false: operator= on an atomic is an implicit seq_cst access — ...
```

And for correct code that is stronger than it needs to be, `minimize` finds the weakest orderings that still pass:

```text
$ rashomon minimize ticket_lock.hpp --spec mutex --out ticket_lock.min.hpp      (output abridged)
 line  call                    op      before   -> after
    5  next.fetch_add          rmw     seq_cst  -> relaxed
    6  serving.load            load    seq_cst  -> acquire
    8  atomic_thread_fence     fence   seq_cst  -> relaxed  (fence removable)
   11  serving.load            load    seq_cst  -> relaxed
   12  serving.store           store   seq_cst  -> release
```

## Install

Tested on Ubuntu 24.04 and 26.04, x86-64, including under WSL2 on Windows. You need Python ≥ 3.10, clang 19 and [GenMC](https://github.com/MPI-SWS/genmc) v0.20.0.

```bash
# 1. compilers and GenMC's build dependencies
sudo apt install -y clang-19 llvm-19-dev g++-14 libffi-dev zlib1g-dev libedit-dev cmake git python3

# 2. build GenMC into ~/genmc (about 5 minutes); Rashomon looks there by default
git clone --depth 1 --branch v0.20.0 https://github.com/MPI-SWS/genmc ~/genmc
cd ~/genmc
CC=gcc-14 CXX=g++-14 cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-19/lib/cmake -B build -S .
cmake --build build -j

# 3. get Rashomon and put its launcher on your PATH
git clone https://github.com/quoituparle/rashomon ~/rashomon
export PATH="$HOME/rashomon/bin:$PATH"        # add this line to ~/.bashrc

# 4. let it check everything and tell you what is missing
rashomon doctor
```

`rashomon doctor` compiles a test file, runs one program that must PASS and one that must FAIL, and checks the specs. For anything wrong it prints the exact command that fixes it. The two things it most often asks for:

- **`export RASHOMON_CLANG_FLAGS=--gcc-install-dir=...`** when clang can't find the C++ headers (Ubuntu 26.04 picks a GCC whose headers aren't installed). doctor finds a directory that works and prints the full line.
- **Admitting the specs** on a new machine or GenMC version (5 to 10 minutes, once): `for s in $(rashomon specs --names); do rashomon admit "$s"; done`. A spec has to prove it can catch bugs before its PASS counts; see [Spec admission](rashomon/README.md#spec-admission).

**Optional, the second checker** ([Dartagnan](https://github.com/hernanponcedeleon/Dat3M)): needs Java 25 and Maven ≥ 3.8.

```bash
git clone --branch 4.5.0 https://github.com/hernanponcedeleon/Dat3M ~/Dat3M
cd ~/Dat3M && mvn clean install -DskipTests
export DAT3M_HOME=~/Dat3M
```

## Using it

**1. Pick a spec.** `rashomon specs` lists them; `rashomon specs NAME` shows exactly what to implement:

```text
$ rashomon specs mutex
What it checks:
  T threads each do K critical sections that increment a plain int; the final count is T*K,
  critical sections never overlap (no data race), and no thread is stuck forever. ...
What you implement (put it in one header, e.g. impl.hpp):
  struct Mutex { void lock(); void unlock(); };  default-constructible, starts unlocked.
Scenarios (sizes are fixed by the spec, you cannot choose them):
  MO_T=2 MO_K=1
  MO_T=2 MO_K=2
  MO_T=3 MO_K=1
```

13 specs ship with it: `spsc_queue`, `spsc_queue_nb`, `mpmc_queue`, `mpmc_queue_relaxed`, `mutex`, `qlock` (MCS lock), `seqlock`, `once`, `latch`, `refcount`, `refcount_saturating`, `freelist` (ABA-safe Treiber stack), `ebr` (epoch-based reclamation). The [spec table](rashomon/README.md#specs-shipped) describes each one.

**2. Write the implementation** in one header file that provides the contract. The rules:

- Every atomic operation states its memory order (`x.load(std::memory_order_acquire)`, not `x.load()`, `x = 1` or `x++`). `rashomon lint impl.hpp --spec NAME` checks this on its own.
- `compare_exchange_weak` only inside a retry loop; use `_strong` for a one-shot attempt.
- If the contract has a width parameter such as `BITS`, really use it for the counter: one scenario sets it to 2 or 3 bits to expose wrap-around bugs that would otherwise need billions of operations.
- No inline assembly (it can't be checked, so the result would be INCONCLUSIVE).

**3. Verify.**

| command | what for |
|---|---|
| `rashomon verify impl.hpp --spec NAME` | the verdict (GenMC, then an ASan/UBSan run) |
| `... --trace` | print the failing execution |
| `... --second-judge dartagnan` | also require the independent checker to agree (slower, about 3 to 50 s per scenario) |
| `... --cert cert.json` | write a certificate: code hash, spec hash, checker versions, per-scenario results |
| `... --json` | machine-readable result |
| `rashomon minimize impl.hpp --spec NAME --out min.hpp` | weakest orderings that still verify |

**In CI**, gate on the exit code: anything but 0 fails the build, INCONCLUSIVE included.

```bash
rashomon verify src/ring_buffer.hpp --spec spsc_queue --second-judge dartagnan --cert rashomon-cert.json
```

**With an AI coding assistant**, give it the output of `rashomon specs NAME` as the task ("implement this contract in one header; every atomic operation must name its memory order"), run `verify`, and paste any FAIL with its `--trace` back to it. You review the spec once, not each version of the code.

## What a PASS does and doesn't mean

- Every execution allowed by the RC11 memory model (the C++20 memory model, slightly strengthened to rule out "out-of-thin-air" values) was explored **for the spec's scenarios**: 2 to 4 threads, a few operations each. It is not a proof for every size, but the specs are built so typical bugs show up at these sizes, including wrap-around bugs through the tiny-width scenario.
- The spec is only as good as what it states. Read the spec's statement: that is the part a human is meant to read.
- Known limits are written down: [what a PASS means](rashomon/README.md#what-a-pass-means-exactly), [known blind spots](rashomon/README.md#known-blind-spots), and where the [second checker](rashomon/README.md#second-judge-dartagnan) can't help (the MPMC specs are too large for it and come back INCONCLUSIVE).

## How it keeps itself honest

- **Spec admission:** before a spec may judge anything, it must catch every small breakage (mutant) of its correct reference implementations.
- **Two independent checkers** (GenMC and Dartagnan), plus a native AddressSanitizer run. PASS needs all of them.
- **Litmus calibration:** 32 programs with known answers, run through the real pipeline on both checkers (`rashomon/tools/litmus.py`). This is how the checkers' own bugs were found; see the [reference](rashomon/README.md#litmus-calibration).
- **Regression:** `rashomon/run_examples.sh`, 80 checks, about 8 minutes.

It has also been used on real libraries (rigtorp's SPSC/MPMC queues and Seqlock, atomic_queue, readerwriterqueue); results and what they mean are in the reference's [audit section](rashomon/README.md#auditing-third-party-libraries), the adapters in [`realworld/`](realworld/).

## Repository layout

```
bin/rashomon            launcher (put bin/ on PATH)
rashomon/rashomon.py    the tool, a single Python file
rashomon/specs/         the 13 specs: spec.json (what a human reads), harness.cpp, reference implementations
rashomon/examples/      correct and broken implementations with their expected verdicts
rashomon/litmus/        calibration programs with known answers
rashomon/tools/         litmus.py (calibration), dartagnan_only.py
rashomon/README.md      the full reference: every design decision and every checker quirk found
realworld/              audits of real open-source libraries
HANDOFF.md              project status and next steps (in Chinese)
```

## Configuration

| variable | default | purpose |
|---|---|---|
| `GENMC` | `genmc` on PATH, else `~/genmc/build/bin/genmc` | GenMC binary |
| `CLANGXX` | `clang++-19` | compiler (must match GenMC's LLVM version) |
| `RASHOMON_CLANG_FLAGS` | none | extra compiler flags, e.g. `--gcc-install-dir=...` |
| `DAT3M_HOME` | `~/Dat3M` | Dartagnan, for `--second-judge dartagnan` |
| `RASHOMON_DARTAGNAN_MEM_MB` | 4096 | Dartagnan is stopped above this (its solver can use a lot of memory); lower it when running several in parallel |
