# Visibility buffer v0: 把主视图 G-buffer 的生产拆成「光栅 + resolve」

> **For agentic workers:** 这是轻量 Nanite 的第一步。它不改变下游（deferred lighting、TAA、post、阴影、透明、角色前向），只改变**主视图不透明几何把属性写进 G-buffer 的方式**。v0 不是默认路径：由配置开关选择，旧路径保留作 A/B 对照与回退。实现前先核对本文的"待验证项"，不要把它们当作已证实的前提。

**Goal:** 在 deren 的渲染图上加入真实可用的 visibility buffer 机制：主视图不透明几何光栅化时只写 **depth + 可见性 ID**，再由一个 resolve pass 从 ID 重建三角形属性、走与现有 `gbuffer.slang` 相同的材质采样，写回现有 G-buffer/velocity/emissive 目标。下游像素处理完全不变。

**Background:** 现有几何路径（`docs/mesh_shaders.md` steps 0-4）已经是 mesh shader：每个 meshlet 一个 workgroup、85 三角形/255 顶点上限、每 meshlet 球+法线锥、host 预剔除 + GPU 内剔除、`vkCmdDrawMeshTasksIndirectEXT`、64 字节交错顶点（position/normal/uv/joints/weights）。v0 复用这套光栅/剔除，不引入 cluster 层级、LOD、软光栅或流送——那些是后续阶段。

## 非目标（v0 明确不做）

- 不做 LOD/cluster 层级/分页流送；不替换 meshlet 表。
- 不做软件光栅；不做 material binning（材质采样在 resolve 的每像素分支里完成）。
- 不做阴影路径的 visibility buffer（阴影仍用现存 meshlet 深度 pass）。
- 不改变透明、Gaussian、角色前向、toon/goo、RT、TAA 的结构；只保证它们读到的 G-buffer 语义不变。
- 不把 v0 设为默认（默认仍是现有路径，A/B 开关开启才走新路径）。

## 数据流（一帧，主视图）

```
现存：  mesh shader (meshlet cull) ──fragment──> GBuffer×3 + velocity + emissive + depth
v0：    mesh shader (meshlet cull) ──fragment──> vis id (R32_UINT)  + depth
        resolve compute (每像素) ──读 vis id + depth + 场景表──> GBuffer×3 + velocity + emissive
        deferred lighting / TAA / post / transparency / ... 不变
```

深度由光栅硬件深度测试决定最近片元；颜色目标只写 ID，因此**不需要 64 位原子**：深度测试通过的片元唯一，ID 写入即 winner。

## 数据模型

### 1. visibility buffer
- 格式：`R32_UINT`，尺寸 = render extent（与 G-buffer 同族），usage 同时要 `color_attachment` + `sampled`（resolve compute 采样读）。
- 生命周期：per swapchain image（与 `gbuffer_*` 相同），由 render graph 声明为 `texture_discard_write`（光栅 lane）和 `texture_read`（resolve lane）。
- heap slot：新增一个 per-image 纹理槽（`render_layout::heap_slots::vis_buffer`，紧接现有 per-image 纹理族；`render_layout` 的 family 计数 +1，`render_graph` 的 `output_targets_pass::ids` 与新 views 数组同步）。shader 侧靠 `heap_slots.glsl` 镜像，需同步 shader 常量文件。

### 2. vis ID（32 位打包，`deren.engine.visibility`）
```
bit  31      valid          1 bit   (写入的 id 恒为 1；0 = 该像素没有几何)
bits 30:21   item          10 bits  (<= 1023 个可见 item/视图；溢出 = 整帧回退旧路径)
bits 20:7    meshlet_in_run 14 bits (<= 16383 meshlet/item；溢出 = 整帧回退)
bits  6:0    triangle       7 bits  (<= 127；meshlet 上限 85，足够)
```
- 空像素 = `id{}.value == 0`（buffer 的 clear 值）；有效 id 恒带 valid 位，所以 item/meshlet/triangle 全 0
  的三角形不会被误读成空像素。`pack_id` 越界返回 `nullopt`，调用方整帧回退而不是截断 id。
- C++ 侧在 `source/engine/visibility/visibility.cppm`，shader 侧将按同一算术镜像（见 V0.3）。
- item = 本帧主视图的一个 (primitive, instance) 绘制项，静态、非实例化、不蒙皮、不 morph。v0 只接纳满足条件的 leaf。
- `meshlet_in_run` = 该项内 meshlet 的顺序号（host-culled 表里的 survivorship 序号或原始表内的序号，由 item 的 flag 决定用哪张表）。
- ID 由 mesh shader 在发射三角形时算好，作为 `nointerpolation` 顶点输出传给 fragment，fragment 原样写 `SV_Target0`。光栅阶段不需要读任何材质/属性表。

### 3. vis item 表（新增，per frame slot，host-visible）
每条 item 是一条 112 字节的记录（`deren::engine::visibility::draw_item`，12 个 32 位 lane + 64 字节
row-major model；112 是 16 的倍数，正好是 std430 struct 的对齐倍数，无需尾部 padding）：
```
vertex_address{low,high}  index_address{low,high}   // 设备地址，按 32 位两半写
first_index               base_vertex               // 该 item 的索引窗口
index_width               meshlet_base              // 该 item 的 meshlet run 在来源表里的起点
meshlet_table             material_index            // 0=全局 meshlet 表 1=本帧 host-culled 表
motion_index              flags                     // flags v0 保留为 0
model[16]                                           // row-major，与 push block 的模型矩阵一致
```
- 由 runtime 在现有"host 剔除 + 建 indirect 参数"的同一遍里写入（那一遍已经知道每个 leaf 的几何地址与 run 起点）。
- 容量：按 item 数量分配（建议 1024 起步，溢出整帧回退旧路径并记录计数）。写入是 host-visible，随帧 slot 轮转。

### 4. 复用现有表
- meshlet 表与 per-frame culled 表：mesh shader 已在用，resolve 读同一份（含 `first_index/index_count/base_vertex`），不需要新表。
- camera/instance/skin/morph 等 scene tables：resolve 声明读取，材质表（`heap_slots::materials`）与纹理堆照旧。

## Render graph 集成

新增一个 adapter（`source/engine/render_graph/visibility.cppm`）和两条 lane：

**lane R: "scene visibility raster"**
- params：`texture_discard_write depth`、`texture_discard_write vis_id`；shared inputs 的 meshlet/item/indirect/camera 等 buffer（`buffer_read`/`indirect_buffer`）。
- record：新 pass 对象 `pass::visibility_raster_pass`（或把光栅记录并入现有 scene pass 的 v0 分支）。它按 item 循环、逐 item push 现有 `MeshGeometryLanes` 和 meshlet run 基址、调用 `draw_mesh_tasks_indirect`；队列来自现成的 `mesh_indirect_buffer` 新 run。
- 这条 lane 取代旧 `graph::geometry` 的 G-buffer 写入职责；旧 lane 在开关关闭时照旧。

**lane V: "visibility resolve"**
- params：`texture_read vis_id`、`texture_read depth`、`texture_discard_write surface×3`、`texture_discard_write velocity`、`texture_discard_write color`；scene table buffer 读取 + vis item 表读取。
- record：新的 compute pass（`visibility_resolve.comp`），每像素一个线程（或 8×8 组）。
- 之后接现有 deferred lighting lane；它读 G-buffer 的方式完全不变。

**绑定与顺序**：`runtime.post_graph` 里当 `visibility_frame_candidate()` 为真时，用 lane R + lane V 替换 geometry lane；否则用旧 geometry lane。图负责所有 barrier（vis id 从 attachment 到 sampled read 的转换由 graph 生成，与 RT visibility 的现有做法一致）。

## Resolve 数学与材质

1. 读 vis id：若 `!id.valid()`（无几何）→ 按 G-buffer 清屏语义写 0/清值并返回（depth 已是清值）。注意 v0 要显式复现现 render pass `load=clear` 的每个 target 初值。
2. 解 item + meshlet：读 item 记录、meshlet 记录；`first_index/base_vertex/index_width` 来自 meshlet 记录（culled 表同样布局）。
3. 取三顶点：`mesh_index_at` 同款索引解码 → `mesh_vertex_at`（64 字节交错），得到 object-space position/normal/uv/joints/weights。
4. 透视校正重心：用 item 的 model 矩阵与 camera 的 `view*proj`（**必须和光栅用的同一对矩阵，含 TAA jitter 的 proj**）把三顶点投到 clip，做屏幕空间边函数求重心；再用 w 做透视校正。深度核对：resolved 的 z 与 depth 附件在容差内一致（v0 断言/调试视图，不依赖它做剔除）。
5. 属性插值：world_pos / normal（归一化）/ uv / prev_world_pos（用 motion_transform 或 previous_transform 表 + item 的 motion_index；静态项的 prev_world = prev_model * object_pos）。蒙皮/morph v0 不接纳，所以不需要逐顶点变形。
6. 材质：用 item 的 `material_index` 走与 `gbuffer.slang` **同一份**材质解码代码（把现有 fragment body 抽成共享 include/函数，避免两份实现）。输出 5 个目标：albedo_metallic、normal_roughness、material、velocity、scene_color(emissive)。
7. 速度/emissive 语义与现路径逐字段对齐（velocity 为 UV 空间 current - previous；emissive 是加到 scene color 的项，清屏为 0）。

## 主机 side 与开关

- 配置：`[render] visibility_buffer = false`（默认关），同步 `config.example.toml`、配置单测与 fixtures。
- 运行时：`visibility_frame_candidate()` 检查：开关打开 + 主视图 + 所有待绘制不透明 leaf 静态/非实例化/非 skin/morph + item 数与每 item meshlet 数未溢出 + 所需表/资源齐备。不满足 → 本帧整帧回退旧路径，日志一次（不逐帧刷）。
- pass 生命周期：新 pass 走现有 `create_stage/recreate_stage` + `record_content` 框架；图外资源（vis item buffer、vis buffer images）走 `resource_cluster`/现有 G-buffer 的分配模式。
- 诊断：把 item 数、meshlet 数、回退原因、resolve 像素分类计数发布到现有 mesh 统计 buffer 或日志一次；可加 debug view（可选）。

## 文件边界（计划）

新增：
- `source/engine/render_graph/visibility.cppm`（raster + resolve 两个 adapter）
- `source/engine/pass/visibility_raster.cppm/.cpp`、`source/engine/pass/visibility_resolve.cppm/.cpp`
- `source/shaders/scene/visibility.slang`（mesh + fragment，发射 ID；含 cull 复用）
- `source/shaders/scene/visibility_resolve.comp`（compute resolve）
- `source/shaders/scene/gbuffer_body.slang`（从 `gbuffer.slang` 抽出的共享材质/属性 body；`gbuffer.slang` 保留 fragment 入口调用它）
- `source/tests/test_visibility_buffer.cpp`（ID 打包往返、item 表写入/溢出回退、resolve CPU 参考的重心/属性插值、配置与 eligibility）

修改：
- `source/engine/render_layout/render_layout.cppm`、`source/shaders/common/heap_slot_constants.slang`：vis buffer family + heap slot
- `source/engine/runtime/runtime.declarations.cppm`、`runtime.cpp`、`runtime.frames.cppm`、`runtime.post_graph.cpp`、`runtime.constructor.cppm`、`runtime.frames` 的 frame services / pass 数组：资源分配、vis item 表、pass 注册、lane 选择、visibility frame
- `source/engine/runtime/runtime.post_graph.cpp`：替换/新增 lane
- `source/engine/pass/scene_tables*`：若 item 表复用 scene table 视图则扩展
- `source/app/application_configuration.cppm`：配置项
- `source/tests/CMakeLists.txt`、`test_render_graph_post`/`test_runtime_injection`/`test_app_config`：覆盖新 lane/回退
- `docs/render_resource_inventory.md`、`docs/render_graph.md`、`docs/mesh_shaders.md`、`docs/slang_migration.md`（shader 清单）
- `scripts/windows/check_render.ps1`、`docs/reviews/2026-10-17-render-graph-gate/`：新参考场景

## 分步任务

- [x] V0.1a 格式：contract 追加 `image_format::r32_uint`（appended 0x80000001u，无 ABI bump）+ backend `VK_FORMAT_R32_UINT` 映射 + `bytes_per_pixel`；`test_image_formats` pin 住枚举号与字节数。提交 `2642ec50`。
- [x] V0.2 ID 与 item 记录：`deren.engine.visibility`（`pack_id`/`id` 的 valid+10+14+7 布局、`draw_item` 112 字节记录及其字段偏移）+ `test_visibility_buffer` 单测（往返、越界拒绝、布局 pin）。
- [ ] V0.1b 资源：`render_layout` 的 vis buffer family + heap 槽 + 镜像 shader 常量；格式属性/创建失败的回退检查（`R32_UINT` 作为 color attachment 的设备支持不在 contract 里，V0.3 的探针负责验证并给出干净回退）。
- [ ] V0.2b host 写入：在现有 host 剔除/indirect 构建的同一遍里填 item 表（溢出 → 整帧回退），按帧 slot 轮转；构建图外的 item buffer 资源。
- [ ] V0.3 光栅 lane：新 mesh/fragment shader + pipeline 注册 + raster adapter + 图内记录；先不 resolve，用 debug 读回/可视化验证 ID 与深度（GPU readback probe 或 gate debug view）。
- [ ] V0.4 resolve lane：compute shader + 共享 gbuffer body 抽取 + 写回 5 个目标；CPU 参考单测（重心/插值/clear 语义）。
- [ ] V0.5 接入帧：runtime pass 注册、lane 选择/回退、配置开关、config/docs 同步；`test_runtime_injection` 类覆盖"开关开→新 lane，开关关→旧 lane，不可用→回退"。
- [ ] V0.6 A/B 验证：同一场景开关对比（图像 diff 统计、记录在 docs/reviews）；越界/validation 零发现；旧路径参考保持 15/15。
- [ ] V0.7 新参考门禁：新增/指定场景的 vis 路径参考 hash + 说明（LOD 尚未引入，v0 应接近旧路径；差异来自重心重建与像素级差异，必须量化记录而不是声称 byte-identical）。
- [ ] V0.8 文档与评审记录：docs/reviews 记录 A/B 数据、已知差异与后续（software raster / material binning / cluster LOD）。

## 待验证项（实现前必须核对，不得当假设）

1. `R32_UINT` 作为 color attachment 的 `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT` 支持；不支持的设备必须干净回退（与 mesh shader 的强制要求区分：vis 是可选路径）。
2. mesh shader 输出新增一个 `nointerpolation uint`（位置/组件数上限、与 fragment 的 interface 匹配）——特别核对 Slang 对 mesh→fragment 传 `nointerpolation` 的写法。
3. 现 render pass 的每个 G-buffer target 的 clear 值/混合语义（尤其 scene_color 的 emissive 是加色还是替换、material flags 的清值）——resolve 必须逐一复现。
4. 光栅与 resolve 的投影矩阵必须同一对（含 jitter）；深度约定（Vulkan 0..1、reverse/normal）在边函数/深度核对里一致。
5. `pbr_shade_vertex` 静态子集等价性：v0 接纳项的 world = push.model（非实例化）、无 skin/morph；核对与 `gbuffer.slang` 走同一投影/属性路径。
6. 现有 indirect 记录与 `mesh_indirect_buffer` 的两个 run 划分；新的 raster lane 需要自己的 run 且不能与阴影/旧 geometry 冲突。
7. graph 对"同一纹理先 discard_write 再 sampled read"的 barrier 生成（RT visibility 已有先例，照抄其 import/transition 语义）。

## 验收

- CTest 全绿（现有 44 + 新单测），`test_docs` 不变。
- A/B：开关关闭 = 旧路径 byte-identical（gate 15/15 不变）；开关打开在同一场景下与旧路径的像素差异有量化记录（预期只在材质着色前的重心重建带来极小差异；若出现结构性差异（洞/错位/速度错）视为失败）。
- validation 层零发现；`check_rt.ps1` PASS；新参考场景的 gate 条目建立并在之后稳定。
- 已知不支持的帧（实例化/蒙皮/morph/溢出）整帧回退旧路径并有一次日志，不出现半新半旧的目标写入。
- 提交本地，不 push，除非用户明确要求。

## 与轻量 Nanite 后续的关系

v0 把「几何生产」与「材质着色」拆开，之后的顺序是：v1 software raster 写同一张 vis buffer（硬件/软件两路合流）→ v2 material binning（resolve 从每像素分支改为按材质 tile binning）→ v3 cluster 层级 + screen-error LOD 写 vis ID（可选）→ v4 分页/流送。v0 的 ID 编码是后续扩展的起点：item 位宽与 meshlet 位宽在 v3 会被 cluster group 取代，届时升级到 64 位 vis buffer 或分两级表是预期变化，不在 v0 里预留复杂度。
