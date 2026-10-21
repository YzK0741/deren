# deren runtime 对象 LOD：消费转换器 `DEREN_subtilis` cluster-DAG（设计）

日期：2026-10-21。状态：**设计 + 分阶段计划已评审通过（2026-10-21）；用户决定先提交文档、暂不开工，尚未实施任何代码。** 方向已由用户确认为四项：
(A) 对象 LOD = deren runtime 消费离线转换器产出的 `DEREN_subtilis` cluster-DAG，每个 static OPAQUE 对象获得**连续**的 per-object LOD（不是经典 LOD0/1/2 离散链）；(B) CPU-first，M2（GPU select/finalize）由测量决定，本阶段不假设 M2；(C) 仅 static OPAQUE；(D) 先写 spec + 分阶段计划供用户审查，再实施。实施计划见
[2026-10-21-object-lod-runtime.md](../plans/2026-10-21-object-lod-runtime.md)，验收记录待实施后写在
`docs/reviews/2026-10-21-object-lod.md`。

代码基线：`f4d3b8e7`（converter Task 1-8 已接受并完成 whole-change review；M1a/M1b/M3 已接受；HEAD 的转换器修复 `733a422f` 与最终审查记录 `492b82ee` 在基线内）。本文件不修改任何代码、根 CMake 或既有计划/审查；文中"设计期只读探针"数字均标注来源与可复现方式（附录 A）。

## 1. 目标与阶段边界

### 1.1 本阶段要交付什么

1. **导入路径**：`gltf_loader` 读入 core glTF 后，runtime 的 import 路径识别每个 primitive 上可选的
   `DEREN_subtilis` payload（extensions JSON + bufferView，见 `docs/extensions/DEREN_subtilis/README.md`），在
   load/import 时完成 wire 解码、身份校验、DAG 校验与到 runtime 层级的翻译，并把结果作为该 item 的 per-frame LOD
   源；core glTF 的 LOD0（原 primitive 索引/属性/材质/节点）保持原样可用。
2. **运行时对象 LOD**：对每个被 visibility lane 接纳的 static OPAQUE item，用**一次**屏幕误差 cut 在整个 DAG 上
   做决定，写出该 item 本帧的 meshlet run；不足/不支持时回退该 item 的完整 LOD0 run；整帧 admission 失败时回退
   legacy geometry lane。raster/resolve 数学不变，HZB 继续在 cut 之后消费候选集。
3. **证据**：真实场景（Bistro 2909-item 场景的派生资产，见 §4.3/§7/§8）的导入成本、每帧 CPU cut 成本、三角形/
   GPU 收益、fallback 计数、OFF 字节一致门禁、容量使用；以及 M2 触发指标与 M4 hand-off 的重探针清单。
4. 文档/CI：`docs/subtilis.md` 状态与章节更新、扩展文档的 runtime 消费说明、新 CPU 测试进 root CTest 与
   `.github/workflows/ci.yml` 的 `VR_TEST_TARGETS` 镜像列表、`test_docs` 不变量保持通过。

### 1.2 明确不做（本阶段）

- 不做 M2（GPU select/finalize）、不做 M4（软件光栅/subpixel cluster）、不做 M5（streaming/paging/硬盘 cache）。
- 不做动态/蒙皮/实例化/MASK/BLEND 的 LOD；这些目标保持 LOD0（现有 admission/`coarse_allowed` 规则）。
- 不改 resolve/raster 数学、不改 48B meshlet record、不改 32-bit packed visibility id（容量结论见 §6；M4 联合决策见 §7.4）。
- 不改转换器的构建算法/格式版本 v1，不新增磁盘格式；转换器工具本阶段只需重跑，不需要新代码路径（若实施中发现必需，
  作为单独变更由用户决定）。
- 不做自动磁盘缓存、不做 import 期的完整几何 boundary 重验（分层校验见 §3.4）。
- 不提前设计 M2/M4；只记录其触发条件与输入。

### 1.3 成功判据（本阶段验收目标）

- 可用性：Bistro 2909-item 派生场景从 glTF 启动加载后，static OPAQUE item 走 cut；禁用的帧/资源与 M1b 一致；
  OFF 路径与冻结参考**字节一致**（§5.6）。
- 正确性：CPU DAG cut 与**转换器 CPU reference cut**在固定 fixture 上逐 cluster 集合相等；相等阈值用 `<=`，
  近面/非有限/不支持视图回退 finest；run 超容量整体回退、绝不截断；整帧 fallback 不出现半新半旧。
- 容量：真实场景 item 数 ≤ 4096、每 item 选中 records ≤ 4096、全局 host-culled 表 ≤ 65536，均有计数/日志；
  任何超限为**确定性回退**而不是静默截断。
- 测量：3 轮中位数、同相机 A/B、OFF/ON 图像差异量化（不宣称像素一致）、导入成本单独报告；收益不与 M1b 的
  显式 upload 路径混报。
- 可回归：root Release 构建 + CTest 全绿；新 CPU 测试进 ASan/UBSan 门禁；本地提交不 push。

## 2. 现状证据（file:line）

### 2.1 转换器已接受（输入契约的来源）

- 独立 CPU 工具 `tools/subtilis` 与 glTF 扩展 `DEREN_subtilis` 已实现并接受：设计
  [2026-10-21-subtilis-gltf-design.md](2026-10-21-subtilis-gltf-design.md)，计划
  [2026-10-21-subtilis-gltf.md](../plans/2026-10-21-subtilis-gltf.md)，证据
  [2026-10-21-subtilis-converter.md](../../reviews/2026-10-21-subtilis-converter.md)。
- 扩展契约：payload 挂在 `mesh.primitives[i].extensions.DEREN_subtilis`（`version:1`、`profile:"shared-vertices"`、
  `bufferView`），原始 primitive（含标准 LOD0 indices）完整保留，不加入 `extensionsRequired`
  （`docs/extensions/DEREN_subtilis/README.md:1-77`）。wire v1 = 128B header + 7 个 32B section descriptor，
  sections 为 INDICES/CLUSTERS/GROUPS/REFERENCES/LEAVES/ROOTS/BUILD_META；CLUSTERS 带 producer/consumer，
  GROUPS 带 fine/coarse 引用范围、LOD 球与 error；`NONE=0xffffffff`；canonical `sourceHash/buildHash` 为 SHA-256
  身份流。README 明确："Wire decoding is **not** DAG or geometry validation; the immutable validated-asset factory
  performs those checks separately."。
- 转换器语义（本设计依赖的三个性质）：每个 cluster 最多一个 producer 与一个 consumer；但**一个 reduction 的多个
  输出 cluster 可被多个后续 group 消费**，因此 group 依赖是 DAG（跨组重新分区）。设计 spec §3.1 给出真实见证：
  `G0:{a,b}->{u0,u1}`、`G1:{c,d}->{v0,v1}`、`G2:{u0,v0}->{w0}`、`G3:{u1,v1}->{w1}`；group 按细到粗拓扑排序
  （依赖 `p < g`）；多根允许。误差单调传播、LOD 球包含输入/输出/依赖球；cluster 几何球与 LOD 球分开存储
  （`2026-10-21-subtilis-gltf-design.md` §3.1-3.2）。CPU cut 参考：先算每个 group 一次投影误差
  `coarsen(g)=E(g)<=threshold`，显式验证向下闭合，再按
  `(producer==NONE || coarsen(producer)) && (consumer==NONE || !coarsen(consumer))` 一次扫描输出；预算不足整体
  返回 LOD0/错误，不截断（同文件 §3.3）。
- 已验收的真实资产证据（`docs/reviews/2026-10-21-subtilis-converter.md:28,36,39,85-89,144-145`：
  `bistro-merged.gltf` 254 primitives / 4,209,006 triangles，231 转换 / 23 跳过（MASK/BLEND 等 profile 外）/
  57 LOD0-only，最大 10 轮，556 次跨组重新分区，4,618 个 group；payload 247,474,732 B、SHA-256 `b235aa40...`；
  `validate --full` 与 Khronos validator 通过。
- 转换器 CPU 模块已按可复用方式拆好：`subtilis_codec`（types/codec/digest）与 `subtilis_dag`（dag/cut）两个
  STATIC target（`tools/subtilis/CMakeLists.txt:15-31`），源文件在 `source/asset/subtilis/`，模块名
  `deren.asset.subtilis.{types,codec,dag}`；根工程当前**不链接**它们（`CMakeLists.txt` 中无
  `source/asset/subtilis` 源文件）。

### 2.2 runtime 现状：M1b 的 per-item CPU cut 已工作，但没有自动导入

- `docs/subtilis.md:3-10` 状态头明确："There is no automatic asset import yet - the runtime consumes an explicitly
  uploaded build result (`primitive_create_info::subtilis_geometry`); every real-scene result below used an
  experiment-only build-at-import patch, never a production hook."；`:15-19` 说明离线转换器"is not wired into the
  runtime import path yet"；`:221-232` 里程碑：M2/M4/M5 未实现，asset pipeline 未实现。
- 现有 runtime 层级是**唯一父森林**：`source/engine/geometry/subtilis_asset.cppm:15-44`（`group_record` 只有
  coarse 范围 + 连续 children group 范围 + error；"CPU order is bottom-up: every child precedes its parent. A node
  replaces ALL children."），`:46` 明确 "This layout is neither a disk format nor a shader ABI."；`:83-224` 的建设期
  校验含唯一父、children 连续、LOD0 叶簇按 first_index 平铺 `[0,lod0_index_count)`、root worst-cut ≤ 叶数。
- CPU cut：`subtilis_lod.cppm:78-150`（根→children 递归；`error*stretch*projection_scale`，透视除最近深度，近面/
  非有限视图强制 finest；容量不足 `cut_error::capacity`）。视图由 `make_item_view`（`subtilis_selection.cppm:30-55`）
  从 `push.model`/view/proj 生成；item 策略 `coarse_allowed`（`:83-93`：64B stride、normal/uv 偏移、材质 flags、
  exact-depth redraw、transparent/overlay、custom pipeline、skin/morph 排除）。
- 帧内选择/发布：`source/engine/runtime/runtime.cpp:2051-2345` `record_visibility_items`：phase 1 候选扫描
  （`:2077` item 数超过 `visibility_item_capacity` 整帧拒绝；`:2087-2106` 逐 leaf 规则）→ phase 2 并行规划
  （`:2108-2177`，每 item `make_item_view`+`prepare_item`，cut 写 host-culled 表）→ phase 3 item 序发布裁决
  （`:2229-2252`）→ phase 4 录制（`:2271-2293` HZB item 描述取 **cut 结果** `plan.run`，`:2302-2307` per-item
  indirect dispatch）。`subtilis_active_frame = subtilis_on && visibility_lane_frame==visibility`
  （`runtime.frames.cppm:875-877`），admission 见 `source/engine/visibility/visibility.cppm:439-500,513-559,565-571`。
- 对象级 AABB 剔除已经在 LOD 之前：`runtime.frames.cppm:634-720` 用 BVH 对 leaf 的 world AABB 做相机视锥剔除，
  结果即 `frame_visible`；`record_visibility_items` 只遍历 `frame_visible`。
- 阴影/BLAS 保持 LOD0：`ray_tracing.cpp:235-252` 用 `caster.draw_index_count`（对 subtilis item 等于
  `lod0_index_count`，`primitive.cppm:2147`）；shadow caster 列表与几何 lane 也用 LOD0 绘制。
- 容量契约（M1b 冻结）：packed id = `valid1 + item12 + meshlet12 + triangle7`
  （`visibility.cppm:43-70`），item 上限 4096、每 item meshlet 上限 4096、全局 host-culled 表 65536
  （`meshlet.cppm:59`）、HZB lane item 上限 4096 / candidate 65536（`hzb.cppm:89-95`）。
- 测量基线（`docs/subtilis.md:108-132`；`docs/reviews/2026-10-20-bistro-performance/README.md`）：
  Bistro 4,208,625 triangles / 2,909 items / 6,006 nodes；显式 upload 的 OFF/ON：三角形
  4,208,625→3,727,209（far，−11.4%），GPU 帧 26.98→24.63 ms（−8.7%），wall 25.25→23.18 ms（−8.2%）；
  CPU cut serial +8.68 ms/frame（约 3 µs/item），并行化后 post phase 8.6→约 2 ms（`docs/subtilis.md:126-128`）；
  容量 1024→4096 把该场景从 legacy 整帧回退 178.5 ms 拉到 26.7 ms（约 6.6×）。这些是**显式 upload 路径**的数字，
  不是本 feature 的数字。
- M1b 的真实场景证据用了 experiment-only build-at-import patch（`docs/reviews/2026-10-20-bistro-performance/
  acceptance-probe.patch`，测量后已还原）；它把 build+upload 塞进 `create_primitive`，**不是**生产 import 策略。

### 2.3 缺口与中心矛盾：converter 是 DAG，engine 层级是唯一父森林

engine 的目标格式要求 group 的 children 是**连续 group id 区间**且每个 group 只有一个父（`subtilis_asset.cppm:19-22,
108-233`）。converter 的 group 依赖是 DAG：一个 producer group 的多个输出 cluster 可被多个后续 group 消费
（§3.1 的 `G0` 例子）。把 DAG 压成唯一父森林**在一般情况下无法无损完成**：

- 一个 producer 的 coarse 输出被两个 consumer（`G2`、`G3`）分用时，若把它作为 `G2` 的 child，则 engine 的
  "child group 的**全部** coarse 表示被父整体替换"语义会把 `u1` 也带入 `G2` 的区域；若为两个父各复制一份该
  producer，则复制体的 children（它自己的细集）无法按"每个输出 cluster 只属于一个父"切分——wire 里没有
  "哪部分细几何生成了哪个 coarse cluster"的映射（reduction 是先合并整个 group 再重新聚类），所以复制体无法
  构造出合法的 children 覆盖。任何"保留一个父 + 丢弃其余边"的近似要么漏几何，要么把粗/细两种表示同时画进同一
  区域（z-fight/裂缝），违反设计 spec §3.3 的替换覆盖规则。
- 这不是理论边角：转换器证据里 `bistro-merged` 有 556 次跨组重新分区；本设计用真实场景派生资产（附录 A）测得
  506 个可转换 mesh 中 77 个（15%）有跨组重新分区，且它们覆盖 **1,491,442 条被实例化绘制的三角形（35.4%）**，
  报告根层相对首层的最大可达减面 40.1%（其余 429 个 mesh 为 22.3%；这是构建报告 levels 的比较，不是本 feature
  的实测收益）。拒绝跨组 payload 等于把 LOD 收益正好丢在大网格上。
- 因此本设计把 **seam 选择**作为第一个必须由用户确认的架构决定（§3、§9 Q1），而不是实现细节。

### 2.4 依赖/链接现状（决定的边界条件）

- `source/asset/subtilis/**` 只被 `tools/subtilis/CMakeLists.txt` 编译，root `CMakeLists.txt` 不包含它；root 的
  `deren_engine`（STATIC，`:613-724`）已有 `meshoptimizer_vendored`/`vstd_lib`/`shared_utility`，并已包含
  engine 自己的 subtilis 模块（`source/engine/geometry/subtilis_*.cppm`）；root 的 `deren_assets`（SHARED，
  `:725-900`）是 glTF 栈，包含 `gltf_loader` + `fastgltf_vendored`，engine 核心刻意 loader-agnostic（`:725-760,
  895-900`）。
- 根工程已有 `yyjson_vendored`/`fastgltf_vendored`/`meshoptimizer_vendored` 目标
  （`cmake/deren_asset_dependencies.cmake:4-31`），且它们在 `VR_VENDORED_TARGETS` 的警告豁免列表
  （`CMakeLists.txt:1708`）；`source/asset/subtilis/{codec,dag}.cpp` 的依赖只有 `vstd_lib`/`shared_utility`/
  `yyjson_vendored`/标准库（`tools/subtilis/CMakeLists.txt:15-31`），没有 Vulkan/meshoptimizer 依赖
  （builder 才需要 meshoptimizer，本 feature 不需要 builder）。
- `deren_assets.dll` 不能引用 `deren_engine`（STATIC 进 exe）符号（`CMakeLists.txt:725-760` 的既有约束）；因此
  **任何用到 engine 类型的翻译/校验必须在 deren_engine 一侧**，DLL 只应传递原始字节。
- import 路径是模板：`runtime::import_scene`（`runtime.declarations.cppm:4602-4790`）从两个结构化 iterator 流
  （node/drawable）拉数据；`fill_info`（`:4625-4684`）把 drawable 的 vertex/index/material 填进
  `primitive_create_info` 后调 `create_primitive("pbr", info)`（`:4700`）；`scene_drawable_iterator` 是
  structural concept（`primitive.cppm:527-545`），glTF loader 的 `drawable_iterator` 满足它
  （`gltf_loader.cppm:1120-1180`，`gltf_loader.cpp:1894-1917` lazily build 64B interleaved vertices）。
  `primitive_create_info::subtilis_geometry`（`primitive.cppm:1607-1616`）已存在，`create_primitive` 在有它时走
  `upload_geometry` + `bind_subtilis_geometry`（`runtime.cpp:2668-2692`），且无论是否有它都执行 meshlet split/
  表分配/AABB 扫描（`:2749-2815`）。
- 加载器：`load_model`（`gltf_loader.cppm:833-841`）→ `fastgltf::Parser`（`gltf_loader.cpp:1557-1584`，选项
  `LoadExternalBuffers|LoadExternalImages`，`:96-98`）；primitive 结构保留 attribute portions/index/material
  （`gltf_loader.cppm:347-357`）。fastgltf 0.9 只解析 `Parser` 构造参数里启用的扩展，未知 extension 的 JSON 不进
  `Asset`；loader 目前也**不保留**原始 JSON DOM —— 因此 payload 需要在 loader 内做一次限定范围的 JSON/字节提取
  （§4.2）。

## 3. 关键决策：seam 选型与依赖方向

### 3.1 三个候选方案

| 方案 | 内容 | 结论 | 理由 |
| --- | --- | --- | --- |
| A. runtime import 侧把 DAG 投影成唯一父森林 | 用 converter payload 在 import 时构造 engine 现有 forest 层级 | **拒绝** | §2.3：一般无法无损；跨组分用（Bistro 上 35.4% 的绘制三角形）只能被丢弃或导致粗/细混画；会引入无法在离线验证的视觉缺陷 |
| B. 离线 pre-flatten 成 engine forest 格式 | 转换器新增"forest 兼容构建/烘焙"输出（新 payload 版本或第二扩展） | **备选（仅当 Q1 否决 C 时）** | 引擎侧简单、可离线全量验证；但：格式分叉（v1 DAG 与 baked forest 两套）、需在转换器内重做构建约束（当前 builder 的跨组分区正是 LOD 收益来源）、须重跑全部真实资产证据、未来 M2/M4 仍需 cluster/group 语义与 64-bit 决策，等于把问题推后；并违反"不同时维护两套正式资产模型"的既定约束（`2026-10-18-subtilis-design.md:186-188`） |
| C. runtime 层级升级为转换器的参考 DAG 语义（推荐） | engine 增加 DAG 层级类型（cluster producer/consumer、group fine/coarse 引用、LOD 球、error、roots/leaves），并把转换器已验证的 CPU cut 公式移植进 engine；forest 成为其特例 | **推荐** | 忠实消费已接受的资产；格式/构建唯一；复用已接受的 codec/validator/cut 参考（不重复解析）；M2/M4 只有一套 cut 语义；engine 现有 builder/upload/显式 upload 路径可保留为参考与测试生产者（其输出可平凡转换为 DAG，见 §5.8） |

### 3.2 推荐 C 的接口与代价

- **engine 新模块** `deren.engine.subtilis.dag_asset`（文件 `source/engine/geometry/subtilis_dag_asset.cppm`）：
  `dag_asset` = 已验证的 DAG 层级（§4.3），只读 views；与现有 `subtilis_asset.cppm` 的 forest 类型并存到迁移完成，
  最终由 implementation plan 的 Task 1/5 合并/弃用 forest 运行路径（现有 forest 测试与 M1b 显式 upload 路径在
  迁移后仍须回归，见 §5.8）。
- **CPU cut 移植** `deren.engine.subtilis.dag_lod`（`subtilis_dag_lod.cppm`）：把 `source/asset/subtilis/cut.cpp`
  /`dag.cpp` 的 `select_cut` 参考（闭包 + cluster 公式）按 engine 的 `view_parameters`/meshlet 记录移植。
- **代价**：engine 的 subtilis 资产/选择测试要按 DAG 语义迁移或新增（forest 仍作为构造特例存在）；exe 会增加
  converter CPU 模块（codec/dag/cut + yyjson，量级为百 KB 级，Task 2 测量并记录，不做未测声明）；DAG cut 的
  CPU 成本待 Task 7 实测（M1b forest cut 并行约 2 ms 是基线，不是保证）。
- 为什么不是"改 engine 现有 forest 模块内部"：forest 的 `group.children` 语义（连续区间、唯一父）正是 DAG 要
  去掉的限制；保留它再旁路会导致两套选择算法（CPU/GPU/M2 分叉），属于该 feature 后续最贵的债务。

### 3.3 链接方向（解析放在哪里、依赖如何流动）

- **解析与翻译放在 deren_engine**（静态库）：新增 root STATIC target（计划名 `asset_subtilis_core`），直接编译
  `source/asset/subtilis/{types.cppm,codec.cppm,codec.cpp,digest.cpp,dag.cppm,dag.cpp,cut.cpp}`，链接
  `shared_utility`/`vstd_lib`/`yyjson_vendored`；由 `deren_engine` PRIVATE 链接。这样：
  - 复用**已接受**的 wire 解码/身份/完整 DAG 校验代码，不写第二个 wire parser（避免双解析器语义漂移）；
  - engine 只依赖 CPU 资产模块，不依赖 glTF/fastgltf；`deren_assets.dll` 不增大、不引用 engine 符号；
  - 转换器工具自身的 CMake 工程不改（同一 repo 源文件被两个构建图分别编译，跨进程无 ABI 共享；两边都从同一基线
    编译，Task 2 用转换器 test 目标 + root 交叉校验测试锁定语义一致）。
- **gltf_loader（deren_assets DLL）只做原始定位**：在 load 时为每个 primitive 提取 `DEREN_subtilis` extension 的
  `{version, profile, bufferView}` 与**bufferView 字节的拷贝**（以及"该 primitive 的 glTF attribute 集合是否在
  shared-vertices profile 内"的布尔事实），放进保留的 primitive 数据；不做 wire 语义解析、不依赖 codec。
  loader 保留的 JSON 解析使用 `yyjson_vendored`（root 已有；fastgltf 的 parser 不保留未知 extension JSON 的
  DOM，见 `source/gltf_loader/gltf_loader.cpp:1572-1584` 与 fastgltf 0.9 的扩展白名单行为）。载荷字节的
  长度/偏移校验（bufferView 范围、总量上限）在此完成，失败只记录"该 primitive 无可用 payload"，不影响 core LOD0。
- **import 侧**：`runtime::import_scene` 的 `fill_info` 通过**可选的结构化检测**读取 drawable 的
  `get_subtilis()`（`requires` 表达式，不修改 `scene_drawable_iterator` 的必需成员，现有测试 stub 不变），调
  engine 侧的 `subtilis::import_payload(...)` 得到共享的 `dag_asset`/`built_geometry` 代际；失败按 §4.4 的
  fallback 矩阵处理并把计数写进 `scene_import_result`（新增 `subtilis_imported`/`subtilis_fallback`/原因直方图）。
- 二进制后果：代码量/二进制增长由 Task 2 记录（`yyjson` 已在根工程编译为 `yyjson_vendored`，没有新增第三方）。
  不新增 DLL、不新增 Vulkan/CUDA 依赖、不改工具链（Clang64/C++23 modules 与 converter 一致）。

### 3.4 验证分层（信任边界）

| 层 | 何时 | 检查 | 失败行为 |
| --- | --- | --- | --- |
| 离线转换器 `validate --full`（已接受） | 资产生成时 | source 绑定、LOD0 三角形多重集/绕序、每次 reduction 的有向外边界重数、单调误差、球包含、预算 | 资产不发布/需修复；已由 converter Task 8 证据覆盖 |
| runtime import 结构校验 | 加载时 | wire 解码（codec）+ `sourceHash` 重算绑定（见 §4.3）+ DAG 结构不变量（拓扑序、producer/consumer 唯一、LEAVES/ROOTS 覆盖、LOD 球有限/嵌套、容量）+ engine 层级建设期校验（LOD0 平铺、group 引用范围、sphere/error 有限） | 该 primitive 不上 LOD，LOD0 legacy；报告原因与计数；import 不失败、不 panic |
| runtime fixture 全验证（测试/可选 debug） | root CTest 与可选 `--verify-import` | 用与转换器相同的 `make_asset(source_geometry, payload)` 做完整几何校验（测试里跑；生产默认不跑） | 测试失败即实现回归；生产路径不受影响 |

不在生产 import 跑完整几何 boundary 重验：它需要逐 group gather fine/coarse 顶点并做边界多重集分析（converter
证据报告 `docs/reviews/2026-10-21-subtilis-converter.md:171` 的 `validate` 阶段在 bistro-merged 上合计
3,794.8 / 5,575.7 ms（serial / parallel）；2909-item 场景启动时逐 primitive 复做会显著拉长加载且属于离线已证明
的性质），故以"离线全验证 + import 结构/身份校验 + CTest 全验证 fixture"分层承担；可信边界在 §9 Q5 让用户确认。

## 4. 资产契约、翻译与导入

### 4.1 runtime 从 payload 需要的事实

- payload 身份：`sourceHash`（=该 primitive 的 canonical source geometry 摘要）+ `buildHash`（构建参数/依赖）+
  wire `version/profile`；runtime 只接受 `version=1`、`profile=shared-vertices`。
- DAG 数据：clusters（window/producer/consumer/几何球）、groups（fine/coarse 引用范围、LOD 球、error）、
  references、leaves、roots、INDICES（原顶点索引流）。
- 与 core glTF 的关系：payload **只补充** LOD 数据；LOD0 的 vertex/index/材质/节点/DOM 一律以 core glTF 为准，
  payload 不覆盖、不改写（转换器契约；`docs/extensions/DEREN_subtilis/README.md:1-10`）。
- 可用性条件（§4.4 矩阵）：payload 存在、wire 合法、身份匹配、glTF attribute 集合在 profile 内、材质 OPAQUE、
  primitive 不是 skin/morph/instanced（node 语义）、容量可容纳。

### 4.2 glTF → runtime 的提取

- `gltf_loader` 为每个 primitive 保留（新增字段，`deren::gltf::primitive`）：`subtilis_payload` =
  `{ std::span<uint8_t const> bytes; uint32_t buffer_view; uint32_t version; std::string profile; bool
  profile_attributes; bool present; }`，其中 `bytes` 是 bufferView 的拷贝（loader 自身拥有，随 `scenes` 存活）；
  `profile_attributes` = POSITION/NORMAL 存在、且属性名集合 ⊆ {POSITION, NORMAL, TEXCOORD_0}（转换器 eligibility
  的同类规则；TANGENT 或其他属性出现即 false）。MASK/BLEND 由现有 material 解析结果
  （`resolved_factors.alpha_mask/alpha_blend`）在 runtime 侧判定，不重复进 loader。
- `drawable_iterator::get_subtilis()` 返回上述只读视图（end 恒 empty）；`import_scene` 用
  `if constexpr (requires { drawable.get_subtilis(); })` 检测。检测失败/字段为空 = 该 drawable 无 payload。
- bufferView 范围/总量上限在 loader 内校验（复用 fastgltf 已校验的 buffer/bufferView 结构 + runtime 侧再次核对
  payload 声明长度）。**提取是显式选项**：`gltf::load_model` 增加一个 load-options 重载（默认不提取，未改调用方
  行为），app 仅在 `[render] subtilis = true` 时传"提取 payload"；OFF 启动因此不拷贝 payload、不增加额外内存/时间
  （§5.6 的 OFF 决策）。

### 4.3 payload → engine 数据（翻译规则）

`deren.engine.subtilis.dag_asset`（新模块，只读）：

- `clusters[]`：engine `meshlet`（48B：first_index/index_count/base_vertex=0/pad=0/center/radius 来自 wire 几何球/
  `axis = 0, cone_cos = 0`）。`axis=0, cone_cos=0` 是"没有法线锥声明"的保守值（`mesh_geometry.slang` 的锥剔除
  注释明确 "the value that states nothing is 0, not 1"），保证 converter cluster 永不被锥剔除。
- `groups[]`：`{fine_range, coarse_range, lod_sphere, error}`，范围直接指向 `references[]`（与 wire GROUPS 的
  fine_first/fine_count/coarse_first/coarse_count 一一对应；converter group 已按 `p<g` 拓扑排序）。
- `references[]`/`leaves[]`/`roots[]`：直接翻译 wire 对应 section（cluster id 语义不变）。
- `lod0_index_count` = INDICES 中 LOD0 cluster（producer==NONE）窗口之和；`lod0_clusters` = 这些 cluster 的
  meshlet 记录（按 first_index 排序后平铺 `[0, lod0_index_count)`）。
- `combined indices` = payload INDICES 原样（u32）；`draw_index_count` = `lod0_index_count`；合并 index buffer 的
  coarse 页 = payload 其余 cluster 窗口，随 generation 上传。设计期只读探针验证了全部 506 个真实 payload 的
  LOD0 窗口按 first_index 平铺前缀且 LEAVES 集合等于 producer==NONE cluster 集合（附录 A），因此**不需要重建
  index 布局**，只做搬运/校验。
- vertices：loader 的 64B interleaved 顶点字节（`gltf_loader.cpp:867-873`），offsets 12/24 与 engine
  `coarse_allowed` 要求一致（`subtilis_selection.cppm:89`）。
- 源几何重建（供 `sourceHash` 重算与测试全验证）：从 64B 顶点读 float3 position/normal/float2 uv0 + primitive
  index 列表（TEXCOORD_0 缺失且材质不需要 UV 时按转换器规则填 0；若 glTF 有 TEXCOORD_0 而 payload 存在，正常读）。
  `sourceHash` 重算必须使用与转换器 `source_digest` 完全相同的规范流（`source/asset/subtilis/digest.cpp:105-131`
  与扩展 README 的 "Canonical identity encoding"）——**直接复用已接受的 `source_digest` 实现**，不手写。
- 身份绑定：`sourceHash == source_digest(重建源几何)` 且 payload 的 INDICES 长度/索引范围与 core glTF 一致；
  不匹配 = 该 primitive 回退 LOD0（计数原因 `source_mismatch`），绝不带病启用。
- 实例共享：import 缓存以 `{sourceHash, wire bytes SHA-256}` 为 key；同一 mesh 的多个 node 实例共享同一份
  `built_geometry` 与 GPU generation（现有 `upload_geometry` 返回共享 generation；`bind_subtilis_geometry` 支持
  多个 primitive 绑定同一 generation，`primitive.cppm:2135-2158`）。每 instance 仍各自占用 host-culled 表 run，
  因为每帧的 cut 结果随 model transform 不同。

### 4.4 严格 fallback / 拒绝矩阵（每个 primitive 独立，不产生半新半旧帧）

| 条件 | 行为 | 计数/日志 |
| --- | --- | --- |
| primitive 没有 `DEREN_subtilis` extension | 现有 LOD0 legacy 路径（完全不变） | 不计（正常资产） |
| extension 字段缺失/version≠1/profile∉{shared-vertices}/bufferView 越界/payload 超限 | 丢弃该 payload，LOD0 legacy | `invalid_payload` 直方图 + 一次样例日志 |
| wire 解码失败/规范化失败（codec 拒绝） | 同上 | `invalid_payload` |
| glTF attribute 集合不在 profile 内（TANGENT/其他属性/缺 NORMAL/POSITION、非 TRIANGLES、morph targets） | 不解析/不采用 | `profile_mismatch` |
| 材质非 OPAQUE（MASK/BLEND） | 不采用（converter 也不会为其产出；防御性检查） | `material_not_opaque` |
| node 语义为 instanced/skinned/morph 或 custom pipeline/exact-depth redraw | 可以导入但该 item 的 `coarse_allowed` 强制 finest（现有策略，无需特殊 payload 处理） | 现有 fallback 计数 |
| `sourceHash` 与重建源几何不匹配 / INDICES 与 core indices 不一致 | 丢弃 payload，LOD0 | `source_mismatch` + 样例日志 |
| DAG 结构非法（拓扑序/唯一 producer/consumer/覆盖/球嵌套/范围）、engine 建设期校验失败 | 丢弃 payload，LOD0 | `invalid_dag` |
| LOD0-only payload（groups 为空，无可简化层级） | 不采用（没有 LOD 收益；保留 legacy，避免无谓 reservation/cut 开销） | `lod0_only` |
| 该 item 的 LOD0 叶簇数 > 4096（id meshlet 上限）或全局 host-culled 表预算不足 | 该 payload 不采用 / 预算用尽后的 primitive 走 LOD0 legacy，确定性（场景顺序） | `capacity_item` / `capacity_table`（每场景一次汇总日志） |
| 帧内 cut 选中 records 超过该 item reservation（=叶簇数） | 该 item 整体回退共享 LOD0 run（`cut_error::capacity` → `shared_run`，绝不截断） | 现有 `fallback item(s)` 计数 |
| 帧 admission 失败（item>4096/材质 MASK/实例/蒙皮等） | 整帧 legacy geometry lane（现有规则） | 现有一次日志 |
| 视图非法/近面穿越/非有限矩阵 | 该 item finest / LOD0（现有 reference 语义） | 现有 `forced_finest` 统计 |

## 5. Runtime 对象 LOD 选择与逐帧数据流

### 5.1 数据流（顺序即约束）

```
gltf_loader.load_model
  -> import_scene (node/drawable streams)
       fill_info: 读 64B vertex/index/材质 + [可选] get_subtilis()
       subtilis::import_payload(...)                 // 解码/校验/翻译（一次，按 payload 去重）
         -> shared dag_asset + built_geometry + GPU generation（多实例共享）
       create_primitive("pbr", info)                 // item run = LOD0 叶簇（见 5.2）
每帧:
  runtime.frames: leaf 世界 AABB 的 BVH 视锥剔除（对象级 AABB，先于 item 发布） -> frame_visible
  record_visibility_items:
    phase 1 候选扫描（item 上限 4096；规则与现有一致）
    phase 2 并行规划：每 item make_item_view -> DAG cut -> 写 host-culled 表（run 前缀）
    phase 3 item 序发布裁决（重叠 run 回退;规则不变）
    phase 4 录制：HZB item 描述取 cut 结果 -> HZB cull -> per-item indirect dispatch
  raster/resolve: 不变（visibility lane 读 host-culled 表 + packed id）
  shadows/BLAS: 继续用 LOD0（draw_index_count = lod0_index_count）
```

### 5.2 item 的 per-frame run 与 reservation

- 对采用 payload 的 item：`meshlet_count = LOD0 叶簇数`（= converter LOD0 cluster count），`meshlet_base` 仍由
  `create_primitive` 单调分配（连续 run，`runtime.cpp:2749-2779`）。cut 选中 records 写入该 run 前缀；
  `apply_meshlet_run`（`visibility.cppm` 既有函数）把 run 的 base/count 写入 draw_item/push block，raster 读同一
  表（M1b 已验收机制）。
- reservation = `{meshlet_base, meshlet_count}`（现有 `prepare_item`/`check_reservation` 语义）；cut 选中数超出
  reservation → `cut_error::capacity` → 该 item `shared_run`（完整 LOD0 run），绝不截断（`subtilis.cppm:30-31,
  70-85`）。
- 为什么 reservation 用叶簇数而不是"worst-cut 上界"：converter DAG 的合法 cut 大小没有可离线证明的
  `worst_cut 求和`分解（设计 spec §3.3 明确"不能沿用旧树的 worst-cut 求和公式作为 DAG 预算证明"）；用叶簇数
  作为 reservation 并在 cut 超限时整体回退是**可证明安全**的策略，且实测 Bistro 的 LOD0 叶簇数等于
  `ceil(triangles/85)`（附录 A），与现有 meshlet 表占用一致（§6.2）。回退频率在 Task 7 测量。
- 采用 payload 的 item 的 LOD0 叶簇数组进入 engine `built_geometry`（新增只读 view），`matches_generation`
  升级为：`leaf.meshlet_count == hierarchy.lod0_cluster_count`、`leaf.meshlets` 与 `lod0_clusters` 一致、
  窗口/base_vertex/记录合法（现有 `subtilis_selection.cppm:56-82` 的检查扩展）。未采用 payload 的 item 走现有
  `build_meshlets` 路径，二者不混用。

### 5.3 误差缩放与视图

- 每个 item 的 view 由 `make_item_view` 生成（`subtilis_selection.cppm:30-55`）：`S = ||view*model||_F` 与
  `P = max(|proj[0]|*w, |proj[5]|*h)/2`（`subtilis_lod.cppm:86-98`），perspective 再除以最近 LOD 球深度；
  正交不除；近面穿越/非有限 → finest（`subtilis_lod.cppm:114-136`）。DAG cut 沿用同一套 `view_parameters` 与
  同一误差标量，只把 `group.error` 换成 DAG group error（转换器已保证单调）。
- `target_error_px` 目前恒为默认 1.0（runtime 从未设置，`subtilis_lod.cppm:24` 默认 + `make_item_view` 不写）。
  本 feature 增加配置 `[render] subtilis_error_px`（float，默认 1.0）并传入 item view；测量 gate 用它扫 1-2 个
  误差档，不改变默认行为。

### 5.4 对象级 AABB 剔除与 item 容量（先于 item 发布）

- 现有 BVH 视锥剔除（`runtime.frames.cppm:634-720`）已经先于 `record_visibility_items`，即对象级 AABB cull 在
  LOD 选择之前。本 feature **保持**该顺序，并新增：
  - item 数预检/日志：`frame_visible.size() > visibility_item_capacity` 时整帧拒绝（现有 `runtime.cpp:2077`），
    新增每场景一次的汇总计数（可见 item 峰值/上限），让运营能看出"何时该做 M2/两级表"；
  - 不新增第二套对象剔除（避免与 BVH/缓存不一致）；HZB 的 item 描述继续用 cut 后的 run（HZB 不改变对象级
    可见性判定，只cull meshlet 候选）。
- 阴影/BLAS 不受 LOD 影响：shadow caster 集合与几何 lane 用 LOD0；`draw_index_count = lod0_index_count`
  （§2.2）。对象级剔除与 LOD 选择不使用跨帧 history（迟滞默认关，见 §5.7）。

### 5.5 HZB 组合次序

HZB lane 在 phase 4 生成 per-item 描述时直接使用 `plan.run`（cut 结果），因此 **HZB 永远在 LOD 之后**
（`runtime.cpp:2271-2293`；M3 已接受）。本 feature 只改变 `plan.run` 的内容（DAG cut），不改变 HZB 的候选
语义、保守性或恢复协议；HZB 的候选数上限（65536）与 item 上限（4096）保持不变（`hzb.cppm:89-95`）。
Task 6 增加一条测试：HZB item 描述中的 `candidate_count` 与 `plan.run.count` 逐 item 一致（现有结构已保证，
用测试钉死）。

### 5.6 OFF 路径字节一致

- `[render] subtilis = false`（默认）时，帧路径与现在完全相同：`subtilis_active_frame` 为 false
  （`runtime.frames.cppm:875-877`），`record_visibility_items` 不做 cut（现有分支）。
- **导入策略**：只有 `[render] subtilis = true` 的启动才采用 payload（app 在 `load_model` 的 load-options 中传
  "提取 payload"；import 时解码/校验/上传）。OFF 启动时 loader 不提取 payload 字节、import 不创建 hierarchy、
  item run 仍由 `build_meshlets` 决定——因此 OFF 帧的 item 表/记录/绘制与不带 payload 的资产**逐字节一致**
  （这也是现有 `check_render.ps1 -Full` 冻结参考的要求）。`runtime::set_subtilis()` 在运行中打开只影响帧消费；
  若启动时未导入 payload，则无 LOD 可用（安全 no-op），文档记录该限制。Q2 让用户确认"单一开关 vs 独立 import
  开关"。
- OFF 一致性验证：`check_render.ps1 -Full`（legacy 冻结参考 `docs/reviews/2026-10-17-render-graph-gate/
  legacy-hashes.txt`，15/15）+ `check_render.ps1 -Visibility -Compare
  docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`（visibility ON 冻结参考）必须不变；本 feature 的 OFF
  资产（带 payload）与不带 payload 的原资产互相比较也应像素一致（Task 6 用 capture 验证）。

### 5.7 迟滞 / popping（默认关，可选）

- 默认 **无迟滞**（确定性 reference，与现有 M1b 一致；CPU/GPU cut 对照也用这一档）。
- 可选 `[render] subtilis_hysteresis = false`（默认 false）：`coarsen` 仍然每帧按误差算，但 group 决定在
  `error <= 0.8*target` 时允许变粗、`error > 1.0*target` 时强制变细、中间区间沿用上一帧同一 item 的决定；
  history 只在同一 `(asset generation, 稳定 item 身份, view epoch)` 内有效；camera cut/resize/资产替换/视图
  变化（projection 或 extent 改变）使 history 失效（`2026-10-18-subtilis-design.md:73,95,229`）。
  稳定 item 身份不能用每帧压紧后的下标；本设计先用 `(import_instance, primitive pointer, generation)` 复合键。
- TAA 交互：LOD 切换改变顶点位置与深度，TAA history 在轮廓上会出现 ghosting/disocclusion；本阶段测量配置
  TAA 关闭（与 Bistro 证据一致），TAA 开启时的表现作为记录项（不承诺改善）；迟滞有助减少切换频率但会引入
  stale 决定，默认关以保持确定性。

### 5.8 现有 M1b 显式 upload 路径不回归

`primitive_create_info::subtilis_geometry`（builder 产物 + `upload_geometry` + `bind_subtilis_geometry`）继续
工作；若其 forest 层级在 Task 5 后需要转换为 DAG 才能被同一 selection 消费，则在 builder 结果创建处做
forest→DAG 的平凡包装（每个 forest group 的 children 已是连续 group 区间，转换为 fine/coarse 引用等价形式），
既有测试 `test_subtilis_{builder,asset,lod,selection,frame}` 全部保留并迁移语义。

## 6. 容量与 32-bit packed visibility id 分析

### 6.1 现有硬上限（file:line）

- packed id：`valid(1) + item(12) + meshlet(12) + triangle(7)`（`visibility.cppm:43-70`）⇒ item 4096、
  每 item 可见 meshlet 4096、triangle 128（meshlet ≤85 tri，`meshlet.cppm:45`）。
- item 表：`visibility_item_capacity = id_item_capacity`（`runtime.declarations.cppm:2232`），HZB item 4096
  （`hzb.cppm:89`），HZB candidate / host-culled 表 65536（`hzb.cppm:92`、`meshlet.cppm:59`）。
- 现有 Bistro 基线（`docs/reviews/2026-10-20-bistro-performance/README.md`、`docs/subtilis.md:108-132`）：
  2909 items（4096 的 71%）、2906 visible items、最大 item 1395 个 meshlet、host-culled 表使用 50,808 条
  （77.5%）。1024 容量曾使整场景回退 legacy（178.5 ms）；4096 是本场景可用的前提。

### 6.2 采用 converter 资产后的实测容量（设计期只读探针，附录 A）

在 Bistro 2909-item 场景的派生资产上（551 个 mesh primitive、2909 次 mesh 实例、4,209,006 被实例化绘制三角形；
506 个可转换）：

| 指标 | 实测 | 上限 | 结论 |
| --- | ---: | ---: | --- |
| item 数（mesh 实例数） | 2,909 | 4,096 | 71%，本场景可用 |
| 采用 payload 的 item 的 LOD0 叶簇数（instance 加权） | 39,518 | 每 item 4,096；全局 65,536 | 每 item 最大 **1,395**；全局占 60.3% |
| fallback item 的 legacy meshlets（45 个 mesh / 166 个实例） | 11,290 | — | 与现有路径一致 |
| host-culled 表总占用（2,909 items） | **50,808（推算：39,518 + 11,290）** | 65,536 | 77.5%，与现有 M1b 基线相同 |
| payload 全部 cluster 数（instance 加权上界） | 71,552 | 65,536 | **不可能按"全部 cluster"预留**；reservation 必须用叶簇数（§5.2） |
| 单 payload 最大 cluster 数 / 最大叶簇数 | 1,605 / 1,395 | — | 叶簇 1,395 < 4,096 ✓；cut 选中数超叶簇时回退（§4.4） |
| 真实 DAG 规模（506 payload，wire 计数） | clusters 30,165 / groups 2,186 / refs 27,884 / leaves 18,469 / roots 13,977 / indices 7,402,740 u32 | — | 层级 CPU 数据量小（§6.3） |

- 关键实测巧合（对容量有利）：506 个 payload 的 LOD0 叶簇数**逐个等于 `ceil(triangles/85)`**，即 converter
  LOD0 聚类与现有 engine `build_meshlets` 的 count 相同（附录 A）；因此采用 payload 不增加全局 meshlet 表占用。
  该性质不能假设为普遍规律（vertex-limited 网格可能更多），实现必须按每个 payload 实测叶簇数并在全局预算里
  预检（§4.4 的 capacity 行）。
- LOD0 前缀可平铺：全部 506 payload 的 producer==NONE cluster 按 first_index 排序后无缝平铺
  `[0, lod0_index_count)`，且 LEAVES 集合与之相等（附录 A）⇒ 翻译不需要重建 index 缓冲（§4.3）。

### 6.3 内存与磁盘（设计期估算，Task 7 正式测量）

- 可转换 payload 磁盘总量：派生资产 GLB 128,862,912 B（含 core LOD0），payload 合计 31,445,448 B ≈ 30.0 MiB
  （506 个；wire DESCRIPTORS + INDICES 7,402,740 u32）。
- CPU 层级（去重后共享）：clusters 30,165×48B + groups 2,186×~56B + (refs+leaves+roots)=60,330×4B ≈ **1.7 MiB**
  （506 payload 去重）。
- 合并 index buffer（采用 payload 的 item）：converter INDICES 全量（LOD0 + coarse 页）比现有 LOD0-only
  index 多约 **+17.2 MiB**（instance 加权 65.4 vs 48.2 MiB；若实现做 generation 共享/多实例去重，实际增量更小）。
  vertex 数据不变（loader 已生成 64B interleaved；现有路径本来就上传）。
- 以上是设计期估算/只读探针结果，不是发布基准；Task 7 用正式 capture/计时矩阵复测并记录（§8.6）。

### 6.4 32-bit id 是否够用（本阶段结论）

- 本阶段对象 LOD 只改变 **item 的 run 内容**，item 身份仍是 12-bit（2909/4096 本场景）；per-item selected
  meshlet 数上限仍受 run reservation（叶簇数，本场景最大 1,395 < 4096）；triangle 7-bit 对 ≤85 tri meshlet 足够。
  因此**本阶段不需要改 id 宽度**；M4 的 64-bit/两级表/storage depth twin 决策在对象 LOD 落地后与 M2 一起用
  测量决定（§7.4），不在本阶段预留复杂度（与 `2026-10-18-subtilis-design.md:37,97-104,151,172` 记录的
  id 路线冲突一并交 M2/M4 联合决策，本阶段保持 32-bit 冻结）。

## 7. 性能预算、M2 触发与 M4 hand-off

### 7.1 已测基线（显式 upload 路径；引用）

| 指标 | 值 | 来源 |
| --- | ---: | --- |
| Bistro 场景 | 4,208,625 triangles / 2,909 items / 6,006 nodes | `docs/subtilis.md:108-112`；`docs/reviews/2026-10-20-bistro-performance/README.md` |
| 显式 upload OFF→ON GPU 帧 | 26.98→24.63 ms（far，−8.7%）；24.03→21.95 ms（near，−8.7%） | `docs/subtilis.md:118-120` |
| 选中三角形 | 4,208,625→3,727,209（−11.4% far）；3,626,626→3,175,714（−12.4% near） | 同上 `:116-117` |
| CPU cut（serial → parallel） | +8.68 ms → post phase 约 2 ms（3 µs/item） | `docs/subtilis.md:121-128` |
| 容量 1024→4096 | legacy 整帧回退 178.5 ms → meshlet lane 26.7 ms（约 6.6×） | `docs/reviews/2026-10-20-bistro-performance/README.md` |
| HZB（M3，cut 之后） | far 71.51% / near 92.83% 候选被完全遮挡；GPU 帧 −61.6%/−78.6% | `docs/subtilis.md:134-137` |

本 feature 的收益/成本**尚未测量**（本文件不宣称）：导入成本、DAG cut 每帧成本、采用 payload 后的三角形减少、
ON/OFF 图像差异、内存增量都归 Task 7 的测量矩阵（3 轮中位数、同相机 A/B、独立报告）。

### 7.2 本 feature 的成本结构（预算口径）

- **一次性导入**：wire 解码 + sourceHash 重算 + DAG 校验 + 翻译 + GPU 上传（每个唯一 payload 一次，多实例共享）；
  Task 7 以"OFF 启动（不采用）→ ON 启动（采用）"的加载墙钟差报告；不设未经验证的硬阈值，若超过 ~2 s 量级需在
  Task 5/7 内做去重/延迟解析优化（目标值，不是承诺）。
- **每帧**：DAG cut 的 CPU 成本 = M1b forest cut 的同阶项 + 每个 group 一次的 coarsen 计算/闭包修正；以 Task 7
  实测报告（对比 M1b 的 ~2 ms 并行基线）。
- **内存**：§6.3；一次性常驻。
- **收益**：选中三角形/meshlet、GPU 帧、wall 帧的 OFF/ON 对比；图像差异量化（changed pixels/均值，不宣称一致）。

### 7.3 M2（GPU select/finalize）触发条件（可测量，写进 Task 7 报告）

用同一 Bistro 参考场景与相机矩阵，满足以下**任一**即触发 M2 设计与 ROI 评估（不是立即实现）：

1. **CPU cut 不再被 GPU 空档吸收**：GPU 帧时间中位数 < 15 ms 且 CPU cut 中位数 ≥ 2.5 ms/帧（或 CPU cut ≥
   实测 CPU active frame 的 25%）；依据是 `docs/reviews/2026-10-20-bistro-performance/README.md` 的结论
   "一旦 GPU 帧时间降到 ~15 ms 以下或 item 数继续增加，host 选层会成为新瓶颈"。
2. **item 容量接近极限**：visible item 峰值 ≥ 3,200（4096 的 78%）或目标场景 item > 4,096（现在会整帧回退
   legacy，代价约 6.6×）；或 item 表/HZB item 表 4096 需要扩展。
3. **per-item run / 全局表压力**：出现持续性的 cut 容量回退（selected > 叶簇数）或全局 host-culled 表占用
   ≥ 65,536（需要更大表/两级表）；或 M5 资产尺寸使单场景 demand 超过预算。
4. **每帧 host lane 写入量**：item 112B×4,096 + meshlet 48B×65,536 上限 ≈ 3.6 MB/帧（当前 Bistro 约
   0.33 MB item（2,909×112B）+ 2.4 MB meshlet（50,808×48B），远低于；若真实场景接近上限，M2 的 GPU-side
   finalize 才值得）。
5. M2 保留 CPU reference：M2 之后必须保留 host cut 作为一致性 reference 开关（`2026-10-18-subtilis-design.md:141`
   已规定 GPU/CPU cut 集合一致测试）；item 排序/发布/fallback/admission/导入校验仍留在 CPU。

### 7.4 M4 hand-off（本阶段只定义接口与重探针，不设计 M4）

对象 LOD 会改变远工作负载构成（粗 cluster 更小/更少、subpixel cluster 比例变化），所以 M4 的收益前提必须在
**对象 LOD 落地后重探**：

1. 重跑 M1b 的 cluster dump + depth probe + 离线分析（`scripts/measure/cluster_occlusion_probe.py`；方法见
   `docs/reviews/2026-10-20-bistro-performance/README.md` 的"三个探针"节）：far/near 的 cluster 投影直径
   <1px/<2px 占比、cut 选中的 cluster/triangle 数、HZB 候选数与剔除率；基线参考 M1b 探针（far <1px 22.05%、
   <2px 38.57%；near <1px 3.09%）。
2. 用 ON/OFF 同相机 capture 与 GPU/CPU 计时，确认 M4 的"subpixel cluster 占比"与收益区间，再决定是否进入 M4。
3. **联合决策**：32-bit id 继续 vs 64-bit vis buffer vs 两级 item/meshlet 表 vs storage depth twin。判据由
   重探针 + M2 的 selection 输出身份模型给出：若 M2 改为 GPU 产出每 item 的 cluster/group 决策，id 的 meshlet
   字段语义可能从"run 内顺序"变为"稳定 cluster 身份"，与 M4 的 subpixel 路径共用一张表；届时
   `2026-10-17-visibility-buffer.md:170` 预留的"v3 换 id 编码"冲突必须由同一次决策合并解决，不允许 M2/M4
   各自改一次 id。本阶段不改 resolve 数学/id 布局（§6.4）。

## 8. 测试与验收

### 8.1 CPU-only 单元测试（新 target，进 root CTest）

| target | 覆盖 |
| --- | --- |
| `test_subtilis_dag_asset`（新） | DAG 层级建设期校验：拓扑序、producer/consumer 唯一、LEAVES/ROOTS 覆盖、LOD0 叶簇平铺前缀、group 引用范围、球/误差有限与嵌套、capacity；合法 cross-DAG fixture 接受；非法输入全部 typed reject |
| `test_subtilis_dag_lod`（新） | DAG cut reference：threshold 0/相等（`<=`）/nextafter、闭合修正、selected 集合公式、perspective/ortho/非均匀缩放/近面/非有限视图、容量不足返回错误而非前缀、确定性；与转换器 CPU reference `select_cut` 在 fixture 上逐 cluster ID 集合一致（Task 2） |
| `test_gltf_loader`（扩展） | payload 提取：extension 字段/ bufferView 边界/外置 buffer+GLB/坏 JSON/未知扩展不影响 core；`get_subtilis()` 视图生命周期 |
| `test_subtilis_import`（新） | 端到端 import（无 GPU）：payload decode→校验→翻译→`built_geometry`（LOD0 前缀/叶簇/身份）；sourceHash mismatch/容量/坏 payload 的 fallback 计数；多实例共享 generation；OFF 启动不采用 |
| `test_subtilis_selection`/`test_subtilis_frame`（扩展） | item run=叶簇数、cut 写表/发布、容量回退、HZB 描述与 run 一致、shadows/BLAS 只见 LOD0、OFF 帧与无 payload 等价 |
| `test_subtilis_asset`/`test_subtilis_lod`/`test_subtilis_builder`/`test_subtilis_upload`（迁移/保留） | forest 路径不回归（M1b 显式 upload）；forest→DAG 包装一致 |

测试 fixture 来源：`source/asset/subtilis` 的 cross-DAG/UV-seam 夹具与转换器 CLI 生成的**小 payload 文件**
（如 `source/tests/fixtures/subtilis/{cross_dag,seam,lod0_only}.bin`，SHA-256 记录在 fixture 清单；生成脚本
放 `scripts/measure/`，不联网、不依赖 Bistro）。真实资产只用于 Task 7 的 GPU 测量与可选的 huge-fixture 测试，
不进 root CTest（保持 CPU/无 Vulkan 可跑）。

### 8.2 ASan/UBSan 与 CI

- 新 CPU target 加进 root `VR_TEST_TARGETS`（`CMakeLists.txt:1253`）与 `.github/workflows/ci.yml` 的镜像列表
  （`:335-360`，`test_docs` 会核对两者相等）；ASan/UBSan Debug 构建（`ci.yml:379`）自动覆盖。
- Windows 无 LeakSanitizer（既有限制，`ci.yml`/converter workflow 均有记录）；leak 覆盖仍等 Linux CI。
- 新测试不使用 Vulkan/设备：`test_subtilis_import` 走纯 CPU 模块；需要 RHI 结构的测试沿用
  `test_subtilis_selection.cpp` 的 `vk_test.h` + `probe_backend.hpp` headless 模式。

### 8.3 root 回归规则

- 每个 Task 完成：本 target 的 `ctest -R` 单测绿 + 相关 subtilis/gltf 测试绿；改 root CMake/引擎模块的 Task
  追加 root Release 构建 + 全量 CTest（数量以实测为准）；改 `gltf_loader`/import 的 Task 追加
  `test_gltf_loader`、`test_runtime_injection`、`test_subtilis_frame`、`test_docs`。
- 合并前最终门禁：root Release `cmake --build` + `ctest` 全绿；ASan/UBSan 树全绿；converter 独立工程
  `ctest --test-dir build-subtilis` 仍全绿（同一份 `source/asset/subtilis` 源文件被两边编译，防止 root 侧
  编译选项/模块污染改变其行为）；`tools/subtilis` 的 CLI 快速回归（build/validate/inspect）。
- 不推送（本地提交），重证据归档 D:，5h 剩余 ≤5% 时记录并暂停（沿用 converter 计划的工作约定）。

### 8.4 OFF/ON 门禁

- OFF（默认）：`check_render.ps1 -Full`（legacy 冻结参考 `docs/reviews/2026-10-17-render-graph-gate/
  legacy-hashes.txt`）与 `check_render.ps1 -Visibility -Compare
  docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt` 必须不变；带 payload 资产在 OFF 启动时与去掉 payload
  的同一资产像素一致（Task 6 用 capture 验证 + 一次字节/哈希核对）。
- ON：同相机 A/B（capture.ps1，validation 开/关各一组，130 帧/次，3 轮交错），记录 changed pixels/均值/最大差
  与 GPU/CPU 计时；不宣称像素一致（LOD 本就有边缘差异，M1b 记录 0.94%/0.10% changed pixels）。
- ON 另跑 validation layers 与 `check_rt.ps1`（若 RT 路径开启）确保零结构性错误。

### 8.5 真实场景测量（Task 7）

- 场景：`D:/deren-assets/bistro/bistro-geometry.gltf`（551 mesh / 6006 nodes / 2909 实例 / 254 materials /
  0 textures / 1,753,630 unique triangles，设计期实测；与 M1b 证据同源资产）。由于 plan 指定资产对转换器
  profile 不可转换（551/551 primitives 带 TANGENT，转换器 exit 4），本 feature 使用**派生测量资产**：
  去掉每个 primitive 的 `TANGENT` attribute（不改 bufferView/accessor/材质/DOM 其他内容）→ 转换器
  `build --threads 12`（默认参数）→ 得到带 `DEREN_subtilis` 的 glTF/GLB。设计期探针已确认派生资产可转换
  （506 converted / 45 skipped / 158 LOD0-only，输出 128,862,912 B）且 506/506 payload LOD0 平铺前缀
  （附录 A）；正式测量在 Task 7 用脚本重建并记录派生资产 SHA-256、转换器二进制的 SHA-256、参数。
  资产本身渲染在 OFF 下应与原资产一致（loader 不读 TANGENT；`gltf_loader.cpp:864-873` 的 interleaved 布局
  没有 TANGENT；Task 7 先用 OFF 截图对比原资产确认，差异即失败）。
- 相机/配置沿用 M1b 证据：1280×720、vsync off、max_fps=0、shadow/TAA/SSAO/bloom off、deferred lit、
  `[render] visibility_buffer=true, subtilis=true, subtilis_error_px=1.0`；far/near 两个相机、130 帧/次、
  3 轮交错、validation 开/关各一组。
- 指标：导入墙钟差（不加/加 payload）、CPU cut（微秒/item 与每帧总计，含 fallback 计数）、选中三角形/meshlet、
  GPU 帧/端到端 wall、host-culled 表占用、item 峰值、内存峰值、ON/OFF 图像差异、validation 零发现；
  3 轮中位数与逐轮表（不保留最好一轮）。
- 结果写 `docs/reviews/2026-10-21-object-lod.md` + `docs/reviews/2026-10-21-object-lod/`（JSON/日志/截图），
  重产物归档 `D:/deren-assets/2026-10-21-object-lod/` 并逐文件 SHA-256 核对；原始 payload/资产不进仓库。

### 8.6 审查

实施完成后按 converter 惯例：**一次 fresh 只读 whole-change review**（5 个 Review Focus，见计划文件），
Critical/Important 修复须有复现与 GREEN，Minor 记账；不启动任务级 agents、不重复审查流程；由 Lead 决定接受。

## 9. 风险与开放问题（用户必须决定；带推荐）

### 9.1 风险

| 风险 | 现状/对策 |
| --- | --- |
| popping/TAA 轮廓差异 | 对象 LOD 切换会造成边缘差异（M1b 已有 0.94%/0.10% changed pixels）；默认无迟滞保持确定性；可选迟滞只降低频率，不消除；TAA 开启的表现单独记录，不承诺 |
| generation 生命周期/dangling | payload→`built_geometry`→`uploaded_geometry` 全部 shared_ptr 租约（现有 `matches_generation` 双 owner 校验，`subtilis_selection.cppm:56-82`）；多实例共享同一 generation，item destroy 不回收共享源；新增测试覆盖 |
| 全驻留层级内存 | 本场景 CPU 层级约 1.7 MiB（去重）、index 增量约 17.2 MiB（估算，Task 7 复测）；不做 streaming（M5）；大场景容量预检见 §4.4/§6 |
| DAG→forest 投影损失（若用户否决 Q1 的方案 C） | 方案 A 无法无损（§2.3）；方案 B 需转换器重做构建约束并重跑全部真实资产证据。两者的代价/周期已在 §3.1 列出；若选 A/B，本 spec 的 §3-§6 需要重写，不得按本计划实施 |
| LOD0 叶簇划分与现有 meshlet split 不一致 | 采用 payload 的 item 使用 converter 叶簇（count 实测等于 ceil(tri/85)）；不采用 payload 的 item 保持 `build_meshlets`；两套不混用；`matches_generation` 明确区分 |
| item/表容量增长（未来场景 >4096 items / 表 >65536） | 确定性回退（整帧 legacy 或按场景顺序不采用 payload）+ 汇总日志；M2 触发条件 §7.3；不在本阶段扩表/改 id |
| 链接方向引入重复 wire parser / 版本漂移 | 复用同一源文件（root 与工具各自编译），root 测试与工具 CTest 都锁 codec/validator/cut 行为；payload version/profile 严格匹配，未知版本回退不解析 |
| 测量有效性（派生资产 vs 原资产） | 派生只删 TANGENT（loader 不读），先用 OFF 截图证明同像；LOD 收益在派生资产上测，报告明确口径；不把 M1b 显式 upload 数字当本 feature 数字 |
| Windows 无 LSan / 单机非发布基准 | 沿用既有记录；ASan/UBSan 覆盖 use-after-free/overflow；测量标注单机、不构成发布基准 |
| 现有 forest 路径迁移回归 | M1b 显式 upload 的 builder/upload/selection/frame 测试全部保留；forest→DAG 包装有等价性测试；root 全量 CTest 门禁 |
| 迟滞状态与稳定身份 | 默认关；开启时 history key 用 `(import_instance, primitive, generation)` 复合键 + view epoch，camera cut/resize/资产替换失效；禁用迟滞时 cut 必须与 reference 完全一致（测试） |

### 9.2 开放问题（用户决定；推荐已给出）

**Q1（阻塞计划批准）seam 方案**：是否接受把 runtime 层级从唯一父 forest 升级为转换器的参考 DAG 语义
（方案 C，§3），而不是离线 pre-flatten（方案 B）或 import 侧投影（方案 A）？
**推荐 C**：忠实消费已接受资产、单一格式、跨组分区收益（Bistro 上 35.4% 绘制三角形所在网格）不被丢弃、
M2/M4 只有一套 cut 语义；代价是 engine subtilis 资产/选择模块的迁移和一次更长的 review，已在计划 Task 1-6
按顺序隔离。若用户选 B，计划需要重写为"转换器 forest 构建模式 + 新 payload 版本 + 现有 forest runtime 适配"，
周期与证据重做成本更高，且本 spec 的 §3-§6 大部分需要重做。

**Q2 导入开关与 OFF 语义**：建议只用现有 `[render] subtilis` 同时控制"启动时采用 payload"和"帧内 cut"（OFF 启动
完全不解析/不拷贝 payload，OFF 帧字节一致最简单）；不新增 `subtilis_import` 开关。若用户希望"OFF 帧也持有
hierarchy"（便于运行中打开），需要单独开关并接受 OFF 启动的额外内存/导入时间。

**Q3 默认质量参数**：建议 `subtilis_error_px` 默认 1.0（与现有 reference 一致）、迟滞默认关闭
（`subtilis_hysteresis=false`）；测量 gate 用 1.0 与 2.0 两档（可选）报告收益-质量曲线。若用户另有默认质量要求，
在 Task 6 前定。

**Q4 测量资产**：plan 指定的 `bistro-geometry.gltf` 对转换器 profile 不可转换（551/551 带 TANGENT，exit 4）。
建议采用"去掉 TANGENT 的派生资产 + 脚本/哈希记录"作为测量基准（§8.5），而不是本阶段扩宽转换器 eligibility
（那会把 converter 的"未建模 TANGENT 约束"承诺打破，需单独设计）。若用户要求直接测原资产，则本 feature 无法
在 Bistro 上取得可转换资产，只能换场景或先做转换器 TANGENT 支持（更大改动）。

**Q5 校验分层/信任边界**：建议生产 import 只做 wire 结构 + sourceHash + DAG/payload 容量校验，完整几何
boundary 校验留在离线转换器（已全验证）与 root CTest fixture（§3.4）；运行时不默认跑 `make_asset` 全验证。
若用户要求运行时可验证，需要把全验证做成可选 `--verify-import` 并从性能预算里扣除。

**Q6 容量回退策略**：建议超出全局 host-culled 表预算时按**场景顺序**确定性采用 payload，剩余 primitive 走
LOD0 legacy（每场景日志一次 + 报告计数），而不是整场景拒绝或静默截断；单 item 超 4096 叶簇时该 item 不采用
payload。M2 触发指标（§7.3）在 Task 7 报告中作为决策输入，不预先承诺 M2 实施。

**评审结论（2026-10-21）**：用户确认 **Q1 = 方案 C**（runtime 层级升级为 converter reference DAG）、
**Q2 = 单一 `subtilis` 开关同时控制导入与 cut**（OFF 启动不解析/不拷贝 payload）、**Q4 = 接受 TANGENT-stripped
派生 Bistro 测量资产**（OFF 截图必须与原资产一致后才测收益）；Q3/Q5/Q6 按上文推荐默认执行。用户同时决定
**先只提交 spec/plan 文档、暂不开工**；后续会话按实施计划 Task 1 起逐任务执行（每任务独立验收、本地提交、不 push）。

## 10. 文件清单、里程碑与文档更新

### 10.1 新增/修改文件（实施计划给出逐 Task 细节）

- 新 engine 模块：`source/engine/geometry/subtilis_dag_asset.cppm`、`subtilis_dag_lod.cppm`（+ 实现 .cpp 如需）；
  修改 `subtilis_selection.cppm`/`subtilis_upload.cppm`/`subtilis.cppm`（generation/选择接线，尽量小改）。
- root CMake：新 STATIC target（`source/asset/subtilis/{types,codec,digest,dag,cut}` + 链接
  `yyjson_vendored`/`vstd_lib`/`shared_utility`）；`deren_engine` 增量 sources/PRIVATE 链接；新测试 target 加入
  `VR_TEST_TARGETS`；`.github/workflows/ci.yml` 镜像列表同步。
- `source/gltf_loader/`：payload 提取（JSON 限定解析 + bufferView 字节拷贝 + `get_subtilis()` 视图）；
  `deren::gltf::primitive` 新字段；`source/gltf_loader/CMake`/root deren_assets 链接 `yyjson_vendored`（若 loader
  用 yyjson 解析）。
- `source/engine/runtime/`：`import_scene` 可选 detection + `import_payload` 调用 + `scene_import_result` 计数；
  `create_primitive` 的 adopted 路径（LOD0 叶簇 run、generation 共享、`matches_generation` v2）；config
  （`application_configuration` + `app/main.cpp` 接线 `set_subtilis_error_px`/迟滞开关）；可选 API `set_*`。
- 测试：新 `source/tests/test_subtilis_dag_asset.cpp`、`test_subtilis_dag_lod.cpp`、`test_subtilis_import.cpp`；
  扩展 `test_gltf_loader.cpp`、`test_subtilis_selection.cpp`、`test_subtilis_frame.cpp`；fixture 目录。
- 脚本/证据：`scripts/measure/prepare_object_lod_scene.py`（派生资产+转换器调用+哈希）、测量/分析扩展；
  `docs/reviews/2026-10-21-object-lod.md` + 结果目录；证据归档脚本沿用 converter 的归档/核对流程。
- 文档：`docs/subtilis.md`（状态头、§2/§5/§8/§10 更新，新增本 spec/plan/验收索引）、
  `docs/extensions/DEREN_subtilis/README.md`（追加"runtime consumer"节，说明导入规则/回退/容量，不改 wire 约定）、
  `docs/visibility_buffer.md` 的 id 布局文字与 `visibility.cppm:43-70` 对齐的陈旧描述（既有不一致，顺手修正）、
  `README` 特性列表（若项目惯例需要）。

### 10.2 里程碑状态

- 命名：把本 feature 记为 **M1c - converter ingest + object LOD**（M1a LOD0 bridge、M1b 显式 upload 之后，
  M2 之前；与 `docs/subtilis.md:221-232` 的里程碑序列一致）。M2/M4/M5 状态不变。
- 完成定义：§1.3 全部满足 + 证据报告 + 一次 fresh whole-change review（Review Focus 见计划）+ Lead 接受。

## 附录 A：设计期只读探针（可复现；实现阶段 Task 7 正式重测）

本设计在写文档前用只读探针验证了"可转换/容量/前缀平铺"三个关键假设（不修改仓库、不改代码，输出在仓库外的
scratch 或仓库内 build-* 目录）：

1. **Bistro 资产结构**（`D:/deren-assets/bistro/bistro-geometry.gltf`，Python JSON 统计）：551 meshes /
   551 primitives / 6,006 nodes / 2,909 nodes-with-mesh（= runtime item 数）/ 254 materials（231 OPAQUE、
   20 MASK、3 BLEND）/ 0 textures / 1,753,630 unique triangles；全部 primitive mode=TRIANGLES、全部带
   `TANGENT/NORMAL/POSITION/TEXCOORD_0`、有 indices、无 morph/skin。因此转换器 exit 4（无 eligible target），
   与 converter 证据 `results/specified-input.json` 一致。
2. **派生测量资产**：在 `D:/deren-assets/2026-10-21-object-lod-probe/`（scratch）生成
   `bistro-object-lod.gltf` = 原 JSON 去掉每个 primitive 的 `TANGENT`（其余不变），`buffers[].uri` 改为
   `../bistro/bistro.bin` / `../bistro/bistro-anim.bin`；用已接受的 `build-subtilis/deren-subtilis.exe build`
   （默认参数、threads=12）转换：**converted=506 / skipped=45 / lod0Only=158**，输出 GLB 128,862,912 B，
   SHA-256 `620f424239909d023b30dd86fe953c579298a39ef2d4706bca59fc0d2982085b`（临时值，Task 7 重建并归档）。
3. **payload 结构/容量**（解析上述 GLB 的 506 个 payload header/section counts + 场景 node 实例计数）：
   506 payload / 1,548,666 unique triangles；wire 总计 clusters 30,165 / groups 2,186 / refs 27,884 /
   leaves 18,469 / roots 13,977 / indices 7,402,740 u32 / payload bytes 31,445,448（≈30.0 MiB）；instance 加权
   LOD0 叶簇 39,518、cluster 总量 71,552；单 payload 最大叶簇 1,395 / 最大 cluster 1,605；506/506 payload 的
   LOD0 叶簇数等于 `ceil(triangles/85)`；506/506 payload 的 producer==NONE cluster 按 first_index 平铺
   `[0, lod0_index_count)` 且 LEAVES 集合相等；与相机无关的规模统计：77/506 payload 有跨组重新分区
   （201 instances，实例加权 1,491,442 triangles = 35.4%，报告根层相对首层的最大可达减面 40.1% vs 其余
   22.3%）。
4. **Bistro 实例化结构**：2,909 个 node 引用 551 个 mesh（一个 mesh 最多 84 个实例）⇒ 多实例共享 generation 的
   收益/必要性（§4.3、§6.3）。
5. **现有 M1b/容量数字**直接从 `docs/subtilis.md` 与 `docs/reviews/2026-10-20-bistro-performance/README.md`
   引用（这些是已接受证据，不是本设计的探针）。

探针命令（Task 7 会做成正式脚本）：读取 glTF JSON 统计 → 生成派生 JSON → `deren-subtilis build ... --report` →
解析 GLB JSON chunk/bufferView 的 payload header/descriptors + node 实例计数。上述数字只用于设计容量判断，
正式性能/图像证据以 Task 7 为准。

## 附录 B：术语与引用索引

- **DAG / cross-group repartition**：converter 的 group 依赖（多 consumer），见 §2.1、`2026-10-21-subtilis-gltf-design.md` §3.1。
- **leaf / LOD0 cluster**：producer==NONE 的 cluster；`LEAVES` section；每 item 的 LOD0 叶簇数 = run reservation。
- **cut / coarsen**：converter 的 CPU reference（闭包 + cluster 公式 + `<=` 阈值），§2.1、§5。
- **generation**：`uploaded_geometry` 的不可变 CPU/GPU 代际；多 primitive 共享（`subtilis_upload.cppm:16-53`）。
- **item / run / reservation**：`record_visibility_items` 的 per-frame 计划单元；`reservation{base,count}` 与
  `visible_run` 语义见 `subtilis.cppm:16-31,70-108`。
- **OFF/ON**：`[render] subtilis` 开关；OFF 必须与冻结参考字节一致。
- 相关文档：[docs/subtilis.md](../../subtilis.md)、[converter 设计](2026-10-21-subtilis-gltf-design.md)、
  [converter 计划](../plans/2026-10-21-subtilis-gltf.md)、[converter 证据](../../reviews/2026-10-21-subtilis-converter.md)、
  [Bistro 性能证据](../../reviews/2026-10-20-bistro-performance/README.md)、[HZB 设计](2026-10-20-hzb-occlusion-design.md)、
  [主 Subtilis 设计](2026-10-18-subtilis-design.md)、[visibility buffer 计划](../plans/2026-10-17-visibility-buffer.md)。


