# 渲染优化开关性能对比

## 结论

原始六配置矩阵（下表）测于三角形每像素重算版本。随后按 UE Nanite 的工作组织方式重写软件光栅：每个 tile workgroup 先协作投影候选三角形一次，再用三角形屏幕包围盒限制覆盖测试。新版本保持截图逐像素哈希不变，软件 pass 显著变快；但相对 Cluster DAG，完整帧仍增加 74.0%（远景）和 14.8%（近景），因此软件路径仍应保持 opt-in。Cluster DAG 和 HZB 的收益结论来自原始矩阵，未因后续 shader 改动而重新测 HZB。

上面的“请求软件光栅 + HZB”数据来自旧版互斥实现，现已被文末三路并发复测取代。新复测确认软件光栅与 HZB 可在同帧工作；见最后一节。

## 测量设置

- 资产：`D:/deren-assets/2026-10-21-object-lod/bistro-object-lod.glb`，SHA-256 `620f424239909d023b30dd86fe953c579298a39ef2d4706bca59fc0d2982085b`。
- 构建：当前 Release `deren.exe`，SHA-256 `c042bdf56b6bfff4f2102abb16bf702921090f8c76cbfa1221ae0419b0a8b3e3`。
- 固定分辨率 1280×720、同一资产与画质选项；远景相机 `35,18,140,0,-83.07,0`，近景相机 `35,8,42,0,-88,0`。
- 每个相机/配置采集 130 帧，统计 129 帧的会话平均 GPU 时间；每格三轮，轮换配置顺序。表中使用三轮中位数。
- VSync、阴影、TAA、SSAO、bloom、FXAA 与验证层关闭。HZB 的 build、cull、recovery 三个闸门均开启。
- 可复现原始数据、逐轮日志、配置和截图位于忽略目录 `build-release-dyn-clang64/render-optimization-comparison/`；入口脚本为 `scripts/measure/measure_render_optimizations.py`。

## 结果

| 路径 | 远景 GPU ms | 对 visibility | 近景 GPU ms | 对 visibility | 实际状态 |
| --- | ---: | ---: | ---: | ---: | --- |
| Visibility buffer | 24.15 | 基准 | 21.57 | 基准 | Visibility 开启 |
| Cluster DAG | 20.43 | −15.4% | 19.07 | −11.6% | Cluster DAG 开启 |
| 软件光栅 + Cluster DAG | 115.37 | +377.7% | 36.54 | +69.4% | 软件光栅开启 |
| HZB + Cluster DAG | 8.90 | −63.1% | 5.49 | −74.5% | HZB 剔除率分别 70.04%、90.84% |
| 请求软件光栅 + HZB | 8.78 | −63.6% | 5.35 | −75.2% | HZB 生效，软件光栅按设计停用 |

软件光栅相对 Cluster DAG 单独运行：远景增加 464.7%（约 5.65×），近景增加 91.6%（约 1.92×）。CPU 活跃时间中位数从 Cluster DAG 的 5.21/4.91 ms 上升至 8.81/7.91 ms；GPU 总帧时是主要退化指标。

## 原始软件路径实际覆盖范围

开关没有把整张场景或所有 meshlet 改成软件光栅。每个 Subtilis 选出的 meshlet 仍按保守投影球直径与阈值拆成硬件前缀和软件后缀；visibility mesh-task 只 dispatch 硬件前缀，软件 CSR 只收录软件后缀，ID 仍以完整 run 中的 meshlet 序号解码。三角形不是在两个路径各画一次。

这两组采集中，远景 Subtilis 保留 41,030 个 meshlet，其中 4,233 个（10.3%）转给软件路径；近景保留 35,444 个，其中 850 个（2.4%）转给软件路径。远景软件 meshlet 产生 4,572 个 tile-entry，近景 913 个。每个 tile-entry 都是一个 meshlet，而 shader 会对它处理 meshlet 的全部三角形。

成本问题在于软件 pass 的执行粒度很宽：compute dispatch 覆盖完整 80×45 个 16×16 tile，逐 tile 写满图像的 software depth/ID；每个有效像素都会遍历该 tile 的 CSR entries，并重新 fetch、变换每个候选 meshlet 的三角形顶点、做三角形覆盖测试。即使三角形投影尺寸小于 1 像素，当前实现仍执行逐像素、逐候选、逐三角形的完整测试。高密度场景中的最热 tile 可以达到 977 个候选，因此“仅部分 meshlet 被分流”仍可能触发很大的重复工作。这是性能退化的直接算法原因，测量没有显示全场景几何都走软件路径。

旧版 legacy 路径远景/近景分别为 152.55/132.93 ms。它使用不同渲染路径，只作背景参照，不纳入以上开关增益的分母。

## 原始矩阵采集有效性与边界

36 次采集都以退出码 0 完成，且每次均有至少 100 个 GPU 计时帧。软件光栅三轮均确认激活；HZB 三轮均确认剔除与恢复阶段激活。HZB 在远景的剔除率三轮均为 70.04%，近景均为 90.84%。

逐轮 GPU 帧时变化较小：软件光栅远景 114.81–118.22 ms、近景 35.88–36.95 ms；Cluster DAG 远景 19.72–20.62 ms、近景 18.90–19.15 ms。部分基线或组合的截图哈希在三轮中有两种，可能来自相机抖动或时间相关画面变化；性能结论只使用 GPU 计时，不以截图一致性证明像素正确。

原始矩阵的计时反映完整帧 GPU 时间，没有软件 pass 独立计时。软件 visibility 当时落在名为 `hzb build` 的间隔里，而 HZB 关闭，所以该字段不是 HZB 构建时间。原始同姿态 ON/OFF 烟测曾有 18 个像素、最大通道差 6 的变化；那次测试不足以单独证明精确一致。

## UE 风格重写后的复测

重写版本将同一 tile 的三角形准备工作摊给 64 个线程组成员，每个候选三角形只做一次顶点读取、投影、面积与屏幕包围盒计算。屏幕像素只测试落入包围盒的候选三角形；tile CSR、每像素确定性 `(depth,id)` 选择以及输出图格式保持不变。该复测仍使用 16×16 tile 和 8×8 workgroup，针对热点是重复三角形准备和无效像素覆盖测试。

使用同一资产、相机、分辨率和采集脚本，软件光栅与 Cluster DAG 交错采集三轮，每轮 130 帧。新版本 Release 可执行文件 SHA-256 为 `c236962a1644fe0a8a2fe8027ba8c759257c036df040326741ca89b699541e76`，数据和截图位于忽略目录 `build-release-dyn-clang64/ue-software-rerun/`。

| 相机 | Cluster DAG GPU ms | 新软件光栅 GPU ms | 相对差异 | 软件可见性 ms | 软件合并 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 远景 | 20.60 | 35.84 | +74.0% | 17.34 | 0.03 |
| 近景 | 18.99 | 21.80 | +14.8% | 3.65 | 0.03 |

旧 shader 的软件可见性区间约为远景 95.78 ms、近景 17.97 ms。新版本分别降至 17.34 ms（−81.9%）和 3.65 ms（−79.7%）；完整软件帧时间从原矩阵的 115.37/36.54 ms 降到 35.84/21.80 ms。软件分流数稳定：远景 4,233 clusters / 4,572 tile entries，近景 850 / 913。

新旧每个相机的软件光栅截图 SHA-256 与原矩阵对应截图完全一致：远景 `291ccf97…f3b80c`，近景 `7f4181c3…ba7eec9`。12 次复测均退出码 0、每轮包含 129 个 GPU 样本，验证层无错误或警告。独立 GPU 时间标记已加入，software visibility 与 software merge 不再混入 HZB 标签。

当前建议：Cluster DAG 保持默认候选，HZB 保留 recovery 并继续扩大场景覆盖验证，软件光栅继续显式 opt-in。UE 风格改造解决了主要重复工作，收益显著；完整帧在这组相机下仍慢于纯 Cluster DAG。这里的数字只适用于该 Bistro 资产、固定相机、分辨率和当前构建。原始运行时 GPU 原始读回及 CPU 参考图严格校验仍未接通，截图哈希证明新旧截图一致，但不能替代软件路径与独立 CPU 光栅参考之间的精确性验证。

## 三路并发复测（2026-10-22）

本节是在互斥移除后新跑的结果，覆盖 DAG、DAG+软件光栅、DAG+HZB、三者全开。固定同一 Bistro 派生资产、1280×720、远景/近景相机和渲染选项；四种配置按轮次交错，每格三轮、每轮 130 帧，统计 129 帧 GPU session average 的中位数。性能计时关闭 validation layers；另有三路全开、validation 开启的 60 帧 smoke，无验证错误。构建 SHA-256 `55f620357ee6f541536079ea8c4ba2e98bce49f6ef67733c867becb3d2fdbbbe`，资产 SHA-256 与上文相同。每次运行日志与配置、SHA-256 和完整 JSON 结果存于忽略目录 `build-release-dyn-clang64/three-way-comparison-20261022/`，截图记录哈希后删除以节省磁盘空间。

| 相机 | Cluster DAG | DAG + 软件 | DAG + HZB | DAG + 软件 + HZB | HZB 剔除率 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 远景 | 20.22 ms | 36.29 ms (+79.5%) | 9.44 ms (−53.3%) | 19.50 ms (−3.6%) | 70.04% |
| 近景 | 18.63 ms | 21.27 ms (+14.2%) | 5.31 ms (−71.5%) | 7.55 ms (−59.5%) | 90.84% |

括号为相对 Cluster DAG 的完整帧 GPU 时间变化。三者全开相对单开 HZB，远景增加 **106.6%**，近景增加 **42.2%**：软件光栅确实在 HZB 之后继续承担了可见软件候选的成本。与此同时，HZB 将远景软件 visibility pass 从无 HZB 时的 17.40 ms 降至 **9.58 ms**，近景从 3.33 ms 降至 **1.85 ms**；硬件 visibility pass 分别为 7.61/3.76 ms，HZB build/cull/recovery 仍分别运行。软件 merge 约 0.03 ms。CPU active 中位数也上升：远景 5.02→8.87 ms，近景 4.84→8.26 ms。

结论：HZB 大幅降低两种镜头下的 GPU 工作；同时开软件光栅会吃掉相当一部分收益。远景三路全开只比 DAG 基线快 3.6%，近景仍快 59.5%，但两者都明显慢于 DAG+HZB。当前应保留同时开启的正确性与 opt-in 能力，性能默认组合仍建议 Cluster DAG + HZB；软件光栅默认关闭，除非后续针对高 occlusion 场景或软件候选 pass 继续优化。三路全开三轮的每相机截图哈希一致；近景 Cluster DAG 的三个样本中出现两个哈希，因此哈希只作运行稳定性线索，不替代独立像素正确性校验。

运行入口：`scripts/measure/measure_render_optimizations.py`。脚本现验证全开时软件/HZB 两条路径都实际激活；`--discard-images` 在保留哈希后删除截图。

## 稀疏活跃瓦片与阈值扫描（2026-10-22）

软件 pass 改为两段 compute：第一段清空 software depth/ID 图；第二段根据 CSR 的紧凑非空 tile 索引列表，只派发活跃瓦片。这样空瓦片不再执行 CSR 查询和三角形遍历；全图清空仍保留，避免空瓦片读取上一帧残留。两段间使用 storage image 写后写屏障。阈值扫描覆盖 0.25、0.5、1.0、2.0 px，并保留 Cluster DAG 与 DAG+HZB 基线。

首次稀疏派发实现曾把紧凑 group 序号误用为输出 tile 坐标，造成错瓦片写入；测试新增了 shader 源检查并捕获此问题。下表只采用修正映射后的最终矩阵。固定同一 Bistro 派生资产、1280×720、相机与渲染选项；每相机/配置三轮、每轮 130 帧，计时关闭 validation layers，统计 129 个 GPU 样本。42 次采集均有效。程序 SHA-256 `aaef19f0fd7e886e742836ec5760234ca00b7675e1a75e91ce1699f84ff6e6c8`；编译 shader bundle SHA-256 `37ac44d071b2cfad808aa1211969f4a323ef44ab9e6e9b80ff23262963ab4d79`；资产 SHA-256 与前文相同。逐轮日志、配置、JSON 和截图哈希位于忽略目录 `build-release-dyn-clang64/sparse-software-raster-threshold-20261022-final/`，截图记录哈希后删除。

| 镜头/配置 | GPU 完整帧中位数 | 对 DAG | 对 DAG+HZB | 软件 clusters / CSR entries | CPU active 中位数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 远景 DAG | 20.73 ms | 基准 | — | — | 5.20 ms |
| 远景 DAG+软件，1.0 px | 19.49 ms | −6.0% | — | 4,233 / 4,572 | 8.34 ms |
| 远景 DAG+HZB | 9.82 ms | — | 基准 | — | 5.95 ms |
| 远景 DAG+HZB+软件，1.0 px | 9.55 ms | — | −2.7% | 4,233 / 4,572 | 8.87 ms |
| 远景 DAG+HZB+软件，0.25 px | 9.39 ms | — | −4.4% | 516 / 519 | 9.33 ms |
| 远景 DAG+HZB+软件，0.5 px | 10.06 ms | — | +2.4% | 1,381 / 1,414 | 8.86 ms |
| 远景 DAG+HZB+软件，2.0 px | 9.56 ms | — | −2.6% | 10,923 / 12,557 | 9.10 ms |
| 近景 DAG | 18.73 ms | 基准 | — | — | 5.05 ms |
| 近景 DAG+软件，1.0 px | 18.59 ms | −0.7% | — | 850 / 913 | 7.99 ms |
| 近景 DAG+HZB | 5.94 ms | — | 基准 | — | 5.34 ms |
| 近景 DAG+HZB+软件，1.0 px | 5.22 ms | — | −12.1% | 850 / 913 | 8.30 ms |
| 近景 DAG+HZB+软件，0.25 px | 5.36 ms | — | −9.8% | 38 / 39 | 8.07 ms |
| 近景 DAG+HZB+软件，0.5 px | 5.36 ms | — | −9.8% | 299 / 308 | 8.32 ms |
| 近景 DAG+HZB+软件，2.0 px | 5.01 ms | — | −15.7% | 2,284 / 2,599 | 8.27 ms |

软件 visibility pass 中位数约 0.03 ms（无 HZB 时为 0.05–0.07 ms），merge 约 0.03 ms，明显低于上一轮全瓦片 shader 的 9.58/1.85 ms。GPU 侧已达到小幅正收益：软件光栅单开时两个镜头均快于 DAG；与 DAG+HZB 相比，远景 1.0/0.25/2.0 px 小幅领先但 0.5 px 回退，近景四个阈值的中位数均领先。远景收益靠近波动区间，近景 HZB 基线三轮本身为 5.26/5.94/6.03 ms，必须把结论限定在本次资产和固定相机，不能据此设默认开关。

CPU active 中位数仍增加约 3 ms（DAG 5.20/5.05 ms，软件 1.0 px 为 8.34/7.99 ms）。所以目前证明的是 **GPU 光栅路径可以有正收益**，不是全系统 CPU+GPU 端到端均正收益。下一步瓶颈是每帧 CPU cluster 投影、分类与 CSR 规划，应减少这部分重复工作，并以动态相机和高密度 subpixel 场景复测。

验证层 smoke：同一 1.0 px 远景 HZB 配置重复两次，截图哈希一致；无 HZB 软件配置重复两次也一致，且与 DAG 图像不同。两种 smoke 均 130 帧退出码 0，Vulkan validation 未发现 VUID、Validation Error、WARNING 或 ERROR。阈值矩阵中少数 HZB 配置截图哈希有两种，因此此 smoke 只能支持稳定性和有效执行，仍不能替代 GPU/CPU 参考图逐像素验证。全量 CTest 为 64/65；唯一失败 `test_backend_boundary` 的两个硬链接用例因 Windows `PermissionError: [WinError 5] 拒绝访问`，单独重跑可复现，与渲染改动无关。

## Subtilis 开始前的历史基线（2026-10-22）

为判断一般渲染流程是否也在 Subtilis 开发期间发生变化，额外测量了 Subtilis 首个脚手架提交 `c5df0913fa838d494585da8ea54121b3bf1a6a0d` 的父提交 `344c3ef82af40714abc2f91d789243c44ce4885f`。这是 Subtilis 尚未进入代码库的历史版本。历史版本原有 visibility 容量不足以覆盖当前 Bistro 派生资产，因此在隔离的历史工作树中把 item 和 meshlet ID 容量都扩到 4096；相关 CPU/GPU ID 布局同步修改。历史灯光导入也有与场景大小无关的越界问题，测试时将导入上限约束到现有四个 GUI 灯光槽，与当前运行行为一致。省略的场景树诊断日志不参与渲染。

两边使用同一资产 SHA-256 `620f424239909d023b30dd86fe953c579298a39ef2d4706bca59fc0d2982085b`、1280×720、相同相机与画质选项，visibility buffer 开启，Cluster DAG、软件光栅和 HZB 关闭。每个相机各跑三轮，每轮采集 130 帧。历史版每轮有两个各 60 帧的 GPU 计时窗口，按帧数加权；当前版取每轮 129 帧 session average。表中先对每轮结果取中位数，再计算相对变化。

| 相机 | Subtilis 前历史版 GPU | 当前全关优化 GPU | 变化 | 历史版 → 当前版 visibility raster |
| --- | ---: | ---: | ---: | ---: |
| 远景 | 24.74 ms | 24.50 ms | −1.0% | 24.14 → 23.88 ms |
| 近景 | 22.08 ms | 21.17 ms | −4.1% | 21.23 → 20.31 ms |

历史版每轮 GPU 时间范围为远景 24.475–24.780 ms、近景 21.860–22.810 ms；当前版为远景 24.43–24.81 ms、近景 20.30–21.82 ms。远景区间几乎完全重叠，近景也有重叠，因此只能说这组测试中当前全关配置略快，不能据此把差异归因于某项一般流程优化。历史版与当前版源码相隔多个改动，仍有提交间的其他变化；结果适合作为整体历史参照，不是单变量实验。

六次历史采集均退出码 0、每轮 120 个 GPU 计时帧；远景截图哈希三轮一致，近景有两种。当前全关结果来自上方稀疏瓦片矩阵，三轮均为 129 个 GPU 样本。历史版没有 Subtilis、HZB 或软件光栅实现。历史可执行文件 SHA-256 为 `483eae1fa8725c051a51e2e7f889df5ffa6ff92506279e875ef330502c510c92`；基线 JSON、逐轮日志和运行配置位于忽略目录 `build-release-dyn-clang64/pre-subtilis-baseline-measurements-20261022/`，采集脚本为 `scripts/measure/measure_pre_subtilis_baseline.py`。

### 容量不足触发 legacy 回退的旧数据

另一个更早的同资产远景对比（用户提供的测量截图，16 帧）显示，1024 容量旧构建 GPU 帧时间为 178.5 ms，其中 scene pass 为 177.8 ms；将 item/meshlet 容量扩为 4096 后，visibility lane 为 26.7–27.0 ms，其中 visibility raster 为 26.2 ms。按 178.5/27.0 计算约为 6.6 倍。这是 Subtilis 渲染系统的容量扩展前置收益：扩容让系统能走 visibility lane，而不再整场景回退到 legacy；它不是 Cluster DAG/LOD 算法自身的收益。

这也解释了此前提到的约 150–180 ms 长帧。它与上节 4096 容量历史版本的 24.74 ms 结果属于不同容量和渲染路径，不能直接拿来衡量 Subtilis 前后的一般流程变化。截图中的新构建约 27 ms、我们扩容后的历史构建约 24.74 ms 量级相近；之后当前构建的优化全开数据则见下一节。

## 三路全开在稀疏瓦片优化后的结果

阈值矩阵中的 1.0 px 配置同时开启 Cluster DAG、软件光栅和 HZB。三轮 GPU 中位数为远景 **9.55 ms**、近景 **5.22 ms**（分别为 9.26/9.55/9.72 ms 与 5.06/5.22/5.34 ms）。相比同一构建的全关优化基线 24.50/21.17 ms，分别降低约 61.0%/75.3%；相比 Cluster DAG 单独开启的 20.73/18.73 ms，降低约 53.9%/72.1%。软件 visibility 与 merge pass 各约 0.03 ms。此处是完整 GPU 帧中位数，和上面 150–180 ms 的 legacy 回退帧属于不同路径与构建阶段。

## 渲染路径总览

| 特性子集 | 远景 GPU 帧 | 近景 GPU 帧 | 实际运行情况 |
| --- | ---: | ---: | --- |
| 传统 legacy 路径 | 152.55 ms | 132.93 ms | 原始六配置矩阵；1024 容量回退的另一轮远景测量为 178.5 ms |
| Visibility buffer 基线（无三项优化） | 24.50 ms | 21.17 ms | 当前构建，作为优化组合的基准 |
| 仅 Cluster DAG | 20.73 ms | 18.73 ms | 单项生效 |
| 仅软件光栅 | — | — | 不可独立生效；运行时要求 Cluster DAG/Subtilis，关闭 DAG 时软件路径不激活 |
| 仅 HZB（DAG 关闭） | 23.95 ms | 22.18 ms | 独立复测；HZB build/cull/recovery 激活，但本场景剔除率为 0% |
| Cluster DAG + 软件光栅 | 19.49 ms | 18.59 ms | 两项组合；对应软件光栅单独增开在 DAG 基线之上 |
| Cluster DAG + HZB | 9.82 ms | 5.94 ms | 两项组合 |
| 软件光栅 + HZB（DAG 关闭） | 23.95 ms | 22.18 ms | 软件光栅因缺少 DAG 未激活，实际等同仅 HZB；未另作重复采集 |
| 三者全开：Cluster DAG + 软件光栅 + HZB | 9.55 ms | 5.22 ms | 三项都实际激活 |

除 legacy 行外，其余行使用同一资产、相机、当前可执行文件（SHA-256 `aaef19f0fd7e886e742836ec5760234ca00b7675e1a75e91ce1699f84ff6e6c8`）和 1280×720。可见缓冲基线来自全关矩阵；DAG、DAG+软件、DAG+HZB、三者全开来自稀疏活跃瓦片矩阵；HZB-only 是本节追加的三轮独立复测。各自都统计每轮 129 个 GPU 样本的三轮中位数。legacy 行来自较早构建，单独作为路径参照，不和当前版本作严格单变量归因。所有采集关闭阴影、TAA、SSAO、bloom、FXAA 与 validation layers。

软件光栅依赖 DAG，因此它的“单项”只能理解为在 DAG 基线上单独增开软件光栅；软件光栅+HZB若要求软件路径真的运行，也会同时包含 DAG，实际就是三者全开。若按字面关闭 DAG，这个两项配置中的软件光栅会被门控停用。当前矩阵说明 HZB+Cluster DAG 是主要帧时收益来源；在其上再加软件光栅，远景中位数由 9.82 降到 9.55 ms，近景由 5.94 降到 5.22 ms。

所有上述“全开”仅指三项几何优化开关；阴影、TAA、SSAO、bloom、FXAA 等画质选项仍关闭，没有测量全部画质选项开启时的结果。
# 2026-10-23 validation correction

Software-enabled results below predate fixes for sparse dispatch bounds, CSR frame stride and software mode push constants. Keep them as historical measurements; the 5.22 ms / approximately 26x result requires remeasurement with correct raster output. See [closeout checkpoint](2026-10-23-subtilis-closeout-checkpoint.md). Configurations without software raster are not invalidated by these three defects.

**Historical record (2026-10-23):** the software-enabled rows below are superseded and withdrawn for performance claims; see the [M4 decision](2026-10-23-m4-decision.md). No trailing blank line is kept at EOF.
