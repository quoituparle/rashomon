# mocheck 交接文档

写于 2026-11-23，从云端会话转到本地。技术细节以 `mocheck/README.md` 为准，这里讲**现状、怎么跑起来、下一步做什么**。

## 一句话现状

C++ 内存序判官：AI 按规格写实现，mocheck 给出 PASS / FAIL（带反例）/ INCONCLUSIVE。13 个规格全部通过准入，72 项回归 0 失败。已经能当 CI 守门员用，但还算不上"Lean 级可信"，主要差在判官本身（GenMC + 我们的预处理）还不可信。下一步就是补这个洞：用 Dartagnan 交叉判决。

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

## 下一步：Dartagnan 交叉判决（优先级最高）

**为什么**：这轮最大的教训是判官会说谎（GenMC 两次给出错误结论且不报错）。让两个独立的检查器都同意才判 PASS，可信度会比加规格高得多。

**已经摸清的**：
- Dat3M 4.5.0（tag `4.5.0`，commit `0d56990`）需要 Java 25 + Maven ≥ 3.8，`mvn clean install -DskipTests`。产物是 `dartagnan/target/dartagnan.jar` 加 `dartagnan/target/libs/`。
- 云端卡住的原因：会话网络访问不了 `repo.maven.apache.org`。本地没有这个限制。
- Dartagnan 能直接读 `.ll`。用法：`java -jar dartagnan.jar cat/rc11.cat --target=c11 prog.ll`，C 程序要设 `DAT3M_HOME`。
- 我们的 harness 只用了 `assert` 和 `pthread`，没有 GenMC 专属 API，大概率能原样使用。

**建议的做法**：
1. 先手动试：用 clang++-19 把一个 harness 编成 `.ll`（加上和 `run_scenario` 一样的 `-D` 宏，GenMC 兼容层大概不需要）喂给 Dartagnan，看它能不能解析 libstdc++ 的 `std::atomic`。最可能出问题的就是这一步。
2. 用已知结论的例子比对：`examples/queue_ok.hpp`（应 PASS）、`queue_bug.hpp`（应 FAIL）、`seqlock_signal_fence.hpp`（GenMC 单独会错判 PASS，Dartagnan 理应判 FAIL）。
3. 能用的话，在 `mocheck.py` 里加 `--second-judge dartagnan`。判决规则：两边都 PASS 才算 PASS，任一边 FAIL 就算 FAIL（附反例），两边不一致时判 INCONCLUSIVE 并报告分歧。
4. 注意 Dartagnan 是有界模型检查（BMC），自旋循环需要循环展开上界（`--bound`）。上界不够时它应该报出来，不能静默当作 PASS。这一点要专门写一个用例验证。

## 其他已知问题和想法

- 预处理改写（`run_scenario` 里那一串 `re.sub`）是用正则改 C++ 源码，很脆弱。长期应该换成基于 clang AST 的改写。
- 缩参数依赖实现真的用了 `BITS` 这类模板参数。lint 只能挡住"完全没用"的情况，挡不住象征性地用一下。根本解法是由工具提供窄整数类型。
- 规格只覆盖 13 种形状，新增规格需要专家投入（写 harness、参考实现、通过准入）。
- 成本模型没有校准过。如果以后还要做性能方面的建议，要先用 `mocheck/bench/` 这类实测去校准成本表。

## 已经排除的方向

- 给 rigtorp/MPMCQueue 提放宽内存序的 PR：x86 机器码完全相同，ARM 上实测收益 ≤ 3%，维护者没有理由接受。
- 在云端会话里用浏览器下载 Dat3M：release 只有源码包，依赖必须从 Maven Central 拉。
