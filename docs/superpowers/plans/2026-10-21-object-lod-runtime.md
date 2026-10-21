# deren Runtime Object LOD（converter `DEREN_subtilis` 消费）实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline to implement this plan task-by-task. Preserve the current-session native execution method and current-checkout authorization. One fresh whole-change reviewer after implementation; no per-task agents. Steps use checkbox syntax for tracking.

**Goal:** 让 deren runtime 在加载 glTF 时消费离线转换器已接受的 `DEREN_subtilis` cluster-DAG payload，为每个 static OPAQUE item 提供连续 per-object LOD（CPU cut 写 host-culled meshlet run），并在 Bistro 2909-item 真实场景上测量收益/成本、记录 M2 触发指标与 M4 重探针输入；OFF 帧字节一致，M2/M4 不实现。
**Architecture:** 复用已接受的 converter CPU 模块（types/codec/digest/dag/cut）在 root 侧编译为静态资产库；engine 新增 DAG 层级（cluster producer/consumer + group fine/coarse 引用 + LOD 球/error + roots/leaves）与移植自 converter reference 的 CPU DAG cut；gltf_loader 只提取 payload 原始字节与扩展字段，import 时由 engine 完成解码/身份/结构校验/翻译并共享 generation；每帧 item cut 继续走 M1b 的 run/发布/HZB 管线，raster/resolve 数学不变。
**Tech Stack:** CMake 4.3、C++23 modules、Clang64/libc++、vessel/mimalloc、yyjson 0.12.0（root 已有 `yyjson_vendored`）、fastgltf 0.9/simdjson（仅在 deren_assets/gltf_loader）、meshoptimizer v1.3（只给现有 builder/forest 参考路径，本 feature 的 import 不使用 builder）。无 CUDA/Vulkan 依赖给新 CPU 测试。
**Spec:** `docs/superpowers/specs/2026-10-21-object-lod-runtime-design.md`。代码基线 `f4d3b8e7`（converter Task 1-8 已接受）。计划时 HEAD `f4d3b8e7`。
**状态：** 已评审通过（2026-10-21：Q1 seam = 方案 C、Q2 单一 `subtilis` 开关、Q4 TANGENT-stripped 派生测量资产；Q3/Q5/Q6 按推荐默认）。用户于 2026-10-22 明确恢复实施；Tasks 1–2 已验收，接下来从 Task 3 继续，逐任务独立验收/本地提交/不 push。

## Global Constraints

- **本阶段只实现对象 LOD 的 CPU 路径**：不做 M2（GPU select/finalize）、M4（软件光栅）、M5（streaming/cache）；不改 resolve/raster 数学、48B meshlet record、32-bit packed id（`valid1+item12+meshlet12+triangle7` = 4096/4096/128）。
- **仅 static OPAQUE**：MASK/BLEND、蒙皮/morph、instanced、custom pipeline、exact-depth redraw 保持现有 admission/`coarse_allowed` 行为，不扩宽；converter profile 外的属性集合（TANGENT 等）不采用 payload。
- **payload v1 不变**：不改 `docs/extensions/DEREN_subtilis/` 的 wire 约定/schema/版本；转换器工具本阶段预期**不需要代码改动**（只用它重跑资产）。若实施中发现必须改转换器，停手并交用户决定，不能顺手改。
- **seam 固定为 spec 方案 C**（engine DAG 层级 + 复用 converter CPU 模块），未经用户批准（spec Q1）不得开始实施（Task 1 起）。
- **OFF 字节一致**：`[render] subtilis = false` 的冻结参考帧（`check_render.ps1 -Full` 的 `docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt`、`-Visibility -Compare` 的 `docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`）必须不变；带 payload 的资产在 OFF 启动下与去掉 payload 的同资产像素一致。
- **容量契约冻结**：item ≤4096、每 item run ≤4096、全局 host-culled 表 65536、HZB item 4096/candidate 65536。超限为确定性回退（整帧 legacy 或按场景顺序不采用 payload），绝不静默截断；汇总日志 + 报告计数。
- **校验分层**：生产 import 只做 wire/身份/DAG/容量校验（复用 converter codec/dag 模块）；完整几何 boundary 校验留在离线转换器 `validate --full`（已接受）与 root CTest fixture；运行时不默认跑 `make_asset` 全验证（spec §3.4/Q5）。
- **不重复 wire parser**：root 与 `tools/subtilis` 各自编译同一份 `source/asset/subtilis/{types,codec,digest,dag,cut}` 源文件，禁止把 wire 解码逻辑抄到 engine/loader。
- **root CMake 只在下列范围改动**：新增 `source/asset/subtilis` 静态 target、`deren_engine` 增量 sources/PRIVATE 链接、`gltf_loader`/`deren_assets` 链接 `yyjson_vendored`（若 loader 用 yyjson）、新测试 target 进 `VR_TEST_TARGETS`；其余 root CMake 行为不变；`test_docs` 不变量保持通过。
- **工作约定**：保留用户未跟踪文件；当前 master 继续修改；本地提交，**不 push**；同一时间只跑一个 CMake 构建，`-j12`；5h 剩余 ≤5% 时记录并暂停；重证据逐文件 SHA-256 核对后归档 D:，仓库不进大资产。
- 每个 Task 结束必须能独立复验（命令写在 Acceptance）；未测收益不写进代码注释/README 的结论句；M2/M4 只记录触发指标与 hand-off，不实现。

## Review Focus

（实施完成后的 fresh 只读 whole-change review 按此 5 条执行；Critical/Important 修复须有复现与 GREEN，Minor 记账。）

1. **Seam/表示保真**：engine DAG 层级是否正确表达 converter 语义（cluster producer/consumer 唯一、group fine/coarse 引用、LOD 球/error 单调、roots/leaves 覆盖），forest 路径是否等价迁移；LOD0/core glTF 是否完全未被改写（索引/属性/材质/节点/DOM）；`sourceHash` 是否严格绑定 import 重建的源几何。
2. **DAG cut 正确性**：阈值 0/相等/`<=`、闭包修正、cluster 选择公式、一次扫描；perspective/ortho/非均匀缩放/近面/非有限视图；与 converter CPU reference `select_cut` 在 cross-DAG/seam fixture 上逐 cluster ID 集合相等；容量不足整项回退而非截断；确定性（同输入逐字节）。
3. **导入健壮性与回退矩阵**：坏 wire/版本/profile/bufferView 越界、属性集合不符、非 OPAQUE、sourceHash 不匹配、DAG 非法、LOD0-only、item/表容量不足、帧 admission 失败——全部 typed 处理且不 panic、不失败 import、不出现半新半旧帧；计数日志完整；未知扩展不影响 core LOD0。
4. **容量/id/次序**：item 4096、meshlet 4096、表 65536 边界行为；run reservation=叶簇数与容量回退；对象 AABB 剔除先于 item 发布；HZB 在 cut 之后且候选数一致；shadows/BLAS 只见 LOD0；OFF 字节一致门禁。
5. **测量有效性与生命周期**：多实例 generation 共享/租约（无 dangling）、导入成本、3 轮中位数、同相机 OFF/ON 图像差异量化、M2 触发指标与 M4 重探针是否记录、无未测性能声明；证据归档/哈希链完整。

## 文件与边界

- **新增 engine 模块**（保留现有 forest 模块到迁移完成）：`source/engine/geometry/subtilis_dag_asset.cppm`（`deren.engine.subtilis.dag_asset`：DAG 层级 + 建设期校验）、`source/engine/geometry/subtilis_dag_lod.cppm`（`deren.engine.subtilis.dag_lod`：CPU DAG cut reference）；修改 `subtilis_selection.cppm`/`subtilis_upload.cppm`/`subtilis.cppm`（接线，最小改动）、`subtilis_asset.cppm`（forest→DAG 包装或标记 deprecated，只在 Task 5 决定）。
- **新增 root 静态资产库**（Task 2）：target 名 `asset_subtilis_core`，源文件 `source/asset/subtilis/{types.cppm,codec.cppm,codec.cpp,digest.cpp,dag.cppm,dag.cpp,cut.cpp}`，链接 `yyjson_vendored`/`vstd_lib`/`shared_utility`；`deren_engine` PRIVATE 链接；**不**把 builder/reduce/topology/parallel 拉进 root。
- **gltf_loader**（Task 4）：`source/gltf_loader/gltf_loader.cppm/.cpp` 增加 payload 提取（yyjson 限定解析：只取 `meshes[].primitives[].extensions.DEREN_subtilis` 的 version/profile/bufferView，长度/边界校验）；`deren::gltf::primitive` 新字段 + `drawable_iterator::get_subtilis()`；root deren_assets 链接 `yyjson_vendored`。
- **runtime/import**（Task 3-6）：`source/engine/runtime/runtime.declarations.cppm`（`import_scene` 可选 detection + `scene_import_result` 计数）、`runtime.cpp`（`create_primitive` adopted 路径、cut 接线、汇总日志）、`runtime.frames.cppm`（AABB 剔除次序断言/日志，如需）、`application_configuration` + `app/main.cpp`（配置键接线）。
- **测试**：新增 `source/tests/test_subtilis_dag_asset.cpp`、`test_subtilis_dag_lod.cpp`、`test_subtilis_import.cpp`；扩展 `test_gltf_loader.cpp`、`test_subtilis_selection.cpp`、`test_subtilis_frame.cpp`；fixture 与生成脚本：`source/tests/fixtures/subtilis/`（小 payload 文件 + `fixtures.json` SHA-256 清单，由转换器 CLI 生成，生成步骤写 `scripts/measure/make_object_lod_fixtures.py`）。
- **测量脚本/证据**：`scripts/measure/prepare_object_lod_scene.py`（派生 Bistro 资产 + 调转换器 + 记录 SHA-256/参数）、`scripts/measure/measure_object_lod.py` + `analyze_object_lod.py`（或复用/扩展 `capture.ps1` + 现有 measure 工具）；`docs/reviews/2026-10-21-object-lod.md` + `docs/reviews/2026-10-21-object-lod/`（JSON/log/截图），重产物归档 `D:/deren-assets/2026-10-21-object-lod/`。
- **文档**：`docs/subtilis.md`（状态头/§2/§5/§8/§10）、`docs/extensions/DEREN_subtilis/README.md`（追加 runtime consumer 规则/回退/容量，不改 wire）、`docs/visibility_buffer.md` 陈旧 id 布局文字与 `visibility.cppm:43-70` 对齐、`README` 特性列表（按仓库惯例）。
- **不改**：转换器 wire/schema/工具代码、resolve/raster/HZB 数学、id/packed 布局、M2/M4 设计、既有计划/审查文档（除 `docs/subtilis.md` 的状态更新）。

## 公共类型约定

接口位于 `deren::engine::subtilis` / `deren::engine::runtime` / `deren::gltf`；容器用 vessel，错误用项目既有 typed 错误/日志惯例，不混用 RHI 错误。

- `dag_asset`（Task 1）：只读已验证 DAG。`clusters()` = `span<dag_cluster const>`（engine `meshlet` record + `producer/consumer`）；`groups()` = `span<dag_group const>`（fine/coarse `{first,count}` 指向 `references()`，`lod` sphere，`error`）；`references()/leaves()/roots()`；`lod0_clusters()` = LOD0 叶簇 meshlet 记录（按 first_index 平铺前缀）；`lod0_index_count()`；`cluster_count()`/`maximum_selected_bound()` 等容量量。只能经 `make(...)` 建设，失败 `std::expected<dag_asset, dag_error>`（拓扑序/唯一 producer-consumer/覆盖/范围/球/误差/capacity）。
- `dag_asset::make` 的 LOD0 前缀不变量：`lod0_clusters()` 的窗口按 first_index 排序后无缝平铺 `[0, lod0_index_count)`，且 `leaves()` 集合 = producer==NONE 的 cluster 集合。
- `select_cut(dag_asset const&, view_parameters const&, uint32_t capacity, vector<meshlet>& out) -> expected<cut_statistics, cut_error>`（Task 2）：与 converter `select_cut` 同语义（闭包 + `<=` + 稳定 ID 顺序），容量不足返回 error（调用方整体回退 LOD0），stats 含 groups_visited/groups_selected/triangles/forced_finest。
- `gltf::subtilis_payload_view`（Task 4）：`{ bool present; span<uint8_t const> bytes; uint32_t version; string_view profile; uint32_t buffer_view; bool profile_attributes; }`；`drawable_iterator::get_subtilis()` 返回该视图（end 为空视图）。
- `runtime::import_scene` 计数：`scene_import_result` 追加 `subtilis_imported`、`subtilis_fallback_lod0` 与按原因的直方图（invalid_payload/profile_mismatch/material_not_opaque/source_mismatch/invalid_dag/lod0_only/capacity_item/capacity_table），并有一次汇总日志。
- `built_geometry`（Task 5 扩展）：新增 `span<meshlet const> lod0_clusters()` 与 `dag_asset const* hierarchy_dag()`（或未来统一为 DAG 层级）；adopted item 的 `meshlet_count`/`meshlets` 与 `lod0_clusters()` 一致；未 adopted item 走现有 `build_meshlets`。
- 配置（Task 6）：`[render] subtilis_error_px`（float，默认 1.0，>0 有限）、`[render] subtilis_hysteresis`（bool，默认 false）；runtime setter（`set_subtilis_error_px`/`set_subtilis_hysteresis`）供测试/测量使用；不改变 OFF 路径。
- 兼容性：`primitive_create_info::subtilis_geometry` 的既有显式 upload 路径继续可用（M1b 测试回归）；新 import 路径与它共享 `uploaded_geometry` 代际模型，不新增第二套资源生命周期。

## 依赖顺序

`T1 -> T2 -> T3 -> T4 -> T5 -> T6 -> T7 -> T8`。T1/T2 是纯 CPU 层（可并行验证），T3 依赖两者，T4 依赖 T3，T5 依赖 T4，T6/T7/T8 顺序。每个 Task 单独提交；T5/T6/T7/T8 前必须跑 root 回归（见各 Task Acceptance）。

---

## Task 1 — engine DAG 层级与建设期校验（纯 CPU）

**目标**：新增 `deren.engine.subtilis.dag_asset` 模块与测试，在不接 runtime 的前提下把 converter 的 DAG 语义完整表达为已验证只读层级。

**Files**：`source/engine/geometry/subtilis_dag_asset.cppm`（+ `.cpp` 如需）；`source/tests/test_subtilis_dag_asset.cpp`；`CMakeLists.txt`（deren_engine PUBLIC FILE_SET 模块 + `VR_TEST_TARGETS`）；`.github/workflows/ci.yml` 的测试列表镜像。

**Interfaces**：`dag_asset`（§公共类型约定）；`dag_error` 枚举（invalid_cluster/invalid_range/invalid_hierarchy/invalid_error/invalid_lod0_coverage/capacity）；`dag_asset::make(clusters, groups, references, leaves, roots, lod0_index_count) -> expected<dag_asset, dag_error>`。

**RED**：
- [x] 写测试：合法 cross-DAG（照 converter 设计 spec §3.1 的 G0/G1→G2/G3 语义构造，或复用 `source/tests/subtilis_dag_fixtures.hpp` 的 cross_dag）被接受；非法输入逐项拒绝：group 依赖倒序、producer/consumer 越界重复、LEAVES/ROOTS 覆盖不精确、fine/coarse 范围越界重叠、LOD0 叶簇不连续/不覆盖、sphere 非有限/半径负、error 非法/父小于子、cluster window 越界/非 3 倍数/<0、capacity 触发。
- [x] 运行 `ctest --test-dir build-release-dyn-clang64 -R '^test_subtilis_dag_asset$'` 确认 RED（缺模块/接口或断言失败），记录输出。

**GREEN**：
- [x] 实现建设期校验：bottom-up 拓扑序、唯一 producer/consumer、fine/coarse 引用范围、LEAVES=producer NONE 集合、ROOTS=consumer NONE 集合、LOD0 叶簇按 first_index 平铺、error 单调（依赖 group error ≤ 本 group error，允许相等与 `nextafter` 浮点保守处理）、LOD 球包含依赖球/自身 cluster 球（可选初始版本用离线已验证 + 本层保守检查）、capacity（record 数、u32 边界、`lod0_clusters().size()`/reservation 上界 ≤ 4096 由 Task 5 用，不在本层拒绝大 asset）。
- [x] `operator==`/确定性视图，无堆分配外副作用；只读 views。

**Acceptance**：
- [x] `cmake --build build-release-dyn-clang64 --target test_subtilis_dag_asset -j12` + `ctest -R '^test_subtilis_dag_asset$'` 通过；`test_docs` 通过（VR_TEST_TARGETS 与 ci.yml 同步）；ASan/UBSan 构建同 target 通过（`build-asan` 或当前 ASan 树）。
- [x] 无 Vulkan/设备依赖（测试只链接 `deren_engine`/`vstd_lib` 的 CPU 部分，如 `test_meshlet` 模式）。

## Task 2 — root 复用转换器 CPU 资产库 + fixture + DAG cut reference（与 converter reference 交叉校验）

**目标**：把已接受的 converter CPU 模块以同一份源文件编进 root（新增 `asset_subtilis_core`），提供 payload 解码/完整校验入口与 engine 侧 DAG cut reference，并用小 fixture 与转换器自己的 `select_cut`/`make_asset` 做逐 cluster 交叉校验。

**Files**：`CMakeLists.txt`（新 STATIC target `asset_subtilis_core`，sources 指向 `source/asset/subtilis/{types,codec,digest,dag,cut}`，链接 `yyjson_vendored vstd_lib shared_utility`；`deren_engine` PRIVATE 链接；新测试 target `test_subtilis_dag_lod` 进 `VR_TEST_TARGETS`）；`source/engine/geometry/subtilis_dag_lod.cppm`；`source/tests/test_subtilis_dag_lod.cpp`；`source/tests/fixtures/subtilis/` + `scripts/measure/make_object_lod_fixtures.py`；`.github/workflows/ci.yml` 镜像列表。

**Interfaces**：`select_cut(dag_asset const&, view_parameters const&, uint32_t capacity, vector<meshlet>& out)`（§公共类型约定）；测试侧使用 `deren::asset::subtilis::{decode, validate_structure, make_asset, select_cut}`（来自同一源文件）作为 reference；fixture 清单 `fixtures.json`（文件名/SHA-256/来源命令/转换器 commit）。

**RED**：
- [x] 用 `scripts/measure/make_object_lod_fixtures.py` 生成 fixture：用已接受 `deren-subtilis` CLI 对合成小资产（cross-DAG/UV-seam/LOD0-only 三类，几何可在测试内构造，或复用 converter 的 fixture 生成器）产出 payload 文件 + 记录 SHA-256；测试先失败（缺模块/接口）。
- [x] 测试用例：threshold 0/相等（`nextafter`）/两侧、perspective/ortho/非均匀缩放/近面/非有限视图/128 seeded views、容量不足返回 error、稳定 ID 顺序、确定性重复；同一 fixture 上用 converter `make_asset` 完整校验 payload（用测试内构造的 source geometry），再用 converter `select_cut` 与 engine `select_cut` 比较 selected cluster ID 序列**完全相等**（同 view 参数/同 threshold/capacity）。
- [x] `ctest -R '^test_subtilis_dag_lod$'` 确认 RED。

**GREEN**：
- [x] `subtilis_dag_lod.cppm`：把 converter `cut.cpp`/`dag.cpp` 的 `select_cut`（先算 group 决定 + 闭包，再按 cluster ID 稳定序扫描 + `<=` + capacity error + forced finest）按 engine `view_parameters`/meshlet 记录移植；`dag_asset` 的 group error 单调性保证闭包前提，仍保留显式保守修正（与 converter 一致）。
- [x] CMake target 编译选项与项目一致（新源文件自身代码走 root 警告/`-Werror`；若 converter 源需要 `-Wno-reserved-module-identifier` 等，在 target 上局部设置并记录）。
- [x] 根工程不链接 builder（不引入 meshoptimizer 到本 feature 的 import 路径）。

**Acceptance**：
- [x] `ctest -R '^test_subtilis_dag_lod$'` 绿；交叉校验测试打印"engine cut == converter reference cut"的用例数；
- [x] `ctest --test-dir build-subtilis`（converter 独立工程）仍全绿（同一源文件在两个构建图一致，无污染）；
- [x] root Release 构建通过，`test_docs` 通过；ASan/UBSan 新 target 绿。

## Task 3 — payload→engine 数据翻译与回退矩阵（纯 CPU，无 loader）

**目标**：实现 `subtilis::import_payload` 的纯函数部分：payload 解码→sourceHash 身份绑定→DAG 校验→`dag_asset` + adopted `built_geometry` 数据（LOD0 前缀/叶簇/合并 index/vertices）+ 逐项 fallback 原因；无 runtime/loader 依赖，可用合成 fixture 单测。

**Files**：`source/engine/geometry/subtilis_dag_import.cppm`（或并入 `subtilis_dag_asset.cppm`，按实现清晰度，计划倾向独立模块）、`source/tests/test_subtilis_import.cpp`（CPU 部分先立）、`scripts/measure/make_object_lod_fixtures.py` 扩展（fixture 覆盖坏 wire/坏版本/LOD0-only/属性不符/源不匹配）。

**Interfaces**：`import_report`/`import_decision`：`struct import_input { span<const uint8_t> payload_bytes; uint32_t version; string_view profile; bool profile_attributes; bool material_opaque; span<const uint8_t> vertex_bytes; uint32_t vertex_stride, vertex_count; span<const uint8_t> index_bytes; uint32_t index_width, index_count; }`；`expected<imported_asset, import_failure>`，`import_failure` 枚举与 spec §4.4 表一致；`imported_asset` 拥有 `dag_asset`、`built_geometry` 所需数据（vertices/indices/lod0_clusters）、sourceHash/buildHash 记录。

**RED**：
- [ ] 测试用例：合法 fixture → adopted，`lod0_index_count`/叶簇/index 前缀正确；sourceHash 不匹配 → `source_mismatch`；version≠1/profile 非法/bufferView 越界/坏 wire → `invalid_payload`；属性集合不符 → `profile_mismatch`；非 OPAQUE → `material_not_opaque`；LOD0-only（groups 空）→ `lod0_only`；叶簇 >4096 或 capacity → `capacity_item`；非法 DAG → `invalid_dag`；全部 typed 返回且不 panic/不 partial 输出。
- [ ] 先运行确认 RED。

**GREEN**：
- [ ] 解码复用 `deren::asset::subtilis::decode` + `validate_structure`；source geometry 从 64B interleaved vertex + index bytes 重建（position/normal/uv0；缺 TEXCOORD_0 且材质不需要 UV 时按转换器规则补 0；用 `source_digest` 重算并与 payload `sourceHash` 比较）。
- [ ] 翻译按 spec §4.3（cluster meshlet record：base_vertex=0/pad=0/axis=0/cone_cos=0/几何球；groups 引用范围直接映射；INDICES 原样；LOD0 叶簇按 first_index 排序平铺；校验 `lod0_index_count == 3*index_count` 且 LOD0 窗口平铺）。
- [ ] DAG 校验复用 `make_asset`？——**不**：生产 import 不做完整几何校验（spec §3.4）；本任务只调用 `validate_structure` + 新 `dag_asset::make` + sourceHash/覆盖率检查；完整 `make_asset` 只在测试（Task 2/本任务 fixture）里调用做交叉验证。
- [ ] 多实例共享设计先落数据侧：`imported_asset` 可被 `std::shared_ptr` 共享；键为 `{sourceHash, SHA-256(payload_bytes)}`（Task 4 接 loader 时使用）。

**Acceptance**：`ctest -R '^test_subtilis_import$'`（CPU 部分）绿；无 Vulkan 依赖；ASan 绿；spec §4.4 表每行有对应用例（打印覆盖清单）。

## Task 4 — gltf_loader payload 提取 + import 接线 + 配置开关 + 计数

**目标**：让 glTF 加载保留 payload 原始定位，`import_scene` 在启用时调用 Task 3 的导入函数并把 adopted 结果交给 `create_primitive`（本任务先只做数据/计数与 OFF 不采用，runtime 绘制行为的完整切换在 Task 5）。

**Files**：`source/gltf_loader/gltf_loader.cppm/.cpp`（`gltf::load_options` 重载（`extract_subtilis_payload=false` 默认，既有调用方不变）+ `subtilis_payload_view` + `get_subtilis()` + yyjson 限定解析；GLB 取 JSON chunk、.gltf 读文件原始字节）、`source/engine/runtime/runtime.declarations.cppm`（`import_scene` detection + counters）、`source/engine/runtime/runtime.cpp`（`create_primitive` 采用 imported 数据的接线骨架）、`source/app/main.cpp`（`settings.render.subtilis` 为 true 时对 `load_model`/`load_model_async` 传提取选项；`background_glb` 同样处理）、root `CMakeLists.txt`（deren_assets 链接 `yyjson_vendored`）、测试 `test_gltf_loader.cpp` + `test_subtilis_import.cpp`（import 端到端 CPU）。本任务不加 `subtilis_error_px`/hysteresis（Task 6）。

**Interfaces**：`gltf::load_options { bool extract_subtilis_payload = false; }`（`load_model`/`load_model_async` 增加带默认值的重载，旧签名保持）；`gltf::subtilis_payload_view`（§公共类型约定）；`drawable_iterator::get_subtilis()`；`scene_import_result` 计数直方图 + 汇总日志；配置行为 = `[render] subtilis` 为 true 时 app 请求提取并启用导入（OFF 启动不提取/不解析 payload；spec Q2 推荐）。

**RED**：
- [ ] `test_gltf_loader` 扩展：构造带 `DEREN_subtilis` extension 的 GLB/glTF（fixture 由脚本从 converter 输出裁剪生成，或测试内拼 JSON+bufferView）：`load_options.extract_subtilis_payload=true` 时 payload bytes/version/profile/attributes 视图正确、生命周期覆盖 `scenes`；默认 options 下 present=false 且加载行为与旧签名一致；bufferView 越界/坏 JSON/未知扩展不影响加载；无 extension 的 primitive 返回空视图。
- [ ] `test_subtilis_import` 端到端（CPU，无 GPU 时用 `probe_backend`/fake）：`import_scene`（stub drawable 或真实 loader scenes）在 subtilis=true 时 adopted 计数正确、fallback 直方图正确；subtilis=false 时零拷贝/不采用（可用观测计数或注入探针断言）。
- [ ] 确认 RED。

**GREEN**：
- [ ] loader：`load_model(path, load_options)`（默认不提取）在 options 打开时读文件原始字节（GLB 取 JSON chunk / .gltf JSON），用 yyjson 只解析 extension 对象→定位 bufferView→按 fastgltf 已解析的 buffers/bufferViews 拷贝字节；校验 `byteLength/totalBytes` 与 bufferView 范围；异常路径返回 present=false 并记录（不影响 core 加载）；一次 JSON 解析后遍历缓存，避免每 primitive 重复解析；未打开 options 时的行为与现有完全一致（旧签名转发）。
- [ ] import：`fill_info` 用 `if constexpr (requires ...)` 检测 `get_subtilis()`；adopted 时调 `import_payload`，成功则设置 `info.subtilis_geometry`（Task 5 完善 `built_geometry` 形态）并累加计数；失败累加原因；`subtilis=false` 时跳过调用且 loader 未提取（OFF 字节一致前提）；`scene_import_result` 打印一次汇总（提取/采用/回退原因）。
- [ ] 导入缓存：`map<{sourceHash, payload_sha}, shared_ptr<...>>`，多实例共享（Task 5 接 upload generation）。

**Acceptance**：`ctest -R 'test_gltf_loader|test_subtilis_import'` 绿；`test_runtime_injection`/`test_subtilis_frame` 不回归；ASan 绿；root Release 构建通过。

## Task 5 — runtime 采用 adopted 数据：item run/LOD0 叶簇、generation 共享、cut 接线

**目标**：把 adopted DAG 真正用于 per-item cut：`create_primitive` 采用 `lod0_clusters` 作为 item run 与 LOD0 回退，`matches_generation` 升级，多实例共享 generation，`record_visibility_items` 的 per-item planning 用 DAG cut（替代森林 cut 对 adopted item），显式 upload/forest 路径保持可用。

**Files**：`source/engine/geometry/subtilis_upload.cppm`（generation 共享/查询）、`subtilis_selection.cppm`（`matches_generation` v2、`coarse_allowed` 复用）、`subtilis.cppm`（`prepare_item` 分派）、`source/engine/runtime/runtime.cpp`（`create_primitive` adopted 路径、planning 调用）、`subtilis_builder.cppm`（forest 结果→DAG 包装，或将 forest 测试迁移到 DAG；按 Task 1/2 的类型决定）、测试 `test_subtilis_selection.cpp`/`test_subtilis_frame.cpp`/`test_subtilis_upload.cpp`/`test_subtilis_asset.cpp`/`test_subtilis_lod.cpp`/`test_subtilis_builder.cpp`（迁移或新增等价性用例）。

**Interfaces**：`built_geometry` 扩展（`lod0_clusters()`/DAG 层级，§公共类型约定）；`prepare_item` 对 adopted item 调 `dag_lod::select_cut`，对 forest item 保持现有路径（或统一迁移，见 T1/T2 结论）；`matches_generation` 校验 `leaf.meshlets` 与 `lod0_clusters`、窗口/记录合法。

**RED**：
- [ ] 测试：adopted item 的 item run = 叶簇数；cut 写出 run 前缀且 dispatch/发布正确；cut 超 reservation → `shared_run`（完整 LOD0，不截断）；多实例绑定同一 generation 且 `matches_generation` 通过；`lod0_clusters` 非空/连续/合法；显式 upload（forest builder）路径行为不变；OFF（无 subtilis_geometry）行为不变。
- [ ] 运行确认 RED。

**GREEN**：
- [ ] `create_primitive`：adopted 时用 `source.lod0_clusters()` 填 `result->meshlets` 与其 meshlet 表 run（不再用 `build_meshlets`；`draw_index_count = lod0_index_count`，AS/shadow 只读 LOD0 前缀）；`bind_subtilis_geometry` 校验扩展；多 primitive 共享同一 `uploaded_geometry`（import 缓存保证）。
- [ ] cut 分派：`prepare_item` 若 generation 的层级为 DAG 用 `dag_lod::select_cut`，否则走现有 forest `prepare_hierarchy`；两者的 run/发布/回退语义一致（`visible_run`/`shared_run` 不变）。
- [ ] forest→DAG 包装：`subtilis::build()` 产物在创建 `built_geometry` 时转换（forest group children 连续区间 → DAG 等价引用），既有 builder 测试改为断言包装前后 cut 集合一致 + 原测试保留。
- [ ] 现有 `matches_generation`/`coarse_allowed` 策略语义不变（MASK/BLEND、deform、custom pipeline、exact-depth 仍 finest）。

**Acceptance**：`ctest -R '^test_subtilis'` 全绿（含迁移用例）；`test_runtime_injection` 绿；root Release 全量 CTest 绿（记录数量）；ASan 绿；显式 upload 路径的 M1b 行为回归通过。

## Task 6 — 对象策略完善：AABB 次序、容量预检/日志、误差配置、迟滞、OFF/HZB/阴影门禁

**目标**：把运行时策略与测量接口补齐：对象级 AABB 剔除先于 item 发布（断言/日志）、item/表容量预检与汇总、`subtilis_error_px`/迟滞配置、OFF 字节一致门禁、HZB 在 LOD 之后与阴影/BLAS LOD0 的测试钉死。

**Files**：`runtime.cpp`（summary 计数/日志、hysteresis 状态、config 接线）、`runtime.frames.cppm`（次序/日志）、`application_configuration`（`subtilis_error_px`/`subtilis_hysteresis` 键 + 文档）、`app/main.cpp`、`test_subtilis_frame.cpp`/`test_subtilis_selection.cpp` 扩展、`scripts/windows/check_render.ps1` 调用（不改脚本，用现有 gate）、`docs/subtilis.md` 配置节。

**Interfaces**：`set_subtilis_error_px(float)`/`set_subtilis_hysteresis(bool)`；每场景一次汇总日志（adopted/fallback 原因/item 峰值/表占用/最大 run）；迟滞 history key `(import_instance, primitive*, generation)` + view epoch。

**RED**：
- [ ] 测试：hysteresis 关闭时 cut 与 reference 逐字节一致；开启时固定视图序列的决定符合"<0.8*target 变粗 / >1.0*target 变细 / 中间保持"，camera cut/resize/generation 变化使 history 失效；`subtilis_error_px` 改变 target（1.0 vs 2.0 的选中集合不同且符合参考）；item 峰值 >4096 时整帧回退（现有行为）且有汇总计数；HZB item 描述 `candidate_count == plan.run.count`；adopted item 的 `draw_index_count == lod0_index_count`（阴影/BLAS 只见 LOD0）；AABB 剔除次序（`frame_visible` 在 planning 前）。
- [ ] 确认 RED。

**GREEN**：实现配置/策略/日志；OFF 帧（subtilis=false）与无 payload 资产逐字节一致（用 capture/gate 验证，见 Acceptance）；迟滞默认 false 不改变确定性 reference。

**Acceptance**：
- [ ] `ctest -R '^test_subtilis|test_visibility_buffer|test_hzb'` 绿；
- [ ] OFF 门禁：`pwsh scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64` 与
  `pwsh scripts/windows/check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt -BuildDir build-release-dyn-clang64`
  不变（记录输出）；
- [ ] 带 payload 资产 OFF 启动 vs 无 payload 资产：同相机 capture 像素一致（脚本 + 保存日志）；
- [ ] root Release 全量 CTest 绿；ASan 绿。

## Task 7 — 真实场景测量（Bistro 派生资产）、M2 决策输入、M4 重探针清单

**目标**：在 Bistro 2909-item 场景上测量本 feature 的导入成本、每帧 CPU cut、GPU/三角形收益、容量/回退、OFF/ON 图像差异；把 spec §7.3 的 M2 触发指标与 §7.4 的 M4 重探针写成带数字的报告输入；不改生产行为（测量脚本/证据）。

**Files**：`scripts/measure/prepare_object_lod_scene.py`（派生资产+转换器调用+哈希+manifest）、`scripts/measure/measure_object_lod.py`/`analyze_object_lod.py`（或复用 capture/measure 工具）、`docs/reviews/2026-10-21-object-lod.md` + `docs/reviews/2026-10-21-object-lod/`；`D:/deren-assets/2026-10-21-object-lod/` 归档。

**RED**：
- [ ] 先写脚本骨架与报告模板，运行到缺数据状态（或空 `results.json`）确认 gate/分析脚本的负向行为；
- [ ] 确认派生资产 OFF 截图与原 `bistro-geometry.gltf` OFF 截图一致（若不一致即停：派生规则错误）。

**GREEN**：
- [ ] 派生资产：`bistro-object-lod.gltf` = 原 JSON 去掉每个 primitive 的 `TANGENT`（其余不变），buffer uri 指向原 `../bistro/*.bin`；用 `build-subtilis/deren-subtilis build --threads 12` 转换（默认参数）；记录转换器 binary SHA-256（`fa9755...` 或重新构建后的值）、源/派生/输出 SHA-256、报告 JSON；converter 输出（GLB 或 glTF package）作为配置 `model` 加载。
- [ ] 矩阵：1280×720、vsync off、max_fps=0、shadow/TAA/SSAO/bloom off、deferred lit、`visibility_buffer=true`、`subtilis=true`、`subtilis_error_px=1.0`（可选 2.0 一档）；far/near 两相机；validation 开/关各一组；130 帧/次；OFF/ON 各 3 轮交错；同相机对比；导入墙钟单独 3 次。
- [ ] 报告：§1 资产/环境/二进制哈希；§2 导入成本（OFF vs ON 加载墙钟、payload 数/内存）；§3 每帧 CPU cut（µs/item、每帧总、fallback 计数、planned 并发）；§4 选中三角形/meshlet、GPU 帧、wall、host-culled 表占用、item 峰值、内存峰值；§5 ON/OFF 图像差异（changed pixels/均值/最大差，不宣称一致）；§6 validation 零发现 + Khronos validator 证据（对转换器输出沿用 converter 证据引用或重跑）；§7 **M2 决策输入**：逐项对齐 spec §7.3 触发条件并给出实测值与是否触发；§8 **M4 重探针**：cluster dump/depth probe 重跑（far/near subpixel 占比、cut 选中 cluster 数、HZB 候选），列出 32-bit vs 64-bit/两级表/storage depth twin 的联合决策输入（不设计 M4）；§9 限制/未测项。
- [ ] 归档：`D:/deren-assets/2026-10-21-object-lod/`，逐文件 SHA-256 核对（manifest），scratch 副本清理；仓库只留报告+JSON 摘要+截图（尺寸允许时）。

**Acceptance**：报告存在且包含上述 §1-§9；`results/` JSON 可从原始日志重算（脚本 gate 自测负向用例）；OFF 截图与 M1b/原资产一致；ON 测量 3 轮数据完整；不 push。

## Task 8 — 文档、CI、最终审查与提交

**目标**：更新入口文档/里程碑/扩展文档，把新测试纳入 CI 契约，跑最终回归，提交本地并为一次 fresh whole-change review 准备输入（由 Lead 决定发起）。

**Files**：`docs/subtilis.md`（状态头：converter 输出已接入 runtime；§2 表/§5 配置/§8 已知限制/§10 里程碑 M1c；索引链到本 spec/plan/验收记录）、`docs/extensions/DEREN_subtilis/README.md`（runtime consumer 节：导入规则/回退/容量，不改 wire）、`docs/visibility_buffer.md`（id 布局陈旧描述与 `visibility.cppm:43-70` 对齐）、`README`（特性列表，按惯例）、`docs/reviews/2026-10-21-object-lod.md` 定稿、`.github/workflows/ci.yml` + `CMakeLists.txt` 最终核对（test_docs）、所有本地提交。

**RED**：
- [ ] 先跑 `ctest --test-dir build-release-dyn-clang64`、`check_render.ps1` 门禁、`test_docs`，记录当前状态并对照 Task 7 报告；确认文档未更新时 `test_docs`/审查检查会怎样失败（如 VR_TEST_TARGETS 与 ci.yml 列表不一致、Doxyfile/索引缺失——按仓库现有不变量）。

**GREEN**：
- [ ] 文档更新（状态/配置/里程碑/证据索引），确保 docs 全绿（`test_docs`）与链接有效。
- [ ] 最终回归：root Release `cmake --build` + `ctest` 全绿；ASan/UBSan 树全绿；converter 独立工程 `ctest --test-dir build-subtilis` 全绿；`deren-subtilis` CLI 快速回归；`check_render.ps1 -Full`/`-Visibility` OFF 冻结参考不变；`git status` 不含意外文件。
- [ ] 提交：按 Task 拆分本地提交（feat/test/docs/perf），**不 push**；记录 commit 范围与 HEAD；把 Task 7 证据与 final review 材料交给 Lead（fresh whole-change review 的 5 Review Focus = 本计划 "Review Focus" 节；不启动任务级 agents、不重复审查流程）。

**Acceptance**：上述回归命令全部记录；`test_docs` PASS；docs 状态与实现一致；本地提交完成、无 push。随后按本计划 **Review Focus** 节由一名 fresh 只读 reviewer 做一次 whole-change review（Lead 决定；不启动任务级 agents、不重复审查流程）；Critical/Important 修复须有复现与 GREEN，Minor 记账；接受与否由 Lead 决定。

---

## Verification Gate（实施完成前的最终门禁）

按顺序全部通过才可交付：

1. **构建/测试**：root Release `cmake --build build-release-dyn-clang64 -j12` + `ctest --test-dir build-release-dyn-clang64 --output-on-failure` 全绿（记录数量与日志）；ASan/UBSan Debug 树同样全绿（Windows 无 LSan，沿用既有记录）；converter 独立工程 `ctest --test-dir build-subtilis` 全绿。
2. **OFF 冻结**：`check_render.ps1 -Full`（legacy 冻结参考 `docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt`）与 `check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt` 不变；带 payload 资产 OFF 启动 vs 无 payload 同资产同相机像素一致（脚本记录）。
3. **ON 证据**：Task 7 报告 §1-§9 完整；3 轮中位数与逐轮表可重算；validation layers 零发现；Khronos validator 对转换器输出无 error/warning（沿用 converter 证据或重跑）；图像差异已量化且未宣称像素一致。
4. **容量/回退**：报告记录 item 峰值/表占用/最大 run/每类 fallback 计数；无静默截断；超限行为与 spec §4.4 一致。
5. **文档/CI**：`test_docs` PASS；`VR_TEST_TARGETS` 与 `.github/workflows/ci.yml` 列表一致；`docs/subtilis.md` 状态/里程碑/索引更新；扩展文档 consumer 节更新。
6. **仓库卫生**：`git status` 只有预期的提交内容与用户未跟踪文件；证据归档 D: 且 SHA-256 核对清单存在；**未 push**。

## 验收命令速查

```
cmake --build build-release-dyn-clang64 -j12
ctest --test-dir build-release-dyn-clang64 --output-on-failure
ctest -R '^test_subtilis|test_gltf_loader' --test-dir build-release-dyn-clang64 --output-on-failure
ctest --test-dir build-subtilis --output-on-failure                 # converter 独立工程
pwsh scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64
pwsh scripts/windows/check_render.ps1 -Visibility -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt -BuildDir build-release-dyn-clang64
pwsh -File scripts/windows/capture.ps1 -Base <object-lod base toml> -Tag object-lod-on -Frames 130 -Camera <far/near>
python scripts/measure/make_object_lod_fixtures.py --out source/tests/fixtures/subtilis
python scripts/measure/prepare_object_lod_scene.py --out D:/deren-assets/2026-10-21-object-lod
```

## 文档写作/提交约定

- 中文文档 UTF-8，与现有 `docs/superpowers/{specs,plans}` 风格一致；ASCII/链接/结构不变量受 `test_docs` 约束。
- 每个本地提交消息用 `feat|test|docs|perf:` 前缀，中文/英文与仓库现有习惯一致；不 push。
- 未测数字不写结论；所有测量数字注明来源（本计划/报告文件名 + 轮次/相机/配置）。


