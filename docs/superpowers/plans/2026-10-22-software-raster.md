# M4 软件光栅（subpixel cluster）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline to implement this plan task-by-task. Preserve the current-session native execution method and current-checkout authorization. One fresh whole-change reviewer after implementation; no per-task agents. Steps use checkbox syntax for tracking.

**Goal:** 在 visibility-buffer lane 上实现 subpixel cluster 的混合生产者：CPU cut 选中的小 cluster 由 tile 化 compute raster 写 `(depth, packed_id)` 辅助图像，**resolve 先读未合并的硬件 depth 选择硬件/软件候选并着色，depth-only merge 随后把 `min(hw,sw)` 写回同一 G-buffer depth**；默认 OFF、32-bit id 不变、M1c 的 cut/run/HZB/OFF 契约不变。先由 Task 1 测出「硬件让出 subpixel 工作量」的上界并给 GO/NO-GO，再由 Task 6 用 Bistro 派生资产三轮测量做最终 ROI 判决。
**Architecture:** CPU 分类与 tile binning 在 M1c 的 per-item planning 阶段完成（确定性，无 GPU 反查）；硬件 dispatch 只覆盖 run 的硬件前缀；新 per-image 两张 `R32_UINT` 资源（software_depth/software_id）+ tile buffer + 一个 compute raster pass + 一个 depth-only merge pass + resolve 的候选选择；图顺序 = 硬件 raster → 软件 raster → **resolve → merge** → HZB build → lighting/post（HZB cull/recovery 帧 hybrid 逐帧互斥）。raster/resolve 既有数学除 resolve 的候选选择与 merge 的 depth 写回外不变。
**Tech Stack:** C++23 modules、Clang64/libc++、vessel/mimalloc、Slang/SPIR-V、CMake 4.3、Vulkan（现有 visibility/HZB lane）；CPU 测试无 Vulkan 依赖；无 CUDA。
**Spec:** `docs/superpowers/specs/2026-10-22-software-raster-design.md`。代码基线 `1ba58d82`（M1c 已接受）。计划时 HEAD `1ba58d82`。
**状态：** Lead 审阅通过（修正了合并/resolve 次序的阻塞设计缺陷，见 spec §3.2/§5.1）；用户已批准 Q2（方案 A/32-bit id 冻结）、Q4（ROI 判决门）、Q5（v1 与 HZB 互斥）（2026-10-22）；Q1/Q3/Q6 按推荐默认，Q7（tile capacity 32/64）由 Task 1 数据决定。已按计划开始 Task 1（GO/NO-GO 测量），T2+ 仅在 T1 GO 后开工。

**Task 1 状态（2026-10-22 收尾）：** 完成，判决 **GO**——far `visibility raster` 3 轮交错中位数差 **1.54 ms = M1c far ON 20.11 ms 的 7.66%**（门限 1.5%/0.302 ms），near 记录 0.09 ms（不作条件）；分类计数与 M1c 探针对照可解释（1.0 px 10.32% ≤ 23.28%），holes 移除工作量与探针后缀逐帧可对账。**但 per-tile 容量实测推翻 Q7 的 32/64 前提：far 单 tile 最大 977 条（2.0 px 1907），near 226/290，32 与 64 都每帧 overflow；T2 开工前需要用户/Lead 决策容量策略（实测最小可用 2 的幂：far 1024/2048，near 256/512，或换溢出策略）。** 报告：`docs/reviews/2026-10-22-software-raster.md`；证据：`D:/deren-assets/2026-10-22-software-raster/`（98 文件逐文件 SHA-256，0 mismatch）。T2+ 尚未开始。

## Global Constraints

- **只做 M4**（subpixel 软件光栅）：不做 M2（GPU cut select/finalize）、M5（streaming/paging）、material binning、CLUSTER 压缩/量化；不改 M1c 的 DAG cut/import 契约。
- **默认 OFF**：`[render] software_raster = false`；OFF 时没有新 pass append、没有新 pipeline、没有新资源请求（output family 通过 `select_resources` 只在 ON 时请求）；frozen Full **15/15** 与 Visibility **1/1** 逐字节不变。
- **32-bit id 冻结**：`valid1/item12/meshlet12/triangle7` 与 4096 item/4096 meshlet/65536 table 契约不变；软件 id 仍指向同一 item run 的后缀记录，resolve 不加 decode 分支以外的新格式。64-bit/两级表/storage depth twin 的联合决策保留给 M2/M4（spec §9 Q2）。
- **不半新半旧**：任何容量/资源/pipeline/开关不满足 → 该帧 hybrid 整帧关闭（硬件完整路径），日志一次；不截断、不写部分软件图像、不产生部分覆盖。
- **次序契约（正确性）**：resolve 必须在 merge 之前运行（winner 规则需要未合并的硬件 depth）；resolve 用所选候选自己的 depth 做重建/tolerance check；merge 在 resolve 之后以 LESS_OR_EQUAL + depth write 写 `min(hw,sw)`；HZB build/lighting/TAA 在 merge 之后读最终 depth。T3/T4 的 graph order 测试与 T4 的 runtime 断言钉死该次序，禁止 merge-before-resolve。
- **硬件 raster/resolve 数学不变**（除 resolve 的最小深度候选选择与 merge 的 depth 写回）；HZB 数学不变；shadows/BLAS 仍只用 LOD0。
- **HZB 组合 v1 互斥**：hybrid 仅在 `hzb_cull_active_frame == false && hzb_recovery_active_frame == false` 的帧启用；M3 帧逐字节保持（spec §5.5/Q5）。
- **不重复 shader 数学**：软件 raster 与 resolve 的顶点 fetch/投影抽成共享 `common/mesh_fetch.slang`（若实施选择抽取），以 Visibility frozen 1/1 证明抽取无行为变化；禁止复制两份表达式。
- **CPU 分类确定性**：分类/tile binning 用 M1c 相同的 view/proj/extent/model 与保守判据（spec §3.1），逐 record 确定；不得依赖 GPU survivor/frustum 结果；tile entry 容量 32 是初始值，Task 1 必须实测 per-tile 最大值/overflow 后决定 32/64（spec §3.4/§9 Q7）。
- **root CMake 改动受限**：新 pass/module/shader/pipeline/测试 target 与 `VR_TEST_TARGETS`、CI 列表镜像、`output_targets`/资源 schema、heap slot 镜像；其余 root CMake 行为不变。
- **工作约定**：保留用户未跟踪文件；本地提交，**不 push**；同一时间只跑一个 CMake 构建，`-j12`；5h 剩余 ≤5% 时记录并暂停；重证据逐文件 SHA-256 核对后归档 D:；仓库不放大资产。
- **ROI 不假设**：Task 1 的上界门不通过则不开始 T2+；Task 6 的实测门不通过则报告 negative result、保持 default OFF（或按用户决定），不允许改口径/跳相机/只报最好轮。

## Review Focus

（实施后的 fresh 只读 whole-change review 按此 5 条；Critical/Important 修复须有复现与 GREEN，Minor 记账。）

1. **run/id/dispatch 拆分保真**：hybrid 的 run 表 `[硬件前缀|软件后缀]`、`block.geometry.index_count=前缀数`、item `meshlet_base/table` 与 32-bit id 语义是否完全保持；硬件 id 与软件 id 是否都能由现有 `item.meshlet_base + id` 解出正确记录；OFF 路径是否无任何新行为（资源/descriptor/pass 都不创建）。
2. **软件 raster 正确性与确定性**：保守分类边界（球跨近面/阈值 nextafter/正交/非均匀 scale/NaN）；tile 覆盖/空像素初始化；screen-linear z 与 resolve 的 depth check 一致；**winner/tie 规则（软件严格更近胜出、相等且硬件 id 有效 → 硬件、相等且硬件 id 无效（背景 1.0）→ 软件、空 id 跳过；软件胜出用 sw_depth 做重建/tolerance）**；无原子/无 race；同输入两次运行像素一致；CPU reference vs GPU 逐像素 fixture 一致。
3. **merge/resolve 深度语义与 HZB**：**resolve 在 merge 之前**（winner 规则需要未合并的硬件 depth），merge 只走 depth test 不读回硬件 depth、写 `min(hw,sw)`；最终 depth = min(硬件,软件)；resolve 候选选择与重建（含 unresolved/depth mismatch 统计）正确；HZB cull/recovery 帧 hybrid 关闭且 M3 帧字节不变；非 recovery 的 HZB build 在 resolve 之后、merge 之后采样最终 depth（保守）。
4. **容量/回退/无部分覆盖**：tile/entry 溢出、图像/pipeline 失败、item 策略排除、blocker 拒绝都以整帧 hardware-only 处理并计数；软件 id 前缀+后缀 ≤4096；不出现部分软件图像/部分 merge/半新半旧；日志与 stats 可复算。
5. **测量有效性与 ROI 诚实**：Task 1 上界与 Task 6 实测是否同相机/同资产/同配置、3 轮中位数 + 逐轮表、GPU pass 时间来自 gpu marks、CPU 分类成本独立报告、图像差异量化不宣称一致、validation 零发现、归档 SHA-256 完整；ROI 判决与 spec §7.4 一致。

## 文件与边界

- **CPU 参考/分类模块（新）**：`source/engine/visibility/visibility_software.cppm`（模块 `deren.engine.visibility.software`）：`classify`（保守 subpixel 判据）、`tile_grid`/`build_tile_lists`（CPU binning + overflow + per-tile 最大值统计）、`software_pixel`/`resolve_candidate`（winner/tie 规则）、CPU reference rasterizer（fixture 期望值）；只 import `deren.engine.visibility`/`deren.engine.meshlet`/`vstd`，无 Vulkan。
- **pass/pipeline（新）**：`source/engine/pass/software_visibility.cppm/.cpp`（compute，`software_visibility.comp.spv`）、`source/engine/pass/software_merge.cppm/.cpp`（depth-only graphics，`software_merge.vert/frag.spv`）；`source/engine/pipelines/pipelines.cppm` 增加 `build_software_merge`（参照 `build_shadow` 的 depth-only 构造）。
- **shader（新/改）**：`source/shaders/scene/software_visibility.slang`、`source/shaders/scene/software_merge.slang`、`source/shaders/common/mesh_fetch.slang`（从 resolve 抽出，若采用）；`source/shaders/scene/visibility_resolve.slang` 的候选选择；`source/shaders/common/heap_slot_constants.slang` 的 `heap_slots_software_visibility`/`software_tiles` + 预留注释更新。
- **资源/图/runtime**：`source/engine/render_resource/render_resource.cppm`（`resource_id::software_depth`/`software_id` + `resource_table`）、`source/engine/pass/output_targets.cppm`（16th family、`select_resources` gating）、`source/engine/render_graph/visibility.cppm`（`append_software_raster`/`append_software_merge`/resolve 可选输入）、`source/engine/render_layout/render_layout.cppm`（heap slot 常量）、`source/backends/vulkan/core/core.declarations.cppm`（后端 heap slot 镜像）、`source/engine/runtime/{runtime.cpp,runtime.post_graph.cpp,runtime.frames.cppm,runtime.declarations.cppm,runtime.constructor.cppm}`（分类/拆分/buffer 写/pass append/frame 冻结/资源 publish/统计）、`source/app/main.cpp` + `source/application_configuration/*` + `config.example.toml`（开关/阈值）。
- **构建/注册**：根 `CMakeLists.txt`（`VR_SLANG_SOURCES`、新测试 target、`VR_TEST_TARGETS`）、shader 编译脚本（两处）、`source/app/chores.cpp`（`load_shader` 注册）、`.github/workflows/ci.yml`（测试列表镜像，`test_docs` 核对）。
- **测试**：新增 `source/tests/test_visibility_software.cpp`、`test_software_visibility_pass.cpp`、`test_software_merge_pass.cpp`；扩展 `test_render_graph_visibility.cpp`、`test_visibility_buffer.cpp`、`test_subtilis_selection.cpp`、`test_subtilis_frame.cpp`、`test_app_config.cpp`、`test_shader_sources.cpp`（若 shader 列表需纳入）、`test_render_resources.cpp`（heap slot 镜像）。
- **测量/证据**：`scripts/measure/software_raster_roi.py`（Task 1 上界/Task 6 ROI 分析，含 `--self-test` 负向用例）、`scripts/measure/verify_software_raster.py`（GPU probe vs CPU reference/A-B/负向自检）、`scripts/measure/prepare_object_lod_scene.py`（归档不可用时重建，M1c 已有）；`docs/reviews/2026-10-22-software-raster.md` + `docs/reviews/2026-10-22-software-raster/`；D: 归档 `D:/deren-assets/2026-10-22-software-raster/`。
- **文档**：`docs/subtilis.md`（M4 状态/已知限制/里程碑）、`docs/visibility_buffer.md`（lane 图/已知限制/门禁）、`docs/render_graph.md`/`docs/shaders.md`（新 pass/slot 索引，如需要）；不改 wire/资产格式与既有计划/审查（除手册状态更新）。
- **不改**：M1c 的 DAG cut/import、HZB 数学、shadows/BLAS、M2/M5、wire/schema、converter 工具。

## 公共类型约定

接口在 `deren::engine::visibility::software` / `deren::engine::pass` / `deren::engine::render_graph::visibility`；容器用 vessel，错误按项目 typed 错误/日志惯例。

- `classify(record, model_3x3, view_parameters, extent, threshold_px) -> classification { bool subpixel; double diameter_upper; bool front_of_near; }`（spec §3.1 的保守判据；纯函数）。
- `tile_grid`：16×16 tile，`tile_entry_capacity` 初值 32（Task 1 依 per-tile 最大值决定 32/64）；`build_tile_lists(items, records, camera, extent, threshold) -> expected<software_tile_plan, software_error>`（CPU binning；overflow → 错误而不是截断）。
- `software_pixel { std::uint32_t depth_bits = 0xFFFFFFFFu; std::uint32_t id = 0; }`；`software_tile_plan { tiles_x, tiles_y, cluster_count, entry_count, counts/entries 视图 }`；`resolve_candidate(hardware_id, hardware_depth, software_pixel) -> candidate { enum source { hardware, software }; id; depth; }`（**merge 之前**的 resolve 语义：`sw_depth < hardware_depth` → software；严格相等且硬件 id 无效（背景 clear 1.0）→ software；其余 → hardware；软件胜出时 chosen depth = sw_depth；纯函数，供 CPU reference 与测试使用）。
- CPU reference rasterizer：`rasterize(plan, run_table, item_table, vertex/index views, extent) -> software image`（与 GPU shader 同数学；fixture 期望值）。
- pass：`pass::software_visibility_pass`（compute，`behaviour_kind::compute`，`extent_rule::full` 不适用时显式 dispatch = tiles，`ready()/has_pipeline()` 同 `visibility_resolve_pass`）；`pass::software_merge_pass`（graphics，depth attachment 无 colour，`behaviour_kind::graphics`、`extent_rule::full`、depth format 来自 `pass_context`）。
- 图：`append_software_raster(builder&, inputs)`（写 `software_depth`/`software_id` storage + `software_tiles`/camera/items/meshlets/culled meshlets/statistics 读）、`append_software_merge(builder&, inputs)`（读 `software_depth` + 写 `gbuffer_depth` depth attachment；**显式 `order_after(resolve)`**）、`append_resolve` 增加可选 `software_depth`/`software_id` 输入（两者为空 = 现有行为）；非 recovery 的 HZB build append 在 merge 之后并由 depth RAW 依赖排序。
- 资源/常量：`render_resource::resource_id::software_depth`/`software_id`（image2d `r32_uint`、per_swapchain_image、sampled|storage、output-size）；heap `heap_slots::software_depth = heap_slot_base + 1239`（预留窗口）、`heap_slots::software_id = heap_slot_base + 1314`（append per-image family）与 `heap_slots::software_tiles = heap_slot_base + 1322`；两个 shader/host 镜像与 `test_render_resources` 同步。
- 配置/API：`[render] software_raster = false`、`[render] software_raster_threshold_px = 1.0`（finite、>0）；`runtime::set_software_raster(bool)`/`set_software_raster_threshold(float)`；帧内冻结为 `software_active_frame`（与 `subtilis_active_frame` 同处，spec §4.3）。
- stats：`mesh_geometry.slang` 的 16-lane stride 用空闲 lane 12..15：`software_clusters`、`software_candidate_triangles`、`software_winning_pixels`、`software_overflow_frames`；C++ stride 契约（`runtime.constructor.cppm:563,1087`）同步注释/断言。
- probe：`DEREN_SOFTWARE_RASTER_PROBE=<dir>`（capture-only，env 开关，不改算法/表/draw metadata；只在截图帧写）dump extent/view/proj/tile plan/software image/depth/vis-id/mesh stats/CPU reference 所需输入（spec §7.2）。

## 依赖顺序

`T1 -(上界门 GO)-> T2 -> T3 -> T4 -> T5 -> T6 -(ROI 判决)-> T7`。T1 的 GO/NO-GO 是硬门：NO-GO 时 T2-T7 不启动，写 negative-result 记录并收尾。T2/T3 是 CPU/GPU 分离的两步（T2 无视觉变化，T3 首次启用 hybrid 于 fixture）。T4 完整接线；T5 边界/互斥/门禁；T6 真实场景测量；T7 文档/CI/最终审查。每个 Task 单独本地提交。

## Task 1 — subpixel workload 与硬件上界测量（GO/NO-GO）

**目标：** 用 M1c 相同的 Bistro 派生资产与相机，测量（a）subpixel 分类的 cluster/三角形/tile 规模与 **per-tile entry 最大值 / 1.0-2.0 两档的 overflow 帧数**（据此记录 capacity 32 vs 64 决定），（b）硬件 raster 把 subpixel 工作量让出去能省多少的**上界**，（c）CPU 分类+binning 成本；给出明确 GO/NO-GO。本任务只增加纯 CPU 分类模块与 capture-only 探针（env 开关），生产行为（默认 OFF）不变。

**Files：** `source/engine/visibility/visibility_software.cppm`（classify + tile binning，纯 CPU）、`source/tests/test_visibility_software.cpp`（分类/binning 单测）、`source/engine/runtime/runtime.cpp`（capture-only probe：`DEREN_SOFTWARE_RASTER_PROBE=workload` 时在 phase 2 分类并只 dispatch 硬件前缀 = "holes control"，**仅截图帧、仅环境开关**）、`scripts/measure/software_raster_roi.py`（上界/统计/负向自检）、`scripts/measure/analyze_object_lod.py`（如需复用 probe 解析）、根 CMake/CI 测试列表；`docs/reviews/2026-10-22-software-raster.md` 的 Task 1 节（初稿）。

**Interfaces：** §公共类型约定中的 `classify`/`build_tile_lists`/`software_tile_plan`（含 per-tile 最大值统计）；probe dump 的 workload 布局（extent/view/proj/每 item model + hw/sw 分类计数 + per-tile 计数 + 每类三角形数；同一 `DEREN_SOFTWARE_RASTER_PROBE` env，`workload` 值为 Task 1 的上界测量，Task 3 起用目录值 dump 完整图像）；`software_raster_roi.py --self-test` 的负向用例（缺轮次/失败 capture/伪造数据拒绝）。

**RED：**
- [x] 写 `test_visibility_software` 的分类/binning 用例（阈值边界、正交/透视、非均匀 scale、跨近面、NaN、capacity/overflow）→ 运行 `ctest -R '^test_visibility_software$'` 确认 RED（模块/接口缺失）。
- [x] 写 `software_raster_roi.py` 的解析/自检骨架，用空/不完整输入确认拒绝而非静默通过；确认 capture-only probe env 尚未实现（dump 缺失）。

**GREEN：**
- [x] 实现 `classify`/`build_tile_lists`（spec §3.1/§3.4 的纯函数与保守边界），通过单测。
- [x] 实现 capture-only workload probe：截图帧按 `software_active_frame` 的判定做分类/tile binning；`DEREN_SOFTWARE_RASTER_PROBE=workload` 时把硬件 dispatch 改为"仅硬件前缀"（这就是 holes control：软件后缀在本任务不绘制，图像有洞是**预期**的测量状态，不允许出现在生产帧/正式回归中），同时 dump 分类统计与 tile 规模；无 env 时完全不改行为（默认 OFF）。
- [x] `software_raster_roi.py` 读取 baseline vs holes-control 的逐轮日志/GPU marks/mesh stats，计算：subpixel clusters/triangles/tiles、**per-tile entry 最大值与 1.0/2.0 两档 overflow 帧数**、被移除的 hardware workgroups/triangles（mesh stats 差分）、`visibility raster` GPU 时间差（3 轮中位数）、CPU 分类+binning 微秒/帧；输出上界表、capacity 决定（32 vs 64）与明确的 GO/NO-GO。

**Acceptance：**
- [x] Bistro 派生 cooked GLB（M1c 归档 `620f4242...`，或 `prepare_object_lod_scene.py` 重建并记录 SHA-256）、同 M1c 相机（far `35,18,140,0,-83.07,0`、near `35,8,42,0,-88,0`）、1280×720、130 帧、validation 关、3 轮交错；baseline = M1c ON 路径（software_raster off），holes-control = env probe；报告 far/near 上界表 + CPU 分类成本 + 统计。
- [x] GO/NO-GO 与 capacity 决定写入报告：建议 GO 条件 = far 相机 `visibility raster` 时间差中位数 ≥ 1.5%（M1c far ON 20.11ms 下 ≈0.30ms），且分类规模与 M1c 探针口径的差异可解释（保守判据计数 ≤ 探针比例），near 记录但不作为 GO 必要条件；同时按 1.0/2.0 两档实测结果记录 `tile_entry_capacity` 默认 32/64 的决定；NO-GO 时停止 T2+ 并在报告记录原因。
- [x] 默认 OFF 下 `check_render.ps1 -Full`（15/15）与 `-Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`（1/1）不变；validation clean；`test_docs` 绿（若新增脚本/文档）；Release 全量 CTest 绿（记录数量）。

## Task 2 — CPU reference 核心 + 资源/pass 脚手架（默认 OFF、无视觉变化）

**目标：** 建立 `software_depth`/`software_id` 资源 family、tile buffer、两个 pass 类与 pipeline、图声明、shader 注册与 CPU reference rasterizer/merge 规则；**不 append 到帧**（没有 runtime 接线），因此默认 OFF 的帧与 baseline 逐字节一致。

**Files：** `source/engine/visibility/visibility_software.cppm`（CPU reference rasterizer + `resolve_candidate` 完整化）

**Interfaces：** §公共类型约定中的 `append_software_raster`/`append_software_merge`/resolve 可选输入、pass 类声明、`resource_id::software_depth`/`software_id`/heap slots、`build_software_merge`、shader 注册名；资源仅当 `select_resources` 选中时创建（本任务不选）。

**RED：**
- [ ] 写 pass 声明测试（io/behaviour/`ready()` gating/非法输入拒绝）与图声明测试（software raster 在 hardware raster 之后；**resolve 在 merge 之前**（显式 order 边 + depth WAR）；merge 读 `software_depth` + depth read-write；resolve 可选输入为空时与旧声明等价；OFF 不 append）；CPU reference/winner 规则用例（软件严格更近胜出、相等且硬件 id 有效 → 硬件、相等且硬件 id 无效 → 软件、空 id → 硬件、软件胜出 chosen depth = sw_depth）；运行确认 RED（缺模块/接口）。
- [ ] 写资源/常量测试期望（software family 的 id/format/flags/heap slot；`software_tiles` slot 在 HZB 之后且 grid 内），确认 RED。

**GREEN：**
- [ ] 实现 CPU reference rasterizer 与 `resolve_candidate`（winner/tie/背景 1.0 tie 语义；软件胜出时用 sw_depth 做重建/tolerance 输入）
- [ ] 实现两个 pass 类与 pipeline（compute 仿 `visibility_resolve_pass`；depth-only merge 仿 shadow pass；shader 先最小实现 entry，后续 Task 3 完整），`build_software_merge` 用 `pass_context` 的 depth format。
- [ ] 资源 family/heap slots/runtime 数组骨架 + `output_targets_pass` 16th family + `select_resources` gating（本任务 runtime 不选中，因此不分配）；后端/常量镜像 + `test_render_resources` 同步。
- [ ] 图声明函数与 pass/graph 测试通过；shader 注册四处同步（CMake/两个编译脚本/chores/`test_shader_sources`）。

**Acceptance：**
- [ ] `ctest -R 'test_visibility_software|test_render_graph_visibility|test_software_|test_render_resources|test_shader_sources'` 绿；`test_visibility_buffer`/`test_visibility_raster_pass`/`test_visibility_resolve_pass` 不回归；ASan/UBSan 新 CPU target 绿。
- [ ] Release 全量 CTest 绿（记录数量）；默认 OFF 的 frozen Full 15/15 + Visibility 1/1 不变（没有视觉变化）；validation 不需要（本任务无帧内新 pass）。
- [ ] `test_docs` 绿（VR_TEST_TARGETS 与 ci.yml 同步）。

## Task 3 — GPU 软件 raster + merge + resolve 候选选择 + probe/fixture 对照（首次启用 hybrid）

**目标：** 实现完整 shader 数学与 probe 验证链：GPU 软件 raster 与 CPU reference 在 fixture 上逐像素一致；**resolve 在 merge 之前**（软件严格更近胜出、tie 归硬件、空 id 跳过、软件胜出用 sw_depth 做 tolerance）；merge 后 depth = min；同一相机两次运行确定；validation clean。

**Files：** `source/shaders/scene/software_visibility.slang`（完整 tile 扫描 + 顶点 fetch/投影 + 深度/id 写）、`source/shaders/scene/software_merge.slang`（fullscreen depth 写）、`source/shaders/common/mesh_fetch.slang`（从 resolve 抽取，如采用；若不抽取则必须用测试固定两份表达式等价）、`source/shaders/scene/visibility_resolve.slang`（候选选择）、`source/engine/runtime/runtime.post_graph.cpp`（probe fixture 帧的 append/order）、`source/engine/runtime/runtime.cpp`（probe readback/capture-only）、`scripts/measure/verify_software_raster.py`（CPU vs GPU/merge/A-B/自检）、`test_visibility_software.cpp`/`test_render_graph_visibility.cpp` 扩展、`docs/reviews/2026-10-22-software-raster/`（fixture 证据）。

**Interfaces：** probe dump（spec §7.2）与 `verify_software_raster.py` 的 CLI/负向自检；shader entry 名；resolve 候选选择的输入/规则。

**RED：**
- [ ] 先写 `verify_software_raster.py` 的期望/自检（缺输入、坏图像、截断 dump 拒绝），并用 fixture 的 CPU reference 生成期望图像；GPU probe 尚未实现 → RED。
- [ ] 写小 fixture 场景（现有 headless/capture 场景 + 固定相机，含 subpixel 三角形、深度 tie、退化、背后三角形、边缘/clamp 像素），确认当前 hybrid ON 之前 probe 缺失。

**GREEN：**
- [ ] 实现完整 software raster shader（tile 扫描、逐像素全序选择、空像素初始化、screen-linear z/clamp/有限检查、跳 MASK/背面由输入保证）与 merge shader；resolve 候选选择与重建按 spec §3.2/§5.3（resolve 先于 merge；软件候选用 sw_depth）；抽取/复用 `mesh_fetch`（若抽取，Visibility frozen 1/1 必须不变）。
- [ ] runtime probe 接线：hybrid 帧按 raster→software raster→resolve→merge 次序 append（先只在该 probe/fixture 路径；T4 再完整化）；dump software image/depth/vis-id/tile plan/stats；`verify_software_raster.py` 比较 CPU reference 与 GPU（0 差异）、**winner/tie 规则（软件严格更近、tie 硬件、空跳过）**、merge min、id decode、determinism（两次 capture hash）、**resolve-before-merge 次序（pass 顺序/gpu marks 区间）**。
- [ ] fixture 的 image A/B（hybrid vs hardware-only）记录 changed-pixel/mean/max，确认差异只在预期亚像素/边缘区域；validation 零发现。

**Acceptance：**
- [ ] `verify_software_raster.py` fixture 结果：CPU vs GPU **0 像素差异**、winner/tie 规则、resolve-before-merge 次序、merge min、id decode、determinism 全 PASS；负向自检拒绝所有伪造/不完整输入。
- [ ] fixture capture 的同相机两次运行 hash 一致；validation **0 error/warning/VUID**；报告 §3/§5 的 fixture 小节。
- [ ] CPU 单测与图测试扩展绿；Release 全量 CTest 绿；默认 OFF 的 frozen gates 不变（hybrid 只在 probe/fixture 路径启用）。

## Task 4 — runtime 完整接线：分类/拆分/buffer/fallback/stats/OFF 门禁

**目标：** 把 T1-T3 的机制接到正式帧路径：phase 2 分类+binning、run 表 `[hw|sw]` 拆分、tile buffer 写、`software_active_frame` 冻结、capacity/资源/pipeline 失败的整帧回退、stats/日志、配置键/API、HZB 互斥；默认 OFF 无新行为。

**Files：** `source/engine/runtime/runtime.cpp`（`record_visibility_items` phase 2/4、fallback、stats、日志）、`runtime.frames.cppm`（`software_active_frame` 冻结）、`runtime.post_graph.cpp`（正式 pass append/order/optional resolve 输入）、`runtime.declarations.cppm`（状态/API/数组）、`runtime.constructor.cppm`（资源 publish/adopt、pass 创建、pipeline）、`source/application_configuration/*` + `source/app/main.cpp` + `config.example.toml`、`source/engine/visibility/visibility.cppm`（hybrid `apply_meshlet_run` 变体）、`test_subtilis_selection.cpp`/`test_subtilis_frame.cpp`/`test_visibility_buffer.cpp`/`test_app_config.cpp` 扩展。

**Interfaces：** `set_software_raster`/`set_software_raster_threshold`；`software_active_frame`；fallback 原因计数（overflow/capacity/resource/pipeline/hzb_excluded/policy）；stats lane 12..15；`scene_import_result` 不变；配置键。

**RED：**
- [ ] 测试：hybrid 帧的 run 表（硬件前缀/软件后缀/id 解码）、`block.geometry.index_count`=前缀、item/base/table 不变；**append 次序 = raster → software raster → resolve → merge → HZB build**（图/pass 顺序断言）；**resolve 使用所选候选深度**（软件胜出像素 tolerance 用 sw_depth；硬件胜出用硬件 depth）；merge 后 `gbuffer_depth == min(hw_depth, sw_depth)`；HZB build 在 merge 之后且采样 merge 后 depth；容量边界（4095/4096/4097 记录、tile cap 31/32/33、entry 上限）→ 整帧回退；HZB cull/recovery 帧 hybrid 关闭且字段/行为与 M3 一致；资源/pipeline 不可用 → hardware-only；`software_raster=false` 时零 append/零新资源；配置解析/校验（finite、>0、默认值）。运行确认 RED。
- [ ] 写 runtime 日志/stats 期望（每 120 帧一次汇总、每场景 overflow 一次）。

**GREEN：**
- [ ] 实现分类/拆分/buffer 写/fallback/stats/日志；图组装在 hybrid 帧按 **raster → software raster → resolve → merge → （非 recovery）HZB build** 次序 append，并在 resolve 传可选输入；resolve 的重建/tolerance 使用所选候选深度；merge 只写 depth；其他帧路径完全不变。
- [ ] 资源 family 仅在 `software_raster` ON 时 `select_resources` 请求；pipeline 懒创建失败 → feature unavailable（hardware-only）；OFF 不创建/不写 descriptor/不 append。
- [ ] 配置键 + config.example + parser + app wiring；`test_app_config` 负向（负数/NaN/0）与默认值。

**Acceptance：**
- [ ] `ctest -R '^test_subtilis|test_visibility|test_app_config'` 绿；Release 全量 CTest 绿（记录数量）；ASan/UBSan CI 形态绿。
- [ ] 默认 OFF：frozen Full **15/15**、Visibility **1/1** 逐字节不变；`software_raster=true` 时 fixture 帧 validation 零发现。
- [ ] overflow→整帧 hardware-only 有日志/stats；无部分软件图像/部分 merge（spec §5.4 矩阵逐行有测试/证据）。

## Task 5 — 边界、容量、确定性与 HZB/merge 组合门禁

**目标：** 把 Task 1-4 的边界从"能跑"提升到"可回归"：4096/overflow/前缀+后缀 id、tile 覆盖/空初始化、depth tie/clamp、determinism 两次 capture、HZB cull/recovery 帧字节不变、非 recovery HZB build 采样 merge 后 depth 的保守性验证、GPU probe 的负向自检、图像 A/B 与 frozen 门禁。

**Files：** 测试扩展（`test_visibility_software.cpp`、`test_subtilis_frame.cpp`、`test_render_graph_visibility.cpp`）、`scripts/measure/verify_software_raster.py`（边界/负向/merge/HZB 检查）、`docs/reviews/2026-10-22-software-raster/`（边界/门禁证据）、如需 `runtime.post_graph.cpp` 的 order 断言。

**Interfaces：** 无新接口；验证既有接口的边界与组合（probe/verify 脚本的检查项）。

**RED：**
- [ ] 先加边界/组合测试（前缀+软件后缀 ≤4096、tile 空像素、**winner 规则：软件严格更近胜出 / 相等且硬件 id 有效 → 硬件 / 相等且硬件 id 无效（背景 1.0）→ 软件 / 空 id 跳过**、depth tie/clamp、**resolve-before-merge 次序**、merge 后 depth=min、两次 capture hash、HZB cull/recovery 帧不变、**非 recovery HZB build 采样 merge 后 depth 的保守性检查**）→ 运行确认 RED（缺实现/断言）。

**GREEN：**
- [ ] 实现/修正到测试全绿；确认 hybrid 帧与非 hybrid 帧在 HZB cull/recovery 开关组合下的行为矩阵；**graph/runtime 断言 resolve→merge→HZB build 的次序与 merge 后 depth=min**；probe/verify 脚本增加 winner/tie/次序/min 检查与自检负向用例。

**Acceptance：**
- [ ] 边界/组合测试全绿；`verify_software_raster.py --self-test` 负向用例全部拒绝；HZB cull/recovery 帧的 frozen 捕获/字段与 M3 一致（字节级核对或逐轮 JSON 对比）。
- [ ] 默认 OFF frozen Full 15/15 + Visibility 1/1 不变；ON fixture 帧 validation 零发现；Release 全量 CTest 绿（记录数量）；ASan/UBSan 绿。

## Task 6 — Bistro 派生资产 3 轮测量与 ROI 判决

**目标：** 用 M1c 相同资产/相机/配置测量 hybrid 的真实收益/成本，按 spec §7.4 给出 ROI 判决。本任务只写测量脚本/证据，不改生产行为（除测量中发现的正确性缺陷，另起修复提交）。

**Files：** `scripts/measure/software_raster_roi.py`（正式矩阵/3 轮中位数/逐轮表/判决）、`scripts/measure/measure_object_lod.py`/`capture.ps1`（复用/扩展）、`docs/reviews/2026-10-22-software-raster.md`（完整报告）+ `docs/reviews/2026-10-22-software-raster/`（JSON/log/小图）、`D:/deren-assets/2026-10-22-software-raster/` 归档。

**Interfaces：** 报告结构（spec §7.3 §1-§10）；ROI gate（spec §7.4）；归档 manifest（逐文件 SHA-256）。

**RED：**
- [ ] 先写分析脚本自检（缺轮次/重复轮次/失败 capture/短 capture/非有限指标/伪造图像拒绝）；用空输入确认拒绝；矩阵模板跑一次确认缺数据时 FAIL。

**GREEN：**
- [ ] 测量矩阵：M1c 派生资产（归档 SHA 核对）、far/near、validation 开/关各一组、130 帧、OFF（hardware-only）与 ON（hybrid）各 3 轮交错、同相机；阈值 1.0 主口径 + 可选 2.0 一档；GPU marks 的 pass 时间（含 **resolve 与 merge 分开的 mark 区间**）、CPU 分类/binning 与 cut 成本、**per-tile entry 最大值 / 1.0-2.0 两档 overflow 帧数与 capacity 决定（32/64）**、容量/回退/统计、图像 A/B（changed pixels/mean/max）、内存/带宽（如可测）、validation 零发现。
- [ ] 报告 §1-§10 完整：3 轮中位数 + 逐轮表（不保留最好轮）、失败轮如实记录、ROI 判决（通过→可选 feature default OFF；不通过→negative result 并记录，不改口径）。第二档 1920×1080 只记录 `software_depth`/`software_id` 资源内存、带宽估算与 validation（不要求完整 ROI 矩阵），4K 仅估算。
- [ ] 归档 `D:/deren-assets/2026-10-22-software-raster/` 逐文件 SHA-256 manifest，在报告中引用；scratch 清理。

**Acceptance：**
- [ ] 报告存在且 §1-§10 完整；所有数字可由归档原始日志重算（脚本 gate 自测负向用例通过）；ROI 判决与 spec §7.4 一致；不 push。
- [ ] 默认 OFF frozen gates 仍 15/15 + 1/1；ON 帧 validation 零发现；图像差异已量化且不宣称像素一致。

## Task 7 — 文档、CI、最终审查与提交

**目标：** 更新手册/索引/里程碑，把新测试纳入 CI 契约，跑最终回归，本地提交，并为一次 fresh whole-change review 准备输入（由 Lead 决定发起）。

**Files：** `docs/subtilis.md`（M4 状态/已知限制/里程碑/证据索引）、`docs/visibility_buffer.md`（lane 图/新 pass/门禁/已知限制）、`docs/render_graph.md`/`docs/shaders.md`（如需要）、`config.example.toml` 注释、`docs/reviews/2026-10-22-software-raster.md` 定稿、CI/CMake 最终核对（`test_docs`）、所有本地提交。

**RED：**
- [ ] 跑 `ctest --test-dir build-release-dyn-clang64`、`check_render.ps1` 门禁、`test_docs` 记录当前状态；确认文档未更新时 `test_docs`/索引检查会失败（如 VR_TEST_TARGETS 与 ci.yml 不一致、shader 列表未同步）。

**GREEN：**
- [ ] 文档更新（状态/配置/里程碑/证据索引/新 shader 列表），`test_docs` 通过、链接有效。
- [ ] 最终回归：Release build+全量 CTest；CI 形态 ASan/UBSan headless；frozen Full 15/15 + Visibility 1/1；converter standalone `ctest --test-dir build-subtilis` 6/6 + CLI inspect；`git status` 不含意外文件。
- [ ] 本地提交（按 Task 拆分，`feat/test/docs/perf:` 前缀），**不 push**；把 Task 1/6 证据与 Review Focus 交给 Lead/最终 reviewer（不启动任务级 agents、不重复审查流程）。

**Acceptance：** 上述回归全部记录；`test_docs` PASS；docs 状态与实现一致；本地提交完成、无 push。随后按计划 **Review Focus** 由一名 fresh 只读 reviewer 做 whole-change review（Lead 决定）；Critical/Important 修复须复现与 GREEN，Minor 记账；接受与否由 Lead 决定。

---

## Verification Gate（实施完成前；按顺序全绿才可交付）

1. **构建/测试**：root Release `cmake --build build-release-dyn-clang64 -j12` + `ctest --test-dir build-release-dyn-clang64 --output-on-failure` 全绿（记录数量与日志）；CI 形态 ASan/UBSan headless 全绿（记录数量；Windows 无 LSan，沿用既有记录）。
2. **OFF 冻结**：`check_render.ps1 -Full -BuildDir build-release-dyn-clang64`（legacy `docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt`，15/15）与 `check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`（1/1）逐字节不变。
3. **ON 证据**：Task 3/5 的 fixture CPU-vs-GPU 0 像素差异、**winner/tie 规则（软件严格更近胜出、tie 归硬件、空 id 跳过）、resolve-before-merge 次序、merge 后 depth=min**、id decode/determinism PASS、负向自检拒绝；Task 6 报告 §1-§10 完整（3 轮中位数 + 逐轮表、图像差异量化、validation 零发现、ROI 判决、per-tile 容量统计）。
4. **容量/回退**：报告与测试记录 hybrid 的 capacity/overflow/resource/pipeline/HZB 排除计数；无部分覆盖；§5.4 矩阵逐行有证据；软件 id 前缀+后缀 ≤4096。
5. **文档/CI**：`test_docs` PASS；`VR_TEST_TARGETS` 与 `.github/workflows/ci.yml` 列表一致；shader 注册四处一致；`docs/subtilis.md`/`docs/visibility_buffer.md` 状态与索引更新。
6. **仓库卫生**：`git status` 只有预期提交内容与用户未跟踪文件；证据归档 D: 且 SHA-256 manifest 核对；**未 push**。converter standalone 6/6 绿（M4 不应触碰 converter；若被触碰则不得接受）。

## 验收命令速查

```
cmake --build build-release-dyn-clang64 -j12
ctest --test-dir build-release-dyn-clang64 --output-on-failure
ctest -R 'test_visibility|test_software|test_subtilis|test_app_config|test_docs' --test-dir build-release-dyn-clang64 --output-on-failure
pwsh scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64
pwsh scripts/windows/check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt -BuildDir build-release-dyn-clang64
pwsh -File scripts/windows/capture.ps1 -Base <object-lod base toml> -Overrides "software_raster=true" -Tag software-on -Frames 130 -Camera "35,18,140,0,-83.07,0"
python scripts/measure/software_raster_roi.py --self-test
python scripts/measure/verify_software_raster.py --self-test
ctest --test-dir build-subtilis --output-on-failure        # converter standalone, 6/6
```

## 文档写作/提交约定

- 中文文档 UTF-8，与 `docs/superpowers/{specs,plans}` 既有风格一致；`test_docs` 的 ASCII/链接/结构不变量必须通过。
- 每个本地提交消息用 `feat|test|docs|perf:` 前缀；不 push。
- 未测数字不写结论；所有性能数字注明来源（报告文件名 + 轮次/相机/配置/validation 开关）；ROI 判决与 negative result 同等记录。
