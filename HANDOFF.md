# mocheck 交接文档

写于 2026-11-23，从云端会话转到本地；2026-11-24 在本地（WSL2 Ubuntu 26.04）续做了第二轮，见下面"第二轮"。技术细节以 `mocheck/README.md` 为准，这里讲**现状、怎么跑起来、下一步做什么**。

## 一句话现状

C++ 内存序判官：AI 按规格写实现，mocheck 给出 PASS / FAIL（带反例）/ INCONCLUSIVE。13 个规格全部通过准入，80 项回归 0 失败（装了 Dartagnan 时；没装时 73 项）。回归的第一项是 litmus 校准：32 个答案已知的小程序，两个判官都要判对。第二轮加上了 `--second-judge dartagnan`：GenMC 判 PASS 之后，再由 Dartagnan 独立检查每个场景，两边都 PASS 才算 PASS。接入过程中发现 Dartagnan 自己也有 5 个会导致错判的问题，都已经在 mocheck 这边防住。现在的主要缺口：MPMC 类规格对 Dartagnan 来说太大，第二判官只能给 INCONCLUSIVE。

## 仓库结构

```
mocheck/
  mocheck.py          判官本体（单文件 Python，约 1500 行）
  specs/<name>/       13 个规格：spec.json + harness.cpp + refs/ + admission.json
  specs/_genmc_compat.h, specs/_genmc_shims/   GenMC 兼容层（只在 GenMC 构建时注入）
  examples/           回归用例（每个都有预期判决）
  run_examples.sh     回归脚本，72 项，约 5 分钟
  demo/               早期演示：AI 草稿 → 判官 → 修正 → 证书
  bench/              rigtorp MPMC 原版 vs 放宽版的性能对比
realworld/
  adapters/           第三方库适配器：库头文件原样内联（首行注明 commit）+ 几行对接规格契约的代码
  audit.sh            一键重跑真实库审计
  audit_specs/        审计专用规格（按库的真实容量），用前复制进 mocheck/specs/
```

第三方源码克隆没有放进仓库。适配器已经内联了需要的头文件，许可证见 `realworld/THIRD_PARTY.md`。

## 本地环境搭建

需要：Linux x86_64（WSL2 Ubuntu 24.04 就行）、Python 3.10+、clang-19、GenMC v0.20.0。

```bash
sudo apt install -y llvm-19-dev clang-19 g++-14 libffi-dev zlib1g-dev libedit-dev cmake python3
git clone --branch v0.20.0 https://github.com/MPI-SWS/genmc && cd genmc
CC=gcc-14 CXX=g++-14 cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/lib/llvm-19/lib/cmake -B build -S .
cmake --build build -j
```

mocheck 通过环境变量找工具：

```bash
export GENMC=/path/to/genmc/build/bin/genmc   # GENMC_INCLUDE 默认推导为 <genmc 源码>/lli/runtime-include/c
export CLANGXX=clang++-19
```

**换机器后第一件事：重新准入。** 准入摘要包含 GenMC 版本和判官代码，所以 `admission.json` 在新环境里很可能失效：

```bash
cd mocheck
python3 mocheck.py status            # 看哪些失效
for s in spsc_queue_nb spsc_queue mpmc_queue mpmc_queue_relaxed seqlock mutex qlock once latch \
         refcount refcount_saturating freelist ebr; do python3 mocheck.py admit $s; done   # 约 10 分钟
./run_examples.sh                    # 应输出 failures: 0
```

如果某个规格准入的分数和 README 表格里的不一样，先别急着改规格，大概率是 GenMC 版本或 clang 版本不同。

**本机（Ubuntu 26.04）额外要注意的：**
- `clang++-19` 会选中 GCC 16，但 GCC 16 的 libstdc++ 头文件没装，报 `'atomic' file not found`。设 `export MOCHECK_CLANG_FLAGS=--gcc-install-dir=/usr/lib/gcc/x86_64-linux-gnu/14`，所有编译（包括 GenMC 内部）都会带上。
- Dartagnan：`~/Dat3M`（tag 4.5.0，已编好）。设 `export DAT3M_HOME=~/Dat3M`。需要 Java 25（已装）。
- 在本机实测：13 个规格的分数和 README 完全一致，准入约 5–7 分钟（4 个并行）。
- **内存**：Dartagnan 内嵌的 SMT 求解器会在 JVM 堆之外大量占内存（`-Xmx3g` 下见过 11 GB）。并行跑两个曾经把 15 GB 的 WSL 虚拟机整个撑崩重启。mocheck 现在会监控它的常驻内存，超过 `MOCHECK_DARTAGNAN_MEM_MB`（默认 4096）就杀掉，判 INCONCLUSIVE。并行跑多个时把这个值调低。

## 这轮做了什么（按时间）

1. 13 个规格 + 准入门（变异测试 + 挑战场景 + 幸存者人工标注）。
2. 双层检查：GenMC 穷举 + 原生 ASan/UBSan。
3. 缩参数暴露潜伏 bug（计数器回绕、ABA 标签溢出等）。
4. 硬件感知的 minimize（x86 / AArch64 / POWER 成本表，按执行频率加权）。
5. **真实库审计**（`realworld/audit.sh`）：
   - rigtorp SPSC / MPMC：通过。
   - rigtorp Seqlock：失败，在 C++ 下是 UB（README 写了仅限 x86）。
   - atomic_queue：失败，`try_*` 不是 FIFO，连单个生产者的顺序都会乱（README 写了 `try_*` 不保证全序）。
   - readerwriterqueue：按真实容量全部通过，包括压到最小块大小、强制走多块链表路径。
6. **抓到 GenMC 的两个可靠性漏洞并已防住**：`atomic_signal_fence` 被当成线程 fence；内联汇编的输出被当成 0。
7. 在 Graviton2 上实测：minimize 预测 ARM 省 53%，实际 ≤ 3%。成本模型已在 README 里降级为"只能排先后，不能当性能预测"。没有给 rigtorp 提 PR。

## 第二轮（2026-11-24，本地）：Dartagnan 交叉判决，已完成

用法：`python3 mocheck.py verify impl.hpp --spec X --second-judge dartagnan`。详细说明和数据见 README 的 "Second judge: Dartagnan" 一节。

**判决规则**：GenMC 判 PASS 的场景才交给 Dartagnan（GenMC 判 FAIL 就已经是最终结论）。Dartagnan 判 FAIL，结论就是 FAIL；Dartagnan 给出其他非 PASS 结果（超时、超内存、循环没展开完、解析失败、碰到不认识的函数），结论就是 INCONCLUSIVE。两种情况都会在输出里标明 `judges disagree`。循环上界依次试 2、4、8、16，到 16 还展开不完就判 INCONCLUSIVE，绝不判 PASS。证书里会同时记录两个检查器的版本。

**交叉跑的结果**（两个判官各自独立跑完回归里全部 45 个非 MPMC 用例，共 154 个场景）：
- 两边都 FAIL 80 个，都 PASS 64 个。
- GenMC PASS、Dartagnan FAIL 的有 4 个，全是 `fl_oob.hpp`：构造函数越界写，GenMC 本来就看不到（之前只靠 ASan 抓）。**Dartagnan 独立抓到了。**
- GenMC PASS、Dartagnan INCONCLUSIVE 的有 6 个：`rc_wrapping` 的 CAS 重试循环在 bound 8 超时；`spsc_queue_nb` 规模太大。
- 按用例算，Dartagnan 单独判 43 个对、2 个 INCONCLUSIVE、0 个错。

**接入时发现 Dartagnan 自己的问题**（每一条都会让它错判，都已经在 mocheck 这边防住）：
1. 它同样把 `atomic_signal_fence` 当成线程 fence（源码里直接丢掉了 syncscope），和 GenMC 是同一个盲点。所以 signal fence 改写对两个判官都保留着。在这一点上两个判官**并不独立**。
2. 它的 `rc11.cat` 用的是 C++11 的 release sequence（同线程后续 store 也能续上），GenMC 用的是 C++20 的。因此原版 Dartagnan 会放过 `ebr_relaxed_announce.hpp`。现在用我们自带的 `specs/_rc11_cpp20.cat`。
3. 它不执行 `@llvm.global_ctors`：全局对象的动态初始化不会跑，程序从全零状态开始。这个在 `ebr_seqcst` 上造成了误判 FAIL，换一份代码同样可能掩盖 bug。现在在 IR 里把构造函数调用插到 `main` 开头。
4. 它不认识 `operator new/delete`（只认 malloc/free），也解析不了 `invoke/landingpad`。现在用 `specs/_dartagnan_compat.h` 加上 `-fno-exceptions` 解决。
5. 调用它不认识的函数时会报 FAIL。mocheck 把这种情况改判为 INCONCLUSIVE，因为这不是反例。

外加内存监控（见上面"本机"一节）、`MOCHECK_CLANG_FLAGS`，以及 `tools/dartagnan_only.py`（单独跑第二判官，回归里用来固定第 1、2 条）。回归新增 7 项。

## 第三轮（2026-11-24）：litmus 校准

起因：两个判官共用 clang、改写和内存模型定义，所以它们会不会"一起错"必须实际量一量。

- `litmus/*.cpp`：32 个小程序，每个文件头里写好预期结果（`// expect: FAIL race` 之类），`tools/litmus.py` 按和真实实现完全一样的流水线跑所有判官。FAIL 还要求原因对得上（断言、数据竞争还是活性）。耗时约 45 秒，已经是 `run_examples.sh` 的第一项。
- 两个方向都测。**允许的**弱行为必须被找出来：判官如果判 PASS，说明它的模型太强，这是会导致误放的危险方向。**禁止的**行为不能被报出来。另有一组专门测流水线：全局构造函数、new/delete、asm、弱 CAS、活锁、循环上界。
- 第一次跑就找出 3 个回归用例没覆盖到的问题：
  1. GenMC 不认识 sized `operator delete`，任何 `delete` 对象的代码都判 INCONCLUSIVE。已在 `_genmc_compat.h` 里补上。
  2. GenMC 遇到两个线程无序地写同一个值时，会因为 in-place revisiting 直接停掉。现在 mocheck 会说明原因。这个没修（关掉它会改变所有已有判决），作为已知盲点记录在 `litmus/same_value_writes.cpp`。它只会造成误报，不会造成误放。
  3. 实现头文件里的全局初始化会带一个 `DILexicalBlockFile` 调试作用域，Dartagnan 遇到就解析崩溃。现在在 IR 里改写掉。
- 现在的结果：32 × 2 全部符合预期，外加 1 个已知的 GenMC 盲点。
- 一个有意保留的取舍：RC11 禁止 load buffering（`lb_relaxed.cpp`），而 ISO C++ 理论上允许（凭空产生值，即 out-of-thin-air 问题）。mocheck 承诺的是 RC11，这一点比标准强，README 里写明了。

## 下一步

0. litmus 可以继续扩充：consume、混合大小访问、`std::atomic_ref`、`wait/notify`。这些目前都没有覆盖，结论不明。每加一个答案已知的测试，判官的可信度就多一分证据。
1. **让 MPMC 类规格也能用第二判官**（`mpmc_queue`、`mpmc_queue_relaxed`、`spsc_queue_nb`）。卡在 harness 自己的历史检查：`while (ndrained < MO_MAXVALS && q.pop(v))`、`for k < nops[t]` 这类循环的次数由数据决定，要 bound ≥ 8，这时 SMT 编码超过 4 GB 或 300 秒。可以试的方向：用 Dartagnan 的 `--bound.load` 给不同循环设不同上界；给 Dartagnan 写一个更省的 harness 变体（需要重新准入，而且要证明它和原版等价）。
2. **CAS 重试循环**（`rc_wrapping`）在 bound 8 超时。可以看看 Dartagnan 有没有对"失败的 CAS 没有副作用"做规约的选项。
3. **把发现的 Dartagnan 问题报给上游**（Dat3M）：syncscope 被丢掉、rc11.cat 的 release sequence、global_ctors。这是对外的动作，没有做，等你决定。
4. 原来的长期问题依旧：正则改写应该换成基于 clang AST 的改写。而且现在两个判官共用这套改写，它是两边共同的信任基础。

## 其他已知问题和想法

- 预处理改写（`run_scenario` 里那一串 `re.sub`）是用正则改 C++ 源码，很脆弱。长期应该换成基于 clang AST 的改写。
- 缩参数依赖实现真的用了 `BITS` 这类模板参数。lint 只能挡住"完全没用"的情况，挡不住象征性地用一下。根本解法是由工具提供窄整数类型。
- 规格只覆盖 13 种形状，新增规格需要专家投入（写 harness、参考实现、通过准入）。
- 成本模型没有校准过。如果以后还要做性能方面的建议，要先用 `mocheck/bench/` 这类实测去校准成本表。

## 已经排除的方向

- 给 rigtorp/MPMCQueue 提放宽内存序的 PR：x86 机器码完全相同，ARM 上实测收益 ≤ 3%，维护者没有理由接受。
- 在云端会话里用浏览器下载 Dat3M：release 只有源码包，依赖必须从 Maven Central 拉。
