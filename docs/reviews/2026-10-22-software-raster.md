# M4 软件光栅验收（2026-10-22）

状态：**Task 1（subpixel workload 与硬件上界，GO/NO-GO）完成，判决 GO。** T2+ 未开始（按计划 T1 是硬门，GO 后才由下一步决定 T2 开工）；默认 OFF 未改变，生产路径无新增开关/资源/pass；证据已按逐文件 SHA-256 归档到 `D:/deren-assets/2026-10-22-software-raster/`。本文件在 T2 开工后继续追加后续 Task 的验收节。

计划：`docs/superpowers/plans/2026-10-22-software-raster.md`（Task 1）；规格：`docs/superpowers/specs/2026-10-22-software-raster-design.md` §3.1/§3.4/§5.1/§7.1-§7.4；基线 `cde71c51`。

## Task 1 — subpixel workload 与硬件上界（GO/NO-GO）

**结论：** far 相机的 `visibility raster` 三轮交错中位数差为 **1.54 ms**，等于 M1c far ON 20.11 ms 的 **7.66%**（上界门 ≥1.5%，即 ≥0.302 ms）→ **GO**。near 记录为 0.09 ms（计划不要求，不作为 GO 条件）。同时，Task 1 的容量实测推翻了 spec §9 Q7 的“32 或 64 条/tile”前提：far 在 1.0 px 档单 tile 最多 **977** 条 entry、2.0 px 档 **1907** 条，near 为 226/290；**32 与 64 都会每帧 overflow**，容量决定与 T2 设计输入见 §7。

### 1. 资产、环境与二进制身份

| 项 | 值 |
|---|---|
| 场景资产 | M1c 已验收的 Bistro 派生 cooked GLB `D:/deren-assets/2026-10-21-object-lod/bistro-object-lod.glb` |
| 资产 SHA-256 | `620f424239909d023b30dd86fe953c579298a39ef2d4706bca59fc0d2982085b`（与 M1c 归档一致，未重建） |
| renderer | `build-release-dyn-clang64/deren.exe`（Release），SHA-256 `b73675586e667b8f02bec9cbb1ec9edcbaea4fa591f62f09ad1abea9131b9a49` |
| GPU / 驱动 | NVIDIA GeForce RTX 4060 Laptop GPU，617.42，8188 MiB |
| 分辨率 / 帧数 | 1280×720 / 130 frames/次（GPU timing 会话为 129 帧，mesh stats 会话 131 帧——本报告的可解释性检查按实测帧数推导，不假设） |
| 相机 | far `35,18,140,0,-83.07,0`；near `35,8,42,0,-88,0`（与 M1c 相同，静态相机） |
| 对照 | baseline = M1c ON 路径（`subtilis=true`、软件光栅 OFF，即本任务默认路径）；holes = 同一配置 + capture-only 环境探针 |

### 2. 被测实现与探针（capture-only，默认 OFF）

新增的纯 CPU 前端（Task 2/3 的生产逻辑将由它派生）：

- `source/engine/visibility/visibility_software.cppm`：`classify`（spec §3.1 的保守判据：模型线性 3x3 的 Frobenius 范数作半径上界、球完全在近面前且留 0.98 余量、透视用球最近点的直径上界、正交用 `r*max(|p00|*w,|p11|*h)`、阈值比较严格 `<`）、`project`（保守屏幕包围盒/tile 范围）、`split_records`（稳定 `[hardware 前缀 | software 后缀]` 拆分 + 64k 存储的 order 输出；后缀第 k 条 id = hardware_count + k，与 §3.3 的 id 解码一致）、`build_tile_lists_measure`（测量用：精确 per-tile 计数/最大值/overflow）与 `build_tile_lists`（生产用：overflow 直接拒绝整帧）。只依赖 `deren.engine.visibility`/`deren.engine.meshlet`/vstd，无 Vulkan。
- `source/tests/test_visibility_software.cpp`：阈值边界（含严格 `<` 与 nextafter）、透视/正交、非均匀 scale/shear、跨近面/负半径/NaN 全部 fail-open、投影包围盒、稳定拆分与 id 映射、tile 确定性/容量/overflow、1.0 vs 2.0 计数。HEAD 上 RED（模块缺失编译失败）→ 实现后 **170 checks PASS**。
- `source/engine/runtime/runtime.cpp` 的 capture-only workload 探针：`DEREN_SOFTWARE_RASTER_PROBE=workload`（可选 `DEREN_SOFTWARE_RASTER_PROBE_DUMP` 指定 JSON 路径，默认工作目录 `software-raster-workload.json`）。环境开关只读一次；无 env 时代码路径完全不变（frozen 15/15 + 1/1 是逐字节见证）。armed 时**每个** visibility 记录帧（subtilis 活跃、且 HZB cull/recovery 不活跃）在 M1c 的 planning 阶段分类并重写 run lane 为 `[hw|sw]`，硬件 dispatch 只覆盖 hw 前缀——这就是 holes control；software 后缀在本任务不绘制，因此截图存在洞是**预期测量状态**。截图帧另外用 host 内存副本做两档阈值（1.0/2.0 px）的整帧 tile 重新推导、与 phase 2 拆分逐 item 交叉校验（不匹配即写 `SELF-CHECK FAILED` 且分析脚本拒绝），并写确定性 JSON（extent/camera view+proj/pose、阈值、tile 网格、总量、容量统计、成本、per-item model+计数、两档 tile 计数数组）。
- **为什么没有配置键**：本探针只服务 capture-only 测量（`--capture-frames` 脚本运行），不是产品行为；spec §4.2 的产品配置（`software_raster`/`software_raster_threshold_px`、runtime setter）属于 Task 4，本任务不提前加键，也没有改 config parser/example（因此 `test_app_config` 未被触碰）。
- 默认 OFF 的守卫：无 env 的 baseline run 不产生 dump、不打印 armed 行、`probe_armed=false`（分析脚本强制校验）；frozen Full/Visibility 逐字节不变。探针对 lane 写入的每个 item 在截图帧抽样回读（prefix 首条、边界、末条）计入 self-check；全 lane 回读在本机上是 host-visible 映射内存的慢速读（一次 2 MiB 约数百 ms），因此没有作为每帧成本的一部分，而 host 副本承担模型/记录重推导。

### 3. 测量方法

- 每个相机 3 轮**交错**：每轮 baseline 与 holes 相邻运行，奇数轮 baseline→holes、偶数轮 holes→baseline（消除顺序/热漂移只偏向一臂的可能）；2 相机 × 3 轮 × 2 臂 = 12 次启动，另加 1 次 validation 开启的 far holes smoke（13 次，单次 15–24 s）。
- 全部验证关闭（除 smoke）、probe/gpu_timings 按 M1c 配置；测量期间无并发构建/测试/其它捕获；同一资产/相机/帧数；每次启动的原始日志、截图、probe JSON、run.json 全部保留。
- GPU 数字取日志 `gpu pass timings (session avg of 129 frames)` 的 `visibility raster` 与 `total`；中位数为 3 轮中位数（逐轮表保留、不取最好轮）。upper bound = baseline − holes 的 `visibility raster` 中位数差（holes 不绘制 software 后缀，因此该差值不含软件侧成本，是上界）。
- 本机 renderer 进程**无法在 D: 创建文件**（M1c 会话已记录同样的限制）：正式矩阵在 `build-release-dyn-clang64/m4-t1-runtime/` staging 产生，验证 SHA-256 后复制到 D: 归档（见 §9）。注意归档的 `runtime/measurements.json` 保留原 staging 的绝对路径；staging 清理后若要从归档重跑 `--analyze`，先把其中的 `.../m4-t1-runtime/<tag>/` 映射到 `D:/deren-assets/2026-10-22-software-raster/runtime/<tag>/`（截图/日志/workload JSON 均已归档），或直接把归档恢复成 staging 目录名再运行。复现命令（workspace 恢复后）：
  ```
  python scripts/measure/software_raster_roi.py --self-test
  python scripts/measure/software_raster_roi.py --capture --work build-release-dyn-clang64/m4-t1-runtime
  python scripts/measure/software_raster_roi.py --analyze build-release-dyn-clang64/m4-t1-runtime/measurements.json \
      --out docs/reviews/2026-10-22-software-raster/task1-upper-bound.json
  ```

### 4. 帧数据（3 轮 + 中位数）

far：

| 轮 | baseline raster ms | holes raster ms | Δ raster ms | Δ % M1c(20.11) | GPU total Δ ms | 移除 workgroups | 移除 triangles |
|---|---|---|---|---|---|---|---|
| 1 | 19.18 | 17.64 | 1.54 | 7.66% | 1.51 | 554,523 | 38,940,536 |
| 2 | 19.65 | 18.13 | 1.52 | 7.56% | 1.54 | 554,523 | 38,940,536 |
| 3 | 20.00 | 17.72 | 2.28 | 11.34% | 2.29 | 554,523 | 38,940,536 |
| **中位** | **19.65** | **17.72** | **1.54** | **7.66%** | **1.54** | 554,523 | 38,940,536 |

near（记录用，不作 GO 条件）：

| 轮 | baseline raster ms | holes raster ms | Δ raster ms | Δ % M1c(18.58) | GPU total Δ ms | 移除 workgroups | 移除 triangles |
|---|---|---|---|---|---|---|---|
| 1 | 17.97 | 16.61 | 1.36 | 7.32% | 1.48 | 111,350 | 5,144,501 |
| 2 | 17.30 | 17.21 | 0.09 | 0.48% | 0.09 | 111,350 | 5,144,501 |
| 3 | 16.97 | 16.92 | 0.05 | 0.27% | −0.10 | 111,350 | 5,144,501 |
| **中位** | **17.30** | **16.92** | **0.09** | **0.48%** | **0.09** | 111,350 | 5,144,501 |

读法：far 的 Δ 远高于门限（门限 0.302 ms）；单轮 baseline 有约 ±0.4 ms（约 2%）的机器噪声，因此用 3 轮交错中位数。near 的可让出工作量只有 far 的约 1/5（850 vs 4,233 clusters），且 near baseline 自身在轮间漂移 17.0–18.0 ms，因此 near 的增益低于本机的分辨能力——如实记录，不用于判决。

### 5. 分类规模、可解释性与独立复算

两档阈值（1.0 主口径 / 2.0 测量档）的分类规模（每帧，from probe JSON，3 轮一致）：

| 相机 | cut clusters | sw 1.0 | sw 1.0 占比 | sw 2.0 | sw 2.0 占比 | sw tri 1.0 占比 | sw tri 2.0 占比 |
|---|---|---|---|---|---|---|---|
| far | 41,030 | 4,233 | 10.32% | 10,923 | 26.62% | 8.96% | 25.27% |
| near | 35,444 | 850 | 2.40% | 2,284 | 6.44% | 1.37% | 5.17% |
| M1c 探针 far（对照） | 41,030 | — | 23.28% | — | 38.23% | 21.92% | 36.75% |
| M1c 探针 near（对照） | 35,444 | — | 5.37% | — | 15.95% | 4.12% | 14.46% |

本判据比 M1c 探针更保守（Frobenius 半径上界 + 球完全在近面前 + 严格阈值），因此两档占比都**低于**探针比例（far 10.32% ≤ 23.28%，near 2.40% ≤ 5.37%），符合 spec §3.1 的“可解释”要求，不是抹平差异。

可解释性（holes 相对 baseline 的整段会话差分）：

- far 移除的 workgroups = 554,523 = 4,233 × 131（每帧 software 簇数 × mesh-stats 会话帧数，帧数由 baseline 自身反推而非假设）；移除 triangles = 38,940,536 = 297,256 × 131，**逐字节精确**。
- near 移除 workgroups = 111,350 = 850 × 131；移除 triangles = 5,144,501，对应候选三角形 39,285 × 131 = 5,146,335 的 99.96%（差 0.04% 来自 mesh 阶段对背面三角形的剔除：near 会话 emitted triangles 比 candidate 少 1.77%），方向与幅度一致。
- 独立复算：用归档的 M1c `DEREN_CLUSTER_PROBE_DUMP`（同资产、同相机、同帧的 41,030 条 cut 记录），在 Python 中独立实现同一保守判据与 tile binning，得到 **4,233 / 10,923** 个 subpixel cluster 与**完全相同**的 tile 直方图（129 个非零 tile、max 977、>32 的 33、>64 的 18）；runtime 相机 view/proj 与归档逐字节相等。脚本与输出见归档 `m4-t1-crosscheck.py/.txt`（`logs/`）。
- 图像 sanity（far round 1，截图逐像素）：baseline vs holes 只有 **64 / 921,600（0.0069%）** 个像素不同，最大通道差 41，集中在 11 个 tile。两个含义： (a) subpixel 簇确实几乎不覆盖像素，硬件让出的 1.54 ms 主要来自 mesh/workgroup/顶点侧而不是像素填充——这同时支持“软件 raster 的像素工作量小”与“上界合理”；(b) lane 重排后的硬件前缀重绘与原图**没有**出现广泛差异（若 id/记录映射broken 会成为整帧错误而不是 64 像素），是本任务的帧级正确性 sanity，不是正式 A/B（正式 A/B 属 Task 3/6）。

### 6. CPU 成本

| 相机 | classify 工作/帧（session avg，8-worker planning 池内求和） | classify 截图帧样本（含 host 副本/回读） | tile binning 截图帧（两档串行） |
|---|---|---|---|
| far | **2,748 µs** | 7,936 µs | **4,096 µs**（1.0: 2,131 + 2.0: 1,987） |
| near | **2,212 µs** | 4,257 µs | **3,402 µs**（1,725 + 1,649） |

口径：分类发生在 M1c 已有的逐 item planning 阶段（并行池），上表 classify 是各 worker 计时之和（不是墙钟），每帧约 41k/35k 条记录；截图帧样本额外包含 2 MiB 级 host 副本与 lane 抽样回读，只用于证据，不作为每帧成本。tile binning 的 4.1/3.4 ms 是**测量侧**从零重新分类并同时构造两档 tile list 的串行成本（为了独立交叉校验）；生产实现（T4）应消费 phase 2 已经算好的分类结果、只 bin 一次（约 4k 条 software 记录），其增量成本必须在 T4 重测。**Task 6 的 CPU 门限（+0.3 ms/帧）是 T4 的设计目标，本轮数字不能直接当作满足或违反**：分类 work 求和已明显大于 0.3 ms，但并行后墙钟约 work/8，仍需 T4 的实测。

### 7. per-tile 容量统计（1.0/2.0）与 Q7 决定

每帧 per-tile entry 统计（3 轮一致，probe JSON 的精确计数，capacity=32 仅作分布测量、不截断）：

| 相机 / 阈值 | tile 数 | 非零 tile | 单 tile 最大 entry | overflow tiles @32 | overflow tiles @64 | 每帧是否 overflow |
|---|---|---|---|---|---|---|
| far 1.0 px | 3,600 | 129 | **977** | 33 | 18 | 是（全部 3 轮） |
| far 2.0 px | 3,600 | 170 | **1,907** | 48 | 39 | 是（全部 3 轮） |
| near 1.0 px | 3,600 | — | **226** | 4 | 4 | 是（全部 3 轮） |
| near 2.0 px | 3,600 | — | **290** | 20 | 7 | 是（全部 3 轮） |

**Q7 决定（证据驱动，不是假设）：Q7 的“32 或 64”两个候选都不成立。** 实测最大值为 far 977（1.0）/1907（2.0）、near 226/290，最小可用 2 的幂为：

- far：1.0 px → **1024**（720p 下 3600×1024×8B ≈ **29.5 MiB/frame-lane**）；2.0 px → **2048**（≈59 MiB/lane）；
- near：1.0 px → **256**，2.0 px → **512**。

这不是一个“默认值 32/64”的小改动，而是 T2/T3 的设计输入：要么接受 1.0 px 默认容量 1024–2048 的 tile buffer 尺寸/带宽（并重新评估 §6.1 的 0.92/1.84 MiB 估计），要么在 T2 采用溢出列表/多级 tile 列表等策略，而不是“overflow → 整帧回退”（后者在 far 每帧都会触发，hybrid 永远不生效）。此外分布极度集中：far 只有 **129/3600** 个 tile 非零，最热 tile 977 条——软件 raster 的 per-tile 列表长度呈长尾，T2 的逐像素扫描成本模型需要用这份分布复核。**需要在 T2 开工前由用户/Lead 确认容量策略**（spec §9 Q7 原本即列为 Task 1 后决策）。

**Q7 decision (2026-10-22): CSR sparse tile lists adopted.** 用户批准把固定 per-tile 容量改为 **CSR 稀疏列表**：per frame lane = `header{tiles_x,tiles_y,entry_count,overflow,cluster_count} + counts[tiles] + offsets[tiles+1] + entries[entry_count]`（entry = `{item_index:u32, meshlet_id:u32}` 8B），host 两遍扫描 + 独占前缀和构造，tile 范围 = `[offsets[t], offsets[t+1])`；全局预算默认 **65,536 entries（512KiB）/lane**（720p offsets/counts ≈28.8KB，1080p ≈65.3KB），只有 `entry_count > budget` 时整帧 hardware-only（日志 + stats，绝不截断），**没有 per-tile 容量/overflow**。依据即本节数据：far 1.0/2.0 总量 4,572/12,557 entries（129/170 个非零 tile）、最热 977/1,907；固定 1024/2048 会为 4.5–12.5k 实际 entries 预留 29.5/59 MiB per lane，因此“per-tile 固定容量”前提不成立。设计 A / 32-bit id / prefix-suffix / resolve-before-merge / HZB 互斥 / ROI 门均不变；spec §3.4/§5.4/§6.1/§9.2 Q7 与 plan T2–T5 已同步更新。该决定解除了 §10 的待决策项。

### 8. GO/NO-GO 判决

| 项 | 结果 |
|---|---|
| GO 条件（plan Task 1）：far `visibility raster` 3 轮中位数 Δ ≥ 1.5% × M1c far ON 20.11 ms = **0.302 ms** | **1.54 ms（7.66%）→ 通过** |
| near 记录（不作条件） | 0.09 ms 中位数（轮间 1.36/0.09/0.05，低于本机 near 分辨能力） |
| 分类计数可解释性 | 通过（§5：workgroups 精确、far triangles 精确、独立复算一致、two-档占比低于 M1c 探针） |
| 容量/信息是否构成否决 | 不构成 GO 否决（上界门只看 GPU 让出量），但**容量策略是 T2 的前置决策**（§7） |
| **判决** | **GO**（T2+ 可以开工；本任务按指示未开始 T2/T3） |

上界口径的限制：holes control 只证明“硬件不画这些簇”能省多少，**不包含** software raster/merge 的成本与带宽；T6 的 ROI 门必须用完整 hybrid 实测，本 GO 只是“值得实现”的门。

### 9. 门禁与验证

- 构建/测试：`cmake --build build-release-dyn-clang64 -j 12` exit 0（最终复核 no work to do）；`ctest --test-dir build-release-dyn-clang64 --output-on-failure` **62/62**（45.69 s；全部文档/计划编辑完成后的最终复核 35.56 s，均含新 `test_visibility_software`；`test_docs` 在列）。converter standalone `ctest --test-dir build-subtilis` **6/6**（未触碰 converter）。
- 默认 OFF 冻结：`check_render.ps1 -Full -Compare docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt` → **15/15 matched，0 mismatched**；`-Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt` → **1/1 matched**（证明无 env 时帧逐字节不变）。
- validation：validation 开启的 far holes smoke capture 退出 0、**0 error/0 warning/0 VUID**（`validation-far-holes`）；正式矩阵按计划 validation 关。hole 的出现只在 env 探针开启的 holes 臂，baseline 臂 `probe_armed=false` 且无 dump（分析脚本强制）。
- RED→GREEN：RED = 新测试 target 在模块缺失时编译失败（`use of undeclared identifier 'sw'`，日志 `logs/m4-t1-red-build.log`）；GREEN = 模块实现后 `test_visibility_software` **170 checks PASS**；analysis gate `--self-test` = 合法矩阵 + **13 项负向控制**（缺轮次/重复轮次/失败 capture/短 capture/validation 脏/非有限指标/baseline 误开探针/holes 缺 dump/探针 self-check 失败/总量不自洽/移除工作量不可解释/伪造指标/低于门限必须 NO-GO）全部按预期拒绝或通过。诚实说明：**probe 的“实现前 dump 缺失”RED 没有单独捕获**（探针在首次 capture 前已实现，顺序受工具准备影响）；等价守卫在 GREEN 侧：无 env 无 dump/无 armed 行 + frozen 逐字节不变 + 分析脚本拒绝缺少 dump 的 holes 轮。
- 证据归档：`D:/deren-assets/2026-10-22-software-raster/`，**98 个文件 / 127,288,969 B**，逐文件 SHA-256 manifest（`sha256-manifest.json`）**0 mismatch**（写入后由独立读取 manifest 的复算再次核对，0 mismatch；含最终 build/CTest 日志）；含 73 个 runtime 原始文件（13 个 run 目录的 debug.log/截图/run.json/workload JSON/config）、`measurements.json`、独立复算脚本/输出、全部门禁与构建/测试日志（`logs/`，19 个文件）、`environment.json`、`archive-verify.py`。仓库只留本报告 + `docs/reviews/2026-10-22-software-raster/task1-upper-bound.json`（约 11 KB 结果摘要）+ `task1-analysis.md`（分析脚本输出）。staging 目录已在归档校验后清理。
- `git status`：仅本任务预期文件（见 §11 文件清单）与用户既有未跟踪文件（`.vscode/`、两个旧 CI checkpoint 文档）；未做任何 git 变更（不 add/commit/push）。

### 10. 限制、未测项与 T2 输入

- 单机 / 单驱动（RTX 4060 Laptop，617.42）、固定两个静态相机、1280×720；数字不可当作发布基准。
- baseline 的单轮 `visibility raster` 有 ±0.4 ms（~2%）机器噪声；near 的轮间漂移与本任务可让出量同量级，因此 near 的收益**未被分辨**（记录而不解释）。far 的 3 轮方向一致且中位数 1.54 ms 远高于噪声与门限。
- 2.0 px 档的 holes control 未单独跑 GPU（holes dispatch 固定用 1.0 px 前缀）；2.0 px 的规模/tile 统计来自同一帧的 CPU 分类重推导（§5/§7 已说明），这满足 Task 1 “记录两档容量数据”的要求，但不是 2.0 px 的独立 GPU 上界。
- 上界不含 software raster/merge 的成本、带宽、resolve 候选选择的额外读；也不含 tile buffer 的写入带宽。T6 才能判决 ROI。
- CPU：分类 2.7/2.2 ms work/帧（并行池口径）+ T4 需要把 binning 增量压到 Task 6 的 +0.3 ms 门限内；本轮测量侧的 4.1/3.4 ms 两档串行 binning 是证据成本，不是生产设计。
- 容量：见 §7，**Q7 前提被实测推翻，T2 开工前需要用户/Lead 决策**（1024/2048 默认、或换溢出策略、或缩小参与分类的 tile 区域/阈值）。
- T2 输入摘要：`classification = 10.32%/26.62%`（far 1.0/2.0）、`2.40%/6.44%`（near）；移除工作量 far 每帧 4,233 clusters / 297,256 candidate triangles（8.96%），near 850/39,285；tile 最大 977/1907（far）、226/290（near）；upper bound far 7.66% / 1.54 ms。

### 11. 本任务文件清单

- 新增：`source/engine/visibility/visibility_software.cppm`、`source/tests/test_visibility_software.cpp`、`scripts/measure/software_raster_roi.py`、`docs/reviews/2026-10-22-software-raster.md`、`docs/reviews/2026-10-22-software-raster/task1-upper-bound.json`、`docs/reviews/2026-10-22-software-raster/task1-analysis.md`。
- 修改：`source/engine/runtime/runtime.cpp`（capture-only workload 探针，env-only，默认路径不变）、`CMakeLists.txt`（新模块注册 + `test_visibility_software` 加入 `VR_TEST_TARGETS` + 该测试链接）。`.github/workflows/ci.yml`（`test_visibility_software` 加入 CI 镜像列表，`test_docs` 核对）。
- 未改：渲染行为/表/id/资源 family/pass/shader/config 键/converter/既有计划与审查文档；`config.example.toml` 与 `test_app_config` 未触碰（本任务无配置键）。
