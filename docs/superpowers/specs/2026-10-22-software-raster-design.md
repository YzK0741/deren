# M4 软件光栅：subpixel cluster 的硬件/软件混合可见性（设计）

日期：2026-10-22。状态：**设计 + 分阶段计划已由 Lead 审阅（含一处阻塞设计缺陷的修正：merge 必须在 resolve 之后，winner 规则才能区分“软件严格更近”与“tie”）；用户已批准 Q2/Q4/Q5（2026-10-22），Q1/Q3/Q6 按推荐默认、Q7 由 Task 1 数据决定；已按计划开始 Task 1，尚未实施 T2+。** 基线 HEAD `1ba58d82`：M1c（converter cluster DAG 的 runtime 导入、共享 generation、CPU cut）已验收，M2/M4/M5 未实现。用户决定：直接产出 spec + 计划（不单设 ROI 探针阶段），但 spec 必须以刚验收的 M1c 重探针证据界定 scope/ROI，并把"实际 subpixel workload 与 hybrid 收益"的证明定义成计划 Task 1 的测量/验收，而不是假设。实施计划见
[2026-10-22-software-raster.md](../plans/2026-10-22-software-raster.md)，验收记录待实施后写 `docs/reviews/2026-10-22-software-raster.md`。

约束：本文件不修改代码、根 CMake、既有计划/审查；不设计 M2/M5/material binning/streaming；M4 保持 default OFF；硬件 raster/resolve 数学除明确设计的 merge 外不变；32-bit id 保持（64-bit/两级表联合决策保留给 M2/M4 联合评估）。

## 1. 目标与阶段边界

### 1.1 目标

在现有 visibility-buffer lane 上增加 **subpixel cluster 的软件光栅**：把 CPU cut 选中、但投影直径小于阈值的 cluster 从硬件 mesh dispatch 中移出，用一个 tile 化 compute raster 直接写"每像素最小深度 + packed id"的辅助结果；一个 depth-only merge pass 把该深度合并进帧自己的 G-buffer depth；resolve 在硬件/软件两个候选间按最小深度选择，然后继续用现有的 32-bit id → item/meshlet/triangle 解码链着色。目标是在**不失覆盖**的前提下减少硬件 raster 对微小三角形的固定开销（mesh workgroup/vertex fetch/primitive setup），把该工作量交给更适合的 tile 扫描。

M4 只改变"谁生产可见 id/深度"，不改变 resolve 的 id 语义、shading、HZB 消费次序（cut → HZB cull → raster → resolve）和 OFF 路径。

### 1.2 非目标

- 不做 M2（GPU cut select/finalize）、M5（streaming/paging）、material binning、CLUSTER 内新压缩或量化。
- 不改变 `valid1/item12/meshlet12/triangle7` 32-bit id、4096 item / 4096 meshlet / 65536 table 容量契约。
- 不引入 64-bit visibility buffer、两级 item/meshlet 表、storage depth twin 之外的 depth 格式变更（本设计使用两张 `R32_UINT` 辅助图像（`software_depth`/`software_id`，复用现有 `image_format::r32_uint`，不新增 RHI 图像格式）；64-bit/两级表仍是 M2/M4 联合决策的保留选项，见 §3.5、§9 Q2）。
- 不改 HZB 数学；M3 的 cull/build/recovery lanes 在 M4 v1 内**保持不变**，且与 hybrid 逐帧互斥（§5.5、§9 Q5）。
- 不改 MASK/BLEND、动态/蒙皮/morph、instanced、custom pipeline、exact-depth 目标的现有路径：这些 item 不进入 hybrid，整项保持现有（硬件）行为。
- 不做跨帧 history/迟滞（M4 逐帧确定性；与 M1c 的 cut hysteresis 正交）。

### 1.3 成功判据

1. **正确性**：CPU reference 软件 raster 与 GPU 软件 raster 在一组 fixture 上逐像素一致（深度/取舍按定义容差）；merge 后帧的 G-buffer depth = min(硬件 depth, 软件 depth)，硬件/软件同深度的 tie 处理确定；resolve 对两种 id 的解码与现有 32-bit 契约完全一致；不出现"半新半旧"帧。
2. **覆盖/回退**：不使用"只画一部分"的近似。任何容量/资源/pipeline/开关不满足 → 该帧的 hybrid 整帧关闭（硬件完整路径），日志一次；无 `software_raster` 的默认帧与基线逐字节一致（frozen Full 15/15、Visibility 1/1）。
3. **收益**：在 Bistro 派生资产、M1c 相同相机/配置下，hybrid ON 的三轮中位 GPU 帧时间相对 hardware-only（M1c ON 路径）有可复现改善；Task 1 先测量并给出上界，Task 6 给最终 ROI 判决（§8.3）。图像差异量化（changed pixels/mean/max），不宣称像素一致。
4. **可回归**：root Release 全量 CTest 绿（数量以实测为准）、CI 形态 ASan/UBSan 绿、frozen gates 不变、`test_docs` 通过、converter standalone 6/6 绿、证据归档 D: 且逐文件 SHA-256 核对、本地提交不 push。

## 2. 现状证据（file:line）

### 2.1 已接受的 lane 与 id 契约

- visibility lane = 两个 opaque producer 之一：mesh-shader raster 写 packed 32-bit id + 帧自己的 G-buffer depth；compute resolve 从 id 重建 5 个 G-buffer 目标（`docs/visibility_buffer.md:14-34`）。id = `valid1/item12/meshlet12/triangle7`（`source/engine/visibility/visibility.cppm:43-70`；`docs/visibility_buffer.md:30-34`）；对应上限 item 4096、每 item selected meshlet 4096、triangle 128、全局 host-culled 表 65536（`visibility.cppm:66-70`；`source/engine/meshlet/meshlet.cppm:59`）。
- raster mesh stage 每 workgroup 一个 meshlet：从 run 表取 48B record，跳过视锥外 meshlet，emit 全三角形并把同一个 id 附到三个顶点；id 的 meshlet 字段 = **该 item run 内顺序**（`source/shaders/scene/visibility.slang:17-26,91-179`，尤其 `:148-155`）。fragment 只做 alphaMode MASK 的 alpha test 后写 id（`:181-198`）。
- resolve 是 8x8 compute：每像素读 id（integer load）和 G-buffer depth（sampled `heap_slots_gbuffer_depth`，`heap_sampler_gbuffer`）；从 item 表取地址/model、从 meshlet 表取记录、读 vertices/indices 重建三角形；做过近平面 Sutherland-Hodgman 裁剪；用 **screen-space 线性 z** 与存储深度做 tolerance=1e-3 的一致性检查；写 5 个 storage 目标并统计 unresolved/depth mismatch（`source/shaders/scene/visibility_resolve.slang:15-38,308-412,466-508`）。CPU 侧参考数学已存在：`clip_to_screen`/`triangle_twice_area`/`screen_barycentrics`/`perspective_correct`（`visibility.cppm:334-422`）。
- draw_item = 112B（item 表一行：vertex/index device address、index_width、meshlet_base、meshlet_table=0/1/2、material、motion、model）；run 元数据由 `apply_meshlet_run`/`apply_hzb_run` 统一写入 `draw_item` + raster push block（`visibility.cppm:130-270`）；`meshlet_source` 三值语义见 `:130-135`。
- 图声明在 `source/engine/render_graph/visibility.cppm`：raster `append`（`:54-82`）、HZB build/cull/recovery（`:132-182`）、resolve `append_resolve`（`:239-287`）；runtime 的帧组装顺序在 `source/engine/runtime/runtime.post_graph.cpp:636-1035`：HZB cull（如有）→ visibility raster（`:747-815`）→ [recovery active：hzb build（`:816-867`）→ hzb recovery cull（`:868-907`）→ visibility recovery raster（`:908-931`）] → visibility resolve（`:932-995`）；非 recovery 帧的 hzb build 在 resolve 之后 append（同文件 `:1020-1023` 附近），因此它 sample 的 depth 是 resolve 之前的最终 depth。

### 2.2 M1c 已验收的 cut/run 与容量

- M1c 把离线 converter 的 cluster DAG 导入 runtime，并在 visibility lane 上做 CPU screen-error cut：`subtilis_dag_asset.cppm`（DAG 层，`lod0_clusters()` `:71`、`maximum_selected_bound()` `:80`）、`subtilis_dag_lod.cppm:46`（DAG cut）、`subtilis_dag_import.cppm`（payload 翻译）、`subtilis_selection.cppm`（run 准备/发布/capacity crash-safe）、`source/engine/runtime/runtime.cpp:2051-2345`（`record_visibility_items` 的 4 个 phase；`:2205` 处 `prepare_item`；`:2222` 处 `apply_meshlet_run`；`:2294-2318` 写表与日志；`:2320-2345` capture-only cluster probe）。
- M1c 的 item run = converter 的 LOD0 叶簇（`dag_asset.lod0_clusters()`），硬件用现有 host-culled table（`meshlet_culled_write`）写入 run 的选中记录；`meshlet_table=1`；`run.count` 同时是 dispatch groups（`apply_meshlet_run` 写 block.geometry.index_count=run.count，`:130-170` 的注释与实现）。
- M1c 真实证据（`docs/reviews/2026-10-21-object-lod.md`）：Bistro 派生资产（551 primitive/2909 实例/4,209,006 实例三角形；`sources §1`），validation 关闭三轮中位 far GPU `24.46→20.11ms（−17.8%）`、near `21.33→18.58ms（−12.9%）`（`:57-63`）；并行 cut wall 1.245/1.202ms（`:44-48`）；far 4,208,625→3,318,617 triangles、41,030 records，near 3,626,626→2,860,109、35,444 records（`:66-67`）；表占用 50,815/65,536、最大 run 1395/4096、cut/容量回退 0（`:69-71`）；host 写入 2,294,912/2,019,392 B/帧（`:71`）。容量/正确性契约没有余量问题。
- M1c 的 M4 重探针（`:135-150`）：far 41,030 cut clusters 中 **23.28% 投影直径 <1px、38.23% <2px**，对应的可见三角形份额 **21.92% / 36.75%**；near 35,444 中 **5.37% / 15.95%**，三角形 **4.12% / 14.46%**（`docs/reviews/2026-10-21-object-lod/probe-far.json`、`probe-near.json` 的 `subpixel_clusters` 块）。离线 HZB 保守遮挡下界 far 0.73% / near 36.22%，但 M1c 矩阵**未启用 GPU HZB**（`:144` 明示列是离线潜在量，不是已实现 launch 收益）。
- **绝对 workload 下降**：M1b 时 far <1px 22.05%（`docs/reviews/2026-10-20-bistro-performance/README.md`），M1c 比例升到 23.28%，但 cut 后的总 records/三角形显著小于 M1b 的 50,808/4.2M；因此不能只凭比例启动软件光栅，必须测量 hybrid 的总收益（`:145`）。这正是 Task 1 的存在理由。
- M1c 结论：32-bit id 仍容纳真实场景，暂无切换 64-bit 的实测依据；M4 的 depth twin 必须结合 subpixel workload、GPU/HZB 收益、带宽与同步一起评估（`:147-148`）；M2 未触发（`:124-134`）。

### 2.3 与 lane 相邻的现有约束/设施

- 预留资源：heap slots `1239..1246` 从 v0.5 起保留给 "the v1 software raster may want"（`source/shaders/common/heap_slot_constants.slang:143-145`；`source/engine/render_layout/render_layout.cppm:352-355`）；`1231..1238` 是 visibility id 的 per-image family；`1087` 是 gbuffer depth；`1308..1313` 是 HZB 六个表；grid 共 2048 slots（`render_layout.cppm:165`，`heap_slot_constants.slang:207`）。M3 已退役的 capture-depth family 的 slots 注释（`docs/superpowers/plans/2026-10-17-visibility-buffer.md:179`）与这里的 1239..1246 是同一份预留。
- per-image image families 由 `pass::output_targets_pass` 拥有：15 个 family 的 ids/heap_slots/formats/flags/`select_resources` 机制（`source/engine/pass/output_targets.cppm:26-104`）；`visibility_buffer` 是其中第 15 个（`:33-50`）。新 per-image 资源按同一模式加入（§5）。
- pass 模式：compute pass（声明空 io、behaviour compute、shader name、`create`/`record_content`）如 `visibility_resolve.cppm:49-101`；depth-only graphics pass 先例是 shadow pass（`pass/shadow.cppm/.cpp`，depth attachment、无 colour）与 pass 框架的 depth 支持（`source/engine/pass/pass.cppm:523-528,653` 的 depth format 契约）。
- 图 barrier/layout 由声明推导，`graph::pass_ref`/`order_after`/`bind_pass` 的用法见 `runtime.post_graph.cpp:747-1035`；`append_*` 的 access scope 语义见 `render_graph/visibility.cppm`。
- 测量/观测设施：`DEREN_CLUSTER_PROBE_DUMP`（capture-only，写 `extent, view, proj, items, per-item model+run.count+records`；`runtime.cpp:2320-2345`）与 `scripts/measure/cluster_occlusion_probe.py`（投影直径/frustum/HZB headroom 的离线分析：world model scale、`radius_px = world_radius*focal/w`、`diameter=2*radius_px`、`<1px/<2px` 统计，见 `:108-138`；`docs/reviews/2026-10-21-object-lod/probe-*.json`）。M4 的 CPU 分类必须用与之一致的定义（§3.2），避免"比例口径不一致"。
- 测量工具：`capture.ps1`（`-Base/-Tag/-Overrides/-Frames/-Camera/-BuildDir`）、`check_render.ps1`（`-Full` 的 legacy 15/15、`-Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt` 的 1/1）、`scripts/measure/visibility_ab.py`、`scripts/measure/analyze_object_lod.py`、`scripts/measure/measure_object_lod.py`；M1c 的归档在 `D:/deren-assets/2026-10-21-object-lod/`（191 文件逐个 SHA-256，`docs/reviews/2026-10-21-object-lod.md:152-158`）。
- 最终审查的 scope ruling：M2/M4/M5 明确排除在 M1c 之外（`docs/reviews/2026-10-22-object-lod-final-review.md:33-43`）；本 spec 是 M4 的独立阶段。

## 3. 方案：判定、分类与输出设计

### 3.1 subpixel cluster 判据（阈值、谁分类、确定性）

一个 cut 选中的 meshlet 记录被归为 **subpixel candidate** 当且仅当它在当前 item 的 model/camera 下满足一个**保守**条件：

```
world_center = model * record.center
model_scale = ||model 的线性 3x3 部分||_F          (Frobenius 范数 ≥ operator norm，保守上界)
world_radius = record.radius * model_scale          (record.radius 已向外取整)
w = (view * world_center).z 的相反数                (标准透视/正交视图；与 cut 的 view 相同)
保守条件：world_radius < 0.98 * (w - epsilon_near) 且 (w - world_radius) > 0   // 球完全在近面前
focal = max(|proj[0][0]| * width, |proj[1][1]| * height) * 0.5
diameter_upper = 2 * world_radius * focal / (w - world_radius)                  // 透视
diameter_upper = 2 * world_radius * max(|proj[0][0]| * width, |proj[1][1]| * height) * 0.5 // 正交
subpixel ⇔ diameter_upper < threshold_px
```

- 用**保守上界**而不是 `cluster_occlusion_probe.py` 的中心近似：只有上界 < 阈值才把 cluster 移出硬件，任何半径/model 尺度/透视误差都只会把 cluster 留在硬件（安全方向）。对球完全在近面前（`w - world_radius > 0`）的检查同时保证 v1 软件 raster 不需要处理近平面裁剪；穿近面的 cluster 一律留在硬件（其三角形由硬件裁剪）。与探针比例的可比性：本判据比探针的 `2*radius_px` 近似更保守，因此 Task 1 报告的分类计数应 ≤ 探针比例，两者差异要解释而不是抹平。
- **谁分类**：CPU，在 M1c 已有的 per-item planning 阶段（`record_visibility_items` phase 2，`prepare_item` 之后、`apply_meshlet_run` 之前；`runtime.cpp:2205,2222`）用与 cut 相同的 `current_ubo.view/proj`、`extent`、`leaf.push.model` 计算。分类是纯函数、逐 record 确定性；**不允许**用 GPU 的 survivor/frustum 结果在 CPU 不可知时反向决定（这会把 v1 与 HZB lane 绑在一起，见 §5.5）。
- 阈值默认 `software_raster_threshold_px = 1.0`（M1c 探针的主口径）；2.0 作为测量用可配档（§9 Q1）。阈值不是 LOD error，只是"硬件 raster 不值"的工程判据。
- 分类结果按 **item 内稳定顺序**：先按原 run 顺序保留硬件记录（前缀），再把 subpixel 记录按原顺序追加为软件后缀。id 语义因此不变：硬件 id 的 meshlet 字段 = 前缀内顺序；软件 id = 前缀长度 + 后缀内顺序；两者都指向同一 item run 的表项（§3.3），resolve 的现有 `item.meshlet_base + id.meshlet_in_item` 解码不需要任何扩展。

### 3.2 输出设计：一个推荐方案 + 备选

| 方案 | 内容 | 结论 |
| --- | --- | --- |
| **A（推荐）** | 硬件 raster 保持写 depth attachment + 32-bit vis-id（但 dispatch 只覆盖硬件前缀）；软件 raster 写两张 per-image `R32_UINT` 辅助图像（`software_depth` = 深度 float bit pattern，`software_id` = packed id）；**resolve 先运行**（读未合并的硬件 depth + 辅助图像，按最小深度选候选并着色），**然后**一个 depth-only merge pass 用深度测试（LESS_OR_EQUAL + depth write）把软件深度合并进帧的 G-buffer depth | 改动局限在 raster 的 dispatch 拆分 + 两个新 pass + resolve 的候选选择；id/table/HZB 契约不变；深度只有一个最终 producer（merge 写回原 depth attachment），后续 lighting/toon/debug/HZB 继续用同一 depth；`1239..1246` 预留正好是一个 per-image family |
| B. 64-bit/two-level vis-id（depth/id 一起放进更宽 id，或两级表） | 改 id 编码/vis buffer 格式/硬件 writer/resolve decode | **不在 M4 v1**：需要改硬件 raster 的附件格式（R64 写入）、resolve 全链路 decode、id/table 契约与 M2 的联合决策；M1c 的实测（32-bit 仍容纳 2909 items/41k records、0 回退）不构成切换依据（`2026-10-21-object-lod.md:147`；`2026-10-18-subtilis-design.md:103,151` 已把 64-bit/两级表列为 M2/M4 联合决策）；作为 §9 Q2 保留 |
| C. 单张 R64 原子最小图（imageAtomicMin） | 硬件/软件都原子写同一 64-bit 图 | 拒绝：硬件 raster 的 attachment 写与 compute 的 storage 原子写同一图像不可行（格式/使用/同步都不允许，且依赖 `shaderImageInt64Atomics` 可选特性）；把硬件 raster 改成 storage 写会失去固定功能光栅/深度测试，等于重写硬件路径 |
| D. 硬件 raster 不动、软件 raster 只补空洞（不减少硬件 dispatch） | 仅增加覆盖 | 可作为 v1 的"纯正确性"回退，但**没有 ROI 机制**（硬件照旧付全价），只用于测量对照/降级，不作为目标路径 |

方案 A 的深度/取舍/保守性细节：

- 软件图像语义：`software_depth` = `float` 深度（NDC z ∈ [0,1]）的 **bit pattern**（`asuint`，便于整数比较/存储），"空" = `0xFFFFFFFF`（比任何合法正 float bit pattern 都大）；`software_id` = 32-bit packed id，"空" = `0`。软件 raster 的每个 tile 都把自己 16×16 像素的两张图全部写一遍（含空/最小值），因此无需 clear、没有跨帧残留。
- 为什么是两张 `r32_uint` 而不是一张 `r32g32_uint`：RHI 的 `image_format` 枚举目前只有 `r32_uint`/`r16_sfloat` 等（`source/promise/rhi/rhi.api_core.cppm` 的 `enum class image_format`），新增 `r32g32_uint` 要同时改 RHI 契约、Vulkan 后端格式映射、host readback 与测试；用两张现有格式的 per-image family 达到同样的 8B/px 与非原子写，改动面显著更小（§9 Q3）。
- 软件 raster 对每个像素在 tile 的候选列表上做**逐像素扫描**，取 `(depth, id)` 的字典序最小：`depth` 更小者胜；**depth 相等时 packed id 更小者胜**。扫描顺序不影响结果（比较是全序），因此确定性不依赖 tile 列表内的原子写入顺序。
- 深度插值：`z_ndc = Σ screen_barycentric_i * (clip_i.z / clip_i.w)`（与硬件 raster 的 screen-space 线性 z 一致，resolve 也用同一表达式做 depth check，`visibility_resolve.slang:408-412`）；只接受有限值并 clamp 到 `[0,1]`；像素中心 `(x+0.5,y+0.5)` 的 barycentric 包含测试（与 resolve 的 `clip_to_screen`/`screen_barycentrics` 相同数学，`visibility.cppm:334-389`）。因为被选中 cluster 的球完全在近面前且投影直径 < 阈值，v1 软件路径不写自己的近平面裁剪器；越界/退化/背后的三角形直接跳过（记录到 stats，由分类的"球在前"条件保证不会丢失应可见的整簇）。
- 软件像素覆盖与硬件 edge rule 的差异：软件写入的像素若硬件也写了，则由 resolve 的候选选择决定（§3.2 候选规则）；两候选深度相等时选硬件 id（tie 归硬件）。因此 1px 边界上可能出现的差异有界、可测量，不宣称像素一致。
- merge pass（**在 resolve 之后**）：fullscreen depth-only graphics pass，无 colour attachment（shadow pass 已有 depth-only pipeline 先例，`pass/shadow.cppm/.cpp`；pass 框架 depth format 契约 `pass.cppm:523-528,653`）。fragment 读软件深度图像：`id==0` → 不写；否则 `gl_FragDepth = clamp(asfloat(depth_bits),0,1)`，pipeline depth test = `less_or_equal`、depth write = true。于是 depth attachment 被更新为 `min(硬件 depth, 软件 depth)`；merge 不需要读回硬件 depth（比较由深度测试完成），resolve 也不写 depth，所以 merge 与 resolve 之间只有「resolve 读 depth、merge 写 depth」的 WAR 依赖。recovery 帧不会与 hybrid 同时活跃（§5.5），因此 merge 之后不会再出现其它 depth writer；lighting/TAA/后续 depth-tested 阶段与（非 recovery 的）HZB build 都在 merge 之后读 merge 后的 depth。
- resolve 候选选择（唯一改动，**在 merge 之前**）：resolve 读硬件 id（现有）、辅助图像 `(sw_depth, sw_id)`，以及**未合并的**硬件 G-buffer depth（现有 sampled `depth_value`）。`software_visible = sw_id != 0`；`win = software_visible && (asfloat(sw_depth) < depth_value || (asfloat(sw_depth) == depth_value && !hw_id.valid()))`。**选中的候选深度同时用于重建**：软件胜出时用 `sw_depth` 作为 reconstruction/tolerance check 的 depth；硬件胜出时用现有 `depth_value`。因此软件严格更近时一定胜出（此时 merge 尚未发生，`depth_value` 仍是硬件深度）；相等且硬件 id 有效时 tie 归硬件；相等且硬件 id 无效（背景/clear 1.0）时软件胜出。比较是读取到的 float 值的**精确比较**（无 epsilon）：硬件/软件在名义相等深度处可能相差 1 ULP，此时按严格 `<` 决定，结果确定且逐帧一致。软件像素随后由 merge 把 `sw_depth` 写进 depth attachment（因为 `sw_depth < depth_value`），所以 lighting/HZB 看到的最终 depth 与该像素的着色几何一致；若两者深度相等，merge 写同值、不改变 depth，着色的硬件 id 与该 depth 一致。
- **id decode 扩展**：不需要。软件 id 与硬件 id 同一 32-bit 布局，只是 `meshlet_in_item` 落在 item run 的软件后缀（`prefix_count + k`），resolve 用现有 `item.meshlet_base + id` 从同一 run 表读记录；M1c 已把 run 表与该解码链固定。这也满足"M4 不切换 32-bit id"的约束。

### 3.3 dispatch 拆分与 run 表（不改变 id 契约）

M1c 的 item run 是 `[选中 records]`，`run.count` = dispatch groups。hybrid 改为：

```
run 表内容 = [硬件前缀 records..., 软件后缀 records...]        // 顺序稳定，见 §3.1
item.meshlet_base = run.base（不变）
item.meshlet_table = 1（culled，不变）
block.geometry.first_index = run.base（不变）
block.geometry.index_count = hardware_prefix_count               // ← dispatch groups 只覆盖前缀
block.geometry.base_vertex = 1                                   // host-culled 标记（不变）
```

硬件 id 的 meshlet 字段 = `group_id`（0..prefix-1）→ 记录在前缀；软件 id 的 meshlet 字段 = `prefix + k` → 记录在后缀；resolve 对两者的表读取完全一致。软件 raster 需要的 `(item_index, meshlet_id)` 由 CPU 分类时直接写入 per-frame tile buffer（§3.4），不需要再从 GPU 反查 run。`apply_meshlet_run` 增加一个带 `hardware_count` 的变体（或扩展 `visible_run`），只影响 push block 的 dispatch groups，不改 item/table 语义；`apply_hzb_run` 的 hzb 路径不参与 hybrid（§5.5）。

### 3.4 tile 化软件 raster（分类、binning、执行）

- **CPU tile binning**（与分类同一 planning 阶段，纯 CPU、确定性）：tile = 16×16 像素；对每个 subpixel record 用其保守投影包围盒（由 sphere 的屏幕外接矩形，clamp 到帧内）得到覆盖的 tile 范围，把 `(item_index, meshlet_id)` 追加到每个 tile 的固定容量槽。默认 `tile_entry_capacity = 32` 条/tile；任一 tile 超容量 → 该帧 hybrid 整帧关闭（硬件完整路径，日志 + stats），**不截断**。CPU 端 binning 的排布与顺序确定（item 顺序 → run 顺序 → 固定 tile 顺序），因此不依赖 GPU 原子。M1c 数据（far 9,552 个 <1px、15,686 个 <2px cluster / 3600 tiles）的**均值**远低于容量，但 far 密集区域的**局部最大值**可能超过 32：Task 1 必须对 1.0/2.0 两档阈值实测 per-tile 最大 entry 数与 overflow 帧数，并据此记录容量决定（32 或 64；720p 下 cap 64 的 entries 为 3600×64×8B ≈ 1.84MB/lane，仍很小）。在 Task 1 数据出来之前不假设 32 足够。
- **buffer**：一个 per-frame-lane 的 host-written buffer `software_tiles`：`header{tiles_x, tiles_y, capacity, overflow, cluster_count, entry_count} + counts[tiles] + entries[tiles×capacity]`（entry = `{item_index:u32, meshlet_id:u32}`）。每个 frame slot 一份（frame ring 的 lane 数，通常 2），host 每帧写；heap slot 追加在 HZB 之后（§5.2）。二进制兼容性：这是引擎内部缓冲，不是磁盘/GPU ABI。
- **GPU 软件 raster pass**（`pass/software_visibility_pass`，compute，`behaviour_kind::compute`、`group_size=8×8`、`extent_rule::full` 不可用于 tile 网格时采用显式 dispatch = tiles）：one workgroup per tile（16×16 px，64 threads，每线程 4 像素）。对每个像素遍历该 tile 的 entry 列表：从 heap 的 draw_item 表/item 表取地址与 model，从 host-culled run 表取 record；把 record 的 ≤85 个三角形的顶点（device address 读 index → vertex，与 resolve 的 fetch 完全同构，`visibility_resolve.slang:274-306`）投影为 clip/screen，逐三角形做像素中心包含与 screen-linear z 插值，在寄存器里维护该像素的 `(min_depth, min_id)`；最后把每像素的 `(depth_bits, id)` 写穿到软件图像。tile 无 entry 时也把 16×16 写成空，保证图像全量初始化。
- **确定性**：pass 内无原子（每像素由唯一线程拥有；tile 列表 per-tile 互斥）；结果只依赖 tile 列表集合（顺序无关的全序比较）与固定数学；同一输入两次运行像素一致（GPU fixture 检查）。
- **成本模型（用于 Task 1 的预算，不是收益声明）**：最坏 tri-pixel 测试 ≈ Σ_tile (entries × ≤85 tri) × 256 px；以 M1c far 9,552 cluster（平均 ~76 tri/cluster，`21.92% × 3,318,617 / 9,552`）和平均 tile 列表 ≤ 3 估算，再乘每测试的少量 FMAs；Task 1 用 CPU 模型 + 实测 GPU fixture 校准，Task 4/6 用真实帧计时。该模型只用于判断"是否有机会"，不作为收益证据。

### 3.5 为什么这是 v1 的正确取舍

- 方案 A 是**唯一**能在不改变 32-bit id/table 契约、不重写硬件 raster、且保持单一最终 depth 的前提下把 subpixel 工作量移出硬件 raster 的设计；B/C 都是跨 M2 的 id/格式决策，D 没有收益机制。
- v1 的 hybrid 只覆盖"球完全在近面前、投影上界 < 阈值、材质可被现有策略接纳（非 MASK/BLEND 等，§5.1）"的 item；其余一律硬件完整路径。这个保守边界让软件 raster 不需要 material/MASK、近平面裁剪、skinning/instancing、HZB survivor 表等复杂语义，先把"tile 化深度/id 生产 + merge"的闭环做对。
- 最终 ROI 不假设：M1c 的重探针给了比例与三角形份额，Task 1 测"硬件把 subpixel 工作量让出去能省多少"的上界，Task 6 用 hybrid 实测判决（§8.3）。

## 4. 接口与公共类型

### 4.1 CPU 参考模块（新，纯 CPU）

`source/engine/visibility/visibility_software.cppm`（模块名 `deren.engine.visibility.software`，只 import `deren.engine.visibility`/`deren.engine.meshlet`/`vstd`）：

- `subpixel_classification classify(meshlet_record + model + view_parameters + extent, threshold_px) -> { bool subpixel; double diameter_upper; bool front_of_near; }`（§3.1 的保守判据；纯函数、无分配）。
- `tile_grid`（tiles_x/y、tile 尺寸 16、capacity）；`build_tile_lists(span<classified record>, items) -> expected<software_tile_plan, software_error>`（§3.4 CPU binning；capacity/overflow 检测；纯 CPU）。
- `software_image_ref`：`struct software_pixel { uint32_t depth_bits = empty; uint32_t id = 0; }`；`resolve_candidate(hardware_id, hardware_depth, software_pixel) -> chosen { id, depth, source }`（**merge 之前**的 resolve 语义：`sw_depth < hardware_depth` → software；严格相等且 `hardware_id` 无效 → software；其余 → hardware。纯函数，供 CPU reference 与测试使用；GPU resolve 的 shader 对照物）。成功后由 merge 把 `min(hardware_depth, sw_depth)` 写进 depth attachment。
- CPU reference rasterizer：`rasterize_tiles(tile_plan, run_table, item_table, vertex/index accessors, extent, out image)`（用与 shader 相同的数学；用于 fixture 的期望值与 GPU probe 的比较）。它消费与 GPU 相同的“已展开数据”而不是重做 import/loader。
- CPU reference 只用于测试/测量脚本（不进生产热路径）；建议放在 `visibility.cppm` 已有 CPU 参考数学旁的独立模块，避免把 shader 对应逻辑混进 admissions 模块。

### 4.2 资源/图/渲染接口（要点）

- **新 per-image 资源 family（两个）**：`render_resource::resource_id::software_depth` 与 `software_id`（均 `image2d`、`r32_uint`、`per_swapchain_image`、output-size、flags `sampled|storage`）。加入 `resource_table`（`render_resource.cppm:244-260` 附近）、`output_targets_pass` 的 ids/heap_slots/formats/flags（resource_count 15→17；`software_depth` 用预留的 `heap_slots::software_depth = heap_slot_base + 1239`，`software_id` 追加 per-image family 于 `heap_slot_base + 1314..1321`）、runtime 的 per-image 数组与 publish/adopt（`output_targets_pass` 的 `select_resources` 只在 feature ON 时请求两者，保证 OFF 启动不额外分配/写 descriptor）、后端 `core::heap_slots` 镜像与 `heap_slot_constants.slang` 同步（`test_render_resources` 会核对两份常量）。
- **新 per-frame-lane buffer**：`software_tiles`（§3.4），heap slot 追加在 `software_id` 之后（`heap_slot_base + 1322`，grid 仍 ≤ 2048），由 runtime 在 extent/帧初始化时分配并 host 写；HZB 的 slot 编号 1308..1313 不变。
- **图声明**（`render_graph/visibility.cppm` 追加）：
  - `append_software_raster(builder&, inputs) -> outputs`：compute pass，参数 = `software_depth`/`software_id`（各 `texture_discard_write`/storage write，scope `compute_shader`）、`software_tiles`（`buffer_read`，storage/compute）、camera（uniform/compute）、items/meshlets/culled_meshlets（`buffer_read` storage/compute）、statistics（`buffer_read_write` storage/compute）、meshlet 表（`buffer_read`）；显式 `order_after(raster)`（run 表与 depth 的依赖之外，id 的语义要求它在 hardware raster 之后，见 §5.1）。
  - `append_software_merge(builder&, inputs) -> outputs`：graphics depth-only pass，参数 = `software_depth`（`texture_read`/sampled/graphics fragment 或 depth-only 用只读 sampled）、`gbuffer_depth`（`texture_read_write` depth attachment，scope `depth_stencil`）；显式 `order_after(resolve)`（resolve 读 depth、merge 写 depth，图的 WAR 会给出同一顺序；显式边把「resolve 先于 merge」变成契约）；非 recovery 的 HZB build 在其后 append，由 depth 的 RAW 依赖排在 merge 之后。
  - `append_resolve` 的 `resolve_inputs` 增加 `std::optional<texture_ref> software_depth`/`software_id` + 参数 `texture_read`（sampled/compute）；两者都为空时完全等价于现有 resolve（OFF 路径）。
- **pass**：`source/engine/pass/software_visibility.cppm/.cpp`（compute，shader `software_visibility.comp.spv`，8×8 或 16×16 workgroup，record 用 push block 的 frame_slot/image_index + tile 网格；`ready()`/`has_pipeline()` 同 resolve）与 `source/engine/pass/software_merge.cppm/.cpp`（depth-only graphics，shader `software_merge.vert.spv`+`software_merge.frag.spv`，pipeline 在 `pipelines` 模块新增 `build_software_merge`，depth format 从 `pass_context` 的 depth 契约取，参考 `build_shadow` 的 depth-only 构造）。
- **shader**：`source/shaders/scene/software_visibility.slang`（compute entry 1 个）与 `source/shaders/scene/software_merge.slang`（vertex+fragment 2 个 entry）。vertex fetch/投影的共享代码建议从 `visibility_resolve.slang:274-306` 抽成 `source/shaders/common/mesh_fetch.slang` 由两边 include（refactor 必须让 Visibility frozen 1/1 仍逐字节一致，作为"抽取无行为变化"的门禁）；不允许复制两份投影/fetch 表达式。
- **shader 注册**：`source/app/chores.cpp` 的 `load_shader` 列表/`load_and_create_pipeline`、根 CMake 的 `VR_SLANG_SOURCES`、两个 shader 编译脚本、`pass_context::shader` 注册名（`software_visibility.comp.spv`、`software_merge.vert/frag.spv`）；`test_shader_sources` 会解析这四处（见 `docs/...` 其 header），必须同步。
- **配置**：`[render] software_raster = false`（默认 OFF）、`software_raster_threshold_px = 1.0`（finite、>0）；`render_settings`/parser/`config.example.toml`/`app/main.cpp` → `runtime::set_software_raster(bool)`/`set_software_raster_threshold(float)`；HZB 用 env gate 的先例（`DEREN_HZB_BUILD/CULL`）说明实验开关也可以用 env 覆盖，但本 feature 用配置键 + runtime setter，便于 capture harness 的 `-Overrides`。OFF 时 `set_*` 不创建 pass/资源。
- **runtime 状态**：`software_raster_on/t`（帧冻结，类似 `subtilis_active_frame`）、per-frame `software_tile_plan`（host 写入、tile buffer 映射）、`software_overflow_logged`、每场景/每 120 帧的汇总统计（clusters/tiles/winning pixels/overflow frames）；`scene_import_result` 不变（与 import 无关）。

### 4.3 分类/发布的插入点（M1c 代码上）

- `record_visibility_items` phase 2 的 `plan_item`（`runtime.cpp:2108-2177`）：在 `prepare_item` 的 publish 回调里拿到完整 selected records（`:2205`），做 §3.1 分类 + §3.4 tile binning，按 `[hw|sw]` 重排后写 run 表（`meshlet_culled_write` 的新 split 变体），并在 plan 里保存 `hardware_count`/`software_count`/tile overflow 状态；`vis::apply_meshlet_run` 的 hybrid 变体写 push block（§3.3）。
- phase 4（`:2253-2400`）：`plan.block.geometry.index_count` 已是 hw count；dispatch 与 HZB item 描述逻辑不变（HZB 帧 hybrid 关闭）；软件 raster/merge 的 pass 在图组装阶段（`runtime.post_graph.cpp`）append；software_tiles buffer 在 phase 2 后、录制前映射写入（每帧 lane）。
- hybrid 的帧级冻结开关（在 `runtime.frames` 的帧判决处，与 `subtilis_active_frame` 相同位置）：`software_active_frame = software_raster_on && subtilis_active_frame && visibility lane admitted && !hzb_cull_active_frame && !hzb_recovery_active_frame && item policy 允许`；冻结后 phase 2/4/图组装不允许再变（否则会出现半新半旧帧）。
- 分类/发布的 per-item 策略：沿用 `coarse_allowed` 的既有排除（MASK/BLEND flags、exact-depth redraw、transparent/overlay、custom pipeline、skin/morph、非 64B layout、instanced）。hybrid 只是把它已同意的 item 内的部分 record 从硬件移到软件。

## 5. 架构与逐帧顺序

### 5.1 推荐顺序

```
(现有) CPU cut + run 发布（含 M4 分类与 [hw|sw] 拆分、CPU tile binning）
  → (现有) HZB cull（如有；hybrid 帧要求 cull lane 不活跃）
  → (现有) hardware visibility raster（dispatch = hw 前缀，写 depth attachment + vis-id）
  → (新)   software visibility compute raster（tile 化，写 software_depth/software_id 两张辅助图）
  → (现有) visibility resolve（读**未合并的**硬件 depth + vis-id + 可选 software 图像，按最小深度选候选并着色）
  → (新)   software merge depth-only pass（在 resolve 之后写回 gbuffer_depth = min(hw, sw)）
  → (现有，recovery 活跃时 hybrid 关闭) HZB build（非 recovery 帧在 resolve 之后 append）
  → (现有) lighting/TAA/post（不变）；shadows/BLAS 仍只用 LOD0（不受 M4 影响）
```

顺序理由：软件 raster 必须在硬件 raster 之后（它的 id 后缀引用完整 run 表）；**resolve 必须在 merge 之前**——resolve 的 winner 规则需要看「未合并的硬件 depth」才能区分「软件严格更近」与「tie」；若 merge 先发生，软件胜出的像素 depth 会被改写成 sw_depth，规则就无法区分二者（这是本次 review 抓到的阻塞缺陷，必须由次序保证）。resolve 不写 depth，merge 用 depth test（LESS_OR_EQUAL + depth write）把软件深度并回同一 depth attachment；随后 lighting/TAA/后续 depth-tested 阶段与（非 recovery 的）HZB build 都读 merge 后的 min depth——**保守方向**（软件几何作为 occluder 也被计入）。在非 recovery 帧里 HZB build 在 resolve 之后 append（`runtime.post_graph.cpp:1020-1023`），merge 也插在 resolve 之后、build 之前，图由 depth 的 RAW 依赖保证 build 在 merge 之后；recovery 帧 hybrid 关闭，M3 行为逐字节保持。

### 5.2 资源 lifetime / 同步（图上声明，不手写 barrier）

- `software_depth`/`software_id`：per swapchain image，与 visibility id 同 generation 生命周期；写入用 graph 的 storage write scope（`compute_shader`），被 merge/resolve 以 `sampled` 读（layout 转换由 graph 生成）；图像在 `output_targets_pass` 的 per-image family 中创建/发布，resize 随 generation 重建；OFF 时 family 不被 `select_resources` 请求（不分配、不写 descriptor），从而 OFF 帧与 baseline 逐字节一致。
- `software_tiles`：per-frame lane host-written；在 phase 2 后、录制前由 runtime 映射写（与 `meshlet_culled_mapped`/items 表的现有 host 写模式相同），图声明为 compute storage read。frame ring 的 lane 隔离保证 in-flight 帧不读被下一帧重写的 lane。
- merge pass 的 depth attachment 与 raster/其它 depth 消费者由 graph 的 WAW/RAW/WAR 推导 barrier；`order_after` 只表达「语义上必须」的次序（raster→software→resolve→merge→HZB build），不替代 hazard 分析。resolve 只读 depth、不写 depth；merge 写 depth；HZB build 读 depth，三者由 WAR/RAW 依赖串起来，显式 `order_after(resolve)` 与 build 侧的依赖断言共同固定 resolve-before-merge-before-build 次序。
- HZB 的 6 张表/visibility 表/items/indirect 的读写 scope 不因 M4 改变；hybrid 帧的 HZB cull 不活跃 → 不出现"raster 读 survivor 表而软件路径读 culled 表"的歧义。

### 5.3 resolve / merge 的 id decode（无扩展）

- resolve 的输入增加可选 sampled `software_depth`/`software_id`；`resolve_pixel` 前置一步候选选择（§3.2）；选中软件 id 后完全复用现有重建：`resolve_indices(item, packed, meshlet_first_index)`（`visibility.cppm:355-380` 附近的现有函数）+ item 表 + meshlet run 表。软件 id 的 `meshlet_in_item` 是 run 内后缀 index，`item.meshlet_base + id` 直接命中；`triangle` 字段仍是 meshlet 内序号。
- 深度一致性：resolve 在 merge 之前运行，读到的 `depth_value` 是未合并的硬件 depth；候选选择用 `sw_depth` 与它比较，**重建/tolerance check 使用所选候选自己的深度**（软件胜出 → `sw_depth`；硬件胜出 → 硬件 `depth_value`），因此软件像素的 z_ndc 与检查值一致（两者都来自软件 raster 的同一插值）。resolve 只写 G-buffer 颜色；随后 merge 把 `min(hw, sw)` 写进 depth attachment，lighting/TAA/HZB 读 merge 后的 depth；因为 resolve 已用所选 id 着色，颜色与最终 depth 属于同一候选。软件像素的 `is_front` 由 screen-space 有向面积决定（与 resolve 现有 `triangle_twice_area` 相同；软件的像素中心覆盖用同一屏幕坐标约定）。
- 材质：hybrid item 被 `coarse_allowed` 限制为非 MASK/BLEND；resolve 仍从 item 行取 material 并做共享 decode，与现有路径一致；MASK/BLEND 目标整项留在硬件（含 raster fragment 的 alpha test），因此软件路径不需要材质数据。
- stats：扩展现有 `mesh_stat_*` 的 16-lane stride（`mesh_geometry.slang:175-203`；C++ 侧 stride 契约见 `runtime.constructor.cppm:563,1087`）的空闲 lane 12..15：`software_clusters`、`software_candidate_triangles`、`software_winning_pixels`（resolve 端每 workgroup 归约后一个 atomic，避免每像素 atomic）、`software_overflow_frames`；host 在 shutdown/每 120 帧打印一次，作为测量与 fallback 证据。

### 5.4 容量/回退（无部分覆盖）

| 条件 | 行为 |
| --- | --- |
| `software_raster=false`（默认） | 不创建 pass/pipeline/资源、不 append 任何新 pass；帧与 baseline 逐字节一致（frozen Full 15/15 + Visibility 1/1） |
| 分类后无 subpixel record（该帧全部硬件） | 不 append software/merge pass（帧即 hardware-only；stats 记 0） |
| 任 tile 超过 capacity（默认 32；Task 1 依据 per-tile 最大值决定 32/64）、或 tile 数/entry 数超过 buffer 容量、或 CPU binning 失败 | **整帧** hybrid 关闭：重新用完整 run 发布（hw prefix = 全部记录），不 append software/merge；日志一次 + stats `software_overflow_frames`。不截断、不写部分 software 图像。Task 1 必须实测 1.0/2.0 两档的 per-tile 最大值与 overflow 帧数；若密集 far 区域频繁 overflow，优先把默认容量提到 64（720p 成本 ≈1.84MB/lane）而不是接受频繁的整帧回退 |
| `software_depth`/`software_id` 资源创建失败 / merge pipeline 创建失败 / pass 未就绪 | 该帧 hybrid 关闭（hardware-only）；若持续失败，`software_raster` 视为不可用（日志一次），后续帧全部 hardware-only |
| HZB cull 或 recovery lane 活跃 | hybrid 关闭（v1 决策；§9 Q5）；M3 帧逐字节保持 |
| item 策略不满足（MASK/BLEND、deform、instanced、custom pipeline、exact-depth、非 64B layout） | 该 item 不分类（全部硬件）；不产生软件 entry |
| 帧 admission 失败（现有 visibility admission） | 现有整帧 legacy geometry lane（与 M4 无关） |
| item run 叶簇数/记录数超过 4096 或全局表不足 | 现有 M1c 的确定性回退（LOD0/整项），M4 不新增截断点 |
| resolve/merge 的 depth 出现非有限/超出 [0,1] | 软件像素视为空（跳过）；stats 记一次；不写 NaN 到深度。resolve 之后 merge 只把 `min(hw,sw)` 写进同一 depth attachment（LESS_OR_EQUAL + depth write），不新增 depth 格式/采样源 |

### 5.5 HZB 组合（v1 互斥 + 保守方向）

- v1：`software_active_frame` 要求 `hzb_cull_active_frame == false && hzb_recovery_active_frame == false`。因此在 M3 cull 活跃的帧，M4 完全关闭，M3 的 survivor 表/indirect/bit2 语义不受影响；recovery 帧同理。非 cull 帧若 HZB build 仍启用（`runtime.post_graph.cpp:1020-1023` 在 resolve 后 append），merge 也插在 resolve 之后、build 之前，因此 build 采样的是 merge 后的 min depth，符合「软件几何也作为 occluder」的保守方向；该次序由图的 depth WAR/RAW 依赖 + 显式 order 断言固定。
- 未来（不在本阶段）：若要用 HZB survivor 列表与 M4 协同，需要 GPU 侧 classification/stable id 映射（survivor 顺序 ≠ run 顺序），与 M2 的 stable cluster identity 决策一起设计；本 spec 不设计它，只记录为 M4 后续。

## 6. 容量、内存与带宽

### 6.1 id/表容量（不改变）

- packed id 仍 `valid1/item12/meshlet12/triangle7`（`visibility.cppm:43-70`）；item ≤4096、每 item selected ≤4096、全局 host-culled 表 65536。M1c 实测 2909 items / 41,030 records / 50,815 table / max run 1395（`2026-10-21-object-lod.md:52-71`）仍有 4096/4096/table 余量；M4 不扩这些上限，只改 dispatch 拆分与软件结果。
- 软件 entry 的 `meshlet_id` 仍是 12-bit 字段的取值（run 内 index），因此 `prefix + software_index < 4096`；分类时检查 `prefix + software_count ≤ id_meshlet_capacity`（现有 M1c item run 已 ≤ 4096，拆分不增加总数）。
- 软件 tile entries 上限：`tiles_x*tiles_y*tile_entry_capacity`；cap 32 时 1280×720（80×45 tiles）115,200 entries ×8B ≈ 0.92MB/frame lane、1920×1080（120×68）≈ 2.09MB/lane、4K ≈ 8.3MB/lane；cap 64 时对应约 ×2（720p ≈1.84MB/lane、1080p ≈4.2MB/lane）。默认 cap 由 Task 1 的 per-tile 最大值/overflow 实测决定（32 或 64）；溢出按 §5.4 整帧回退，因此不需要更大的固定上限。
- `software_depth`+`software_id` 合计 8B/px per swapchain image（各 4B/px）。1280×720 = 各 3.52MiB、合计 7.03MiB/图；3 个 swapchain image ≈ 21.1MiB（4 个 ≈ 28.1MiB）。1920×1080 = 各 7.91MiB、合计 15.82MiB/图，3 个 ≈ 47.5MiB；4K 合计 35.6MiB/图，3 个 ≈ 106.7MiB。1280×720 是本阶段测量基准；一个缩放分辨率（建议 1920×1080）在 Task 6 记录内存/带宽，不要求全部达到。
- 带宽（每帧，1280×720）：软件 raster 写满两张辅助图合计 921,600×8B = 7.03MiB（每 tile 全写）；读 tile buffer ~0.9MiB + tile 内 vertex/index/run 表稀疏读；merge pass 读 `software_depth` 3.52MiB + depth attachment load/store 约 7.0MiB；resolve 读 `software_depth`+`software_id` 7.03MiB（其余为现有读）。新增合计约 20-25MiB/帧（<1.3GB/s at 60fps），相对 20ms 级帧预算不是主导项；Task 6 报告实测 GPU pass 时间而不是带宽推断。
- 与 64-bit id 方案的内存对比（仅作决策输入）：64-bit vis buffer 在 720p 只多 3.5MiB/图，但硬件 raster 的 attachment 写、深度/ID 合并方式、resolve decode 与 M2 的 selection identity 都要一起变；M4 v1 不付这个复杂度（§9 Q2）。

## 7. 测试与验收

### 7.1 CPU reference 与 fixture（CI、无设备）

- `test_visibility_software`（新 CPU target）：`classify` 的阈值边界（正好阈值、`nextafter`、透视/正交、非均匀 scale/shear、球跨近面、NaN/负半径、`w-world_radius≤0` 一律不分类）；CPU tile binning 的确定性/容量/overflow（含 per-tile 最大值统计函数）；`resolve_candidate` 的 winner/tie 规则（软件严格更近胜出；相等且硬件 id 有效 → 硬件；相等且硬件 id 无效（背景 clear 1.0）→ 软件；空 id → 硬件；软件胜出时 chosen depth = sw_depth）；CPU reference rasterizer 对固定 fixture（1-4 个 subpixel 三角形、已知屏幕位置/深度、深度相等的 tie、退化三角形、背后三角形、clamp 边界）产生期望 `(depth_bits,id)` 图像，并与独立 Python 参考（`scripts/measure/` 的 fixture 校验器）一致。测试不依赖 Vulkan/设备；所有 fixture 的输入数据（item 表/run 表/顶点索引）在测试内构造。
- `test_visibility_software_pass`/`test_visibility_merge_pass`（新，headless）：pass 声明/behaviour/`io()`/pipeline gating/record 前置条件；参考 `test_visibility_raster_pass.cpp`/`test_visibility_resolve_pass.cpp` 的模式（不能靠设备，只能钉声明与非法输入拒绝）。
- `test_render_graph_visibility`（扩展）：新的 graph 声明：software raster 在 hardware raster 之后；**resolve 在 merge 之前**（显式 order 边 + depth 的 WAR）；merge 读 `software_depth` 并写 `gbuffer_depth`（depth attachment）；非 recovery 的 HZB build 在 merge 之后（depth RAW）；software 图像被 resolve 以 sampled 读；OFF 时不 append 新 pass（现有断言不回归）；HZB cull 帧/recovery 帧不 append software merge pass（由 runtime 决策，但 graph lane 的合法性/order 测试覆盖）。
- `test_subtilis_selection`/`test_visibility_buffer`/`test_subtilis_frame`（扩展）：分类/拆分后的 run 表与 `apply_meshlet_run` 变体（硬件 prefix 的 dispatch count、id 语义、容量边界 4095/4096/4097、overflow 整帧回退）；`admission`/`coarse_allowed` 的 item 策略不回归；resolve 候选选择的纯函数在 CPU 侧测试（id 选择结果），与 shader 的表达式由 fixture/测量对照。

### 7.2 GPU fixture 与 probe（单机、capture-only）

- 新 probe：`DEREN_SOFTWARE_RASTER_PROBE=<dir>`（capture-only 环境变量，与 `DEREN_CLUSTER_PROBE_DUMP` 同样只在截图帧动作、不改算法/表/draw metadata），dump 每帧：extent、view/proj、tile header/counts/entries、`software_depth`/`software_id` 两张图像（readback）、hardware vis-id/depth（可选 readback）、mesh stats；另外 dump 一份 CPU reference 输入（item/run/vertex/index 所需的已展开数据或可重放的引用）。`scripts/measure/verify_software_raster.py`（新增）：
  - **CPU reference vs GPU**：用 dump 的 camera/model/记录重放 CPU reference，与 GPU 软件图像逐像素比较（深度按 exact bit 或定义容差；id 应完全一致；记录差异清单，要求 0 个像素差异，否则 FAIL）；
  - **resolve/merge 一致性**：dump 的 merge **后**最终 depth 应等于 `min(hw_depth, sw_depth)`（tie 归硬件，写同值）；resolve 选软件候选的像素，其 tolerance check 使用 `sw_depth` 且应通过；选硬件候选的像素使用硬件 depth。检查随机采样与全图抽查；`verify_software_raster.py` 还必须断言图/录制顺序为 resolve→merge（probe dump 的 pass 顺序或 gpu marks 区间），否则 FAIL；
  - **id decode**：软件 id 经现有 `resolve_indices`（Python 侧复算 item.meshlet_base+id 与 run 表）解析出的 record 与 CPU reference 的选择一致；
  - **determinism**：同一相机两次 capture 的 software 图像/最终帧 hash 完全一致；
  - 负向自检：缺 dump/格式错/伪造图像/截断 capture 必须拒绝（与现有 verify 脚本一致的 self-test 风格）。
- Vulkan validation：fixture 与 Bistro 测量都在 `validation_layers=true` 下跑；report 必须 0 error/warning/VUID（M1c 有先例 `2026-10-21-object-lod.md:114`）。
- 图像 A/B：hardware-only（M4 off）vs hybrid（M4 on）同相机同帧数；用 `scripts/measure/visibility_ab.py` 或扩展的 image diff 工具给 changed-pixels/mean/max，**不宣称像素一致**（预期差异集中在原先空洞/亚像素边缘）。

### 7.3 真实场景测量（Task 6，Bistro 派生资产）

- 资产：M1c 已接受并已归档的 Bistro 派生 cooked GLB（`derived JSON 895c7e...`、cooked `620f4242...`；见 `2026-10-21-object-lod.md:17-24`）——M4 不重新派生、不改变资产；若 M1c 归档不可用则用 `scripts/measure/prepare_object_lod_scene.py` 重建并记录 SHA-256。
- 配置/相机：与 M1c 相同（1280×720、vsync off、max_fps=0、shadow/TAA/SSAO/bloom/FXAA off、`visibility_buffer=true`、`subtilis=true`、`subtilis_error_px=1.0`、hysteresis off；far `35,18,140,0,-83.07,0`、near `35,8,42,0,-88,0`；130 frames/次；validation 开/关各一组；3 轮交错；同一相机对照）。A/B：`software_raster=false`（M1c ON 路径）vs `software_raster=true`（hybrid）；阈值 1.0 主口径，2.0 可选一档（记录收益/质量曲线）。
- 报告（`docs/reviews/2026-10-22-software-raster.md`，结果目录 `docs/reviews/2026-10-22-software-raster/`）：§1 资产/环境/二进制哈希；§2 分类统计（clusters/triangles/tiles/overflow，far/near）；§3 每帧 CPU 分类+binning 成本（µs/帧、per item）；§4 GPU pass 时间（visibility raster 与新增两个 pass 的 mark 区间、帧总、wall）；§5 选中/软件候选/软件胜出像素、resolve 统计、depth mismatch/unresolved；§6 图像 A/B 差异（changed pixels/mean/max，注明不宣称一致）；§7 内存/带宽实测；§8 validation 零发现、frozen gates 结果；§9 **ROI 判决**（§8.3 的 gate 是否通过、是否保留为可选 feature、是否退回 default OFF/不实现）；§10 限制与未测项。重产物归档 `D:/deren-assets/2026-10-22-software-raster/`，逐文件 SHA-256 manifest；仓库只留报告+JSON 摘要+小图。
- 所有性能数字 3 轮中位数 + 逐轮表（不保留最好一轮）；GPU pass 时间来自现有 gpu mark 报告；CPU 分类成本来自 runtime 的微秒级计时（与 M1c cut 计时的同一观察方式）。不把单机/单驱动结果写成发布基准。

### 7.4 ROI 判决（Task 6 的验收门，不假设收益）

- 通过条件（同时满足，建议；数值在 §9 Q4 让用户确认）：far 相机 `hybrid` 相对 `hardware-only` 的 **GPU 帧中位数改善 ≥ 1.5%**（M1c far ON 20.11ms 下约 ≥0.30ms），near 不恶化超过 1%；CPU 分类+binning 的增量 ≤ +0.3ms/帧；图像差异只出现在预期的亚像素/边缘区域（统计记录，不宣称像素一致）；validation 零发现、容量/回退计数为零（或每次回退都有日志且不影响帧基线的解释）。
- 不通过：报告如实记录，M4 保持 default OFF（或按用户决定不实现后续 Tasks 的收尾）；**不允许**为了通过门禁改变测量口径/跳过 near 相机/只报告最好一轮。
- Task 1 另有一个**前置上界门**（§7.3）：先测硬件把 subpixel 工作量让出去能省多少（上界）；若上界本身低于 1.5% 量级，则后续 Tasks 不启动，报告记录原因。这把「是否值得做」的证明拆成「上界 → 实现 → 实测」两段，避免在未验证收益时写完整 feature。Task 1 同时必须输出 per-tile entry 最大值与 1.0/2.0 两档的 overflow 帧数，并记录 `tile_entry_capacity` 默认取 32 还是 64（依据数据，不依据假设），该决定写入后续 Task 输入。

## 8. 计划接口（实施计划中的 Task 对应）

见 [2026-10-22-software-raster.md](../plans/2026-10-22-software-raster.md)：T1 上界测量/分类/tile 数据（含 GO/NO-GO）→ T2 CPU reference + pass 声明/资源/pipeline 脚手架（默认 OFF、无视觉变化）→ T3 GPU 软件 raster + merge + resolve 候选选择 + probe/A-B（含 fixture 对照）→ T4 runtime 接线/容量/回退/HZB 互斥/统计/OFF 门禁 → T5 分类与 dispatch 拆分优化 + 边界测试（4096/overflow/确定性）→ T6 Bistro 3 轮测量 + ROI 判决 → T7 文档/CI/最终审查。每 Task 的 Files/Interfaces/RED→GREEN/Acceptance 在计划文件。

## 9. 风险与开放问题

### 9.1 风险

| 风险 | 现状/对策 |
| --- | --- |
| 软件 raster 成本超过硬件节省（ROI 不成立） | 这正是 Task 1 上界门 + Task 6 实测门的原因；默认 OFF；不通过则保留 negative result，不硬上 |
| 原子/竞争/非确定性 | v1 软件 raster 与 tile binning 都无原子（CPU binning + 每像素单线程）；深度取舍用全序 `(depth,id)`；determinism fixture + 两次 capture hash 检查 |
| 深度语义/merge 竞态 | 单一最终 depth attachment；**resolve 先于 merge**（winner 规则需要未合并的硬件 depth），merge 在其后用深度测试完成 min，无需读回硬件 depth；resolve tie 归硬件；软件胜出像素的 tolerance check 用 sw_depth；HZB 在 non-recovery 帧在 resolve 之后、merge 之后采样 min depth（保守）。若把次序改成 merge-before-resolve，软件严格更近的像素会因 depth 已被改写而永远输给硬件（本次 review 抓到的阻塞缺陷），计划 T3/T4 的 graph order 测试钉死该次序 |
| 1px 边缘/tie 与硬件 raster 的差异 | 不宣称像素一致；A/B 量化 changed pixels；tie 规则固定并测试；边界差异不进入验收的"逐字节"要求（只 OFF 有该要求） |
| HZB 交互 | v1 与 cull/recovery 帧逐帧互斥（§5.5），M3 帧保持不变；hybrid+HZB 需要 GPU survivor 的 stable id 映射，留作 M2/M4 联合后续 |
| 软件光栅的顶点 fetch/投影重复实现 | 从 resolve 抽出 `common/mesh_fetch.slang` 共享，Visibility frozen 1/1 门禁保证抽取无行为变化；禁止复制第二份表达式 |
| 内存/带宽（8B/px twin、tile buffer） | 720p 21-28MiB、1080p 48-51MiB 估算；Task 6 实测；扩容分辨率不达标时明确记录 |
| 与 M2 的 id/表决策冲突 | 本阶段不改 32-bit id；64-bit/两级表/storage depth twin 仍由 M2/M4 联合决策（`2026-10-18-subtilis-design.md:103,151`；M1c `:147`），本 spec 只为此提供数据 |
| 默认 OFF 的回归 | 新资源 family 用 `select_resources` 只在 ON 时请求；OFF 不 append pass/不建 pipeline/不写 descriptor，frozen Full 15/15 + Visibility 1/1 逐字节核对 |
| 测量有效性（单机/validation off 对比等） | 与 M1c 同口径；validation 开/关两组都测；不把短进程启动成本混入稳态帧；报告逐轮数据与失败轮 |

### 9.2 开放问题（用户决定；带推荐；标记阻塞项）

**Q1（不阻塞实现，但影响 Task 1/6 口径）默认阈值**：`software_raster_threshold_px` 默认 1.0 还是 2.0？
**推荐 1.0**（M1c 主口径，保守类别；2.0 作为测量第二档报告收益/质量曲线）。理由：2.0 会把 15.95-38.23% 的 cluster 移入软件，软件工作量显著增大，先以 1.0 验证机制与 ROI。

**Q2（阻塞计划批准）输出/id 设计**：接受方案 A（硬件 depth + 软件 `software_depth`/`software_id` 两张 `R32_UINT` 辅助图 + depth-only merge + resolve 候选选择），并把 64-bit id/两级表/storage depth twin 的联合决策保留给 M2/M4？
**推荐接受 A**；理由见 §3.2/§3.5（唯一不改 32-bit id/硬件 raster/单一 depth producer 的可行 v1），B/C 都有跨 M2 的格式/identity 依赖。若用户要求 v1 就做 64-bit/two-level，需另写 spec 并把 M2 的 selection identity 一起设计，周期与风险显著更大。

**Q3（不阻塞）depth 布局**：接受"硬件 depth 仍为 depth attachment；软件深度只存在于辅助图的 depth lane；merge 用 depth test 写回原 depth attachment"这一布局（不改 depth 采样格式/不新增 sampled depth 图像）？
**推荐接受**；它让 lighting/toon/debug/HZB 的 depth 消费点零改动。若结论要求"sampled storage depth twin"，那是更大改动（所有 depth 消费者换源），不建议 v1。

**Q4（阻塞计划批准）ROI gate**：接受 §7.4 的判决门（far GPU 中位 ≥1.5%、near 不恶化 >1%、CPU ≤+0.3ms、图像差异仅预期区域、validation 零发现），以及"不通过则保持 default OFF/不硬上"？
**推荐接受**并把阈值写进计划验收；否则 Task 6 无法给出可执行的"做/不做"结论。

**Q5（阻塞计划批准）HZB 组合 scope**：接受 v1 "hybrid 与 HZB cull/recovery 帧互斥、M3 帧不变"，把 hybrid+HZB survivor 的 stable id/GPU 分类留作 M2/M4 联合后续？
**推荐接受**；否则需要把 HZB survivor 顺序→run 顺序的映射和 GPU 分类一起设计，等于把 M2 的身份模型提前，且 M1c 测量矩阵本身未启用 GPU HZB。

**Q6（不阻塞）测量分辨率**：1280×720 为主、1920×1080 作为缩放的第二档（只记录内存/带宽与可行性，不作为 ROI 判决主口径）？
**推荐是**；4K 只做内存估算，不在本阶段实测。

**Q7（不阻塞；Task 1 后决策）tile entry 容量**：默认 `tile_entry_capacity` 用 32 还是 64？
**推荐**：先按 32 设计并在 Task 1 测量 1.0/2.0 两档的 per-tile 最大值与 overflow 帧数；若密集 far 区域出现 overflow，则把默认提到 64（720p 成本 ≈1.84MB/lane）。该决定不改变设计 A，只改常量与 buffer 大小，Task 1 报告后由用户确认。

## 10. 文件清单、里程碑与文档更新

### 10.1 新增/修改文件（实现阶段）

- 新 CPU 模块：`source/engine/visibility/visibility_software.cppm`（+ `.cpp` 如需）；测试 `source/tests/test_visibility_software.cpp`。
- 新 pass：`source/engine/pass/software_visibility.cppm/.cpp`、`source/engine/pass/software_merge.cppm/.cpp`；新 pipeline builder 在 `source/engine/pipelines/pipelines.cppm`（`build_software_merge`，参照 `build_shadow` 的 depth-only 构造）。
- 新 shader：`source/shaders/scene/software_visibility.slang`（compute）、`source/shaders/scene/software_merge.slang`（vertex+fragment）、`source/shaders/common/mesh_fetch.slang`（从 resolve 抽出的共享 fetch，若实施选择抽取）；`source/shaders/scene/visibility_resolve.slang` 的候选选择改动；`source/shaders/common/heap_slot_constants.slang` + `source/engine/render_layout/render_layout.cppm` + `source/backends/vulkan/core/core.declarations.cppm` 的 heap slot 镜像与 family 常量。
- 资源/图：`source/engine/render_resource/render_resource.cppm`（新 `resource_id::software_depth`/`software_id` + resource_table 项）、`source/engine/pass/output_targets.cppm`（16th family + heap slot + flags）、`source/engine/render_graph/visibility.cppm`（`append_software_raster`/`append_software_merge`/resolve 可选输入）、runtime 的 per-image 数组/publish/adopt（`runtime.constructor.cppm`/`runtime.declarations.cppm`/`runtime.cpp`）。
- runtime：`source/engine/runtime/runtime.cpp` 的 cut 发布/分类/拆分/tile buffer 写/统计（§4.3）、`runtime.post_graph.cpp` 的 pass append/order、`runtime.frames.cppm` 的 frame 冻结开关、`runtime.declarations.cppm` 的状态/API。
- 配置/入口：`source/application_configuration/application_configuration.cppm/.cpp`（`software_raster`/`software_raster_threshold_px`）、`source/app/main.cpp`、`config.example.toml`、`test_app_config`。
- shader/CMake：根 `CMakeLists.txt` 的 `VR_SLANG_SOURCES`/新测试 target/`VR_TEST_TARGETS`、两个 shader 编译脚本、`source/app/chores.cpp` 的 shader 注册、`.github/workflows/ci.yml` 测试列表镜像（`test_docs` 核对）。
- 测试：扩展 `test_render_graph_visibility.cpp`、`test_visibility_raster_pass.cpp` 风格的新 pass 测试、`test_subtilis_selection.cpp`/`test_visibility_buffer.cpp`/`test_subtilis_frame.cpp`；新增 `test_visibility_software`/`test_software_visibility_pass`/`test_software_merge_pass`。
- 测量/证据：`scripts/measure/verify_software_raster.py`（probe/CPU reference/A-B/负向自检）、`scripts/measure/software_raster_roi.py`（Task 1/6 的上界/ROI 分析）、`scripts/measure/prepare_object_lod_scene.py`（如归档不可用时的重建，沿用 M1c 脚本）；`docs/reviews/2026-10-22-software-raster.md` + 结果目录；D: 归档 `D:/deren-assets/2026-10-22-software-raster/`。
- 文档：`docs/subtilis.md` 状态/§8 已知限制/§10 里程碑（M4 状态）、`docs/visibility_buffer.md` 的 lane 图/已知限制/门禁、`docs/render_graph.md`/`docs/shaders.md`（如新 pass/slot 需要索引）、`docs/extensions/` 不需要改（wire/资产不变）。不修改既有计划/审查文档（除状态更新所需的手册页）。

### 10.2 里程碑状态

- 命名：M4 = subpixel cluster 的软件光栅（visibility lane 的硬件/软件混合 producer），在 M1c 之后、M2 的 id/selection 决策之前。
- 完成定义：§1.3 全部满足 + Task 6 ROI 判决（通过则 documented optional feature default OFF；不通过则 negative result 记录、feature 不启用）+ 一次 fresh 只读 whole-change review（Review Focus 见计划）+ Lead 接受。
- 本阶段 M2/M5 状态不变（未实现）。

## 附录 A：M1c 重探针数字（本设计依据）

来源 `docs/reviews/2026-10-21-object-lod.md:135-150` 与 `docs/reviews/2026-10-21-object-lod/probe-{far,near}.json`：

| 相机 | 实际 cut clusters（HZB 候选） | <1px 直径 | <2px 直径 | <1px 三角形份额 | <2px 三角形份额 | 簇视锥剔除 | 离线 HZB 保守遮挡下界 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| far | 41,030 | 23.28% | 38.23% | 21.92% | 36.75% | 0.00% | 0.73% |
| near | 35,444 | 5.37% | 15.95% | 4.12% | 14.46% | 1.88% | 36.22% |

解读：far 的 subpixel 分类覆盖约 9.5k cluster / ~727k 三角形；near 约 1.9k cluster / ~118k 三角形。M1c 的 hybrid 候选规模足以值得一次上界测量，但"硬件 raster 对这些 cluster 的实际耗时"与"软件 raster 成本"都未测量，因此 Task 1 必须先测上界、Task 6 必须实测收益（§7.4）。

## 附录 B：术语与引用索引

- **subpixel cluster**：cut 选中的 meshlet 记录，在 item 的 model/camera 下投影直径的保守上界 < 阈值（§3.1）。
- **hardware-only / hybrid**：M4 off（硬件 raster 全量 dispatch）/ on（硬件前缀 + 软件后缀）。
- **software images**：per-image 两张 `R32_UINT` 辅助结果：`software_depth`（depth float bits）与 `software_id`（packed id）（§3.2）。
- **merge pass**：depth-only fullscreen，在 **resolve 之后**运行，depth test `less_or_equal` + depth write，把软件深度合并进帧的 G-buffer depth（= min(硬件, 软件)）；resolve 在它之前用未合并的硬件 depth 做候选选择。
- 相关文档：[docs/subtilis.md](../../subtilis.md)、[M1c 验收](../../reviews/2026-10-21-object-lod.md)、
  [M1c checkpoint](../../reviews/2026-10-22-object-lod-checkpoint.md)、[M1c final review](../../reviews/2026-10-22-object-lod-final-review.md)、
  [visibility buffer 计划](../plans/2026-10-17-visibility-buffer.md)、[visibility manual](../../visibility_buffer.md)、
  [Subtilis 主设计](2026-10-18-subtilis-design.md)、[HZB 设计](2026-10-20-hzb-occlusion-design.md)、
  [M1c runtime 设计](2026-10-21-object-lod-runtime-design.md)、`source/engine/visibility/visibility.cppm`、
  `source/shaders/scene/visibility.slang`、`source/shaders/scene/visibility_resolve.slang`。

