# HZB camera recovery（M3 phase C）验证

日期：2026-10-21；基线 `4db78b04`。计划：`docs/superpowers/plans/2026-10-21-hzb-recovery.md`；规格：`docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md`。

## 行为

`history cull → visibility raster CLEAR → 本帧 HZB build → recovery → visibility raster LOAD → resolve`。第二遍只复测历史预测拒绝的候选；需要恢复的记录按原候选顺序追加到第一遍 survivor 后，fetch 和 packed meshlet ID 使用同一偏移。第一遍的记录、计数与 ID 不被第二遍重新解释。

- 使用既有 indirect buffer 的第四组命令（每帧槽4×65536条），无新 RHI 接口或 ABI 变化。
- 保留一次 CPU 候选选择，第二遍只重放冻结的 push/cull/item-index，避免重复选层或覆盖第一组命令。
- 图声明 recovery 的 results/survivors/commands 为 read-write；第二个 raster 的 ID/depth 是 read-write、LOAD。跨 pass 同步仍全部由图负责。
- flags bit0 保留历史拒绝决定，bit1 标记本帧复测后保留；有效值0/1/3。第一遍 prefix、第二遍 suffix、两组实际命令分别验证。
- sentinel 无效结果保留原始第一遍绘制，第二遍为0；当前 HZB header 无效则恢复全部历史拒绝项。pipeline/表未就绪或运动过大，保留完整单遍绘制。

## 开关与边界

仍默认关闭。`DEREN_HZB_RECOVERY=1` 需要同时设置 `DEREN_HZB_BUILD=1` 和 `DEREN_HZB_CULL=1`。不设置 recovery 时仍为 B2 的逐字节静态门控。

预测质量门控比较当前相机与本 frame-slot 上次 build 的快照，默认：平移不超过场景半径5%，旋转不超过5°，projection最大元素差不超过0.01。三个阈值分别可由 `DEREN_HZB_RECOVERY_MAX_TRANSLATION_RATIO`、`DEREN_HZB_RECOVERY_MAX_ROTATION_DEGREES`、`DEREN_HZB_RECOVERY_MAX_PROJECTION_DELTA` 设置；无效/非正/非有限值使用默认值。它们决定预测是否值得执行，正确性来自 current-frame recovery。这是 history 间隔阈值，比规格按每帧阈值更严格，超限只影响收益。

本阶段沿用 B2 的完整 host-cut 输入范围，验收配置为 `subtilis=true`。shared/import-time 或其他不支持的 item 保留原来源，不声称这些来源获得遮挡收益。产品配置/GUI 与更宽来源留在后续阶段。

## 正确性证据

RTX 4060 Laptop，Bistro geometry，1280×720，130帧。相机轨迹使用既有 `--capture-sweep=0.02`，near初始 `35,8,42,0,-88,0`；轨迹同时改变相机朝向和世界位置。

| 场景 | 结果 |
|---|---|
| 移动相机、无 TAA | session复测5,041,585项，恢复6,435项；末帧42,990个历史判断与37,488个 recovery判断均与CPU一致，prefix/suffix稳定顺序和counts匹配；独立脚本18,420项通过 |
| 131帧、frame_slot=1 | CPU/命令/顺序验证通过，独立脚本18,421项通过；覆盖以前偶数帧截图隐藏的单lane读回索引问题 |
| TAA抖动+移动 | 修正投影guard后持续有效剔除；session复测5,040,273项、恢复7,868项；独立脚本18,421项通过；相对OFF颜色0差异、最终depth0word差异 |
| 快速转向4°/帧 | history预测门关闭，recovery不执行；相对OFF颜色0差异、depth0word差异 |
| 当前header无效注入 | 全部5,041,585个被拒绝候选均恢复，仍拒绝0项；深度逐字相同，颜色与OFF同变体相同 |
| 第一遍overflow注入 | 366,196次item overflow；recovery复测/补画均0，原始第一遍完整；深度逐字相同，颜色与OFF同变体相同 |
| 静态近景C（C1精确对照） | recovery ON 对同变体 OFF：颜色0像素差异、max0；最终depth 0 word差异（921,600 words）。另一既有变体2901px/max10，见下节 |
| 负控 | 旧B2 probe被 `--require-recovery` 明确拒绝；手动损坏第四组命令后脚本明确报告1项命令不匹配；旧B2正常dump仍通过（near-on1-probe 6,978、far-on2-probe/final-far-probe 7,044 checks） |

### 已有颜色变体与C1

近景存在已记录的两变体像素抖动（B1/B2 moving 形式为2,870pixels/max11）；本轮静态近景C的精确对照表明：

- `static-c.png`（当前 phase C recovery ON，静态 near 相机，130帧）与归档 `D:/deren-assets/hzb-b2/captures/near-perf-off1.png`、`near-off1.png` **逐像素完全相同**（0 differing pixels，max |d| 0），并与本轮 fresh OFF（当前构建、`-Depth`、validation 关、同相机同130帧）**也完全相同**（0 pixels）；fresh OFF 的最终深度与 `static-c-depth` **0 word差异**。
- 归档 `near-perf-off2.png`/`near-perf-off3.png` 是另一变体：它们彼此完全相同（0 pixels），与 static-c 差 2,901 pixels / max10（`|d|>1` 25、`|d|>4` 2）；`near-perf-off1` vs `near-perf-off2` 自身也正好差2,901/max10。因此这是既有的两变体 jitter，不是 phase C 新增。本轮早先的 `static-off.png`/`static-b2.png` 也落在这一另一变体，两者彼此完全相同，深度与 static-c 0 word差异。
- 判据：C1 用**同变体**对照（0像素）判定；跨变体2,901 pixels（约0.31%）单独列出，不并入 C1。三个静态深度读回（fresh OFF / static-off / static-b2）与 recovery ON 全部 0 word差异，未出现丢失几何。

### 验证脚本的 Important 修复（RED→GREEN）

独立审查发现的唯一 Important：`scripts/measure/hzb_cull_verify.py` 解析完整4096行 items 表后必须按 metadata 的 `item_count` 截断，否则视锥变化后旧帧留在 lane 尾部的**合法旧行**会污染计划总数。修复只在完整4096行形状断言之后执行 `items = parse_items(items_raw)[:item_count]`，其余检查一律未改。

- RED 夹具：复制真实 v3 recovery 捕获 `moving-probe-probe.*`（item_count=2840）为 `stale-tail-probe`，把 items 第4095行写成一个合法旧条目 `{meshlet_base=0, candidate_count=1, source=SOURCE_CULLED, pad=1}`（在 item_count 之外）。旧脚本 exit1、5项FAIL，首项 `the plan's candidate total matches the metadata (42991 == 42990)`；修复后同一夹具 PASS（17,164 checks，与未改动的正常 v3 捕获检查数相同）。
- 行为保持：正常 v3 捕获 PASS（旧18,420/新17,164 checks；差的1,256正好是 4096−2840 个尾部零行的逐行检查，活跃行检查一个不少）；legacy B2 dump PASS（near-on1-probe 旧6,978/新5,722；far-on2-probe 与 final-far-probe 旧7,044——即 ledger 记录的7044——新5,854）；损坏命令负控仍 exit1，且仍恰好1项 `item 0: recovery command matches pass2 count`。HEAD 已提交脚本在 legacy dump 上与修复前工作脚本报同样检查数，对照无回归。
- 证据：`.superpowers/sdd/2026-10-21-hzb-recovery/verify-fix/` 的 `EVIDENCE.md`、`static-c-comparison.txt`、`*-old.log`/`fixed-*.log`、`stale-tail-probe.*` 与生成/分析脚本。

### TAA投影guard的RED→GREEN

旧CPU/GPU guard拒绝 `P[0,2]`/`P[1,2]`，导致TAA jitter后的预测几乎全部fail-open；同时未拒绝公式真正不支持的 `P[2,0]`/`P[2,1]`（clip Z混入XY）。新增3项失败测试分别钉住正/负jitter的有效遮挡、Z剪切的fail-open，再修正两侧检查。屏幕矩形原本就用完整投影的8个角点，XY偏移不改变单调近深度界；随机逐像素保守性测试也加入jitter覆盖。

### 探针

v3分别保留历史与本帧pyramid/header，先冻结expected历史矩阵，最后在recovery后复制survivors；直接读取的commands/results显式发布到host。读回文件只含一个slot lane，CPU索引从0开始。旧格式兼容。

## 工程验证

- 收尾轮复核：`cmake --build build-release-dyn-clang64 -j 12` exit0（no work to do），完整 CTest 58/58（39.77秒），日志 `finish-pass-build.log` / `finish-pass-ctest.log`；暂停前最终格式整理版 Release 构建成功，完整CTest58/58（52.45秒），日志 `final-release-tests.log`。
- 最终相关ASan/UBSan构建成功，5/5（6.16秒）：hzb、render_graph_visibility、visibility_raster_pass、visibility_buffer、subtilis_frame。
- 对改动代码执行clang-tidy；延续既有做法排除会崩溃的 `bugprone-unchecked-optional-access`。改动行的braces/isolate-declaration/ifelse样式已定向修复，未广泛修改旧代码；trailing-comma对空range误报与既有噪声保留。
- 额外动态后端ASan整链仍有B2已记录的backend DLL sanitizer链接限制，本轮不扩展该构建策略。
- 独立审查完成：无Critical；唯一的1项Important（验证脚本按item_count截断）已修复并有上述RED→GREEN与负控证据；渲染主链未发现其他可行动问题。
- 静态近景C与颜色变体精确对照完成（见上节）；三轮交错 OFF/recovery 性能测量完成（见下节）。
- 临时shader注入已恢复并重编译，无生产注入开关；本轮未改源码/着色器/测试/CMake/config。

## 性能

### 方法

RTX 4060 Laptop，Bistro geometry，1280×720。moving 视点（phase C 的用途）：near `35,8,42,0,-88,0`、far `35,18,140,0,-83.071,0`，`--capture-sweep=0.02`（130帧内 yaw 约+2.58°）、130帧；subtilis=true。每个视点3轮交错，每轮先 OFF（不设 `DEREN_HZB_CULL`/`DEREN_HZB_RECOVERY`，即无 HZB 的基线路径）再 `-Recovery`（BUILD+CULL+RECOVERY），共12次启动（two-view × 3 rounds × 2 arms）。validation 关闭、probe 关闭；测量期间无并发构建/测试/其它捕获；12次启动总耗时164.8秒。数值取每次启动日志的 “session avg of 129 frames” GPU 行与最后一个 “cpu frame phases (avg of 60 frames)” 窗口；delta = ON中位数 − OFF中位数。原始日志与解析脚本在 `.superpowers/sdd/2026-10-21-hzb-recovery/perf/`。

所有6次 ON 启动都记录 `hzb recovery: prediction gate OPEN`，recovery 为真实两遍链：near session 5,041,585 复测 / 6,435 恢复，far 4,705,395 / 6,778；cull session near 5,041,585被剔除（90.79%）、far 4,705,395（71.79%）；三轮的 session 计数完全相同（同一确定性负载）。

### 逐轮 GPU（session avg of 129 frames，ms）

| 视点 | 轮 | OFF total | OFF raster | ON total | ON raster | ON cull | ON build | ON recovery | ON vis-recovery |
|---|---|---|---|---|---|---|---|---|---|
| near | 1 | 22.48 | 20.90 | 4.80 | 2.80 | 0.22 | 0.47 | 0.23 | 0.24 |
| near | 2 | 22.44 | 20.84 | 4.77 | 2.75 | 0.22 | 0.46 | 0.20 | 0.24 |
| near | 3 | 22.29 | 20.72 | 4.87 | 2.83 | 0.22 | 0.48 | 0.22 | 0.23 |
| far | 1 | 25.06 | 23.78 | 9.66 | 7.61 | 0.48 | 0.51 | 0.22 | 0.24 |
| far | 2 | 26.89 | 25.46 | 9.58 | 7.52 | 0.47 | 0.52 | 0.22 | 0.23 |
| far | 3 | 25.13 | 23.81 | 9.66 | 7.61 | 0.48 | 0.51 | 0.22 | 0.24 |

### 逐轮 CPU（最后一个60帧窗口，ms）

| 视点 | 轮 | OFF post | OFF scene | ON post | ON scene |
|---|---|---|---|---|---|
| near | 1 | 5.63 | 0.14 | 7.19 | 0.12 |
| near | 2 | 6.37 | 0.17 | 5.82 | 0.07 |
| near | 3 | 5.54 | 0.12 | 6.89 | 0.11 |
| far | 1 | 6.45 | 0.15 | 6.61 | 0.09 |
| far | 2 | 5.87 | 0.13 | 6.69 | 0.10 |
| far | 3 | 5.90 | 0.12 | 7.60 | 0.12 |

### 3轮中位数与 ON−OFF 增量

| 视点 | 指标 | OFF 中位 | ON 中位 | ON−OFF |
|---|---|---|---|---|
| near | GPU 帧总时间 | 22.44 | 4.80 | **−17.64（−78.6%）** |
| near | GPU visibility raster | 20.84 | 2.80 | −18.04（−86.6%） |
| near | GPU cull/build/recovery/vis-recovery | — | 0.22/0.47/0.22/0.24 | HZB pass 合计 +1.15 |
| near | CPU post+scene | 5.77 | 7.00 | **+1.23** |
| far | GPU 帧总时间 | 25.13 | 9.66 | **−15.47（−61.6%）** |
| far | GPU visibility raster | 23.81 | 7.61 | −16.20（−68.0%） |
| far | GPU cull/build/recovery/vis-recovery | — | 0.48/0.51/0.22/0.24 | HZB pass 合计 +1.45 |
| far | CPU post+scene | 6.02 | 6.79 | **+0.77** |

端到端墙钟（同一 CPU 窗口的 total 中位数）：near OFF 22.01 ms → ON 9.12 ms；far OFF 24.92 ms → ON 9.06 ms。逐轮 CPU post+scene 差值为 near +1.54/−0.65/+1.34、far +0.10/+0.79/+1.70（中位 +1.34/+0.79）；轮间波动约 ±1 ms。

### CPU 0.2 ms 预算判定

规格 §0.2/§1.2 的 0.2 ms 预算是“`record_visibility_items` **之外**的 CPU 增量”（新增的是两张表写入与一次矩阵快照比较）。本表测量的 `post`+`scene` 是整帧 CPU 相位窗口均值，`post` 包含 graph 录制（其中包含 `record_visibility_items` 内的候选/replay 录制），因此它不是该预算量的精确测量，而是端到端 post 对比。

以该指标如实判定：**ON−OFF 中位增量 near +1.23 ms / far +0.77 ms，超过 0.2 ms 预算**；逐轮值有 ±1 ms 波动，但近/远两个视点的中位与多数轮次为正，且与暂停前 ledger 的 prelim noVal post 4.96 ON vs 4.26 OFF 方向一致。B2 归档同指标的 ON−OFF 约 −0.1 ms（无退化），phase C 新增的约0.8–1.2 ms 与第二遍 raster replay/录制一致，但本轮没有 instrumentation 能把 `record_visibility_items` 内外的部分分开。结论：**不宣称满足 0.2 ms CPU 预算**；GPU 收益（near −78.6%/far −61.6% 帧总时间）远大于该 CPU 增量，精确测量与优化保留为 phase D 的 CPU 剖析项。

与 B2 归档（同方法，上一构建）对比：near 21.80→5.09 ms、far 24.47→10.62 ms；本轮 near 22.44→4.80、far 25.13→9.66，同量级（ON 端差异在机器噪声内，且 phase C 还多出 recovery/第二遍 raster 标记）。

## 完成状态（2026-10-21 恢复轮）

暂停后的四项收尾全部完成，本轮未改源码/着色器/测试/CMake/config，未做任何 git 变更（不 add/commit/checkout；本地提交由 Lead 执行，之后按授权归档到 D 盘并 SHA256 校验）。

1. 审查 Important 修复完成，RED→GREEN 与正常/legacy/损坏命令负控证据齐全（见“验证脚本的 Important 修复”）。证据目录 `.superpowers/sdd/2026-10-21-hzb-recovery/verify-fix/`。
2. 静态近景C精确对照完成：同变体颜色0像素、最终depth 0 word；另一变体 2,901px/max10 为既有两变体 jitter，单列不并入 C1（见“已有颜色变体与C1”）。
3. 性能三轮交错测量完成：GPU 帧总时间 near −78.6%、far −61.6%；CPU post+scene 中位增量 +1.23/+0.77 ms，如实记录超过 0.2 ms 设计目标（见“性能”）。原始日志 `.superpowers/sdd/2026-10-21-hzb-recovery/perf/`（12份日志 + run-summary.txt + analysis.md/analyze_perf.py）。
4. 本文件、`docs/superpowers/plans/2026-10-21-hzb-recovery.md` 复选框与 `.superpowers/sdd/2026-10-21-hzb-recovery/progress.md` 完成条目已更新。

验证状态：最终 `cmake --build build-release-dyn-clang64 -j 12` exit 0；`ctest --test-dir build-release-dyn-clang64 --output-on-failure` 58/58；工作区仅剩预期的 phase C 实现 + 验证脚本修复 + 两份文档（`.superpowers` scratch 未跟踪）。

证据归档（2026-10-21 Lead 验收后）：`D:/deren-assets/hzb-recovery`，268 个文件 / 508.1 MB，逐文件 SHA256 校验 0 mismatch、0 extra（manifest：`D:/deren-assets/hzb-recovery/archive-manifest.json`）；本地 `.superpowers/sdd/2026-10-21-hzb-recovery` scratch 按既有做法保留（未跟踪）。Lead 提交：`bfd77a6e`（实现 + 脚本修复）、`7ac980e6`（本报告与计划）。

残余风险/限制：

- CPU 端到端 post 增量超出 0.2 ms 设计目标，且本轮未用 instrumentation 分离 `record_visibility_items` 内外的增量；phase D 若按该预算验收需要 CPU 剖析/优化。
- CPU 数字是130帧会话最后一个60帧窗口，轮间波动约 ±1 ms；已按要求给三轮中位数与逐轮值，但这不是显著性检验。
- 近景两变体像素差异（本轮静态 2,901px/max10；B1/B2 moving 形式 2,870px/max11）是既有平台现象；C1 由同变体 0 差异满足，跨变体差异不并入 C1。
- 快速运动门控关闭、far 静态“0剔除/逐像素相同”、TAA-jitter 移动等行为沿用暂停前证据，本轮测量只覆盖 moving 视点的性能，未重跑这些正确性场景。
- ASan 5/5 与 clang-tidy 结论沿用暂停前最终产物（本轮无源码改动）；动态后端 ASan 仍受 B2 已记录的 backend DLL sanitizer 链接限制。
- 默认仍关闭：`DEREN_HZB_RECOVERY=1` 还需 `DEREN_HZB_BUILD=1`+`DEREN_HZB_CULL=1`；产品配置/GUI 留给 phase D。
