# 软件光栅在 HZB 出货配置下的 headroom 与 subpixel-share 扫描（E1+E2）

> **状态（2026-10-23）：按用户决定关闭 subtilis 工作流；本文件是 E1+E2 的收尾记录，仅测量/分析，未改生产行为。**
> 收到关闭决定时不再启动任何新 capture；补充的 tight-alternation A/B（ABBA）只完成了开头一小部分、没有结果文件，
> 因此**不用于任何结论**并在下文明确标为未完成。软件光栅保持 optional、默认 OFF，不再继续优化。
> 原始证据归档：`D:/deren-assets/2026-10-23-software-raster/headroom/`（251 个文件 / 482,293,598 B，
> `sha256-manifest.json` 独立复读+复算 **missing 0 / mismatched 0 / extra 0**）。

## 1. 要回答的问题

用户假设：**软件光栅没有净收益，是否只是因为测试场景缺少（像 Nanite 那样的）亚像素三角形？** 如果是，那么
subpixel 占比更高的配置/场景应该出现稳定净收益；如果不是，则 HZB 出货配置下可让出的硬件光栅工作量本身
就很小，或者被软件成本/CPU 路由成本抵消。

T1 的 GO 上界（far `visibility raster` 1.54 ms / 7.66%）是在 **没有 HZB 的 M1c 路径**上测的：baseline raster
中位 19.65 ms，holes control 把 554,523 个 workgroup / 8.96% 三角形从硬件 dispatch 中移除，得到 1.54 ms
raster（也等于 total）差；它**不含**软件 pass/merge/resolve 成本。出货路径是 DAG+HZB：HZB 已先剔除约
70.6%（far）/91.0%（near）候选，所以 T1 上界不能直接搬过来。

## 2. 方法（measurement-only）

### 2.1 资产与构建（可复现）

- 源资产：`D:/deren-assets/bistro/textured-verified/bistro-core-mr-subtilis-v1.gltf`
  SHA-256 `0759576741d3c68998d52fcc6920087e5236eda942637c92d3aedd316a634c35`。
- 用离线转换器重新生成 DAG 兼容包（embedded → package 模式）：
  `build-subtilis/deren-subtilis.exe build <source> -o tmp/headroom/subtilis-textured.gltf --format gltf --report tmp/headroom/build-report.json`；
  产物 `subtilis-textured/subtilis-textured.gltf` SHA-256 `5b626f6f...5c46f3`，**与先前所有矩阵使用的同一包逐字节一致**
  （转换器确定性，复现成功）。
- 执行文件 SHA-256 `443e06477f93f8fca4bb33b02e6f8e248aeaeb89bd4eb4a2d8d74a369e6689cf`，shader bundle
  SHA-256 `f7806b9dec4b1c09f1c6bf7e76fddee511a45bea23fd942e4aac037ceed42b58`；测量结束时再次核对未变。

`tmp/` paths below are workspace scratch only; the archived, hash-verified measurements and logs are under
`D:/deren-assets/2026-10-23-software-raster/headroom/` (251 files, SHA-256 manifest), so replaying a command
should use a fresh work directory and the archived asset/build hashes.

### 2.2 矩阵

新脚本 `scripts/measure/measure_software_headroom.py`（48 次运行）+ `scripts/measure/analyze_software_headroom.py`
（汇总/校验）。每个配置：**3 轮交错**、每轮 130 帧（129 个 GPU timing 样本）、DAG+HZB build/cull/recovery 全开、
1280x720 纹理验收同设置（shadows/TAA/SSAO/bloom/FXAA/validation 关、`software_raster_threshold_px=1.0`、
`subtilis_error_px=1.0`、hysteresis off、gui 关）；相机为既有 far/near 位姿，另加一个更远相机
（yaw 35 / pitch 18 / distance 240 / target 同 far，键 `1280x720-far_farther`）。7 个配置：

| 配置 | 分辨率 | 相机 | 模式 |
| --- | --- | --- | --- |
| 640x360-far / -near | 640x360 | far / near | off, triangles |
| 1280x720-far / -near | 1280x720 | far / near | off, tiled, triangles |
| 1280x720-far_farther | 1280x720 | farther | off, triangles |
| 1920x1080-far / -near | 1920x1080 | far / near | off, triangles |

共 48 次运行（16 配置-模式组合 × 3 轮）。三角模式 = `DEREN_SOFTWARE_RASTER_TRIANGLES=1`；tiled 模式仅作为
E1 的“旧生产者”对照，不参与 E2 假设检验。交错方式：配置顺序每轮旋转，模式顺序每轮旋转；每行记录
`config/log/screenshot` 和全部指标。变更像素统计对同配置同轮的 on/off 截图做逐通道差分，另以 OFF 轮间差异
作为已知重复性噪声对照（PIL/numpy）。

### 2.3 指标与 pass 分解的准确口径

运行时已有的 GPU marks 标签对应（在 visibility 帧上）：`visibility raster` = **硬件 mesh-task raster 本体**；
`scene` = **visibility resolve**（该帧 geometry lane 不记录，resolve 的 mark 区间沿用 scene 标签）；`software visibility`
= 软件光栅 compute pass；`software merge` = depth-only merge；`hzb cull/build/recovery`、`visibility recovery` =
HZB 与 recovery 光栅。label 与 pass 的对应在 `runtime.declarations.cppm` 的 `gpu_timing_labels` 和
`runtime.post_graph.cpp` 的 mark 位置处可核对；本报告没有把 resolve 错算进硬件 raster。

Subpixel-share 指标（只使用运行时实际打印的计数，单位都是 cluster/meshlet record per frame）：

- `share_vs_hzb_survivors = 软件 clusters/frame ÷ HZB survivors/剔除帧`（软件路由占“HZB 后仍可见”候选流的比例）；
- `share_vs_hzb_candidates = 软件 clusters/frame ÷ HZB candidates/剔除帧`（占 history-test 后候选流的比例）。

`software clusters/frame` 来自周期统计行（"software raster: N frames; X software clusters/frame; ..."），
`HZB survivors/candidates` 来自 session 汇总行（"hzb cull: session ... over F culling frame(s), C candidate(s)/frame"），
两者平均窗口不同（前者全部帧、后者仅剔除帧），报告里按上述定义直接相除，不额外外推。

## 3. E1：HZB 出货配置下的可让出 headroom（1280x720）

### 3.1 pass 分解（每模式 3 轮中位数）

| 模式 | GPU frame | visibility raster | software visibility | software merge | resolve (`scene`) | HZB build/cull/recovery | CPU-active | cut wall |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| far — off | 10.59 ms | 8.57 [7.38–9.28] | 0 | 0 | 0.21 | — | 8.80 ms | 1.475 ms |
| far — tiled | 10.84 ms | 8.13 [7.12–8.21] | 0.65 ms | 0.03 | 0.20 | 同上 | 9.59 ms | 2.287 ms |
| far — triangles | 9.19 ms | 7.25 [7.07–8.24] | 0.10 ms | 0.04 | 0.18 | 同上 | 10.30 ms | 2.135 ms |
| near — off | 4.84 ms | 2.73 [2.70–4.23] | 0 | 0 | 0.46 | — | 8.20 ms | 1.264 ms |
| near — tiled | 5.05 ms | 2.60 [2.33–3.38] | 0.22 ms | 0.05 | 0.45 | 同上 | 9.78 ms | 2.264 ms |
| near — triangles | 4.63 ms | 2.48 [2.47–3.72] | 0.08 ms | 0.05 | 0.43 | 同上 | 9.32 ms | 1.835 ms |

far HZB 剔除 70.60%，survivors 12,332/frame；near 剔除 91.05%，survivors 3,254/frame。HZB build/cull/recovery
在两种软件模式下相同（软件不影响 HZB 输入；build 采样的是 software merge 之前的深度，保守方向已在实现中记录）。

### 3.2 headroom 计算与逐对（同一轮）差分

- 以中位数相减：far 硬件 raster 8.57 − 7.25 = **1.32 ms** 被路由走（software pass + merge 只增加 0.14 ms，
  resolve 基本不变）；near 为 **0.25 ms**（增加 0.13 ms）。
- 但**同一轮内相邻的配对差**没有这么稳定。far 三轮配对 raster 差（on − off）为 **−0.13 / −0.33 / −2.21 ms**、
  配对 GPU frame 差 **−0.07 / −0.31 / −2.21 ms**（中位 −0.31 ms）；near 配对 raster 差 **−1.75 / +1.02 / −0.26 ms**、
  GPU frame 差 **−1.70 / +1.40 / −0.19 ms**（中位 −0.19 ms）。同一模式的轮间 spread：off far raster 7.38–9.28
  （**±13%**）、near 2.70–4.23；on far 7.07–8.24。所有配置的 on/off GPU frame spread 都重叠。
- 因此 E1 的诚实结论：**HZB 已先移走大部分候选，软件路由实际让出的 raster 时间在 ~0.1–1.3 ms 量级，单次最好的
  配对到过 2.2 ms，但被 GPU 时钟/散热漂移淹没，无法稳定测出**；它不是 T1 的 1.54 ms 的同口径数字。
- CPU 侧：软件路由让 cut wall 从 1.475 → 2.135 ms（far，+0.66 ms）、1.264 → 1.835 ms（near）；
  CPU-active 中位 +1.50 ms / +1.12 ms（含分类/元数据发布）。对比 tiled 的 cut wall 2.287/2.264 ms，三角形 producer
  已把 CPU 路由成本显著降低，但仍高于 hardware-only。

### 3.3 T1 上界 vs 现在 hybrid 的定量解释

T1 的 1.54 ms 是在 **raster 19.65 ms（无 HZB）、8.96% 三角形/554,523 workgroups 被移出硬件**的帧上测的。
出货配置里：

- HZB 先剔除 70.6%（far），所以同一分类 population 的大部分候选**已经不画**；软件实际接走的是
  2,399 clusters/frame，即 HZB survivors 的 **19.5%**（占剔除前后候选流的 5.7%）。
- 按“每候选/每 workgroup 成本近似均匀”的工作量比例放大：`1.54 ms × (8.57/19.65) × (5.7%/8.96%) ≈ 0.43 ms`；
  这是 HZB 出货路径的**工作量口径估计**。上界口径（按 survivor 占比 × 当前 raster）：`8.57 × 19.5% ≈ 1.67 ms`，
  但被判为 subpixel 的簇像素工作量小、每 workgroup 更便宜，实际应低于该上界。实测中位 1.32 ms 落在两者之间，
  配对中位只有 0.33 ms。
- 成本回收：GPU 侧 software pass 0.10 ms + merge 0.04 ms（far），CPU 侧 +0.66 ms cut / +1.50 ms CPU-active；
  要形成稳定净收益，需要稳定回收 >~1 ms 的 raster 时间，这在 3 轮内没有出现（配对差符号在大部分配置翻转）。
- 这与此前 textured matrix 的 “ triangles 10.52 vs off 10.31（+0.21 ms，落在 spread 内）” 不矛盾：本 E1 的
  far 中位差 −1.40 ms 同样落在其自身 2 ms 级别的轮间 spread 内；把 T1 的 1.54 ms 当作可回收预算会高估约 3–5 倍
  （0.43 ms 工作口径 vs 1.54 ms），再加上软件/CPU 成本后就是“没有稳定净收益”。

## 4. E2：分辨率/距离（subpixel-share）扫描

### 4.1 每配置结果（off / triangles 均为 3 轮中位数；配对差为同轮 on − off，负值 = hybrid 更快）

| 配置 | share(survivors) | share(candidates) | off GPU | tri GPU | 中位 Δ | 逐轮配对 Δ | CPU-active off→tri | cut wall off→tri | 变更像素（3 轮） |
| --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- |
| 1280x720-far | 19.5% | 5.7% | 10.59 [9.26–11.28] | 9.19 [9.07–10.28] | −1.40 ms | −0.07 / −0.31 / −2.21 | 8.80→10.30 | 1.475→2.135 | 678/678/678 px（mean 0.00029） |
| 1280x720-near | 23.3% | 2.1% | 4.84 [4.79–6.33] | 4.63 [4.60–6.24] | −0.21 ms | −1.70 / +1.40 / −0.19 | 8.20→9.32 | 1.264→1.835 | 14,823/14,823/0（mean 0.00855） |
| 1280x720-far_farther | 32.8% | 10.0% | 9.58 [8.62–10.04] | 9.60 [8.76–10.08] | +0.02 ms | +1.46 / −1.28 / +0.02 | 8.79→9.24 | 1.448→1.930 | 0/0/368 px |
| 640x360-far | 41.7% | 12.0% | 9.17 [8.40–9.37] | 8.36 [8.22–9.61] | −0.81 ms | +0.44 / −1.15 / −0.04 | 8.86→9.57 | 1.474→2.160 | 0/0/0 |
| 640x360-near | 49.4% | 4.4% | 5.01 [3.60–5.05] | 4.54 [3.80–5.45] | −0.47 ms | +1.85 / −0.47 / −1.25 | 8.25→9.28 | 1.276→1.894 | 0/2,276/0 px |
| 1920x1080-far | 12.6% | 3.7% | 10.80 [9.86–11.79] | 10.58 [10.23–12.22] | −0.22 ms | +0.43 / +0.37 / −0.22 | 9.10→9.97 | 1.477→2.111 | 0/0/0 |
| 1920x1080-near | 12.0% | 1.1% | 6.77 [6.69–6.97] | 8.23 [6.58–8.33] | +1.46 ms | +1.64 / +1.26 / −0.19 | 8.54→9.78 | 1.326→1.866 | 43,052/43,052/0 px（mean 0.01102） |

### 4.2 假设是否成立

- **share 确实被改变了**：从 12%（1080p near）到 49%（640x360 near），软件 clusters/frame 从 400 到 4,968；
  同时软件 pass 成本始终只有 0.04–0.20 ms。HZB 剔除率在各配置都保持 69–91%，说明“更靠近/更低分辨率”确实把更多
  候选变成 subpixel。
- **hybrid 的收益没有随 share 单调上升**：中位差在 640x360-near（share 49%）是 −0.47 ms、640x360-far（41.7%）
  −0.81 ms、720p-far（19.5%）−1.40 ms、1080p-near（12%）+1.46 ms；七个配置的 Spearman 相关（share vs frame Δ）
  = **−0.39**（n=7，方向与假设一致但远不显著，且由中位数/漂移主导）。逐轮配对差在 5/7 个配置里符号翻转；
  只有 1280x720-far 的三轮配对差全为负（−0.07/−0.31/−2.21），但幅度跨 30 倍且轮间 spread 重叠。
- 更重要的是：**没有任何一个配置出现“超出 run spread 的稳定净收益”**——所有配置的 on/off GPU frame
  spread 区间都重叠（表内方括号），CPU 侧在所有配置都一致变差（+0.5–1.6 ms CPU-active、+0.4–0.7 ms cut wall）。
- 图像差异：far/640/1080p 的 on-vs-off 变更像素为 0–678 px；near 在两个配置上出现 14.8k/43k 变更像素
  （mean channel diff 0.0086/0.0110、max 27–30），量级与既有报告里 **硬件路径自身的 near 重复性差异**
  （同一 DAG+HZB 配置重复截图 mean ≈0.0086）一致；1080p-far 的 OFF 轮间本身就有 1,636 px 差异。因此这些图像差异
  不能归因于软件光栅，也不构成图像正确性失败证据（本轮没有做 validation-enabled 图像验收）。

**判定：unresolved——现有证据不支持“场景缺少亚像素三角形是软件光栅没有收益的原因”这一假设。**
提高 subpixel share 到 ~42–49% 没有带来稳定净收益；但 GPU 时钟/散热漂移使单次 Δ 的符号都会翻转，因此也不能
把假设“refuted”为物理不可能，只是在本资产 + 本出货配置上没有支持证据。

## 5. 限制（必须和数字一起读）

1. **T1 的 holes probe 不能与 HZB 同时启用**：探针 admission 明确要求
   `!hzb_cull_active_frame && !hzb_recovery_active_frame`（`source/engine/runtime/runtime.cpp`），所以本任务没有
   在 HZB 配置下重建 holes control（按要求不改生产行为去强行 arm）。E1 的 headroom 全部来自 pass 分解和
   配对差，不是 holes 直接测量。
2. **GPU 时钟/散热漂移**：同一配置同一模式的轮间 raster 差可达 ±13%（例如 720p-far off 7.38→9.28 ms 随轮次单调
   变慢）；本机没有固定 GPU 时钟（此前也观测到 P8/210 MHz idle）。这让 sub-ms 的 Δ 不可分辨，是本报告所有
   “不稳定”的主要来源。补充的 tight-alternation ABBA（相邻 off/on 配对，3 对/配置）在关闭决定时尚未跑完，
   **没有结果文件、未用于结论**，已作为不完整数据归档。
3. **单资产、单机、单一 producer 设置**：thr 1.0 px、triangle producer 默认 metric、HZB build/cull/recovery 全开；
   没有 2 px/edge metric、没有 M5/streaming、没有其它场景。
4. 截图颜色有已知的非确定性（硬件路径自身轮间也会差 678–43k 像素；本报告只用作 sanity/变更量记录，不作为
   正确性验收）。
5. CPU-active 不是端到端呈现延迟；cut wall/classification 是 CPU 侧成本的直接观测。

## 6. 推荐（按用户“关闭工作流”的决定）

1. **把 M4 软件光栅作为出货配置的负结果关闭**：HZB 之后可让出的硬件 raster 工作量按工作量口径约 0.4 ms、
   最好配对也只到 2.2 ms，且不稳定的来源主要是时钟；软件 pass/merge 成本低（0.1–0.2 ms），但 CPU 路由成本
   +0.5–1.6 ms 且没有跨配置的稳定净收益。软件光栅保持 optional/默认 OFF，不再投入优化。
2. **不建 E3 合成密集 subpixel 场景**：本扫描已把 subpixel share 从 12% 拉到 49%（软件 clusters/frame 400→4,968）
   仍无稳定 win；在当前测量噪声（同一模式轮间 ±13%）下，一个合成场景更难分辨真实效应，先修测量环境
   （固定 GPU 时钟/隔离机器）才有意义。
3. 若未来重开：先固定 GPU 时钟 + 每次 off/on 相邻 ABBA 配对 + validation 开图像验收，再做合成场景；这与
   用户当前“关闭工作流”不冲突。

## 7. 证据、命令与复现

- 测量脚本：`scripts/measure/measure_software_headroom.py`（48 runs，`--self-test` 检查 plan/parser/marker 负向控制）；
  分析脚本：`scripts/measure/analyze_software_headroom.py`（矩阵完整性 + 5 项负向控制 + 汇总）；
  未完成的补充脚本：`scripts/measure/measure_software_headroom_abba.py`（ABBA，未运行完成）。
- 关键命令：
  - `python scripts/measure/measure_software_headroom.py --work <fresh> --asset tmp/headroom/subtilis-textured/subtilis-textured.gltf`
  - `python scripts/measure/analyze_software_headroom.py --results <fresh>/results.json --output docs/reviews/2026-10-23-software-raster-headroom/e1-e2-summary.json`
  - 资产重建：`build-subtilis/deren-subtilis.exe build <source>.gltf -o <out>.gltf --format gltf --report <report>.json`
- 结果 JSON：`docs/reviews/2026-10-23-software-raster-headroom/e1-e2-summary.json`（80 KB，含每配置中位数/spread、
  hybrid 分解、share、变更像素、E2 spearman、E1 分解；逐轮原始行在归档的 `measure/results.json`）。
- 原始证据：`D:/deren-assets/2026-10-23-software-raster/headroom/`，**251 files / 482,293,598 B**：
  `measure/`（48 runs 的 config/debug.log/截图/results.json）、`abba-incomplete/`（关闭时未完成，明确标注）、
  `analysis.log`、`measure-runner.log`、`build-report.json`、`e1-e2-summary.json`、`build-identity.txt`；
  `sha256-manifest.json` 独立复读+复算 **0 missing / 0 mismatched / 0 extra**。
- 构建/身份：exe `443e0647...`、shaders `f7806b9d...`、package `5b626f6f...`（见 §2.1）。
- 本轮未改生产代码；默认 OFF 不变。关闭决定后未再跑任何 capture（frozen Full/Visibility 门没有被本轮重跑，
  但生产路径无改动；同 exe 的 15/15 + 1/1 结果见上一份 routing 验收记录）。
