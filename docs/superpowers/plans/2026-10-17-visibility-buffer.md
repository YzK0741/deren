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
- heap slot：新增一个 per-image 纹理槽（`render_layout::heap_slots::visibility`，紧接现有 per-image 纹理族；`render_layout` 的 family 计数 +1，`render_graph` 的 `output_targets_pass::ids` 与新 views 数组同步）。shader 侧靠 `heap_slots.slang` / `heap_slot_constants.slang` 镜像，需同步 shader 常量文件。（2026-10-18 核对：槽名是 `heap_slots::visibility`＝base+719，证据 source/engine/render_layout/render_layout.cppm:346；`output_targets_pass` 现为 15 个 family，可见性纹理是第 15 项，证据 source/engine/pass/output_targets.cppm:26、:40。）

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
- `source/shaders/scene/visibility_resolve.slang`（compute resolve；输出名仍是 `visibility_resolve.comp.spv`，见 V0.4b）
- `source/shaders/scene/gbuffer_body.slang`（从 `gbuffer.slang` 抽出的共享材质/属性 body；`gbuffer.slang` 保留 fragment 入口调用它）
- `source/tests/test_visibility_buffer.cpp`（ID 打包往返、item 表写入/溢出回退、resolve CPU 参考的重心/属性插值、配置与 eligibility）

修改：
- `source/engine/render_layout/render_layout.cppm`、`source/shaders/common/heap_slot_constants.slang`：vis buffer family + heap slot
- `source/engine/runtime/runtime.declarations.cppm`、`runtime.cpp`、`runtime.frames.cppm`、`runtime.post_graph.cpp`、`runtime.constructor.cppm`、`runtime.frames` 的 frame services / pass 数组：资源分配、vis item 表、pass 注册、lane 选择、visibility frame
- `source/engine/runtime/runtime.post_graph.cpp`：替换/新增 lane
- `source/engine/pass/scene_tables*`：若 item 表复用 scene table 视图则扩展
- `source/application_configuration/application_configuration.cppm`（及其 `.cpp` 解析器）：配置项（**2026-10-18 核对：原文写作 `source/app/application_configuration.cppm`，该路径不存在**——配置结构体在 source/application_configuration/；`visibility_buffer` 现位于 application_configuration.cppm:284，默认 `false`，`config.example.toml:406` 同步一致。）
- `CMakeLists.txt`（仓库根；`source/tests/CMakeLists.txt` 不存在，测试目标都在根文件的 `VR_TEST_TARGETS` 与 `add_test` 里）、`test_render_graph_post`/`test_runtime_injection`/`test_app_config`：覆盖新 lane/回退（**2026-10-18 核对**：`test_visibility_buffer`、`test_render_graph_visibility`、`test_visibility_raster_pass`、`test_visibility_resolve_pass` 均已登记，CMakeLists.txt:1321）
- `docs/render_resource_inventory.md`、`docs/render_graph.md`、`docs/mesh_shaders.md`、`docs/slang_migration.md`（shader 清单）
- `scripts/gates/check_render.ps1`、`docs/reviews/2026-10-17-render-graph-gate/`：新参考场景

## 分步任务

- [x] V0.1a 格式：contract 追加 `image_format::r32_uint`（appended 0x80000001u，无 ABI bump）+ backend `VK_FORMAT_R32_UINT` 映射 + `bytes_per_pixel`；`test_image_formats` pin 住枚举号与字节数。提交 `2642ec50`。
- [x] V0.2 ID 与 item 记录：`deren.engine.visibility`（`pack_id`/`id` 的 valid+10+14+7 布局、`draw_item` 112 字节记录及其字段偏移）+ `test_visibility_buffer` 单测（往返、越界拒绝、布局 pin）。
- [x] V0.1b 资源：`render_resource` 新 family `visibility_buffer`（schema 74 项）+ `output_targets_pass` 第 15 个 family（R32_UINT，sampled + color attachment，per swapchain image）+ `render_layout` 的 `visibility_format` 与 heap 槽 719..726（MegaLights 移除后留下的空档）+ shader 镜像 + runtime 的 borrowed handles/adopt/heap rewrite。设备格式支持与创建失败回退留 V0.3 探针。
- [x] V0.2b item 表资源：`resource_id::visibility_items`（schema 75 项）+ `scene_tables_pass` 第 10 个 family（per-frame-slot lane，`frame_slots × 1024 × 112 B`）+ heap 槽 755 + `render_layout`/shader 镜像 + runtime 的 borrowed buffer/mapped/write endpoint（越界回退并记录一次）。填写时机在 V0.3 的光栅 lane 里（同一遍构建 item 与 indirect 参数）。
- [x] V0.3a shader 本体：`scene/visibility.slang`（mesh 入口：每 meshlet 一个 workgroup、复用保守视锥剔除与 64 字节顶点取数、每三角形一个 flat packed id；fragment 入口：只把 id 写进 R32_UINT target；push block 是该 stage 自己的 104 字节形状，不与 material push 耦合）+ 四个命名处注册（`VR_SLANG_SOURCES`、两个 compile 脚本、输出名；chores 的加载留到 pipeline 真正被创建时）+ `test_shader_sources` 全绿。没有 pipeline 加载它们，所以画面不动。
- [x] V0.3b-1 pipeline 脚手架：`runtime::make_visibility_pipeline`（mesh stage + id-only fragment，单 R32_UINT target、depth test LEQUAL、无混合）+ `visibility_pipeline` 句柄 + chores 启动时加载两个 module 并创建（失败即 panic 带原因）。尚未绑定/派发，画面由构造不变；门禁的 validation 日志会检查 pipeline 创建本身。
- [x] V0.3b-2a 图声明：`render_graph/visibility.cppm`（`scene visibility raster` lane：vis id 颜色附件 + 共享 depth 附件，四个表读取 camera/meshlets/items/indirect，别名拒绝、conflicting_import 与 geometry lane 同规则）+ `test_render_graph_visibility` 单测（完整声明 6 个 access、attachments-only 2 个、vis/depth 同 handle 被拒）+ CMake/CI 注册。
- [x] V0.3b-2b pass 对象：`pass/visibility_raster`（`visibility_frame::record_items` 回调契约；io = vis id 颜色 + 共享 depth；behaviour = graphics/full/resync_viewport；`create()` 从 `pass_context.shader` 取两个 module 自建 pipeline 并 `begin_pipeline`；record = 开两个 attachment 的 CLEAR instance、设 viewport/scissor、让 renderer 逐 item 记录、关 instance）+ `test_visibility_raster_pass`（声明 pin、未建 pipeline 时拒绝记录、无 renderer 回调时拒绝）。
- [x] V0.3b-2c host 侧布局：`visibility::push_block`（104 B：model/item/frame_slot/8 条 geometry lanes，偏移 static_assert 钉死）+ `make_draw_item`（64 位地址拆半、meshlet_source=table）+ `test_visibility_buffer` 扩展（push 布局、builder 往返、以及**解析 visibility.slang** 的字段顺序与 id 常量，双向钉住手写镜像）。
- [x] V0.3b-3a runtime 帧与记录器：`visibility_item_candidate`（v0 接纳规则）、`record_visibility_items`（逐 leaf 写 item 表 / 104 B push / 第三 run 的 indirect 命令 / dispatch）、`make_visibility_frame`；mesh 命令表 2→3 个 run（前两个是旧 geometry lane 的命令类，capture 必须独占第三个）。提交 `8b39b69e`。
- [x] V0.3b-3b capture-only 接线（专用 depth）：新增 managed family `visibility_depth`（per-swapchain-image，sampled + depth_attachment，heap 槽 727..734，schema 76 项/77 ids）+ runtime 的 `visibility_depth_images/views` 与 publish/adopt + pass 的 depth target 改为它；`record_post_graph` 在 geometry lane 之前追加 `graph::visibility::append`（vis id + capture depth 导入；camera/meshlet/items/indirect 四个 refs；shared_imports 扩到 13 项），demo chain 注册 pass，chores 注册两个 shader 让 pass 自建 pipeline；`publish_frame_resources` 发布两个 family。**结果：两次运行同 hash、门禁 15/15 逐字节一致、零 validation。** 这次没有共享 writer，之前的顺序不确定性消失。
- [x] V0.3b-3c 证据尝试一：把 vis id target `export_resource` 以防图裁剪。结果：pass 确实执行了（validation 立刻报出问题），但出现 `VUID-vkCmdBeginRendering-pRenderingInfo-09592`——导出的 imported attachment 在 begin_rendering 时仍是 `UNDEFINED`。原因：`initial_contents::undefined` 的导入把首次布局转换交给图，而导出的、无消费者的 lane 没有拿到那次转换。修复方向（下次）：像 TAA history 首次使用那样，由 runtime 在导入前对 vis id / capture depth 各做一次 `undefined -> GENERAL` 的首次转换（并记录"该 image 指针已初始化"，换 generation 时失效），然后用 `initial_contents::defined + prior GENERAL` 导入；或者干脆跳过 export，等 V0.4 resolve 成为真实消费者（图就不会裁剪它）。
- [x] V0.3b-3c 证据尝试二：按"TAA history 首次使用"规则，在导入前对 vis id / capture depth 各做一次
  `undefined -> attachment` 的 barrier（并用 image 指针做 generation 标记），再以 `defined + GENERAL`
  导入、并继续 export。结果：**同样的 VUID-vkCmdBeginRendering-pRenderingInfo-09592 仍在**。这说明问题不在
  首次转换本身，而在"export 一个被 import 的 attachment"这条图路径（导出的资源被当作外部/最终状态处理，
  与 pass 的第一 writer 声明冲突）。结论：不再用 export 强推"无消费者的 lane"执行——正确的消费者是 V0.4 的
  resolve pass；它一出现图就不会裁剪，且首次转换也会由图按常规 import 生成。这条尝试已回退。
- [x] V0.3b-3c capture 正确性证据（正式）：**由 V0.4c 的 resolve 消费者完成**。两次 export 尝试都因 `export 一个 imported attachment` 报 `VUID-vkCmdBeginRendering-pRenderingInfo-09592` 而放弃；resolve 成为真实消费者后图不再裁剪该 lane，日志出现 `visibility raster: recorded N item(s)`（sponza 62、deferred 1），且 resolve 的 unresolved/depth-mismatch 计数器随场景变化——三者同时成立才算 capture 工作的证据。
- [x] V0.4a 的 CPU 参考（三小步，全部有单测，shader 将镜像）：`9f359039` 重心/透视校正/插值；`4a5a45bc` clip->屏幕（Vulkan y 向下、w<=0 → nullopt）；`4c5779bf` id->表查询（meshlet_table_index = item.meshlet_base + 顺序；first_index = record 窗口基址 + 3*triangle）。
- [x] V0.4b resolve 的共享 body + `visibility_resolve.comp`（源文件按 shader 规则是 `scene/visibility_resolve.slang`，输出名仍是 `visibility_resolve.comp.spv`）：`common/surface_body.slang`（材质解码，**两阶段**：`gather_surface_base` 做材质/底色/MASK，`gather_surface_finish` 做其余贴图、TBN 与 glTF 规则）+ `scene/gbuffer_body.slang`（五输出 struct、`motion_vector`、`gbuffer_from_surface`/`gbuffer_empty_output`）；`surface.slang`/`gbuffer.slang` 改为调用共享 body，fragment 包装保持 legacy 顺序（MASK discard 在取导数之前）。compute 侧：id + capture depth + item/meshlet 表 + V0.4a 的数学 → 五个 G-buffer；两个失败计数器 `visibility_unresolved`/`visibility_depth_mismatch`。**实测教训**：(1) compute 里隐式 LOD 的 `Sample` 会让 Slang 声明 `ComputeDerivativeGroupQuadsKHR`，`vkCreateShaderModule` 拒绝该 module——新增 `heap_sample_lod0` 显式 LOD0（与 fragment 的导数 LOD 有意不同，值差异留给 V0.6 量化）；(2) 把解码抽成单个函数会让 sponza 10.5% 像素变化（MASK discard 与导数取的先后改变 alpha-tested 叶子的 TBN），所以 body 是两阶段、fragment 包装保持 legacy 顺序——门禁把这两点当验收。
- [x] V0.4c resolve pass 对象 + 接图：`pass::visibility_resolve_pass`（compute、8×8、自建 pipeline、`record_content` 追加 heap lane 后 dispatch）+ `graph::visibility::append_resolve`（2 sampled 读 + 5 storage 写 + 6 表读；raster lane 的 `shared_inputs` 扩成 camera/meshlets/indirect 三 refs）+ `runtime::record_post_graph` 在 geometry lane 之前追加 resolve lane（geometry 会清屏重画五个目标，所以画面逐字节不变）。**实测教训**：(1) payload 的 lane 是主图+post 图共享的命名空间：raster/resolve 先给 3/4（撞 bloom）、再给 10/11（撞 TAA 的 resolve 与 copy 两个 pass），两类碰撞都只在"pass 真的执行"那天才炸——最终 raster=19、resolve=20、数组 21，完整 lane 表写进 `runtime.post_graph.cpp` 的注释；(2) 批量改 lane 号时误伤 TAA 块的 `ios[10]/ios[11]`（被改成 19/20），TAA 场景变成黑帧、hash 是"全黑不透明"——TAA 块与 visibility 块必须逐块按名字核对；(3) 五个 storage 目标不是固定 image：TAA 帧的 scene colour 是 family 5、否则是 family 0，所以五个 storage twin（槽 756/764/772/780/788）在每帧 publish；(4) raster pass 必须先把 viewport/scissor 写进 pipeline handle 再 `begin_pipeline`（否则 0 宽卡住）。验收：`visibility raster: recorded N item(s)`（sponza 62、deferred 1）与 resolve 计数器同时出现；deferred/sponza/TAA 三个场景 hash 与冻结 legacy 一致；CTest 49/49（新增 `test_visibility_resolve_pass`，CMake、`test_docs` 与 CI workflow 同步）。
- [x] V0.4 resolve lane：compute shader + 共享 gbuffer body 抽取 + 写回 5 个目标；CPU 参考单测（重心/插值/clear 语义）——并入 V0.4b/V0.4c 完成（`test_visibility_buffer` 的镜像测试 + `test_visibility_resolve_pass`）。v0 已知限制（MASK 材质被排除、near-plane 三角形无法重建、LOD0 vs 导数 LOD）写进 shader 注释与计数器，留给 V0.5/V0.6 量化。
- [x] V0.5 接入帧：`[render] visibility_buffer`（默认 false）贯穿 struct/parser/`config.example.toml`/`scripts/make_config.py`/两个 fixture/`test_app_config`；`deren::engine::visibility` 新增 `admission`（每个拒绝原因一个名字）、`admit_leaf`（逐 leaf 条款：custom pipeline / instanced / deforming / geometry 缺失 / 无 meshlet run / MASK 材质 / 材质索引越界 / meshlet 溢出 / item 溢出）与 `choose_opaque_lane(switch, admission)`——"关"和"载不动"必须落到同一条旧 lane，`test_visibility_buffer` 钉住每个条款与真值表；runtime 新增 `set_visibility_buffer`/`visibility_available`/`visibility_frame_admission`，`record_main_drawcalls` 每帧冻结 verdict+lane（像 `main_graph_frame` 一样），拒绝时整帧旧 lane 并**一次**日志带原因；`record_post_graph`：on+admitted → raster lane 写 vis id **和帧自己的共享 G-buffer depth**、resolve lane 成为五目标 producer、geometry lane 整帧不记录（V0.4c 的 side-by-side capture 路径移除），off/refused → 只有 geometry lane（画面逐字节不变）。raster pass 声明里的 depth target 从 family 15 改为 `gbuffer_depth`（family 15 capture-depth 从帧中退役，资源保留到 v1 software raster 的资源整理）；`record_visibility_items` 按材质 `doubleSided` 逐 item 设 cull mode，与旧路径的 per-draw 规则一致。**实测**：(1) 首个 admitted 帧整帧变暗——resolve 插值的是 OBJECT-space 法线，忘了过 `item.model`（G-buffer normal 通道 x/y 与旧路径一致而 z 偏，正是 glTF Y-up→Z-up 的模型旋转），补上 model 变换后 A/B 的 mean_changed 从 63 降到 7/255；(2) MASK 材质必须**整帧**回退（resolve 无法 discard 已写深度），sponza ON 因此回退并打印原因，deferred ON 正常 admitted；(3) ON 路径两次运行同 hash、零 VUID；ON/OFF 的 deferred A/B 初测 13.6% 像素不同、全图平均差 0.95/255（mip LOD0 与三角形 TBN 对导数 TBN 的差异，V0.6 量化）；(4) CTest 49/49（`test_runtime_injection` 新增 visibility fallback 子进程：开关是请求、不可用时帧照旧、ON 两帧后 OFF 立即恢复），capture gate 15/15（开关默认关 = 旧路径逐字节不变）。
- [x] V0.6 A/B 验证：同一 build 上跑两轮完整 capture gate（同一场景/相机/帧数；一轮默认、一轮 `-ExtraConfig 'visibility_buffer = true'`），`scripts/measure/visibility_ab.py` 读两轮的 `<scenario>.last.png`/`.debug.log` 并按场景给出"实际 lane + 拒绝原因 + validation + ON/OFF 逐像素差"。结果：7 个场景 admitted（差异 0.51–6.91% 像素、全图均值 0.006–0.56/255、变化像素均值 1.1–8.1/255，两个无 normal-map 的球体扫描场景是 0.5%/0.006 的近似同一对照），8 个 fallback 且**全部 0.00%**（MASK ×2、depth-EQUAL ×4、skinned ×1、no draws ×1，原因来自 runtime 的 `admission` 词汇）；15 个 ON 日志零 validation 发现；两轮都是 flaky 0（每场景两次运行同 hash）；同 build 的 OFF 全量 gate 15/15。**A/B 逼出的两个实测缺陷**：v0.5 的 object-space normal（已修，mean_changed 63→7）与本轮新发现的 depth-EQUAL re-shading stage——卡通角色 pass 用 `depth_compare::equal` 对同帧 G-buffer 深度做第二次着色，而跨 pipeline module 的深度可差 1 ULP，导致该 pass 覆盖率少约 106k 像素、均值 67/255；新增帧级 admission 条款 `depth_equal_stage` 整帧回退，并把"深度生产者统一"列为后续。细节与复现命令见 `docs/reviews/2026-10-18-visibility-buffer-ab.md`；新增工具 `scripts/measure/visibility_ab.py` 与 gate 的 `-ExtraConfig`（不能与 `-Update` 同用）。
- [x] V0.7 新参考门禁：gate 新增 `tier = "visibility"` 与 `-Visibility` 选择开关（`-Full` 明确不含该 tier，legacy 15/15 的判决不变）；场景 `deferred_visibility` = `deferred` 的模型/相机/帧数再加 `visibility_buffer = true`（一键之差 = 单帧 A/B）；冻结参考在该场景自己的 `docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt`（`104E00AFDF3630EA`），本轮 `-Visibility -Compare <file>` 通过（1/1 matched、`flaky: 0`、零 validation）。（2026-10-18 核对：该 ON 参考已被重新基线——v0.8 之后的 clip-position 修复把可见性 mesh shader 改成嵌套 `mul` 形式后，文件现值是 `AEE07350B796AC07`（docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt:2、docs/reviews/2026-10-18-visibility-buffer-ab.md:893-894）；旧值 `104E00AFDF3630EA` 只对应修复前的构建，`-Compare` 今天必须用文件里的现值。）该参考是**第二套**参考集：旧路径的 15 个 hash 与 ON 路径的 1 个 hash 分文件保存，前者不变、后者随 resolve 变更并在同一 commit 手工更新，两者一起保证"旧路径没动 / 新路径按需变更"可分。`-Compare` 轮不再要求机器本地 baseline（冻结文件即参考），但仍保留两次运行的 determinism 检查。
- [x] V0.8 文档与评审记录：`docs/reviews/2026-10-18-visibility-buffer-ab.md` 记录 A/B 数据（7 admitted / 8 fallback、差异统计、15 个 ON 日志零 validation、两轮 flaky 0、同 build OFF gate 15/15）、两个实测缺陷（object-space normal、depth-EQUAL re-shading stage）与后续（software raster 写同一 vis buffer、material binning、cluster LOD、深度生产者统一、MASK 的 resolve 方案、family 15 的退役）；plan checklist 收尾。

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

## Non-extension follow-up (started after V0.8; the "fix first, then optimize" list)

- [x] Resolve material mip: derived from the triangle's UV-vs-screen scale, replacing explicit LOD 0 (deferred ON/OFF changed-pixels 6.88% -> 4.57%, mean over changed 8.05 -> 2.74; OFF byte-identical).
- [x] ON path GPU marks: the geometry lane's scene_end/rt_shadow_end marks are emitted by the lanes that replaced it (refused marks 492 -> 0), making per-pass timings measurable on the path.
- [x] Lane-level timing split: a path-independent mark separates the mesh raster from the resolve (measured: raster 0.01 ms, resolve 0.92 ms vs the geometry pass 0.19 ms - the resolve is the cost centre the optimization item must move).
- [x] MASK materials carried: the visibility raster fragment runs the glTF alphaMode MASK test and discards before the depth write; the `masked_material` admission clause is retired (sponza and transparent_blend now record the new path).
- [x] Near-plane clip: Sutherland-Hodgman in the resolve + fan rebuild, unclipped triangles keep v0's exact acceptance rule (sponza unresolved 858,868 -> 15,539, -98.2%; >64 pixel deltas vs OFF 0.951% -> 0.106%).
- [x] Family 15 (retired capture depth) removed from the producer/adoption/publication and both heap-slot mirrors (~33 MB at 1080x960 + its descriptor writes); the schema id stays and slots 727..734 remain reserved.
- [x] Storage twins publish only on a frame that reads them (5 descriptor writes per frame saved on every frame that does not run the resolve).
- [x] Item 1, depth-producer unification: the `depth_equal_stage` whole-frame fallback is GONE. The toon re-shading
  pipelines now test `depth_compare::less_or_equal` (EQUAL OR NEARER) instead of exact EQUAL: on the old path the
  depth is bit-identical so the frozen frames do not move (full OFF gate 15/15 PASS), and on a visibility frame a
  microscopic mismatch DRAWS the character instead of dropping it. All four `laevatain_*` scenarios now record the
  visibility lane (measured ON vs OFF: changed 8.66-13.48%, frame mean 4.04-6.50, max 200-255). Root cause of the
  mismatch was NOT found (mechanism measured; SPIR-V arithmetic, pipeline state and cull decisions all identical) -
  the static search space is closed and the readback harness is the recorded entry point.
- [x] Quality follow-up for item 1: ROOT CAUSE FOUND AND FIXED. A depth-probe readback harness showed the
  visibility mesh shader's precomputed `world * (view * proj)` clip form gives a depth 1-2 ULP NEARER than the
  toon pipeline's nested `mul(world_pos, mul(view, proj))` form on every one of 184,782 differing pixels; with
  the nested form the two stages' depths are BIT-IDENTICAL (0 differing pixels), the exact `depth_compare::equal`
  test passes again by construction (the `depth_equal_stage` fallback stays retired), and the toon frames measure
  0.00-2.13% changed against the old path (from 8.66-13.48%). See the review record's "root cause found" section.

- [x] Material binning, DECIDED AGAINST by measurement (see the review record): material is per ITEM (already fully coherent on the measured frame), the whole fragment G-Buffer pass costs 0.19 ms so any material-only change is capped at ~21% of the resolve, and a bin consume pass needs an RHI indirect-compute verb that does not exist. The measured gap is geometry reconstruction. Recorded alternatives for a future pass: multi-pixel-per-thread resolve with hoisting (20-40% if latency-bound; needs >=5-repeat measurement), and a scoped -O2/-O3 for this shader.


### 2026-10-19 路线调整（subtilis M1a 获准实施）

后续顺序改为 CPU LOD0 可见 run → 全驻留层级资产 → GPU select/finalize → Hi-Z；软件光栅与分页按测量需要再实施。M1 保留 32-bit ID，meshlet 字段为 run 内序号；不勾选未完成的 GPU 阶段。以 [subtilis §11](../specs/2026-10-18-subtilis-design.md) 和 [M1a 计划](2026-10-19-subtilis-m1a.md) 为准，取代上面的早期 v1-v4 顺序。
