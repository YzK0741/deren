# Subtilis（细织几何）设计：轻量层级 LOD 的初版与扩展

> **状态：设计草案（2026-10-18）。已做仓库适配性审查（同日），修正见 §10 末的审查结论；剩余 owner 决定项见 §3 与 §9。**
> 本文是实现设计，不是验收记录；文中标"现状"的事实都以当前代码为准，标"计划"的是尚未实现的接口。
> 命名：**Subtilis**——拉丁语 `subtilis`（"细织的、精微的"，`sub + tela`＝织机），格言 **subtilitas in minimo**（至小之中，见其精微）。
> 代码中不出现 "Nanite"（商标）；需要类比时在文档里写"Nanite 风格的虚拟几何"。

## 1. 定位与边界

**Subtilis = 静态不透明网格的层级 cluster LOD 系统**：资产离线构建 cluster 层级与保守误差；运行时按屏幕误差为每个 draw item 选一条 group cut，把选中的 cluster 写成可见列表；现有 visibility buffer 光栅/解析路径消费它。

**首版（M1）范围**：静态、不透明、全驻留、硬件光栅，复用现有 G-buffer/visibility resolve 与 PBR。
**非目标（后续里程碑）**：M2 GPU 选择与剔除、M3 Hi-Z 遮挡、M4 软件光栅（subpixel）、M5 分页流送；动态蒙皮/实例/透明/Gaussian 保持现有路径；不承诺 Nanite 的完整能力。

## 2. 现状基线（设计所依赖的、已实现且已验证的事实）

| 事实 | 证据（当前代码） |
| --- | --- |
| 32-bit vis id：valid(1) + item(10) + meshlet(14) + triangle(7)，占满 32 位 | `source/engine/visibility/visibility.cppm:42-61` |
| cluster/meshlet 记录 **48 字节**：`first_index/index_count/base_vertex/_pad/center/radius/axis/cone_cos`，带 16 字节对齐的 center/axis | `source/engine/meshlet/meshlet.cppm:70-115`；`source/shaders/geometry/mesh_geometry.slang:119-129`（shader struct 的 std430 stride 同为 48） |
| **host 侧逐 meshlet 剔除已存在，但只在 pbr meshlet lane 上闭合**：`meshlet_culled_write` 把 survivors 压缩写入 per-frame 表；`geometry.base_vertex` bit0 让 shader 读该表（bit1 = 允许背面剔除） | `source/engine/primitive/primitive.cpp:72-119`；`render_environment.cppm:140`；`runtime.cpp:1705-1724`；bit0 的消费端 `source/shaders/scene/pbr.slang:427-433`（visibility raster **没有**这个分支，见 §5 任务 1） |
| 每帧 culled meshlet 表的容量 65536 记录（= `meshlet_capacity`），按 `frame_slot * 65536 + index` 排布；index 里的 item 基址就是导入期的 `meshlet_base` | `source/shaders/geometry/mesh_geometry.slang:145-153`；`render_layout.cppm:203-214`；`meshlet.cppm:59`；`runtime.cpp:1705-1724`、`2089-2091` |
| visibility raster lane：per-item indirect mesh dispatch，shader 从 meshlet 记录取窗口、写 id + 共享 G-buffer depth，并在同一 fragment 里做完 alphaMode MASK 的 `discard`（深度写入之前） | `source/engine/pass/visibility_raster.cppm:87`（`visibility_raster_pass`）；`source/shaders/scene/visibility.slang:90-173` |
| visibility resolve lane：id → item → meshlet 表分支（`item.meshlet_table != 0` 时读 per-frame 表）→ 重建五个 G-buffer 目标；已完成近平面裁剪、材质 mip 对齐；MASK 判定不在这一 lane（它在 raster 的 fragment） | `source/shaders/scene/visibility_resolve.slang:252-262`（表分支）、`:301` 起（近平面裁剪）、`:68`（`visibility_texture_lod`）；`visibility.cppm` 的 id/draw_item 表 |
| 准入：deforming / instanced / custom pipeline / 无 meshlet 等整帧回退；MASK 已不是准入条款；toon 深度-EQUAL 回退已由根因修复移除（枚举值保留、条款退役） | `source/engine/visibility/visibility.cppm:491-512`（`admit_leaf`）、`:504-506`（MASK 不再是条款）；`docs/reviews/2026-10-18-visibility-buffer-ab.md` 的 "Item 1 ROOT CAUSE FOUND AND FIXED" |
| 门禁：OFF legacy 15/15 冻结参考；ON 参考 `docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`（`-Full` 不含 visibility tier，`-Visibility -Compare` 判它）；`-Visibility` tier 与 `-ExtraConfig` | `scripts/windows/check_render.ps1:88-111`、`:172-179`、`:529-530` |
| 测量：lane 级 GPU 计时；`visibility_ab.py`；3 次重复仍方差过大，性能结论需 ≥5 次或 `DEREN_GAUSSIAN_BENCHMARK` CSV | `docs/reviews/2026-10-18-visibility-buffer-ab.md` |

**关键结论（决定了 M1 的做法）**：这条"选中记录按 per-frame 基址写入表 → lanes bit0 → shader 读表 → resolve 用同一基址反查"的通路，在 **pbr meshlet lane** 上四处都闭合；在 **visibility lane** 上只闭合了两端——写入端（`meshlet_culled_write`）与 resolve 端（`culled_meshlet_at` 分支）都在，但中间两处缺失：`visibility.slang:99` 的 mesh entry 只调 `meshlet_at()`，没有 bit0 分支；`runtime::record_visibility_items` 自己拼 push block（`runtime.cpp:1650-1665`），把 `groups_x` 写成整条 run（`1669`）、`base_vertex` 写 0（`1661`），且 `make_draw_item` 固定 `meshlet_table = table`（`visibility.cppm:246`）——所以 resolve 的 culled 分支至今没有生产者。因此 **M1 不是零改动**：resolve 的数学与 `item.meshlet_table` 分支不用动，但 visibility shader 要加 bit0 分支、host 要补四处接线（§5 的 M1 任务 1-4）。

## 3. 与既有计划的关系

- 总计划：`docs/superpowers/plans/2026-10-14-render-extensibility-cluster-geometry.md` 的 **阶段三（GPU 驱动可见几何）与阶段四 4A（全驻留层级 LOD）** 就是本系统。该计划明确："阶段三验收后冻结 4A 协议"、"先使用现有 48-byte meshlet 数据，只改可见性生产者，不要同时换资产布局"。
- **现状差异**：阶段三尚未实现（3.1-3.7、4A.1-4A.7、4B.1-4B.4 的复选框全为未完成，阶段二的 11 项已完成；当前 GPU cull 是 mesh shader 内逐 meshlet 剔除 + host 剔除，不是 GPU 可见列表）；4A 的"GPU 层级遍历"因此没有前置。**Subtilis M1 按计划的最小起步原则，先做 host 选择 + 现有可见表通路**，把 GPU 化留给 M2（等价于计划 3.2/3.3 的能力，但以 LOD cut 为第一目标先交付）。
- visibility buffer v0（`docs/superpowers/plans/2026-10-17-visibility-buffer.md`）是 M1 的消费端与测量床；Subtilis 不修改其 resolve 数学。
- **路线顺序与 id 位宽的冲突（需 owner 决定并回写计划）**：同一份 visibility buffer 计划把后续顺序写成 "v1 software raster → v2 material binning → v3 cluster 层级 + screen-error LOD → v4 分页/流送"，并明说 "item 位宽与 meshlet 位宽在 v3 会被 cluster group 取代，届时升级到 64 位 vis buffer 或分两级表是预期变化"（该文件第 170 行；`docs/reviews/2026-10-18-visibility-buffer-ab.md` 的 "Follow-ups" 同义）。本设计把 cluster LOD 提到软光栅之前（M1 就是它），并在 M1 冻结现有 32-bit id（§4.2、§9），软光栅推到 M4。两处不是同一个顺序、也不是同一个 id 结论，M1 冻结前必须在计划里显式记录这次重排，否则验收时两份文档会各说各话。
- 本设计与计划的文件边界对应关系：计划 4A 的 `cluster_asset/cluster_builder/cluster_lod/cluster_select.slang/test_cluster_lod` 在实现时以 **Subtilis 命名**落地（见 §6），文档在这一层注明映射，验收时同步更新计划复选框与命名。

## 4. 数据模型（M1 冻结，M2 起只增不改）

### 4.1 资产层级（离线构建）

```
SubtilisAsset (versioned header: magic/version/endian/hash/builder_version/unit)
  groups[]   : GroupRecord           // 选择单位；group 内 cluster 共享 LOD
  clusters[] : MeshletRecord(48B)    // 复用现有记录；first_index/index_count/base_vertex 指向本 LOD 页窗口
  vertex[]   : MeshVertex(64B)       // 每 item 一条拼接缓冲（含全部 LOD 页）
  index[]    : uint16/uint32         // 同上，按页拼接；一个 item 的全部 LOD 页必须同一 index 宽度
```

`GroupRecord`（草案：字段 44 字节；作为 std430 `StructuredBuffer` 元素时 `float3` 的 16 字节对齐把 stride 补到 48 字节）：
```
float3 center; float radius;        // asset space，保守包围球
float error;                        // asset-space 保守半径误差（非负、父 >= 子累计）
uint   cluster_base; uint cluster_count;
uint   child_base;  uint child_count;  // 子 group 区间（0 表示叶）
uint   level;       uint flags;        // flags：根/叶/材质边界锁定等
```
- **唯一 parent 的 replacement group 树**（按计划 4A：先避开 UE 完整 DAG）；组内所有 cluster 共享同一次选择决定；父级误差非减且保守累计。
- 叶 cluster = 现有 48B meshlet 布局的扩展用法；**M1 不改记录宽度**（计划：不要同时换资产布局）。
- 混合 LOD 页的边界：per item 只有一对缓冲地址与一个 `index_width`（`draw_item` 与 geometry lanes），页间差别只能落在记录的 `first_index`/`base_vertex` 上——所有 LOD 页必须拼进同一对缓冲、同一 index 宽度（见 §5 任务 4）。
- 初版全驻留、无压缩、无量化（计划 4A：压缩/量化单独验收）。

### 4.2 可见列表（每帧）

复用现有 per-frame culled meshlet 表：`table[frame_slot * 65536 + meshlet_base + i] = 选中的 MeshletRecord`。item 基址**不是新分配**：它就是导入期该 primitive 在全局 meshlet 表里的 `meshlet_base`（`runtime.cpp:2089`，单调累加），所以一个 item 的区间大小等于它自己的 `meshlet_count`，选中的 cluster 数必须 ≤ 它——`meshlet_culled_write` 只校验全局容量（`runtime.cpp:1711-1719`），写超了会静默写进下一个 item 的区间，resolve 于是读到别人的记录。**id 布局不变**：meshlet 字段 = 该 item 基址下的槽位（survivor 序），resolve 侧 `culled_meshlet_at(frame_slot, item.meshlet_base + id_meshlet)` 已存在（`visibility_resolve.slang:257-258`），但 host 从未置过 `item.meshlet_table`（`visibility.cppm:246` 固定写 `table`），M1 要第一次把它打开（§5 任务 2）。

### 4.3 误差与选择

- 误差单位：**asset-space 距离**，与包围体分离；`error` 为保守上界（父 >= max(子) + 父级简化误差 + 量化误差）。
- 屏幕误差：`e_px = error * projection_scale / clamp(distance_to_bounds, near_epsilon)`，透视/正交分别处理；projection_scale 从当前视图矩阵（含 jitter）取；包围体跨近裁面**强制细化**。
- 默认 `target_error_px = 1.0`（质量预算，不是逐像素严格保证）；迟滞：`coarsen <= 0.8*target`、`refine > 1.0*target`，中间保留上一帧决定；禁用迟滞时 CPU/GPU cut 必须一致。
- 覆盖规则：同一区域不能同时选父与子；选父就必须选完整 child 集；输出容量溢出 → **最高细节全量回退**（计划 3 的 overflow 协议），不得截断。

## 5. 运行时数据流

### M1（首版，host 选择，无新 GPU pass）

```
[每帧, host]  对每个 admitted item:
    roots 遍历 group 树 → 选 cut（视锥粗剔 + 屏幕误差 + 迟滞）
    → 选中的 cluster 写入 per-frame 可见表（现有 meshlet_culled_write 通路）
    → 记录 survivors 计数与 lanes bit0（visibility 自己的 push block：runtime.cpp:1650-1665，见任务 2）
[raster]      dispatch groups = survivors；shader 需新增 bit0 分支读 culled 表 → 输出 id（任务 1）
[resolve]     完全不变（item.meshlet_table 分支）
```
- **M1 的任务清单（本次审查按代码核实：这几条现在都不存在，漏一条就是错图而不是编译错）**：
  1. `source/shaders/scene/visibility.slang:99` 的 mesh entry 只读共享表；要加与 `source/shaders/scene/pbr.slang:427-433` 同构的两行——`host_culled = (geometry.base_vertex & 1u) != 0u`，记录取 `culled_meshlet_at(push.frame_slot, geometry.first_index + group_id.x)`。
  2. `runtime::record_visibility_items`（`runtime.cpp:1611-1678`）：选中 cluster 经 `meshlet_culled_write(leaf->meshlet_base, ...)` 写表；`groups_x` 从 `leaf->meshlet_count`（`1669`）改为 survivors；`block.geometry.base_vertex`（`1661`，现固定 0）置 bit0；`draw_item.meshlet_table` 置 1（现由 `make_draw_item` 硬编码 `table`，`visibility.cppm:246`）。
  3. item 区间保护：见 §4.2——选中的 cluster 数必须 ≤ 该 item 的 `meshlet_count`；否则 M1b 的 cut 会写坏后一个 item 的区间。
  4. 混合 LOD 页：所有页拼进同一对缓冲、同一 index 宽度，差别只落在记录的 `first_index`/`base_vertex`；`meshlet_record_sound` 的窗口检查要按拼接后的总长度跑（`meshlet.cppm:131-134`）。
- 准入沿用 visibility lane 的 admission（静态/非实例/非形变）；溢出/失败 → 该 item 回退"最高细节全量 run"（现有 host-cull 失败语义：写表失败就不置 bit0、按整条 run 派发）。这个回退在 M1 成立的前提是 LOD0 记录本来就躺在共享 meshlet 表的该 item 区间里——正是"复用现有记录/表"的意思；若以后把 LOD0 也搬去独立资产表，这条回退要跟着改。
- M1a：**不换资产布局**，只用 LOD0 cluster 做"选择→表→raster→resolve"的端到端验证（cut 只有一个 level，等价于过滤）；M1b：引入多级资产与真实 cut。
- 迟滞 history 按 `(item/asset version, view)` 保存，camera cut/resize 失效。

### M2 起（逐步 GPU 化，均不改 resolve 数学）

| 里程碑 | 内容 | 新增/修改边界 |
| --- | --- | --- |
| M2 | `subtilis_select` compute pass：GPU group 遍历 + 视锥/背锥剔除 + 写可见表与 indirect 计数；host 选择保留为参考开关 | 新增 `pass/subtilis_select.*` + `shaders/geometry/subtilis_select.slang`；runtime 插入点 |
| M3 | Hi-Z（本帧 occluder depth prepass → 保守遮挡；历史预测可选） | 新 depth pyramid（复用现有下采样模式）或合并进 select pass；计划 3.6 的保守规则与 recovery 协议 |
| M4 | 软件光栅只处理 subpixel cluster；触发条件由 M1/M2 的"subpixel cluster 占比"测量决定（未测前不做） | 64-bit vis buffer 或 storage depth twin；resolve 的 id 解码扩展 |
| M5 | 全驻留 → 分页流送（计划 4B） | 新 `geometry/subtilis_streaming.*` + residency 页表 |

## 6. 模块与文件边界（Subtilis 命名）

| 层 | 文件（新增） | 说明 |
| --- | --- | --- |
| 词汇/纯逻辑 | `source/engine/geometry/subtilis.cppm` | 记录布局、误差公式、cut 选择、CPU reference（无 RHI 依赖，可单测） |
| 资产构建 | `source/engine/geometry/subtilis_builder.cppm/.cpp`；离线入口 `source/tools/subtilis_builder/main.cpp`（新增 `source/tools`） | M1b 起；M1a 可先只做测试网格的内嵌构建 |
| 运行时装配 | `runtime.frames.cppm`（`subtilis_lane_frame` 冻结）、`runtime.cpp`（选择+写表）、`runtime.post_graph.cpp`（M2 pass 插入） | 与 visibility lane 对称 |
| GPU pass（M2） | `source/engine/pass/subtilis_select.cppm/.cpp` | 选择/剔除 compute |
| shader（M2） | `source/shaders/geometry/subtilis_select.slang`；M1 **要改** 现有 `scene/visibility.slang` 的 mesh entry（加 bit0 分支，§5 任务 1），不改 `visibility_resolve.slang` | 语言/Slang 约束沿用现有流水线 |
| 表/槽位 | 复用 `heap_slots::meshlets`/`meshlet_culled`（M1）；M2 新增 `heap_slots::subtilis_groups/clusters/visible` | `render_layout.cppm`；新槽只能追加在已用区域末尾（`heap_slot_count` 1024、现用 703），并同步 `source/shaders/common/heap_slot_constants.slang`（`test_render_resources` 对照两个镜像） |
| 测试 | `source/tests/test_subtilis.cpp`（CPU reference）；CMake 的 `VR_TEST_TARGETS` 注册（`CMakeLists.txt:1321`），并同步 `.github/workflows/ci.yml` 的 `--target` 列表 | 静态分析测试：覆盖/无缺口/父误差非减/确定性；`test_docs` 的第 6 条不变量要求两边一致，漏一边就是 CI 红 |
| 配置 | `[render] subtilis = false`；`application_configuration.cppm/.cpp` + `config.example.toml` + `scripts/make_config.py` + 两个 fixture + `test_app_config.cpp` 同步 | 与 `visibility_buffer` 同级：它的七个接线点是 `application_configuration.cppm:284`、`application_configuration.cpp:270-272`、`config.example.toml:406`、`scripts/make_config.py:186/311/453`、`fixtures/config_full.toml:41` 与 `config_generated_defaults.toml:40`、`test_app_config.cpp:59/94/126/200` |
| 文档/遥测 | 日志前缀 `subtilis:`；统计 `mesh_stat_subtilis_*`（选中/剔除/回退计数）；`docs/reviews/…-subtilis/` 存证据 | 沿用现有 stat lane 机制 |

**命名迁移说明**：计划 4A 里的通用名（`cluster_asset/cluster_builder/cluster_lod/cluster_select`）在实现时映射到上述 `subtilis_*`；计划本身在 4A 验收时更新，不新开平行计划。`source/engine/geometry/` 与 `source/tools/` 目前都不存在，但计划阶段三的 file boundary 已声明 `source/engine/geometry/visibility.cppm`、4A 已声明 `source/tools/cluster_builder/main.cpp`，所以这两个目录是计划先声明的，不是本文档新造的。`visibility.cppm:20-21` 的注释原文是 "THE PACKING IS v0's, NOT THE FINAL ONE. The lightweight-Nanite roadmap replaces the item/meshlet half with cluster groups and a hierarchy"，随 M1 首个提交改为 "Subtilis roadmap"。

## 7. 配置、降级与兼容

- 开关默认 **关闭**；关闭时完全不执行选择、不写表、不改变任何现有帧（OFF 冻结参考必须逐字节不变）。
- 失败/溢出：单 item 回退到最高细节全量 run；整 item 无法提供几何 → 沿用 visibility admission 的整帧回退；不得静默截断或部分覆盖。
- 旧资产（无 Subtilis 层级）继续走现有路径；不要求所有 glTF 一次迁移。
- RT 的 BLAS proxy 保持最高细节普通网格，不随每帧 cut 重建（计划 4A.6）。

## 8. 验收与测量（逐阶段）

**M1a（无资产改动）**
1. `test_subtilis`：CPU cut 参考（覆盖完整、父/子互斥、误差单调、确定性 hash、溢出回退）。
2. 端到端：deferred_visibility 等 admitted 场景 ON 参考不变或按流程重基线；画面与"现有 host-cull 全量"逐像素对照（M1a 应无差异——前提是 §5 任务 1-4 都做了：只写表而不加 shader 分支时，raster 仍是"共享表 + group_id"而 dispatch 已经缩到 survivors，画面会少几何）。
3. OFF 冻结 gate 15/15、CTest 全绿（当前 49/49）。

**M1b（真实 LOD 资产）**
4. 与最高细节参考图对照：接缝/轮廓特写、误差预算 1px；不能只看全图平均。
5. 工作量统计：每帧选中 cluster 数、三角形数、显存、构建时间；距离拉远时工作量随误差预算下降。
6. 性能：≥5 次重复或 CSV 的 GPU pass 中位数；报告收益与开销，不预设加速倍数。

**M2+**：GPU/CPU cut 一致性（同容差、无迟滞时逐项集合相等）、overflow 强制触发、Hi-Z 保守性回归、流送缺页降级（计划 3/4B 的测试清单）。

## 9. 风险与未决问题

| 风险 | 现状/对策 |
| --- | --- |
| 跨 group 裂缝 | 组内共享 LOD + 简化锁组边界；M1b 用"最高细节参考图"量化接缝像素；不达标再加裙边 |
| 资产构建依赖 | 需 vendor meshoptimizer（third_party 现无）或自写简化器；M1a 不阻塞 |
| 现计划阶段三未实现 | M1 用 host 选择绕过；M2 需决定是先在现有可见表上做 GPU select，还是先补计划 3.2/3.3 的可见列表基础设施 |
| 现有记录/注释不一致 | `mesh_geometry.slang:119` 的注释仍写 "exactly the 28 bytes `deren::vulkan::meshlet` holds"：数字错（std430 下 `float3` 要求 16 字节对齐，实际 48B），类型名也错（宿主记录是 `deren::engine::meshlet`，`meshlet.cppm:70`）。本设计以 48B 为准，M1 首个提交把这两点一起改掉 |
| `item 10bit` 容量 | 每帧 item ≤1024（现有约束，`id_item_capacity`）；本设计在 M1 冻结 id 布局、把 64-bit 推到 M4——但 visibility buffer 计划预期**在 cluster LOD 这一步**就换 id 编码，两处结论不一致，见 §3 的路线条目 |
| 性能不确定性 | 当前 ON 路径 resolve 已是成本中心（post-clip 中位数 **0.68 ms** [0.64..0.96] vs 几何 pass **0.19 ms**，3×130 帧；早先的 0.92 ms 是近平面裁剪之前的值），同一 session 的 ON 总 GPU **1.21 ms** vs OFF **0.94 ms**，差值就在这一个桶里；Subtilis 的 LOD 收益必须先抵掉这个基线差值，收益只能用真实高模资产测量，测量口径见 §8 |
| "零改动"假设 | 本轮审查确认 M1 不缺 resolve 数学、但缺 shader 分支与四段 host 接线（§5 任务 1-4）；其中任务 2 与任务 1 必须同一次提交，否则 raster 的 group 数已缩而它读的表没换，画面少几何 |
| 计划/文档过时点 | 计划**阶段三**的"48-byte meshlet/window 数据"与当前一致（4A 本身不复述宽度）；但"GPU 层级遍历"的前置（阶段三）尚不存在，且阶段三/4A 的复选框全空；本文档与计划需在 M1 验收时同步复选框与命名映射 |

## 10. 审查清单（本设计冻结前需确认）

1. §2 的每个"现状"事实是否与代码一致（行号/字段/分支条件）。
2. M1 复用 host-cull 表 + lanes bit0 的通路是否真的无 shader/resolve 改动（id 基址语义、resolve 的 `item.meshlet_table` 分支、dispatch 组数来源）。
3. 48B 记录与 std430 对齐、per-frame 表容量、item 基址分配是否支持"混合 LOD 页"。
4. 命名/文件边界是否与既有计划和仓库习惯一致（`geometry/`、`pass/`、`shaders/`、测试注册）。
5. 配置、fixtures、门禁脚本、文档清单是否遗漏。
6. 是否有与 visibility buffer v0 已验证结论冲突的假设（LOD/近平面/MASK 已完成项）。

**2026-10-18 仓库适配性审查结论（逐条对着代码核过；只读，未构建/未跑测试）**

1. §2 已按代码修正：id 位宽（`visibility.cppm:42-61`）、48B 记录与 std430 对齐、culled 表容量与 `frame_slot` 排布、基址来源都属实；两处原描述不准确已改——bit0 的消费端只在 `pbr.slang`（不在 visibility raster），MASK 判定在 raster 的 fragment（不在 resolve）。
2. **"无 shader/resolve 改动"不成立**：resolve 数学与 `item.meshlet_table` 分支确实不用改，但 visibility mesh entry 缺 bit0 分支、host 缺四处接线（§5 任务 1-4，含 dispatch 组数来源与 `meshlet_table` 从未置位这一条）。
3. 48B 与 std430 对齐成立；"混合 LOD 页"在现有记录格式里可表达，但受两条约束：per item 一个缓冲对 + 一个 `index_width`，且选中数 ≤ 该 item 的 `meshlet_count`（§4.2、§5 任务 3-4）。
4. 文件边界与计划一致：`source/engine/geometry/`、`source/tools/` 都还不存在，但计划阶段三/4A 的 file boundary 已先声明它们；测试注册除 `VR_TEST_TARGETS` 还要进 CI workflow（`test_docs` 第 6 条不变量），配置接线点见 §6。
5. 配置/fixtures/make_config.py 的清单以 §6 为准；门禁的三条选择（`-Full`/`-Visibility`/`-ExtraConfig`）与参考文件已核。
6. 与 v0 已验证结论无冲突（MASK 在 raster、近平面裁剪与 mip 对齐在 resolve、OFF 参考逐字节不变），但 **id 布局与 visibility buffer 计划 v3 的预期冲突**，见 §3 的 "路线顺序与 id 位宽的冲突"。

**仍需 owner 决定**（本文档不替它拍板）：§3 的路线重排与 id 位宽结论；M2 是先在现有可见表上做 GPU select，还是先补计划 3.2/3.3 的可见列表基础设施（§9）。
