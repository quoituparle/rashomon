# Rashomon

A memory-order judge for C++ concurrent code written by someone (or something) you don't want to have to read.
The human reads the **spec**. The tool reads the code.

This is the **reference**: how it works, why, and every quirk found along the way. To install and get started, read the [top-level README](../README.md) first.

```
rashomon doctor                            # check the installation, print the fix for anything missing
rashomon specs    [SPEC]                   # list the specs / show what one requires
rashomon lint     impl.hpp [--spec SPEC]   # Rust rule: every atomic op states its ordering
rashomon verify   impl.hpp --spec SPEC     # tool-owned scenarios, exhaustive under RC11
rashomon minimize impl.hpp --spec SPEC     # weakest orderings that still verify
rashomon mutate   ref.hpp  --spec SPEC     # score the SPEC: how many small breakages does it catch?
rashomon admit    SPEC                     # a spec must earn trust before it may judge anything
rashomon status                            # which specs are admitted and unchanged
```

Exit codes: 0 pass, 1 fail, 2 refused by lint, 3 inconclusive / compile error, 4 spec not admitted.

## Spec admission

`verify` and `minimize` refuse to use a spec that has not been admitted, or that has changed in any byte since admission. "Changed" covers spec.json, the harness, the reference implementations, the judge (`rashomon.py`) itself, and the GenMC version. `--allow-unadmitted` overrides this, and the certificate then records `"spec_admitted": false`.

`rashomon admit SPEC` takes each reference implementation listed in `"references"` and checks:

1. The reference passes lint and verifies.
2. Every single-point mutant (`mutate`) is killed by the spec's `"scenarios"`.
3. A mutant that survives is re-run under `"challenge_scenarios"`, which are bigger and slower and used only here. If a challenge scenario kills it, the spec is **rejected** and the message names the scenario to promote into `"scenarios"`.
4. Anything that still survives, or can't be decided, needs a human-written entry in `"survivor_annotations"`, with a reason. Otherwise the spec is **rejected**. The allowed statuses are:
   - `equivalent`: the behaviour is identical.
   - `outside-spec`: the behaviour differs, but only in something the spec deliberately does not constrain. Example: saturating one value later than the reference.
   - `undecided`: the checker could not finish.
   - `blind-spot`: a real defect that none of the checking layers can observe. Recorded so the reader of the spec knows exactly where trust ends (see "Known blind spots").

On success it writes `admission.json` with the digest, per-reference scores and every annotated survivor.

The gate is what keeps a human out of the code and in the spec. A human never has to read an implementation. What they read is the spec statement plus the short list of annotated equivalent mutants, each with a one-line reason. That list is the spec's entire "trust me" surface.

What admission has caught so far:

- An MPMC spec with only CAP=2 scenarios is rejected. `head+1 → head-1` survives because modulo 2 hides the direction, and the CAP=4 challenge kills it.
- A **judge bug**: on an INCONCLUSIVE scenario, judging stopped before reaching a later scenario that would have FAILed. FAIL now always beats INCONCLUSIVE.
- A pop that consumes an item but returns `false` made GenMC run forever. The SPSC spec now starts with a single-threaded prologue, which kills it in one execution.

## Trust model

The author of `impl.hpp` provides only the implementation. The tool owns everything else:

- `specs/<name>/spec.json` holds a one-paragraph statement plus the interface contract. **This is the part a human reads.**
- `specs/<name>/harness.cpp` holds threads, assertions and the history checker.
- Scenario sizes are fixed by the spec, so an author can't pass by choosing a tiny test.

`verify` refuses to judge code that fails `lint`. A `PASS` can be exported as a certificate (`--cert`) that pins the implementation hash, spec hash, memory model, checker version and per-scenario execution counts.

`mutate` keeps the spec honest. It takes a correct reference implementation, makes single-point breakages (orderings weakened, `<`→`<=`, `+`→`-`, constants ±1, `true`↔`false`) and reports which ones the spec fails to catch. Every survivor is either an equivalent mutant or a hole in the spec. It has already found one: all MPMC scenarios used CAP=2, and modulo 2 makes `head+1` and `head-1` indistinguishable. A CAP=3 scenario was added.

## Checking layers

1. **GenMC** explores every interleaving and weak-memory behaviour of each scenario.
2. **Dartagnan** (optional, `--second-judge dartagnan`): only if GenMC passes, every scenario is checked again by an independent model checker. PASS needs both. See [Second judge](#second-judge-dartagnan).
3. **Native ASan + UBSan** pass: only if the model checkers pass, the same harness is compiled natively with AddressSanitizer and UndefinedBehaviorSanitizer and run 3 times per scenario on real threads.

GenMC does not check memory safety inside objects. The freelist admission found this: a constructor loop that writes `next[CAP]` survived every GenMC scenario, and ASan catches it immediately (`examples/fl_oob.hpp`).

## Known blind spots

- **One-past-the-end access to a member array of atomics, via a member call** (e.g. `ann[N].store(...)` inside the object). GenMC does not check in-object bounds. ASan only adds intra-object redzones (`-fsanitize-address-field-padding`) for classes with a non-trivial destructor of their own. UBSan accepts forming the one-past-the-end address, and the access happens inside `atomic::store`. Found by `ebr` admission and recorded there as `blind-spot`.
- **Promptness.** No spec bounds how long garbage may wait before reclamation, only that it is eventually reclaimed.

## Weak CAS

`compare_exchange_weak` may fail spuriously. Inside a retry loop, a spurious failure only makes the loop go round again with the same expected value (a stutter). Exploring unboundedly many stutters makes GenMC never terminate: the freelist timed out on every scenario. So:

- Weak CAS **inside a loop** is modelled as strong for GenMC. This is part of the trusted base and is stated in the certificate.
- Weak CAS **outside any loop** is rejected by lint (`weak-cas-outside-loop`). There, a spurious failure is real behaviour, e.g. code that reads "failed" as "someone else won". The rule is syntactic and conservative: a `try_lock` whose weak CAS is retried by its caller is rejected too. Use strong.

## The reference implementations were not always right

The judge found real problems in references written for this project, which are exactly the kind of code an AI would also write:

- **EBR, first draft:** the announce store in `enter()` was `relaxed`. It overwrites `exit()`'s release store, and since C++20 a later plain store does not continue a release sequence. A scanner reading the new announcement no longer synchronizes with the previous critical section, so reclaiming the node it read is a data race. On x86 this would almost certainly never misbehave. Caught by the scenario where a reader re-enters (`examples/ebr_relaxed_announce.hpp`).
- **EBR, announcement width:** packing `(epoch << 1) | active` into 32 bits loses the top epoch bit at EBITS = 32. Reclamation would stall for good after 2^31 epochs. A `static_assert` surfaced it at compile time, and announcements are now 64-bit.
- **Over-strong orderings:** minimize weakened orderings in four references (freelist CAS success, EBR epoch CAS, refcount, call_once CAS failure), each time with a valid argument behind it (e.g. C++20 / P0668 seq_cst-fence ordering).

## What a PASS means, exactly

- Every execution allowed by RC11 (the C++11 memory model) is explored by GenMC for each bounded scenario. This is **not** an unbounded proof.
- GenMC prunes side-effect-free spin iterations ("spin-assume"). That pruning is part of the trusted base.
- When GenMC can't finish within the timeout, the verdict is **INCONCLUSIVE**, never PASS.
- With `--second-judge dartagnan`, Dartagnan has also found no violation of the same scenario under the same model, with every loop fully unrolled. The certificate then names both checkers (`checker`, `second_judge`) and the loop bound each scenario needed.

## Shrinking parameters: pulling latent bugs into range

Some bugs need billions of operations before they fire: counters wrapping, sequence numbers repeating, the N-th lap of a ring. Bounded model checking never gets that far. So specs make those widths **template parameters** and include a scenario where they are tiny, pre-advanced to just below the wrap point by solo operations.

`examples/seqlock_wrapcmp.hpp` checks "the counter did not move forward" (`!(s2 > s1)`) instead of "the counter did not change". Both 32-bit scenarios pass. It fails only in the 3-bit scenario, where the counter goes 6 → 7 → 0 during a read. In production it would fire when a read straddles the 2^31-th write.

`examples/rc_wrapping.hpp` is a reference count that wraps at 2^BITS like a `uint32_t` would. It passes every 32-bit scenario. In the 2-bit overflow scenario (5 references, stored as 1) a release reads 1 and frees the object while a worker still holds a reference. That is the classic refcount-overflow use-after-free, normally 2^32 references away.

Shrinking only works if the implementation honours the parameter. An implementation that ignores `BITS` runs at full width in the "small" scenario, and the scenario tests nothing. Specs therefore list `shrink_params`, and lint rejects an implementation that never uses them (`unused-shrink-parameter`). This catches negligence but not malice: a token use of `BITS` passes the check. The real fix is a tool-provided narrow integer type, which is not done yet.

`examples/fl_carry.hpp` keeps the ABA tag in the low bits and bumps it by adding 1 to the whole word. When the tag is all ones, the carry corrupts the index. With a 16-bit tag that is 65536 allocations away. It passes the 16-bit scenarios and fails the 2-bit one.

Designing the overflow scenario also needs care. The first version used 4 references with a 2-bit counter (stored as 0), and the wrapping implementation passed, because releases never counted down to 1. The total has to be ≡ 1 (mod 2^BITS).

A shrunk parameter is not a proof about the full-width one. The argument is parametricity: the same generic code runs at every width, so a bug that depends only on wrap-around shows up at the smallest width too.

## Liveness

Specs declare one of two modes:

- `genmc` (spsc_queue, mutex): GenMC's spin-loop liveness check. It catches, for example, a consumer that waits forever because of an off-by-one.
- `termination` (mpmc_*): GenMC's liveness heuristic **misreports lock-free retry loops** (e.g. Vyukov's) as livelocks, so it is not used here. Instead, every scenario must have completing executions, and a single-threaded phase at the end forces the full/empty failure paths to actually return.

## Lint rules

The front end runs on clang's AST of the **instantiated** code. The spec supplies an explicit instantiation, so atomics behind template-dependent types (`Cell& c; c.seq = 1;`) are checked too.

| rule | what |
|---|---|
| `implicit-seq-cst` | `x++`, `x = v`, `int y = x`, `x.load()`, `atomic_load(&x)`, … |
| `implicit-failure-order` | a CAS with only one ordering (Rust requires both) |
| `invalid-order` | `store(acquire)`, `load(release)`, CAS failure with release (C++: UB, Rust: runtime panic, here: rejected statically) |
| `weak-cas-outside-loop` | a single-shot `compare_exchange_weak` (it can fail with nothing changed) |
| `unused-shrink-parameter` | the spec shrinks a template parameter (e.g. `BITS`) to expose wrap-around bugs, but the implementation never uses it |
| `opaque-order` (warning) | ordering passed as a variable; it can't be tuned |
| `uninstantiated-template` (warning) | lint was run without `--spec`, so dependent code could not be checked |

## minimize

The search runs in two phases:

1. Group moves: every operation on the same atomic variable is set to all-relaxed, then to all-acquire/release.
2. Single-site moves, weakest candidate first.

Group moves exist because mixing seq_cst and relaxed on one variable can stop GenMC from recognising spin loops, which blows up the state space. Single-site-only search on Vyukov took 492s and left 6 sites untested. With group moves it takes 8.5s and recovers the canonical orderings exactly.

The result is *locally* minimal: weakening any single ordering breaks some scenario. It is only as good as the scenarios. Candidates that time out are kept at the stronger ordering and listed as untested. The report shows the input's estimated cost next to each result's. The cost table is static and uncalibrated: measured on Graviton2, a predicted −53% turned out to be ≤ 3%.

## Specs shipped

All thirteen are admitted. Their references live in `specs/<name>/refs/`.

| spec | contract | notes |
|---|---|---|
| `spsc_queue` | `template<unsigned CAP> struct Queue { bool push(int); bool pop(int&); }` | solo prologue, then FIFO 1..N with N ≥ CAP+2. Mutation score 19/19 |
| `mutex` | `struct Mutex { void lock(); void unlock(); }` | plain-int counter, overlap = data race. Refs: CAS spinlock, minimized ticket lock. 17/17 |
| `seqlock` | `template<unsigned BITS> struct SeqLock { void write(int,int); bool try_read(int&,int&); }` | no torn / stale / backwards reads, payload must be atomic, includes a 3-bit wrap-around scenario. 18 killed + 2 annotated equivalent |
| `refcount` | `template<unsigned BITS> struct RefCount { void acquire(); bool release(); }` | exactly one 'last', destroyer sees all writes, no use after free. Refs: Boost-style (release + fence), libstdc++-style (acq_rel). 27/27 |
| `refcount_saturating` | same | also: honour BITS, hold ≥ 2^BITS − 2 refs, saturate before wrapping (leak, never free early). 2-bit overflow scenario. 20 killed + 1 outside-spec |
| `freelist` | `template<unsigned CAP, unsigned TAG> struct FreeList { int alloc(); void free(int); }` | no node owned twice, nothing lost after the run, atomic next links; a hoarder thread drives the classic ABA; 2-bit tag scenarios from 0 and across the wrap. 36 killed + 4 annotated |
| `ebr` | `template<unsigned NT, unsigned EBITS> struct Epoch { void enter(int); void exit(int); void retire(int,int); void drain(); }` + harness-provided `rashomon_free` | no use after free (also for a writer retiring inside its own pinned section), no double free, drain reclaims all, object in non-zeroed heap memory; 2-bit epoch wrap scenario. 39 killed + 20 annotated (incl. 1 blind spot); promptness deliberately unconstrained |
| `qlock` | `struct QLock { struct Node; void lock(Node&); void unlock(Node&); }` | MCS-style queue lock, nodes on the caller's stack: mutual exclusion, no lost update, no thread stuck. 11/11 |
| `once` | `struct Once { void call(void (*fn)()); }` | init runs exactly once; every call returns after it and sees its writes. 14 killed + 1 equivalent |
| `latch` | `struct Latch { explicit Latch(int); void count_down(); void wait(); }` | wait() opens only after all count_downs and sees everything written before each of them. 6/6 |
| `mpmc_queue` | same as spsc, non-blocking | strict: a failed push/pop must be justified by a moment the queue was really full/empty. 15 killed + 1 annotated equivalent |
| `mpmc_queue_relaxed` | same | failures may be spurious; successes must be explained by one sequential FIFO. Refs: Vyukov, locked. 45 killed + 6 annotated equivalent |
| `spsc_queue_nb` | same as spsc | non-blocking SPSC on the MPMC harness: spurious full/empty allowed (with acquire/release indices a stale full/empty cannot be pinned to one moment), so libraries with cached indices (rigtorp) fit. 19/19 |

The MPMC history checker searches for one total order of all logged operations that respects each thread's program order and replays correctly on a sequential bounded FIFO. That is sequential consistency of the object, which is weaker than real-time linearizability; real-time order has no clean definition under weak memory without adding synchronization that would mask bugs.

**Vyukov's bounded MPMC queue fails `mpmc_queue` and passes `mpmc_queue_relaxed`, even with every ordering seq_cst.** A producer that has claimed a slot but not yet published it makes other threads see "full" or "empty". This is a design trade-off, not a memory-order bug. Choosing which contract you want is exactly the human's job.

## Requirements

Step-by-step install: see the [top-level README](../README.md#install); `rashomon doctor` checks the result.

GenMC (LLVM 19–22) and `clang++-19`. Override paths with `GENMC=…` and `CLANGXX=…`.
On Ubuntu 24.04:

```
apt install llvm-19-dev clang-19 g++-14 libffi-dev zlib1g-dev libedit-dev
git clone https://github.com/MPI-SWS/genmc && cd genmc
CC=gcc-14 CXX=g++-14 cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-19/lib/cmake -B build -S .
cmake --build build -j
```

On Ubuntu 26.04, `clang++-19` selects GCC 16, whose libstdc++ headers are not installed by the packages above (`fatal error: 'atomic' file not found`). Point it at GCC 14 for every compile, GenMC's included:

```
export RASHOMON_CLANG_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14
```

For `--second-judge dartagnan`: Java 25 and Maven ≥ 3.8, then build Dat3M 4.5.0 (`git clone --branch 4.5.0 https://github.com/hernanponcedeleon/Dat3M && cd Dat3M && mvn clean install -DskipTests`) and `export DAT3M_HOME=/path/to/Dat3M`.

`./run_examples.sh` runs the regression suite (80 checks with Dartagnan installed, 73 without; about 8 minutes; the first one is the litmus calibration below). Run `rashomon status` first: after changing the judge or a spec, re-admit (`rashomon admit SPEC`, 15s–2min each).

## Auditing third-party libraries

`../realworld/audit.sh` judges real GitHub libraries. Each `adapters/*.hpp` inlines the library's headers verbatim at a pinned commit and adds a few lines mapping its API onto a spec's contract. Audit mode adds `--lint-warn` (their style rules are not ours) and `--no-shrink` (their parameters are fixed, so shrunk-parameter scenarios do not apply).

| library | spec | verdict | what it means |
|---|---|---|---|
| rigtorp/SPSCQueue | `spsc_queue_nb` | PASS | |
| rigtorp/MPMCQueue | `mpmc_queue_relaxed` | PASS | lint: implicit seq_cst `fetch_add`/CAS on `head_`/`tail_`; minimize: all of them can be relaxed, but measured on Graviton2 this buys nothing (below) |
| rigtorp/Seqlock | `seqlock` | FAIL | data race on the plain payload. Its README says x86 only; with a race-free payload the protocol still tears, because `atomic_signal_fence` orders nothing between threads. Correct on TSO hardware, undefined in C++ |
| max0x7ba/atomic_queue (both variants) | `mpmc_queue_relaxed` | FAIL | the `try_*` API is not FIFO, not even for one producer: a lagging producer at index i lets the producer at i+CAP fill the shared slot first, and the consumer of i takes the newer value. Pure interleaving, reachable under SC. Its README says only blocking `push`/`pop` in total-order mode are ordered, so this is a documented non-guarantee, made concrete |
| cameron314/readerwriterqueue | `spsc_queue_nb` | FAIL at CAP=2 | holds 3: capacity is "at least n", exactly `ceilToPow2(n+1)-1`. At its exact capacities (1, 3) PASS, and with `MAX_BLOCK_SIZE=2` forcing the multi-block path PASS (131k executions) |

**minimize on a passing library.** rigtorp/MPMCQueue's `try_push`/`try_pop` use `acquire` loads and implicit-seq_cst CAS on `head_`/`tail_`. With the CAS orderings spelled out (`adapters/rigtorp_mpmc_explicit.hpp`, same semantics), minimize weakens all eight `head_`/`tail_` orderings to `relaxed` and keeps only `slot.turn` acquire/release, the same shape as Vyukov's canonical queue: the ticket counters only hand out slots, the per-slot turn does all the publishing. The cost model estimated x86-64 unchanged, AArch64 about −53%, POWER about −61%. **Measured, the AArch64 figure is wrong.** x86-64: both builds produce byte-identical machine code. Graviton2 (t4g.small, Neoverse-N1, GCC 11): the relaxed build really does use `cas` instead of `casal` and 7 instead of 11 `ldar`, but throughput is unchanged (1P1C 11.60 vs 11.56 Mops/s, ratio 0.997; median of 11) and an uncontended single-thread push+pop pair goes from about 28 to 27 ns (≤ 3%, within noise on a burstable instance). The static cost table prices barriers in isolation; on a modern Arm core an uncontended `casal` costs about the same as `cas`, and under contention the cache-line transfer dominates everything. Treat `--target` costs as a ranking hint between equally correct answers, not as a performance prediction. (`bench/` has the benchmark.) The blocking `push`/`pop` path was deliberately left alone: the adapter never runs it, and minimizing unexercised code "succeeds" vacuously. The result is bounded and holds for this spec's consistency (sequential consistency of the object, not real-time linearizability).

The lesson in the table: none of the FAILs is an undocumented bug, but two of them (Seqlock off x86, atomic_queue's per-producer reordering) contradict what most users would assume after reading the API. The verdict turns a one-line caveat into a 5-step counterexample.

Making real code run under GenMC took a compatibility layer that `rashomon` owns and includes in every spec digest:

- `specs/_genmc_compat.h` (pthread/libc declarations, `__dso_handle`, aligned `new`) and `specs/_genmc_shims/` (`<memory>`, `<functional>`, `<stdexcept>`, `<chrono>`, `<semaphore.h>`) replace libstdc++ pieces that clash with GenMC's `<atomic>` or need a libc GenMC lacks. They are injected only into GenMC builds; the native ASan/UBSan pass uses the real headers.
- Staging rewrites, again GenMC-only, each semantics-preserving: weak CAS in loops → strong; `throw` → an unreachable call; `atomic_signal_fence` → nothing; `_mm_pause` → nothing; empty-template inline asm → nothing; `= {}` on member arrays and value-initialised `std::atomic` members → explicit `T{}` (otherwise clang emits `memset`, which GenMC rejects).
- Inline assembly that survives preprocessing makes the verdict INCONCLUSIVE. It is checked on the LLVM IR, so asm in other CPUs' `#if` branches does not count.

## GenMC quirks found along the way

- `--check-liveness` reports false livelocks on retry loops whose next iteration differs from the current one (the loop-carried `pos` in Vyukov's queue).
- Whether a loop gets the spin-assume reduction depends on code shape. When it doesn't, weak-CAS retry loops are unbounded and exploration never ends.
- `--unroll=N` (N ≥ 5) combined with that situation hits an internal assertion (`inErrorReplay`) in v0.20.0. `--unroll=3` finishes but also truncates the harness's own loops, so its PASS is meaningless. rashomon therefore never uses `--unroll`.
- **`atomic_signal_fence` is treated as `atomic_thread_fence`** (GenMC drops `syncscope("singlethread")`). A seqlock whose payload is ordered only by signal fences passes GenMC but tears in C++. This is unsound; rashomon strips signal fences before GenMC sees them. Found through rigtorp/Seqlock; pinned by `examples/seqlock_signal_fence.hpp`.
- **Inline asm outputs evaluate to 0.** `asm("" : "+r"(x))`, a common register-pinning idiom that emits no instruction, silently sets `x` to 0 and produced a false data race on atomic_queue. Pinned by `examples/queue_asm_barrier.hpp` / `queue_asm_real.hpp`.
- Value-initialising an atomic (`std::atomic<T> x{};`, `: x()`) lowers to `llvm.memset`, which GenMC refuses ("Invalid call to memset()").

## Second judge: Dartagnan

GenMC twice gave a wrong verdict without any error (signal fences, inline asm), so a single checker's PASS is only as good as that checker. `verify --second-judge dartagnan` asks a second, independent one: [Dartagnan](https://github.com/hernanponcedeleon/Dat3M) 4.5.0, an SMT-based bounded model checker. Both checkers read the same LLVM IR, so they share clang and rashomon's staging rewrites, but not the exploration algorithm, the runtime, or GenMC's compat layer.

**Rule.** Dartagnan runs only on scenarios GenMC passed (a GenMC FAIL is already final). Any Dartagnan FAIL makes the verdict FAIL; anything else short of PASS (timeout, memory limit, loops not fully unrolled, parse error, unmodelled call) makes it INCONCLUSIVE. Either way the row says `judges disagree (GenMC: PASS, Dartagnan: …)`. Dartagnan is a *bounded* checker: rashomon tries loop bounds 2, 4, 8, 16, and "Not fully unrolled loops" at the last bound is INCONCLUSIVE, never PASS (regression: `RASHOMON_DARTAGNAN_BOUNDS=2` on `ebr`).

**Measured on the regression examples** (both judges run independently on every scenario of the 45 non-MPMC verify cases, 154 scenarios):

| GenMC / Dartagnan | scenarios | |
|---|---|---|
| FAIL / FAIL | 80 | |
| PASS / PASS | 64 | 46 at bound 2, 18 at bound 4 |
| PASS / **FAIL** | 4 | all `fl_oob.hpp`: the out-of-bounds write GenMC cannot see (it is caught by the ASan pass), Dartagnan reports it as a data race |
| PASS / INCONCLUSIVE | 6 | `rc_wrapping.hpp` (CAS retry loops: 300 s timeout at bound 8), `spsc_queue_nb` (too large, see below) |

Judged per example, Dartagnan alone gets 43 of 45 right, 2 inconclusive, none wrong. It costs about 5 s per small scenario and up to 20–50 s for ebr/freelist, against GenMC's sub-second.

**Not covered.** Specs built on the MPMC harness (`mpmc_queue`, `mpmc_queue_relaxed`, `spsc_queue_nb`) need loop bound ≥ 8 for the harness's own history check (`while (ndrained < MO_MAXVALS && q.pop(v))`, `for k < nops[t]`), and at that size the SMT encoding exceeds 4 GB or 300 s. With `--second-judge` they are INCONCLUSIVE: no wrong verdict, but no second opinion either.

**Dartagnan quirks found while wiring it in.** Each would have made the second judge wrong in its own way, and each is handled. `run_examples.sh` pins the `operator new` (latch) and global-constructor (ebr) fixes through `verify`. The signal-fence and release-sequence fixes can't be pinned that way, because GenMC FAILs those examples before Dartagnan runs, so they are pinned with `tools/dartagnan_only.py`, which runs the second judge alone:

- **It also treats `atomic_signal_fence` as a thread fence** (`VisitorLlvm.visitFenceInst` drops `syncscope`). Without rashomon's staging rewrite it PASSes `seqlock_signal_fence.hpp`, the same blind spot as GenMC. The two checkers are not independent here, so the rewrite stays in front of both.
- **Its `rc11.cat` uses C++11 release sequences**: `rs = [W]; (po & loc)?; …` lets a later same-thread store continue a release sequence. C++20 (P0982) removed that and GenMC implements C++20, so stock Dartagnan PASSes `ebr_relaxed_announce.hpp`. rashomon ships `specs/_rc11_cpp20.cat` with the C++20 rule.
- **It never runs `@llvm.global_ctors`**, so dynamically initialised globals (`Epoch &ebr = *new Epoch`, any non-constexpr constructor) start out zeroed. This produced a false FAIL on `ebr_seqcst.hpp`, and in other code it could equally hide a bug (`litmus/global_ctor_bug.cpp` is one). rashomon inserts the constructor calls at the top of `main` in the IR. Such an initializer, when it sits in the implementation header, also gets a `DILexicalBlockFile` debug scope that crashes Dartagnan's parser; rashomon rewrites it into a plain lexical block.
- **No `operator new`/`delete`**, only `malloc`/`free`, and **no `invoke`/`landingpad`** (which clang emits for `new T(...)`). `specs/_dartagnan_compat.h` defines the replaceable allocation functions on top of `malloc`, and the Dartagnan build uses `-fno-exceptions` (code that really throws is INCONCLUSIVE).
- **Calling an unknown function is reported as FAIL** ("Calling unknown function"). rashomon reports it as INCONCLUSIVE: it is the judge not knowing a library call, not a counterexample.
- Full debug info does not parse (template value parameters); rashomon uses `-gline-tables-only`. Unparsed inline asm becomes a nondeterministic value with a warning; rashomon refuses asm for this judge as for GenMC.
- **Memory**: the embedded SMT solver allocates outside the JVM heap (11 GB resident under `-Xmx3g`), and two parallel runs took down a 15 GB VM. rashomon kills it above `RASHOMON_DARTAGNAN_MEM_MB` (default 4096) and reports INCONCLUSIVE.

## Litmus calibration

Two judges only help where their errors are independent, and they share a lot: clang, the staging rewrites, the memory-model definition. So the judges are calibrated on programs whose answer is known. `litmus/*.cpp` holds 32 tiny complete programs, each with its expected verdict in a header (`// expect: FAIL race`, optionally `// expect-dartagnan: …` or `// known-wrong: genmc: …`). `tools/litmus.py` stages each one exactly like an implementation under test (same `stage_impl`, flags and model) and runs every installed judge. A FAIL must also fail for the stated reason (assertion, data race or liveness). It takes about 45 s and is the first check of `run_examples.sh`.

Both directions are tested. An *allowed* weak behaviour (store buffering, message passing without synchronization, IRIW under acquire/release, 2+2W, signal fences between threads, a C++20 release sequence broken by a same-thread store) must be found: a judge that PASSes it has a model that is too strong, which is the dangerous direction. A *forbidden* one (the same shapes with seq_cst, release/acquire, fences or RMWs) must not be reported. The rest probe the pipeline: global constructors, `new`/`delete`, empty and real inline asm, weak CAS loops, livelock vs. a correct spin handoff, a bug only reachable after 6 iterations of a run-time loop, and correct code whose loop exceeds Dartagnan's largest bound (must be INCONCLUSIVE, never PASS).

The first run found three problems that the regression examples had not:

- **GenMC could not run `delete`** on a class object (sized `operator delete`, "unknown external function `_ZdlPvm`"): INCONCLUSIVE on correct code. Fixed in `specs/_genmc_compat.h`.
- **GenMC stops on "Unordered writes"**: when two threads store the same value to one location without ordering, its in-place revisiting turns that warning into an error. rashomon now says so instead of an empty INCONCLUSIVE. Not fixed (disabling it would change every existing verdict), recorded as a known blind spot: `litmus/same_value_writes.cpp`. It can only cause a false alarm, never a false PASS.
- **Dartagnan crashed on the debug scope of a global initializer** in the implementation header (see the Dartagnan quirks above).

Current result: 32 tests × 2 judges, all as expected, with one known GenMC blind spot. One deliberate choice is visible in `litmus/lb_relaxed.cpp`: RC11 forbids load buffering (`po ∪ rf` acyclic), while ISO C++ technically allows it for relaxed atomics (the out-of-thin-air gap). rashomon promises RC11, so code that is correct under RC11 but relies on the absence of out-of-thin-air values gets a PASS that the standard alone would not guarantee. No real compiler and CPU pair is known to produce those values.
