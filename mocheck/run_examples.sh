#!/usr/bin/env bash
# Regression: every example must get the expected verdict.
cd "$(dirname "$0")/examples" || exit 1
M="python3 ../mocheck.py"; fails=0
check(){ local want=$1 got; shift; "$@" >/dev/null 2>&1; got=$?
  if [ "$got" = "$want" ]; then echo "ok    exit=$got  ${*:3}"; else echo "WRONG exit=$got want=$want  ${*:3}"; fails=$((fails+1)); fi; }
orders(){ grep -v 'atomic_thread_fence(std::memory_order_relaxed)' "$1" | grep -o 'memory_order_[a-z_]*'; }  # a relaxed fence is a no-op
same_orders(){ diff <(orders "$1") <(orders "$2") >/dev/null \
  && echo "ok    minimize($3) == $(basename "$2")" || { echo "WRONG minimize($3) != $(basename "$2")"; fails=$((fails+1)); }; }
# exit codes: 0 pass, 1 fail, 2 lint refused, 3 inconclusive/compile error, 4 spec not admitted
check 0 $M status
# tampering: a byte-identical copy of an admitted spec is still admitted; an edited one is refused
rm -rf ../specs/_tamper && cp -r ../specs/mutex ../specs/_tamper
check 0 $M verify mutex_ok.hpp --spec _tamper
echo "// harmless-looking edit" >> ../specs/_tamper/harness.cpp
check 4 $M verify mutex_ok.hpp --spec _tamper
check 0 $M verify mutex_ok.hpp --spec _tamper --allow-unadmitted
rm -rf ../specs/_tamper
check 1 $M lint   queue_template_lazy.hpp --spec spsc_queue
check 0 $M verify queue_ok.hpp            --spec spsc_queue
check 0 $M verify queue_seqcst.hpp        --spec spsc_queue
check 1 $M verify queue_bug.hpp           --spec spsc_queue
check 1 $M verify queue_offbyone.hpp      --spec spsc_queue
check 2 $M verify queue_lazy.hpp          --spec spsc_queue
check 0 $M verify mutex_ok.hpp            --spec mutex
check 1 $M verify mutex_bug.hpp           --spec mutex
check 0 $M verify mutex_ticket_seqcst.hpp --spec mutex
check 0 $M verify mpmc_locked.hpp         --spec mpmc_queue
check 1 $M verify mpmc_vyukov.hpp         --spec mpmc_queue           # by design: spurious full/empty
check 0 $M verify mpmc_vyukov.hpp         --spec mpmc_queue_relaxed
check 0 $M verify mpmc_vyukov_seqcst.hpp  --spec mpmc_queue_relaxed
check 1 $M verify mpmc_vyukov_bug.hpp     --spec mpmc_queue_relaxed
check 1 $M verify mpmc_naive.hpp          --spec mpmc_queue_relaxed
check 0 $M verify seqlock_seqcst.hpp       --spec seqlock
check 0 $M verify seqlock_acqrel.hpp       --spec seqlock
check 1 $M verify seqlock_plain.hpp        --spec seqlock               # plain payload: data race (UB in C++)
check 1 $M verify seqlock_nofence.hpp      --spec seqlock               # torn read
check 1 $M verify seqlock_wrapcmp.hpp      --spec seqlock               # only the shrunk 3-bit scenario catches it
check 2 $M verify seqlock_ignores_bits.hpp --spec seqlock               # BITS unused: shrunk scenario would be fake
check 1 $M verify rc_relaxed.hpp           --spec refcount              # destroyer misses writes: data race
check 1 $M verify rc_nofence.hpp           --spec refcount              # release without acquire on the destroy path
check 1 $M verify rc_offbyone.hpp          --spec refcount              # fetch_sub returns the old value
check 0 $M verify rc_seqcst.hpp            --spec refcount
check 0 $M verify rc_wrapping.hpp          --spec refcount              # fine unless you care about overflow...
check 1 $M verify rc_wrapping.hpp          --spec refcount_saturating   # ...2-bit overflow: freed while referenced
check 2 $M verify ../specs/refcount/refs/refcount_std.hpp --spec refcount_saturating   # ignores BITS
check 1 $M verify fl_notag.hpp             --spec freelist              # textbook Treiber stack: ABA
check 1 $M verify fl_plainnext.hpp         --spec freelist              # next links not atomic: data race
check 1 $M verify fl_carry.hpp             --spec freelist              # tag carry into index: only the 2-bit scenario
check 1 $M verify fl_oob.hpp               --spec freelist              # out-of-bounds write: only the ASan pass
check 0 $M verify fl_seqcst.hpp            --spec freelist
check 2 $M verify mutex_single_weak_cas.hpp --spec mutex                # single-shot weak CAS
check 1 $M verify ebr_nofence.hpp          --spec ebr                   # no seq_cst fence after announcing
check 1 $M verify ebr_acqrel.hpp           --spec ebr                   # acq_rel fences cannot order store->load
check 1 $M verify ebr_early.hpp            --spec ebr                   # reclaims one epoch too early
check 1 $M verify ebr_ltcmp.hpp            --spec ebr                   # '<' instead of '!=': only the 2-bit wrap scenario
check 1 $M verify ebr_relaxed_announce.hpp --spec ebr                   # the reference's own first draft
check 0 $M verify ebr_seqcst.hpp           --spec ebr
check 1 $M verify qlock_relaxed_handoff.hpp --spec qlock                 # hand-over store relaxed
check 1 $M verify qlock_relaxed_xchg.hpp   --spec qlock                 # exchange without acquire
check 1 $M verify qlock_nowait.hpp         --spec qlock                 # CAS failed, does not wait for successor
check 0 $M verify qlock_seqcst.hpp         --spec qlock
check 1 $M verify once_dclp.hpp            --spec once                  # double-checked locking, relaxed fast path
check 1 $M verify once_flag.hpp            --spec once                  # check-then-act: init runs twice
check 1 $M verify once_relaxed_done.hpp    --spec once                  # 'done' published relaxed
check 0 $M verify once_seqcst.hpp          --spec once
check 1 $M verify latch_relaxed_down.hpp   --spec latch                 # relaxed count_down
check 1 $M verify latch_relaxed_wait.hpp   --spec latch                 # relaxed wait
check 1 $M verify latch_offbyone.hpp       --spec latch                 # opens one count early
check 0 $M verify latch_seqcst.hpp         --spec latch
# GenMC soundness defences (each would be a silent wrong verdict without the staging rewrite)
check 1 $M verify seqlock_signal_fence.hpp --spec seqlock              # GenMC alone: PASS (signal fence = thread fence)
check 0 $M verify seqlock_thread_fence.hpp --spec seqlock              # control: real fences
check 0 $M verify queue_asm_barrier.hpp    --spec spsc_queue           # GenMC alone: asm output = 0 -> false FAIL
check 3 $M verify queue_asm_real.hpp       --spec spsc_queue           # real asm: refuse to judge
check 0 $M verify queue_ok.hpp             --spec spsc_queue_nb
# second judge (Dartagnan): PASS needs both checkers. Skipped when Dartagnan is not installed.
if [ -f "${DARTAGNAN_JAR:-${DAT3M_HOME:-$HOME/Dat3M}/dartagnan/target/dartagnan.jar}" ]; then
check 0 $M verify queue_ok.hpp             --spec spsc_queue --second-judge dartagnan
check 0 $M verify latch_seqcst.hpp         --spec latch      --second-judge dartagnan   # operator new: needs compat + -fno-exceptions
check 0 $M verify ebr_seqcst.hpp           --spec ebr        --second-judge dartagnan   # Dartagnan skips global ctors unless we call them
MOCHECK_DARTAGNAN_BOUNDS=2 \
check 3 $M verify ebr_seqcst.hpp           --spec ebr        --second-judge dartagnan   # loop bound too small: INCONCLUSIVE, never PASS
check 1 $M verify latch_relaxed_wait.hpp   --spec latch      --second-judge dartagnan
# Dartagnan alone (GenMC FAILs these first, so verify never asks it): its own defences
D="python3 ../tools/dartagnan_only.py"
check 1 $D seqlock_signal_fence.hpp        --spec seqlock    # Dartagnan also ignores syncscope: needs the rewrite
check 1 $D ebr_relaxed_announce.hpp        --spec ebr        # stock rc11.cat has C++11 release sequences: needs ours
else echo "skip  second-judge checks (Dartagnan not found; set DAT3M_HOME)"; fi
$M minimize qlock_seqcst.hpp --spec qlock --out /tmp/qlock.min.hpp --quiet >/dev/null
same_orders /tmp/qlock.min.hpp ../specs/qlock/refs/mcs.hpp qlock_seqcst
$M minimize once_seqcst.hpp --spec once --out /tmp/once.min.hpp --quiet >/dev/null
same_orders /tmp/once.min.hpp ../specs/once/refs/once_ref.hpp once_seqcst
$M minimize latch_seqcst.hpp --spec latch --out /tmp/latch.min.hpp --quiet >/dev/null
same_orders /tmp/latch.min.hpp ../specs/latch/refs/latch_ref.hpp latch_seqcst
# hardware-aware minimize: on POWER, fences win for seqlock (Boehm) and refcount (Boost)
$M minimize seqlock_seqcst.hpp --spec seqlock --prefer auto --target power --out /tmp/sl.pw.hpp --quiet >/dev/null
same_orders /tmp/sl.pw.hpp ../specs/seqlock/refs/seqlock_ok.hpp seqlock_seqcst@power
$M minimize seqlock_seqcst.hpp --spec seqlock --prefer auto --target aarch64 --out /tmp/sl.a64.hpp --quiet >/dev/null
same_orders /tmp/sl.a64.hpp seqlock_acqrel.hpp seqlock_seqcst@aarch64
$M minimize rc_seqcst.hpp --spec refcount --prefer auto --target power --out /tmp/rc.pw.hpp --quiet >/dev/null
same_orders /tmp/rc.pw.hpp ../specs/refcount/refs/refcount_std.hpp rc_seqcst@power
$M minimize ebr_seqcst.hpp --spec ebr --out /tmp/ebr.min.hpp --quiet >/dev/null
same_orders /tmp/ebr.min.hpp ../specs/ebr/refs/ebr_ref.hpp ebr_seqcst
$M minimize fl_seqcst.hpp --spec freelist --out /tmp/fl.min.hpp --quiet >/dev/null
same_orders /tmp/fl.min.hpp ../specs/freelist/refs/freelist_tagged.hpp fl_seqcst
$M minimize rc_seqcst.hpp --spec refcount --out /tmp/rc.min.hpp --quiet >/dev/null
same_orders /tmp/rc.min.hpp ../specs/refcount/refs/refcount_acqrel.hpp rc_seqcst
$M minimize queue_seqcst.hpp --spec spsc_queue --out /tmp/q.min.hpp --quiet >/dev/null
same_orders /tmp/q.min.hpp queue_ok.hpp queue_seqcst
$M minimize mpmc_vyukov_seqcst.hpp --spec mpmc_queue_relaxed --out /tmp/vy.min.hpp --quiet --candidate-timeout 15 >/dev/null
same_orders /tmp/vy.min.hpp mpmc_vyukov.hpp mpmc_vyukov_seqcst
echo "failures: $fails"; exit $fails
