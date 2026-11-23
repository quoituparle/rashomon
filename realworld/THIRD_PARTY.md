# Third-party code in `adapters/`

Each adapter inlines the upstream headers verbatim (commit in its first line); only the trailing adapter block is ours.
`rigtorp_mpmc_explicit.hpp` / `rigtorp_mpmc_minimized.hpp` and `rigtorp_seqlock_protocol_experiment.hpp` are modified derivatives, labelled as such.

| upstream | commit | license |
|---|---|---|
| github.com/rigtorp/SPSCQueue | cd49c7c | MIT, (c) 2018 Erik Rigtorp |
| github.com/rigtorp/MPMCQueue | 62f249d | MIT, (c) 2018 Erik Rigtorp |
| github.com/rigtorp/Seqlock | 3883462 | MIT, (c) 2018 Erik Rigtorp |
| github.com/max0x7ba/atomic_queue | b7f1bef | MIT, (c) 2019 Maxim Egorushkin |
| github.com/cameron314/readerwriterqueue | 87b1df1 | Simplified BSD, (c) Cameron Desrochers |

The full license texts are in the upstream repositories; the copyright headers are kept in the inlined code.
