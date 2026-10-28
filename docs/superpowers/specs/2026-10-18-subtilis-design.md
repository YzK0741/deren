# subtilis（细织几何）设计：轻量层级 LOD 的初版与扩展

> **状态更新（2026-10-21）：M1a 与 M1b 均已实现并验收；M2（GPU 选层/finalize）未实现；M3（Hi-Z 遮挡）已以独立 HZB lane 落地（2026-10-21，含 camera recovery）；M4/M5 未开始。当前入口、实测数字与文档索引见 [docs/subtilis.md](../../subtilis.md)。**
> 本文保留为设计记录：§2–10 是实现前的适配性基线，§11 是路线，§12–17 是逐步实现范围；与更早草案冲突时以 §12–17 为准。
> 具体通过项以独立验收记录为准（索引见 `docs/subtilis.md` §9）。
> 命名：**subtilis**——拉丁语 `subtilis`（"细织的、精微的"，`sub + tela`＝织机），格言 **subtilitas in minimo**（至小之中，见其精微）。
> 代码中不出现 "Nanite"（商标）；需要类比时在文档里写"Nanite 风格的虚拟几何"。

## 1. 定位与边界

**subtilis = 静态不透明网格的层级 cluster LOD 系统**：资产离线构建 cluster 层级与保守误差；运行时按屏幕误差为每个 draw item 选一条 group cut，把选中的 cluster 写成可见列表；现有 visibility buffer 光栅/解析路径消费它。

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
| 门禁：OFF legacy 15/15 冻结参考；ON 参考 `docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`（`-Full` 不含 visibility tier，`-Visibility -Compare` 判它）；`-Visibility` tier 与 `-ExtraConfig` | `scripts/gates/check_render.ps1:88-111`、`:172-179`、`:529-530` |
| 测量：lane 级 GPU 计时；`visibility_ab.py`；3 次重复仍方差过大，性能结论需 ≥5 次或 `DEREN_GAUSSIAN_BENCHMARK` CSV | `docs/reviews/2026-10-18-visibility-buffer-ab.md` |

**关键结论（决定了 M1 的做法）**：这条"选中记录按 per-frame 基址写入表 → lanes bit0 → shader 读表 → resolve 用同一基址反查"的通路，在 **pbr meshlet lane** 上四处都闭合；在 **visibility lane** 上只闭合了两端——写入端（`meshlet_culled_write`）与 resolve 端（`culled_meshlet_at` 分支）都在，但中间两处缺失：`visibility.slang:99` 的 mesh entry 只调 `meshlet_at()`，没有 bit0 分支；`runtime::record_visibility_items` 自己拼 push block（`runtime.cpp:1650-1665`），把 `groups_x` 写成整条 run（`1669`）、`base_vertex` 写 0（`1661`），且 `make_draw_item` 固定 `meshlet_table = table`（`visibility.cppm:246`）——所以 resolve 的 culled 分支至今没有生产者。因此 **M1 不是零改动**：resolve 的数学与 `item.meshlet_table` 分支不用动，但 visibility shader 要加 bit0 分支、host 要补四处接线（§5 的 M1 任务 1-4）。

## 3. 与既有计划的关系

- 总计划：`docs/superpowers/plans/2026-10-14-render-extensibility-cluster-geometry.md` 的 **阶段三（GPU 驱动可见几何）与阶段四 4A（全驻留层级 LOD）** 就是本系统。该计划明确："阶段三验收后冻结 4A 协议"、"先使用现有 48-byte meshlet 数据，只改可见性生产者，不要同时换资产布局"。
- **现状差异**：阶段三尚未实现（3.1-3.7、4A.1-4A.7、4B.1-4B.4 的复选框全为未完成，阶段二的 11 项已完成；当前 GPU cull 是 mesh shader 内逐 meshlet 剔除 + host 剔除，不是 GPU 可见列表）；4A 的"GPU 层级遍历"因此没有前置。**subtilis M1 按计划的最小起步原则，先做 host 选择 + 现有可见表通路**，把 GPU 化留给 M2（等价于计划 3.2/3.3 的能力，但以 LOD cut 为第一目标先交付）。
- visibility buffer v0（`docs/superpowers/plans/2026-10-17-visibility-buffer.md`）是 M1 的消费端与测量床；subtilis 不修改其 resolve 数学。
- **路线顺序与 id 位宽的冲突（需 owner 决定并回写计划）**：同一份 visibility buffer 计划把后续顺序写成 "v1 software raster → v2 material binning → v3 cluster 层级 + screen-error LOD → v4 分页/流送"，并明说 "item 位宽与 meshlet 位宽在 v3 会被 cluster group 取代，届时升级到 64 位 vis buffer 或分两级表是预期变化"（该文件第 170 行；`docs/reviews/2026-10-18-visibility-buffer-ab.md` 的 "Follow-ups" 同义）。本设计把 cluster LOD 提到软光栅之前（M1 就是它），并在 M1 冻结现有 32-bit id（§4.2、§9），软光栅推到 M4。两处不是同一个顺序、也不是同一个 id 结论，M1 冻结前必须在计划里显式记录这次重排，否则验收时两份文档会各说各话。
- 本设计与计划的文件边界对应关系：计划 4A 的 `cluster_asset/cluster_builder/cluster_lod/cluster_select.slang/test_cluster_lod` 在实现时以 **subtilis 命名**落地（见 §6），文档在这一层注明映射，验收时同步更新计划复选框与命名。

## 4. 数据模型（草案，M1b 验收后再冻结 GPU/资产协议）

### 4.1 资产层级（离线构建）

```
subtilisAsset (versioned header: magic/version/endian/hash/builder_version/unit)
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
- 覆盖规则：同一区域不能同时选父与子；选父表示输出该父节点的完整粗表示，细化时才替换为完整子覆盖，不能同时输出父和孩子。输出容量溢出 → **最高细节全量回退**（计划 3 的 overflow 协议），不得截断。

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

## 6. 模块与文件边界（subtilis 命名）

| 层 | 文件（新增） | 说明 |
| --- | --- | --- |
| 词汇/纯逻辑 | `source/engine/geometry/subtilis.cppm` | 记录布局、误差公式、cut 选择、CPU reference（无 RHI 依赖，可单测） |
| 资产构建 | `source/engine/geometry/subtilis_builder.cppm/.cpp`；离线入口 `source/tools/subtilis_builder/main.cpp`（新增 `source/tools`） | M1b 起；M1a 可先只做测试网格的内嵌构建 |
| 运行时装配 | `runtime.frames.cppm`（`subtilis_lane_frame` 冻结）、`runtime.cpp`（选择+写表）、`runtime.post_graph.cpp`（M2 pass 插入） | 与 visibility lane 对称 |
| GPU pass（M2） | `source/engine/pass/subtilis_select.cppm/.cpp` | 选择/剔除 compute |
| shader（M2） | `source/shaders/geometry/subtilis_select.slang`；M1 **要改** 现有 `scene/visibility.slang` 的 mesh entry（加 bit0 分支，§5 任务 1），不改 `visibility_resolve.slang` | 语言/Slang 约束沿用现有流水线 |
| 表/槽位 | 复用 `heap_slots::meshlets`/`meshlet_culled`（M1）；M2 新增 `heap_slots::subtilis_groups/clusters/visible` | `render_layout.cppm`；新槽只能追加在已用区域末尾（网格 2048 槽：纹理数组占 0..1023，其余已用槽从 1024 起、最高到 1307），并同步 `source/shaders/common/heap_slot_constants.slang`（`test_render_resources` 对照两个镜像） |
| 测试 | `source/tests/test_subtilis.cpp`（CPU reference）；CMake 的 `VR_TEST_TARGETS` 注册（`CMakeLists.txt:1321`），并同步 `.github/workflows/ci.yml` 的 `--target` 列表 | 静态分析测试：覆盖/无缺口/父误差非减/确定性；`test_docs` 的第 6 条不变量要求两边一致，漏一边就是 CI 红 |
| 配置 | `[render] subtilis = false`；`application_configuration.cppm/.cpp` + `config.example.toml` + `scripts/make_config.py` + 两个 fixture + `test_app_config.cpp` 同步 | 与 `visibility_buffer` 同级：它的七个接线点是 `application_configuration.cppm:284`、`application_configuration.cpp:270-272`、`config.example.toml:406`、`scripts/make_config.py:186/311/453`、`fixtures/config_full.toml:41` 与 `config_generated_defaults.toml:40`、`test_app_config.cpp:59/94/126/200` |
| 文档/遥测 | 日志前缀 `subtilis:`；统计 `mesh_stat_subtilis_*`（选中/剔除/回退计数）；`docs/reviews/…-subtilis/` 存证据 | 沿用现有 stat lane 机制 |

**命名迁移说明**：计划 4A 里的通用名（`cluster_asset/cluster_builder/cluster_lod/cluster_select`）在实现时映射到上述 `subtilis_*`；计划本身在 4A 验收时更新，不新开平行计划。`source/engine/geometry/` 与 `source/tools/` 目前都不存在，但计划阶段三的 file boundary 已声明 `source/engine/geometry/visibility.cppm`、4A 已声明 `source/tools/cluster_builder/main.cpp`，所以这两个目录是计划先声明的，不是本文档新造的。`visibility.cppm:20-21` 的注释原文是 "THE PACKING IS v0's, NOT THE FINAL ONE. The lightweight-Nanite roadmap replaces the item/meshlet half with cluster groups and a hierarchy"，随 M1 首个提交改为 "subtilis roadmap"。

## 7. 配置、降级与兼容

- 开关默认 **关闭**；关闭时完全不执行选择、不写表、不改变任何现有帧（OFF 冻结参考必须逐字节不变）。
- 失败/溢出：单 item 回退到最高细节全量 run；整 item 无法提供几何 → 沿用 visibility admission 的整帧回退；不得静默截断或部分覆盖。
- 旧资产（无 subtilis 层级）继续走现有路径；不要求所有 glTF 一次迁移。
- RT 的 BLAS proxy 保持最高细节普通网格，不随每帧 cut 重建（计划 4A.6）。

## 8. 验收与测量（逐阶段）

**M1a（无资产改动）**
1. `test_subtilis`：CPU cut 参考（覆盖完整、父/子互斥、误差单调、确定性 hash、溢出回退）。
2. 端到端：deferred_visibility 等 admitted 场景 ON 参考不变或按流程重基线；画面与"现有 host-cull 全量"逐像素对照（M1a 应无差异——前提是 §5 任务 1-4 都做了：只写表而不加 shader 分支时，raster 仍是"共享表 + group_id"而 dispatch 已经缩到 survivors，画面会少几何）。
3. OFF 冻结 gate 15/15、CTest 全绿（实施时以实际测试数量为准）。

**M1b（真实 LOD 资产）**
4. 与最高细节参考图对照：接缝/轮廓特写、误差预算 1px；不能只看全图平均。
5. 工作量统计：每帧选中 cluster 数、三角形数、显存、构建时间；距离拉远时工作量随误差预算下降。
6. 性能：≥5 次重复或 CSV 的 GPU pass 中位数；报告收益与开销，不预设加速倍数。

**M2+**：GPU/CPU cut 一致性（同容差、无迟滞时逐项集合相等）、overflow 强制触发、Hi-Z 保守性回归、流送缺页降级（计划 3/4B 的测试清单）。

## 9. 风险与未决问题

| 风险 | 现状/对策 |
| --- | --- |
| 跨 group 裂缝 | 组内共享 LOD + 简化锁组边界；M1b 用"最高细节参考图"量化接缝像素；不达标保留细节并终止该分支的简化；不默认用裙边遮盖缺口 |
| 资产构建依赖 | 需 vendor meshoptimizer（third_party 现无）或自写简化器；M1a 不阻塞 |
| 现计划阶段三未实现 | M1 用 host 选择绕过；M2 需决定是先在现有可见表上做 GPU select，还是先补计划 3.2/3.3 的可见列表基础设施 |
| 现有记录/注释不一致 | `mesh_geometry.slang:119` 的注释仍写 "exactly the 28 bytes `deren::vulkan::meshlet` holds"：数字错（std430 下 `float3` 要求 16 字节对齐，实际 48B），类型名也错（宿主记录是 `deren::engine::meshlet`，`meshlet.cppm:70`）。本设计以 48B 为准，M1 首个提交把这两点一起改掉 |
| `item 10bit` 容量 | 每帧 item ≤1024（现有约束，`id_item_capacity`）；本设计在 M1 冻结 id 布局、把 64-bit 推到 M4——但 visibility buffer 计划预期**在 cluster LOD 这一步**就换 id 编码，两处结论不一致，见 §3 的路线条目 |
| 性能不确定性 | 当前 ON 路径 resolve 已是成本中心（post-clip 中位数 **0.68 ms** [0.64..0.96] vs 几何 pass **0.19 ms**，3×130 帧；早先的 0.92 ms 是近平面裁剪之前的值），同一 session 的 ON 总 GPU **1.21 ms** vs OFF **0.94 ms**，差值就在这一个桶里；subtilis 的 LOD 收益必须先抵掉这个基线差值，收益只能用真实高模资产测量，测量口径见 §8 |
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

## 11. 2026-10-19：建议的轻量实现路线

本节将现有草案收敛成可逐步验证的实施范围。目标是让高几何压力场景随距离降低真实绘制工作量；暂不实现完整虚拟几何系统，也不预设比现有 G-buffer 更快。其中 M1a 已进入实现；更后阶段仍是建议范围，实际验收见 §12。

### 11.1 路线选择与依赖

推荐 **CPU cut → 全驻留层级资产 → GPU cut → 本帧 Hi-Z**；软件光栅和分页流送等实测说明必要后再做。32-bit visibility ID 在 M1 保留，其 meshlet 字段表示本帧可见 run 内的序号，并不直接编码资产 cluster/group 身份。容量不足明确回退，不能以掩码截断来维持这个编码。

这会重排 §3 所引旧路线；开始 M1 前应将 visibility 计划和总计划的路线说明同步，但不勾选尚未实施的 GPU 阶段。M1 只替换可见记录生产者，M2 再交付阶段三的 GPU select/finalize 基础。

资产侧建议采用 meshoptimizer 的聚类/简化基础算法，自有代码只实现小规模唯一父 group 树、cut 和校验。官方已有连续 cluster LOD 的 `demo/clusterlod.h`，但其表达是带 refined-group 关系的 DAG，不能直接当成这里的唯一父树；可作为对照实现，后续若切换模型需单独设计适配。

唯一父树让每个 group 只在父替代物和整套子表示之间选择，CPU reference、覆盖校验和容量上界都较容易实现；代价是分组自由度较低，锁边界可能限制简化率。先测真实资产上的层级规模与简化收益；若固定树长期无法有效降面，再评估直接适配 clusterlod 的 DAG，不同时维护两套正式资产模型。

参考：[meshoptimizer 官方实现与说明](https://github.com/zeux/meshoptimizer)，[clusterlod.h](https://github.com/zeux/meshoptimizer/blob/master/demo/clusterlod.h)。真正接入时 vendor 固定 tag/commit、记录许可证与 builder 版本；本轮不下载或安装依赖。

### 11.2 M1a：先证明记录通路

首个交付只使用现有 LOD0 meshlet，做 host 选择和表通路，不引入离线工具、简化、资产序列化或新的 GPU pass。

1. 用纯 CPU 的选择结果形成一次完整的 run：记录来源、基址、选中记录和 dispatch 数。raster 的 flag、draw-item 的 `meshlet_table` 与 indirect 的 `groups_x` 从同一个结果生成，避免三个位置分别判断。
2. 可见表写入与元数据发布必须成套：先检查 item 区间与全局容量，写完整记录，再发布 draw item 和命令。写入失败时使用原 LOD0 表，清掉 culled 标记并恢复全量 dispatch；禁止半套元数据进入光栅。
3. visibility mesh entry 增加与 pbr meshlet entry 一致的 bit0/culled-table 分支。resolve 原有 `meshlet_table` 分支即可消费，不改重建数学。
4. 所选数为零是合法剔除，不是失败；失败和空结果用不同状态表示。所有图像仍由既有 visibility pass 清空，避免零 dispatch 留下上一帧内容。

首版开关默认关闭；没有层级资产的 item 使用现有 LOD0。沿用整帧 opaque-lane admission：若现有 visibility 路径不能承载某个 item，整帧继续走几何 lane，不能同时记录两个 opaque G-buffer 作者。MASK 在现有 visibility lane 已支持，但 M1 不简化 MASK 网格，仍用其 LOD0。

验收分别比较：OFF 对现有冻结参考；M1a ON 对同配置的既有 visibility 参考。已有 visibility 相对直接几何的残差另列，不能把它算成 subtilis 新误差，也不能通过重基线掩盖新缺口。

### 11.3 M1b：再做真正的层级 LOD

每个 group 保存两套可替换表示的关系：本节点的一组 coarse clusters，以及覆盖同一区域的全部 child groups。选择节点就输出其完整 coarse run；细化才递归选择所有 children。group 可以有多个 cluster，不能把“父节点”误作“一张父 meshlet”。叶节点保留最高细节。

构建顺序：按 primitive/材质分区 → LOD0 紧凑聚类 → 相邻 clusters 分组 → 合并组内三角形并锁定外边界 → 属性感知简化 → 重新聚类为 coarse run → 递归生成父级，直到无有效减量或达到根。第一版建议每组 4–8 个 cluster、沿用现有 85 三角形工作组上限；这些是待测参数，不修改现有记录宽度。

边界约束包括位置、UV/法线接缝和材质边界。锁定的是被替换区域的完整外边界，不只是某一级各 meshlet 的局部边界；相邻不同层级不能出现 T-junction。没有有效简化就结束该分支，不能启用删除组件或丢三角形的兜底来强行达到目标。裙边也不作为正确性修复的默认办法。

可先仅生成新索引并复用原顶点，再评估顶点更新；这样资产与现有 fetch 最容易对接。所有 LOD 记录仍须指向同一 item 的有效缓冲窗口和统一 index width。CPU 树采用自己的逻辑记录，不将未经 shader 反射验证的 C++ struct 大小当成 GPU ABI。

### 11.4 容量与覆盖是硬约束

M1 沿用 item 的现有可见区间：`[meshlet_base, meshlet_base + meshlet_count)`。写入之前必须校验这个区间，不能只通过全局 65536 容量检查；选中数量还必须满足 14-bit meshlet ID 的上限。

离线校验树的最大 cut 容量：`B(node) = max(coarse_cluster_count, sum(B(child)))`，叶为其细节 cluster 数，forest 为各根容量之和。它可能大于最高细节 cluster 数，即使每一级三角形都更少。若超过现有 reservation，M1b 要拒绝该层级并使用 LOD0，或另行扩展 reservation；不能默认“LOD 更粗就一定放得下”。

回退必须在任何光栅命令之前完成；M1 的每 item 全量回退仍使用它自己的完整 LOD0，不能受本次可见表残留影响。验证非法 child 区间、环、重复覆盖、非有限误差、无表示根、损坏索引和超预算。校验失败不会生成部分可绘制资产。

### 11.5 误差与变换

区分几何距离误差和属性质量代价。meshoptimizer 的属性感知误差可能混合位置与属性项，不能未经换算就当成严格 Hausdorff 距离或严格 1px 保证。builder 应记录所用度量、单位、累计方式和版本，采用保守策略，并以最高细节图像验证轮廓、接缝与材质质量。

asset-space 误差必须先乘模型线性变换的保守伸缩上界。非均匀缩放和 shear 不能只乘平均缩放；可用 Frobenius 范数作为初版安全上界。透视按视空间的最近深度与投影尺度估计，正交不除距离。包围体跨近裁面或投影/变换非有限时选择最高细节。屏幕误差仍是选择指标，不承诺最终着色逐像素严格界。

迟滞状态按 asset generation、稳定 item 身份和 view 标识保存，不能用每帧压紧后的 item 下标当历史身份。camera cut、resize、资产替换和视图变化失效；先提供无迟滞确定性模式用于 CPU/GPU 对照。主视图 cut 与阴影策略分离，M1 阴影及 BLAS 继续使用稳定 LOD0；另测切换时的 TAA 轮廓与自阴影误差。

现有 admission 允许部分 toon item 后续通过 character_forward 做 depth-EQUAL 重着色，而该 pass 仍调用原 leaf 几何。M1b 必须将这些 item 固定 LOD0；仅让 visibility 使用简化 cut 会使原几何与深度不一致，导致局部拒绝或错着色。将来要让它们参与 LOD，必须让重着色 pass 消费完全相同的 cut。M1a 全程 LOD0 不受影响。

### 11.6 模块边界与 M2 接入

纯 CPU `geometry/subtilis` 负责树校验、误差估计、完整 cut 和 fallback 结果；不依赖 RHI。builder 负责分组、简化及带 source hash/builder 版本的资产。runtime 只做帧槽准备、选择结果转可见记录和生命周期保留；shader 只消费冻结后的 run。M1a 不提前发布磁盘资产格式，也不在 runtime 中嵌入整个离线构建器。CPU 容器沿用 utility::vessel；资产无效或预算不足是可报告并回退的结果，进程 OOM 则沿用 fast_panic，不引入异常恢复分支。

M2 复用已经证明正确的 run 协议：compute select 产生可见表与每 item 的 indirect 参数，finalize 检查容量并选择细节回退，然后才允许 raster。主机可保持固定 item 循环，读取 GPU 生成的每 item 间接参数，不以 GPU readback 决定本帧 dispatch 数。图声明 compute-write → mesh/resolve-read，以及 compute-write → draw-indirect-read；CPU reference 保留作无迟滞集合对照。

不因为 M1 尚未 GPU 化就假造 GPU 生产 pass；CPU publication 继续现有帧槽协议。M2 才增加真正的 select/finalize 图节点。全驻留资产和可见 run 都由 cluster/generation 持有，图只导入并保留 lease。分页、GPU 分配器、64-bit ID 和软件光栅仍不属于 M1。

### 11.7 最小验收闭环

- CPU：覆盖完整、父子互斥、确定性、最大 cut 预算、损坏资产拒绝、零结果与失败分开；含非均匀缩放、shear、近裁面与正交视图。
- M1a：两套表选择标记及 dispatch 数一致；强制容量失败回到完整 LOD0；OFF 冻结图像不变。
- M1b：高几何压力资产的距离序列、邻组混合 LOD 接缝、轮廓近景、材质缝、镜头切换、resize 与 TAA；无裂缝优先于简化率。
- 性能：分别报告构建时间/体积、CPU 选择、上传字节、光栅、resolve、总 GPU 中位数与 P95、选中 cluster/三角形、回退次数。现有 visibility resolve 的成本要计入，不用 cluster 数下降代替整体收益。

建议下次实现只交付 M1a；M1b 在记录通路验收后再冻结资产与选择器实施计划。


## 12. M1a 实现边界（2026-10-19）

已实现 `geometry/subtilis.cppm`、默认关闭的 `[render] subtilis` 开关、CPU 保守 LOD0 选择、原子式 run 发布/回退，以及 raster/resolve/indirect 三处统一元数据。这里的“原子式”指只在整批记录写入成功后发布消费元数据，不是增加 GPU 原子操作。

实际接线补充了两条共享表约束：同帧已发布区间不能被后续 alias item 覆写；启用的 visibility 帧禁止其它 camera host-cull 写者覆写该表，toon 仍以 LOD0 做 exact-depth 重着色。图的 raster/resolve 分别声明可选 culled-table 的 mesh/compute 读取。OFF 不增加这两个可选读取。

默认运行时仍是 LOD0 选择/记录通路，尚未自动导入真实层级资产或接入 GPU select。M1b 的独立 CPU 基础见 §13，CPU builder 见 §14，显式上传入口见 §15，尚未改变运行时选层。配置需要同时开启 `visibility_buffer = true` 和 `subtilis = true`；不满足 admission 的帧仍整体使用几何 lane。

证据与限制见 [M1a 记录](../../reviews/2026-10-19-subtilis-m1a.md)，执行进度见 [实施计划](../plans/2026-10-19-subtilis-m1a.md)。

## 13. M1b CPU 基础（2026-10-20）

`geometry/subtilis_asset.cppm` 提供拥有数据的只读 `asset`：底向上 group 顺序、唯一父树、多根完整覆盖；每组拥有一段完整 coarse records，并引用一段完整 children。工厂校验记录窗口、重复归属、非法边、根遗漏、非有限/非单调误差、叶层对规范化 LOD0 索引前缀的精确覆盖，以及 forest 最坏 cut 不超过 LOD0 reservation 和 14-bit 容量。空资产合法；失败返回错误，不发布半个资产。

此阶段要求所有记录 `base_vertex = 0`，各层共享同一索引缓冲的有效窗口。它校验结构和元数据；记录球体是否包住实际顶点、父子几何是否等价、接缝是否锁住，以及几何误差的真实性仍由后续 builder 负责。规范化索引前缀不代表已验证几何无裂缝。

`geometry/subtilis_lod.cppm` 提供无历史的 CPU reference cut：按根和孩子顺序确定性遍历，整组选粗层或完整细化，不混用同一组的父子表示。保守包围球含自身 coarse 与全部后代；视锥直接在物体空间测试，误差经 model/view 线性部分的 Frobenius 上界和投影尺度换算。透视使用最近球体深度，正交不除距离。近裁面强制细节的状态沿后代传递；无效视图使用全量最细层并跳过剔除。有效视图采用项目的右手视空间（前方为 -Z）及 Vulkan [0,w] 深度约定。

选择器有显式 `force_finest`，供后续 MASK、toon exact-depth 和阴影策略接入；本轮尚未将这些策略接入运行时。完整 run 沿用 M1a 先写记录、后发布元数据的协议；容量、alias、来源数或写入失败回到完整 shared LOD0。遍历与校验均不递归。

本轮没有 meshoptimizer builder、磁盘格式、GPU ABI 改动或真实多级几何上传，因此不能据此宣称已经降低运行时绘制三角形。下一入口是固定 meshoptimizer 版本与许可证，先实现保持 primitive/材质边界的离线索引构建，再用本工厂校验资产；上传和视觉/性能验收完成后才启用实际 LOD。实施与验证记录见 [基础计划](../plans/2026-10-20-subtilis-m1b-foundation.md)。

## 14. M1b CPU builder（2026-10-20）

在 §13 基础之后实现 `geometry/subtilis_builder.cppm`，固定 meshoptimizer v1.3 commit 并复用 utility::memory。结果同时拥有原始顶点、LOD0 索引前缀、追加的 coarse 索引和 validated asset。完整源区域边界、UV/normal seam 和各源组件须保留；候选必须同时减少三角形和记录，失败保持完整子组。反向广度层次打包支持不等深森林。

规则网格实测 2048 → 510 三角形、25 → 6 条根记录；group_size=2 得到三层父级。Release 54/54、subtilis ASan/UBSan 4/4、builder 70 项检查和新增模块静态检查通过。独立审查发现的上游浮点中间溢出已用失败回归验证并修复，异常数值候选保留完整细节。

此处仍未接入真实多级几何上传，运行时绘制量和视觉/性能收益尚未验收。下一步把同一构建结果绑定到一个资源代际，再接主视图合法静态 opaque item 的 CPU cut。完整边界、范围与后续策略见 [builder 设计](2026-10-20-subtilis-m1b-builder-design.md) 和 [验证记录](../../reviews/2026-10-20-subtilis-m1b-builder.md)。

## 15. M1b 显式上传代际（2026-10-20）

在 §14 之后实现 `subtilis::upload_geometry` 与 `primitive_create_info::subtilis_geometry`。显式提供构建结果时，runtime 将完整索引 payload 上传为 uint32，并从同一 source 取得顶点、stride 和 LOD0 计数。CPU hierarchy、两个 GPU buffer 和 pass-chain 同代际持有；普通绘制、阴影与 BLAS 仍限定 LOD0 前缀。无 source 的旧路径保持原状，没有自动导入简化或运行时层级选层。

Release 55/55、相关 ASan/UBSan 7/7、上传/绑定 44 项检查及新增模块静态检查通过。独立审查发现的构造 key 绕过已用负向编译检查复现并修复。下一步接主视图合法静态 opaque item 的 CPU cut 和帧槽发布；详见 [上传设计](2026-10-20-subtilis-m1b-upload-design.md) 与 [验证记录](../../reviews/2026-10-20-subtilis-m1b-upload.md)。


## 16. M1b 主视图 CPU cut 接线（2026-10-20）

已实现显式 uploaded_geometry 的身份/布局检查和当前帧视图适配，并在 record_visibility_items 消费完整 hierarchy cut。同一 run 控制可见表、resolve row、raster push 和 indirect；不支持策略保留最细层，来源不匹配回 shared LOD0，无来源保留原路径。character_forward 的保护覆盖所有非背景 opaque，不限 toon_family。

本地 Release57/57、相关 sanitizer10/10；实际帧102项与纯适配51项检查。详见 [主视图选层验证](../../reviews/2026-10-20-subtilis-m1b-selection.md)。自动导入构建和真实 GPU 图像/性能验收仍未完成。


## 17. Visibility4096项容量（2026-10-20）

按用户要求将现有R32_UINT布局调整为valid1/item12/meshlet12/triangle7，最多4096个item，每item最多4096个meshlet；超大单item按既有整帧admission回退。普通共享meshlet表65536容量不变，CPU asset预算校验仍独立。主机、raster、resolve与帧槽行跨度同步变更，必须成套重编译shader。此前10/14bit和1024项记录是历史状态，不再是当前上限。

原始Bistro2909个绘制对象，最大单primitive1395个meshlet，满足当前编码，无需按材质合并。边界测试先RED后GREEN，完整Release57/57、相关ASan/UBSan4/4通过。带纹理验收已完成：场景纹理数组容量同步 128→1024（网格 1024→2048 槽，materials 起点 512→1024），由网格推导单点定义；textured Bistro 实测 516 个运行时纹理槽、无白染降级，原始层级 2909 primitives/2906 可见项出图。证据与剩余项（纹理场景 subtilis OFF/ON）见 docs/reviews/2026-10-20-bistro-textured/README.md。
