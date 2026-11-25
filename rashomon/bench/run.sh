#!/bin/bash
# usage: ./run.sh   (needs g++, git). Builds upstream vs relaxed-ticket variant, interleaves runs, prints medians.
set -e
command -v g++ >/dev/null || { sudo dnf install -y gcc-c++ git 2>/dev/null || sudo apt-get install -y g++ git; }
[ -d MPMCQueue ] || git clone -q https://github.com/rigtorp/MPMCQueue
mkdir -p relaxed/rigtorp && cp MPMCQueue/include/rigtorp/MPMCQueue.h relaxed/rigtorp/
sed -i -e 's/\(head_\|tail_\)\.load(std::memory_order_acquire)/\1.load(std::memory_order_relaxed)/' \
       -e 's/compare_exchange_strong(\(head\|tail\), \1 + 1)/compare_exchange_strong(\1, \1 + 1, std::memory_order_relaxed, std::memory_order_relaxed)/' relaxed/rigtorp/MPMCQueue.h
diff MPMCQueue/include/rigtorp/MPMCQueue.h relaxed/rigtorp/MPMCQueue.h | grep -c '^>' 
g++ -O2 -std=c++20 -pthread -IMPMCQueue/include bench.cpp -o b_orig
g++ -O2 -std=c++20 -pthread -Irelaxed bench.cpp -o b_relaxed
uname -m; nproc; g++ --version | head -1
for b in b_orig b_relaxed; do printf "%-10s " $b; objdump -d --no-show-raw-insn $b | sed -n '/<main>:/,$p' | grep -oE '\b(ldar|ldapr|ldaxr|ldxr|stlxr|stxr|casal|casa|casl|cas|dmb)\b|__aarch64_cas8_[a-z_]+' | sort | uniq -c | tr -s ' \n' ' '; echo; done
med(){ sort -n | awk '{a[NR]=$1} END{print a[int((NR+1)/2)]}'; }
n=$(nproc); cfgs="1 1|2 2"; [ $n -ge 8 ] && cfgs="$cfgs|4 4"; [ $n -ge 16 ] && cfgs="$cfgs|8 8"
IFS="|"; for cfg in $cfgs; do
  o=""; r=""
  IFS=" "; for i in $(seq ${REPS:-9}); do o+="$(./b_orig $cfg ${N:-2000000} 1024)"$'\n'; r+="$(./b_relaxed $cfg ${N:-2000000} 1024)"$'\n'; done
  mo=$(echo -n "$o" | med); mr=$(echo -n "$r" | med)
  echo "P C = $cfg   upstream $mo Mops/s   relaxed $mr Mops/s   ratio $(awk "BEGIN{printf \"%.3f\", $mr/$mo}")"
  IFS="|"
done
