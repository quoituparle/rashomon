#!/usr/bin/env bash
# Third-party audit. adapters/*.hpp inline each library header VERBATIM (pinned commit in the
# first line) plus a few-line adapter to the spec's contract. Audit mode: --lint-warn (their style
# is not ours), --no-shrink (their parameters are fixed), --allow-unadmitted for audit-only specs.
cd "$(dirname "$0")" || exit 1
M="python3 ../mocheck/mocheck.py"
run(){ echo "== $1 on $2"; $M verify "adapters/$1.hpp" --spec "$2" --lint-warn --no-shrink "${@:3}" 2>&1 | grep -E '^\s+\[|VERDICT'; }
run rigtorp_spsc                         spsc_queue_nb        # PASS
run rigtorp_mpmc                         mpmc_queue_relaxed   # PASS (lint: implicit seq_cst on head_/tail_)
run rigtorp_seqlock                      seqlock              # FAIL: data race on plain payload (x86-only by README)
run rigtorp_seqlock_protocol_experiment  seqlock              # FAIL: torn read (signal fences order nothing between threads)
run atomic_queue                         mpmc_queue_relaxed   # FAIL: not FIFO, even per producer (README: try_* not totally ordered)
run atomic_queue2                        mpmc_queue_relaxed   # FAIL: same
run readerwriterqueue                    spsc_queue_nb        # FAIL at CAP=2 only: holds 3 (documented "at least n")
# exact-capacity audit specs (scenarios at the library's real capacities): copy into ../mocheck/specs to run
#   readerwriterqueue (CAP 1,3) and readerwriterqueue_multiblock (MAX_BLOCK_SIZE=2, CAP 5): PASS
# over-strong orderings: minimize rigtorp MPMC's try_* path (CAS orders spelled out, blocking path untouched)
#   python3 ../mocheck/mocheck.py minimize adapters/rigtorp_mpmc_explicit.hpp --spec mpmc_queue_relaxed \
#       --lint-warn --no-shrink --prefer auto --target aarch64      -> adapters/rigtorp_mpmc_minimized.hpp
run rigtorp_mpmc_minimized               mpmc_queue_relaxed   # PASS: head_/tail_ all relaxed, slot.turn acq/rel
