# deren 渲染扩展与轻量层级几何实施计划

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking. 本文是四阶段总计划；阶段一、二可先细化实施，阶段三、四须在前一阶段验收后补充具体算法和接口设计。不要将后续路线中的设计方向当作已经实现或已验证的接口。

**Goal:** 提高 deren 渲染路径的可组合性，在保留已有画质和功能的基础上，实现静态不透明网格的 GPU 驱动、层级细节选择，随后扩展几何流送。

**Architecture:** CPU 场景组织对象和变换，渲染快照提供稳定实例与资产引用；engine resource_cluster 管理图外资源所有权，render_graph 编译访问、依赖、裁剪和同步；几何路径产生可见几何 cluster 和绘制命令，再接入 GBuffer、PBR 和后处理。旧 registry/passchain 在对应路径验收后逐段退出。

**Tech Stack:** 当前 C++ modules、Slang、动态 RHI、Vulkan、mesh shader、间接绘制和 CTest；新路径复用 engine resource_cluster/resource_lease，迁移期保留 registry 与 generation lease。

**Spec:** 本文内的范围、接口边界和验收条件，以及本次对话已提出的四步路线。本文是规划产物，不代表已授权修改全部生产代码。

**2026-10-14 修订：** 参考本地 UE 5.8.2 源码，补充稳定身份、快照历史、访问编译、两阶段遮挡、group LOD 和流送发布协议。源码核对入口与 deren 差异见 [UE 源码参考记录](2026-10-14-render-extensibility-ue-source-notes.md)。下面新增的类型和签名均为 deren 拟定接口，不是当前已有 API，也不是照搬 UE 接口。阶段一、二按本文细化；阶段三、四的候选设计须在前阶段验收后冻结，不提前实现。

## 当前基础与约束

- 生产 pass 及共享 GPU 资源已完成本轮所有权迁移；现有未提交工作必须保留，实施前记录工作区基线，不混入无关提交。
- `scene_tree` 已有局部变换、层级、primitive 叶子；节点内嵌在 vector 中，结构变化会使节点引用失效。外部修改依赖手动 `scene_changed()`。
- `render_resource` 已有资源种类、作用域、生命周期、binding_access 和 shader stage 声明；它们尚不足以覆盖所有非 descriptor 访问和自动同步。先做声明覆盖审计，再扩展现有体系。
- `meshlet` 已有固定三角形窗口、包围球、法线锥；主视图存在 CPU 筛选与上传，以及 mesh shader/间接派发入口。不能把现有窗口格式直接视作最终层级 cluster 格式。
- 维持现有图像布局策略、heap 语义与帧槽同步；不要为了架构重写改变它们。引入新 RHI 能力时按项目规则显式升级 ABI 和边界测试。
- 首版范围：静态、不透明、全驻留几何，硬件光栅化，沿用现有 GBuffer/PBR。动态蒙皮、透明物体和 Gaussian 保持现有路径。
- 首版不要求软件光栅化、visibility buffer、通用 ECS、异步计算、显存别名复用或完整 UE RDG。
- UE 本地源码仅作为机制参考；实现使用 deren 自身接口，不将 UE 源文件移入第三方目录。
- 设置进入 `config.toml`，同时支持无窗口渲染与既有窗口渲染。只在 GPU 完成相应帧后释放在途资源。
- 配置实现同时更新 `config.example.toml`、`source/tests/test_app_config.cpp` 和配置 fixtures；运行目录生成的 `config.toml` 不当作仓库源文件。
- 当前 RHI ABI 为 32（2026-10-18 核对：现为 33，证据 source/promise/rhi/rhi.contract.cppm:300；实施时以该常量为准，本条数值仅保留当时的基线）。不是每次追加 enum 都升级 ABI：遵守 contract 中 enum、`struct_size` 与虚函数边界的现有规则；变更前列出兼容性分类及测试。
> 历史规划文档：本文写作时的 RHI ABI 为 33 / GUI ABI 为 3；当前值为 RHI ABI 45 / GUI ABI 5（见 `rhi.contract.cppm` 与 `gui.cppm`）。

## 依赖与交付边界

阶段一场景/快照与阶段二 CPU 图核心、后处理可以独立推进；按当前用户选择先完成资源簇，再推进阶段二。主场景接入复用阶段一稳定快照，阶段三、四仍依次在前置能力验收后实施。生产迁移期间保留可切换旧路径；已经部分提交的帧不能自动重跑旧路径。

阶段四有两个交付点：4A 为全驻留层级 LOD，是轻量版首版；4B 为分页流送，是后续扩展。先验收 4A，再决定是否投入 4B。

每阶段开始记录基线，完成后保存构建、行为回归、GPU 验证和图像/性能证据。实现期间不自动推送；提交按用户授权和当时工作区状态处理。

## 审查重点

1. 删除并复用节点槽位时，旧句柄必须失效；旧帧快照仍可安全完成渲染。
2. 资源通过 bindless、GPU 地址、附件或间接参数访问时，都必须进入访问声明；同一资源别名不能逃避依赖检查。
3. 相机跳变、模型移动、resize 和无历史首帧不得因陈旧遮挡数据误删可见几何。
4. 镜像缩放、双面材质、不同阴影视图和 LOD 边界不得出现错误剔除、空洞或重复覆盖。
5. 可见列表溢出、资源分配失败、流送缺页时必须有明确降级行为，不能越界写或渲染未就绪地址。

## 阶段一：共享几何资产、稳定实例和渲染快照

**实际设计提案：** [阶段一场景与快照设计](../specs/2026-10-14-stage1-scene-snapshot-design.md)。该文档将阶段一落到当前 primitive、动画、motion、固定表和 RT 调用点，审阅后据此编写阶段一逐任务实施计划；未实施项仍保持未勾选。

**交付：** 场景可安全增删、重挂接和共享几何，渲染不再依赖可变树中的节点地址。

**文件边界：**
- 修改 `source/engine/scene_tree/scene_tree.cppm`、`scene_tree.cpp`：稳定身份、层级修改、变换传播。
- 新增 `source/engine/scene/geometry_asset.cppm`：共享不可变几何与 GPU generation lease，不承担场景变换。
- 新增 `source/engine/scene/render_snapshot.cppm`、`render_snapshot.cpp`：提取实例和固定帧数据。
- 修改 `source/engine/primitive/primitive.cppm`、`primitive.cpp` 与 runtime 场景收集/帧入口：适配资产与实例。
- 修改 `source/engine/animation/controller.cppm`、`controller.cpp`：动画目标、skin mesh 节点及世界矩阵缓存改用稳定身份，所有动画局部矩阵写入经过变化记录入口。
- 修改 `source/engine/pass/scene_tables.cppm`、`scene_deformation.cppm` 与 AS/RT 场景收集入口：分开静态共享资产与动态实例资源，清除 caster/leaf 借用引用对删除安全的依赖。
- 新增 `source/tests/test_scene_tree.cpp`、`test_render_snapshot.cpp`，在 `CMakeLists.txt` 注册。
- 扩展 `source/tests/test_animation.cpp`、`test_runtime_injection.cpp`、`test_acceleration_structures.cpp`、`test_render_resources.cpp`：覆盖旧入口兼容及在途帧。

**拟定接口边界：**
- `node_handle { uint32_t index; uint32_t generation; }`；名称仅用于查找和显示，不作为身份。
- `scene::create_node(parent)`、`erase_subtree(node)`、`set_local(node, matrix)`、`reparent(node, parent, mode)`；修改接口返回明确成功/失败结果。
- `reparent_mode::keep_local / keep_world`；拒绝循环、自挂接、失效句柄，以及 keep_world 所需的不可逆父变换。
- `geometry_asset` 持有共享几何；`render_instance` 持有实例 ID、资产引用、材质引用、当前/上一帧变换和包围盒。
- `render_snapshot` 发布后只读，持有资产/资源 lease；不包含指向可变节点的借用指针。
- 场景先保持单写者；在帧边界提取快照，不声称支持任意线程并发修改。

### 阶段一的存储、历史与迁移协议

**身份与存储：** 采用 scene 私有槽位表，槽位保存 generation、存活状态、parent/children 句柄及节点数据；空闲表复用槽位。CPU 句柄、`render_instance_id` 和 GPU 表的 dense index 分开：dense index 可随快照压紧，历史、动画和删除不能用 dense index 作为身份。无效句柄使用保留 index；generation 将回绕时永久退役该槽位，不能复活旧句柄。句柄只允许在所属 scene 内使用；跨 scene 的边界传递 `(scene_id, node_handle)` 并校验 owner。

**拟定 CPU 接口（属于 `scene_tree`）：**
- 下列 `optional/expected` 使用项目现有对应类型。`render_instance_id` 使用 scene incarnation、node generation 身份与 leaf 实例序号组合；替换 primitive leaf 产生新实例身份，节点未删除也不能让新 leaf 继承旧运动历史。
- `create_node(optional<node_handle> parent) -> expected<node_handle, scene_error>`；无 parent 表示根节点。
- `erase_subtree(node_handle) -> expected<void, scene_error>`、`clear() -> void`；清空也失效全部旧句柄，不重置 generation 使旧身份复活。
- `set_local(node_handle, glm::mat4 const&) -> expected<void, scene_error>`、`reparent(node_handle, optional<node_handle>, reparent_mode) -> expected<void, scene_error>`。
- `resolve(node_handle) const -> scene_node const*` 仅供当前单写者操作内短暂读取；不能跨结构修改保留返回值。局部矩阵、parent/children 不再允许无通知的直接写入。
- `scene_error` 区分 invalid handle、cycle、non-invertible parent、invalid transform、capacity 和 allocation failure。拒绝 NaN/Inf；局部奇异矩阵可保留，但不能作为 keep_world 所需的可逆父矩阵。

**原子修改与脏更新：** reparent 先校验祖先关系、父世界矩阵和存储容量，再提交树边及 local；失败时原树、dirty 集合和历史完全不变。`keep_world` 使用操作当下已更新的世界矩阵，不能读取上一快照的世界矩阵。记录结构/变换/几何/材质分类变化；父变换将后代标脏并合并重复 dirty roots。材质变更只更新对应实例/材质记录；静态场景允许快照打包遍历，但不得重算所有世界矩阵。分别计数节点访问、世界矩阵计算与 GPU 上传字节，避免将“少算矩阵”误报为“无需遍历”。

**资产边界：** `geometry_asset` 保留现有 vertex/index `geometry_buffer::lease()`、draw ranges、bounds 与 meshlet 元数据，首轮不重新分配或复制已上传几何。几何内容变化创建新版本；仅几何相同不能自动共享不同 skin/morph 可变输出。材质版本与纹理/采样器 lease 独立于 geometry；实例持有不可变材质参数版本，GPU material table/heap 槽位在旧帧完成前不得原地覆写或复用。先利用现有帧边界替换/等待协议确保正确，再测量是否需要表版本化，不能只加 `shared_ptr` 就声称 GPU 内容被冻结。

**快照及历史：** `extract_snapshot(scene const&, snapshot_history const&, uint64_t render_serial) -> expected<render_snapshot, snapshot_error>`；快照拥有实例数组、材质版本及 GPU lease，不保存 mutable leaf/caster 指针。上一帧指最近成功提交的渲染快照，不指上一次 simulation tick、extract 调用或 frame slot。成功提交后才执行 `commit_submitted(snapshot_history&, render_snapshot const&)`；失败/丢弃的录制不推进历史。各 submitted slot 持有快照直到对应 GPU completion；引用释放与提交历史推进是两个不同事件。

- 新增实例、删除后复用槽位、显式 teleport/reset：`previous_world = current_world`，`history_valid = false`；下一次连续成功提交才可使用历史。
- 连续存在的实例保留上次成功提交世界矩阵；普通 reparent 不自动 reset，按实际世界矩阵产生速度，keep_world 的速度保持连续。
- 几何拓扑/变形历史不兼容时清除实例 history_valid；材质参数变化不必清除刚体变换历史，但需向 TAA 等消费者传递表面历史失效信息。
- 动画映射保留 `(import_instance, source_index) -> handles`，避免同一 glTF 多次导入串联；已删除 target 跳过并清除缓存。缺失必要 skin joint 时禁用该 rig 的本帧更新并诊断，不能用残留地址或错误骨骼矩阵继续上传。
- 旧 `get_scene()`、`add_root/add_child`、直接写 `.local` 和 iterator 接口逐个列出调用点并迁移；若保留 legacy adapter，明确只提供冻结结构的短期引用及保守全量 dirty 通知。不承诺旧裸引用在结构变化后仍有效。

**实施拆分及测试断言：**
- [ ] 1.0 保存基线，列出所有节点/primitive/caster 指针、动画缓存及直接 local 写入；记录 scene_tree/runtime module 版本和导入接口兼容策略，再修改存储。
- [ ] 1.1a `test_scene_tree`：erase、clear、复用、扩容均不复活旧句柄；测试 owner 校验及接近 generation 回绕的退役行为。先确认失败，再实现槽位及 resolve。
- [ ] 1.2a `test_scene_tree`：重挂接到根、循环、多级父变换、奇异父矩阵、分配拒绝；keep_world 误差在测试指定容差内，失败前后树和 dirty 集合相等。
- [ ] 1.3a `test_scene_tree`：静态第二次更新世界矩阵计算数为 0；改父节点仅更新对应子树，改材质世界矩阵计算数为 0；动画只能通过 `set_local` 标脏。
- [ ] 1.4a `test_render_snapshot`：两个实例的几何 owner 相同；删除、材质替换后旧快照字节及 lease 不变；提交 A、抽取并丢弃 B、提交 C 时，C 的 previous 来自 A；GPU 槽完成前 lease 不释放。
- [ ] 1.5a `test_animation` / runtime / AS：导入两次同源资产，删目标并扩容后更新动画无悬空访问；新增/删除/清空正确更新实例表、BVH、TLAS 和 motion slots。动态 skin/morph 继续原绘制路径，但也必须满足安全节点身份和旧帧资源存活协议。

以上细化项分别并入下列 1.1–1.6 的验收，不重复实现。每个实现任务都执行“新增断言 → 运行确认失败 → 最小实现 → 受影响测试”的周期；测试命令及注册见通用验证。

- [ ] 1.1 编写句柄测试：删除后失效、槽位复用不复活旧句柄、扩容不改变身份；确认旧实现不能满足，再实现节点存储和修改入口。
- [ ] 1.2 编写层级测试：循环拒绝、keep_world 保持世界矩阵、keep_local 保持局部矩阵、奇异父矩阵拒绝且原树不变；实现重挂接。
- [ ] 1.3 编写变化传播测试：父变换影响后代，材质变化不重算无关变换，几何变化更新包围盒；实现结构/变换/几何/材质变化记录与增量更新。
- [ ] 1.4 编写资产与快照测试：两个实例共享几何，删除一个不破坏另一个；删除场景节点后旧快照有效，新快照无该实例；实现提取和帧历史交接。
- [ ] 1.5 接入现有剔除、实例表和光追的场景更新；显式处理删除、清空、新增、变换变化与 motion history，不只设置单个 BVH 标志。
- [ ] 1.6 完成场景导入/动画兼容、构建、完整 CTest、现有场景图像回归及资源销毁验证；记录实际需要遍历/更新的节点数。

**验收：** 现有场景运行与画质不变；动态增删、重挂接不产生悬空引用；共享资产无需复制几何；已发布快照与 GPU 在途资源生命周期一致。静态场景避免全量世界矩阵重算，实际收益由测量确认。

## 阶段二：基于资源簇的渲染图

**2026-10-17 收尾：** 渲染图对**整帧**的覆盖已完成并默认开启：15/15 门禁场景整帧走一张组合图（逐字节等价 legacy、验证层零发现），`[render] main_render_graph` 默认 `true`（该键现已不存在，见本段末 2026-10-18 核对）；清单上除"光追阴影 / MegaLights"之外的全部渲染内容已进图，两项非渲染内容（上传/截图回读、次级命令缓冲）以范围判定关闭。**光追阴影已进图**（RT stage 1-2b：rhi 的 AS 屏障语义 + 图的 ray_tracing 阶段 + RT 阴影 lane，RT 帧开图 vs legacy 逐字节一致、check_rt PASS）；**剩余两项已由 owner 决策收口**：① MegaLights 作为**实验特性保留在图外**（后已被整链移除，见段末核对）（其验收不可捕获复现，不做逐字节判据；legacy tail 现在仅作为它的回退路径，其它任何特性都不再选择它）；② 图核心的**加速结构（AS）资源种类**（TLAS 作为一等图资源，完整 B 模型）作为后续可选切片推迟——光追阴影已通过 out-of-model 的 TLAS + RT 第一阶段屏障进图，因此该种类不再是覆盖前提。完整证据、图内/图外清单与续做步骤见 [渲染图迁移记录](../../reviews/2026-10-17-render-graph-gate.md) 的 Closure 一节。（2026-10-18 核对：本段的“默认 `true`”和上面 ① 的收尾均已被后续清理改写——“MegaLights 作为实验特性保留在图外、legacy tail 仅作其回退”不再成立：MegaLights 整链已移除，随后 legacy scene tail 与三个启动开关（含 `set_main_render_graph`）连同 `[render] main_render_graph` / `post_render_graph` / `bloom_render_graph` 配置键一并删除，图是唯一录制路径，没有可选的默认值。证据：docs/reviews/2026-10-17-render-graph-gate.md:969-1014；source/application_configuration/application_configuration.cppm:255；source/engine/runtime/runtime.frames.cppm:3181-3184。

**2026-10-15 修订：** 以 [Render Graph 设计](../specs/2026-10-15-render-graph-design.md) 为本阶段依据，替换早期固定链访问计划。资源创建和所有权放在图外的 engine `resource_cluster`；图负责资源引用、参数访问、内容版本、依赖、裁剪、排程和同步。不提供 graph.create 或用 extraction 取得资源所有权，首轮不要求资源池。新增设计均为待实现接口，已完成簇机制除外。

**已有基础：** [资源簇首版计划](2026-10-15-resource-cluster.md) 已完成：空簇、buffer/image/sampler/view 工厂、owning lease、跨簇同设备共享及 runtime 帧槽保留；完整 CTest 29/29 和资源簇 ASan/UBSan 通过。旧生产资源尚未迁移，旧 snapshot 仍服务原调用者。

**职责与边界：**
- 资源包在图外通过簇创建/替换，向构图函数提供明确 lease；簇不持有 layout、有效内容或提交历史。
- 一个场景/视图不强制一张图；同一次提交的多个子图优先组合，独立图显式交接状态和就绪条件。
- 同设备实际 image/buffer 及范围是 hazard 身份，不用簇地址或全局 resource_id 去重；view 关联 parent image。
- 编译器纯 CPU，无设备调用；类型化参数 visitor 产生唯一访问列表，先裁剪值依赖，再为存活访问生成 RAW/WAR/WAW。
- 执行器使用已有资源/view，runtime 保留实际 lease 和辅助 owner，并按真实 submit 结果发布状态；submit 成功但 present 失败不能回滚。
- 首版单 graphics queue。图外已创建资源不会因 pass 裁剪自动省去分配；池化、延迟创建和别名后续单独设计。

**实施划分：**
- [x] 2.0 engine 资源簇与 runtime 帧槽保留；保留原 registry/passchain。
- [x] 2.0a Boost.Graph 1.92.0 头文件子集接入与最小验证：vessel 存储、显式遍历工作区、稳定排序、可恢复环错误和 OOM；原始头文件保持不改，图库不进入 RHI ABI。
- [x] 2.1 / A1：按 [CPU 图核心实施计划](2026-10-15-render-graph-a1.md) 实现局部 ref、完整资源访问、内容版本、输出根、裁剪、物理依赖和稳定拓扑计划；不改生产帧循环。
- [x] 2.2 / A2：子资源范围、view/重复 import 归一化、参数 visitor、抽象同步和 dump。验证 GENERAL 写依赖、范围/溢出、死写裁剪与 preserve/read-write。（验收记录：reviews/2026-10-16-render-graph-{ranges,parameters,sync,diagnostics}.md）
- [x] 2.2 范围部分：mip/layer/aspect 与字节区间、稀疏状态切分、局部输出根、范围 hazard、越界/溢出验证。
- [x] 2.2 内容参数部分：类型化纹理/缓冲区字段、visitor、只读参数快照和自动访问登记。
- [x] 2.2 同步基线：用途/stage 校验、显式初始状态、按活跃执行顺序产生稀疏范围屏障、GENERAL 内存依赖、读者 stage 与只读布局转换、保守最终状态。
- [x] 2.2 诊断与审计：文本/JSON dump、错误 dump、活跃执行使用区间及 RHI 映射审计；前端保留抽象布局，后端做语义映射。
- [x] 2.2/B 转换准备：独立 RHI 同步数据、图 scope adapter 和 Vulkan GENERAL 未绑定 barrier 模板；不改变前端布局、不因原生布局相同而丢依赖；绑定/录制/提交尚未接入。
- [x] 2.3 / B：执行绑定、旧 registry/WSI 适配、RHI 访问矩阵、错误传播、提交包、状态和历史发布。辅助 descriptor/管线/上传 owner 有明确帧槽保留入口。（验收记录：reviews/2026-10-16-render-graph-execute.md、-composition.md；主图/后处理已按此录制并提交，未覆盖的分支见 2.5）
- [x] 2.4 / C：图外 post_resources + 四级 Bloom 子图，composite 人工交接；旧/图路径可切换。先验证声明，再启用图屏障，单个边界只有一个管理者。（验收记录：reviews/2026-10-16-render-graph-{bloom,post}.md；默认仍为旧路径）
- [ ] 2.5 / D：逐段接管 composite/Output、TAA 和主帧资源流，补齐 bindless/GPU 地址、附件、间接访问及 AS 声明。**当前进度（2026-10-16）**：主链第一边界（geometry -> GBuffer 交接 -> deferred LOAD -> post）与共享只读输入已验收（reviews/2026-10-16-render-graph-{geometry,lighting,shared-inputs}.md）；TAA、GBuffer debug、光追阴影、MegaLights、Gaussian、透明与三个角色/描边阶段仍在 `runtime.frames.cppm` 的资格判定里走旧 scene tail，IBL 三张图已进图（`lighting::ibl_inputs`，Stage 2E），per-mesh 地址与 per-material 纹理为堆描述符/推送常量、记录为图外；cluster 生产者尚未迁移（**2026-10-18 核对：这句所列六类现全部是图内 lane**——cluster 8、shadow 9、TAA 10/11、GBuffer debug 12、透明 13、角色前向 14、toon/goo rim 15/16、Gaussian 17、光追阴影 18；证据 source/engine/runtime/runtime.post_graph.cpp:36-40 的 lane 表，以及同文件 :468 `graph::cluster::append`、:495 `graph::shadow::append`、:669 `graph::rt_shadow::append`、:721/:743/:764/:785 的 debug/gaussian/transparent/character。旧 scene tail 已删除，“资格判定里走旧 scene tail”不再是可成立的描述；MegaLights 已整链移除，故不在该名单里。）**2026-10-17 增补**：主图帧里还剩两条由 runtime 手发的瞬态描述符屏障（光追可见性、MegaLights 解析），图的内容模型明确拒绝"读未定义内容"（`undefined_read`），因此它们暂不在图内——见 reviews/2026-10-17-render-graph-gate.md 的 Stage 2B；要收掉需图核心新增概念，或改为在图形帧把这两个堆描述符指向已知良好的占位图。（2026-10-18 核对：MegaLights 移除后只剩光追可见性这一张图需要该种子，且已改为按 generation 只做一次的首次转换，见 source/engine/runtime/runtime.post_graph.cpp:239-254；图的内容模型拒绝“读未定义内容”这一约束本身仍然成立。另：`[render] main_render_graph` 已不存在，故“主图帧”现在指每帧由 `main_graph_candidate()` 冻结的 `main_graph_frame`，证据 source/engine/runtime/runtime.declarations.cppm:1464、runtime.frames.cppm:837。）
- [ ] 2.6：按段完成固定场景、Gaussian、resize、feature、失败与在途生命周期回归；该段成为默认且没有旧调用者后，删除其旧分支和字段。最后才移除整个 registry/passchain/runner。**尚未开始**（该判定已过时，见段末 2026-10-18 核对）：`[render] main_render_graph` 默认 false（source/application_configuration/application_configuration.cppm）。（2026-10-18 核对：该键与 `set_main_render_graph` 均已删除，原文的“默认 false”不再成立，也不能再用它描述 2.6 的剩余工作；图现在是唯一录制路径（source/application_configuration/application_configuration.cppm:255、source/engine/runtime/runtime.frames.cppm:3146-3150）。2.6 仍未勾选，但剩余范围只剩“旧 registry/passchain/runner 的删除”（source/engine/pass/chain.cppm:60 仍在使用），回归本身已由 2026-10-17 门禁轮覆盖。）

**首段生产范围：** `source/engine/pass/post.cppm` / `post.cpp` 中 HDR -> Bloom prefilter -> 三次 downsample -> 四级 sampled 输出；composite、FXAA/upscale、UI、截图与 present 初期保留人工边界。曝光是 push constant，不另建曝光 pass。UI 位于 rendering instance 内，不随意插入 transition。

**验收：** A1 可独立 CPU 测试；A2+B+C 完成时证明声明驱动、裁剪、自动同步和提交生命周期已有生产用途。无需为每个图资源添加全局 resource_id；图像与旧路径一致，同步验证无新增错误。资源池和多队列不是首段可用前提。场景快照阶段独立推进，不阻塞后处理接入。

## 阶段三：GPU 驱动的可见几何与间接绘制

**交付：** 从渲染快照到 GPU 可见列表，再到 GBuffer 的完整路径；作为层级几何的可替换基础。

**文件边界：**
- 新增 `source/engine/geometry/visibility.cppm`：实例、cluster 引用、列表容量与视图标识。
- 新增 `source/engine/pass/geometry_cull.cppm`、`geometry_cull.cpp` 与 `geometry_indirect.cppm`、`geometry_indirect.cpp`。
- 新增 `source/shaders/geometry/instance_cull.slang`、`cluster_cull.slang`、`build_indirect.slang`。
- 修改 `source/shaders/scene/gbuffer.slang`、primitive/render_environment 中实际绘制接口和 runtime 接入点。
- 新增 `source/tests/test_geometry_visibility.cpp`，扩展现有间接派发/运行时注入验证。

**接口边界：** 快照输入 → 每视图实例列表 → 可见 cluster 列表 → 按兼容管线/材质路径组织的间接命令。CPU 不读取可见数量再决定本帧提交；旧 CPU 路径继续作为 fallback 和对照。

### 阶段三候选数据流（阶段二验收后冻结）

**最小起步：** 先使用现有 48-byte meshlet/window 数据，只改变可见性生产者；不要同时换资产布局。当前 shader 独立输出三个顶点/三角形，上限为 85 triangles、255 vertices；第一轮保持它。cluster local vertex reuse 与新格式放在 4A，不能直接套用 UE 的 cluster 尺寸。GPU instance table 从快照提供 model、previous、bounds、资产 run、material/path key 和历史有效位。

**每视图资源：** `view_key` 包含视图种类、主视图/阴影 cascade 标识及视图 epoch；instance candidates、visible clusters、indirect args、overflow flags 各按 view/frame slot 独立。命令按 pipeline compatibility key 分桶，key 至少含 shader 路径、vertex layout、双面/cull mode、镜像 winding 与材质访问方式；不是简单按 material id 排序。新几何路径只有一个 scene draw provider，GBuffer 消费它，不在 runtime 复制另一份材质/资源所有权。

**候选命令结构：** `visible_cluster { instance_index; asset_cluster_index; }`；CPU 仅维护快照的稳定资产 runs 和可提交批次。GPU 初始化 counters → instance cull → cluster cull/压缩 → finalize indirect → GBuffer。起步利用现有 `draw_mesh_tasks_indirect(buffer, offset, count, stride)` 的固定桶数，GPU 将空桶写成零工作量；不假设已有 indirect-count 或 dispatch-indirect 能力。超过设备 task group 上限时切成 CPU 已知的保守批次；新 RHI 命令有实测必要性再引入。

**完整覆盖的溢出机制：** 在首次光栅前，由 finalize 读取所有 candidate/visible/bin counters 的 overflow 位，选择下面互斥的一条路径：
- 未溢出：正常 compact args 非零，fallback args 全部为零。
- 溢出：本视图 compact args 全部为零，fallback args 使用 CPU 快照预建的全量 instance/asset run；fallback shader 按 run 与 workgroup 算出 meshlet，不访问溢出的 compact list。
- CPU 同时录制两组固定批次 indirect，GPU 参数决定实际工作，诊断读回只影响之后的扩容。fallback run/args 容量在 admission 时按快照完整工作量检查；连基础路径也分配失败，则提交前整视图选择既有 CPU 路径，不先提交半份新路径。任何原子计数/写入先做边界校验，计数不能 uint wrap。

4A 接入后 fallback 用最高细节的稳定全量 runs；不能把“选中的 LOD 列表”作为溢出备用，因为该列表可能已经不完整。主视图恢复队列与二次剔除也要有 overflow 协议：若首次光栅已开始后才溢出，放弃该视图的暂存 depth/GBuffer 输出，在重新 clear 的目标上执行全量 fallback，再交给 PBR/TAA；首版做不到重录/重建目标时，历史遮挡模式保持关闭，使用下面的本帧深度方案。禁止把缺失列表交给后续 shading 或在已有部分绘制上盲目追加全量造成重复覆盖。

**遮挡分两步交付：**
1. 3.6A 优先实现本帧保守遮挡：已知、不透明、可保守绘制的 occluder depth prepass → current Hi-Z → cluster cull → GBuffer。occluder 必须来自本帧变换和有效几何，初版可用普通全细节路径；无法提供可靠 occluder 时只做视锥。报告 prepass 成本，不能只统计剔除收益。
2. 3.6B 可选历史预测：previous Hi-Z 的“被遮挡”结果进入 recovery 队列；先绘制预测可见几何，生成本帧 Hi-Z，再复测 recovery 并补绘，之后才运行 PBR/速度消费/TAA。历史只是调度预测，不能永久删除 recovery 几何。相机跳变、resize、view epoch 改变、无历史、变化的 occluder/实例均保守处理；首轮阴影视图保持仅视锥/锥剔除，不共享主视图 Hi-Z。

**保守数学：** 先核对 deren 的 depth convention、clear 值和投影矩阵；Hi-Z 用最远深度归约（normal Z 为 max、reverse Z 为 min），投影包围矩形覆盖完整 texel 范围，越近裁面/非有限投影无法证明遮挡时保留。世界 bounds 与误差的缩放对含 shear 的矩阵使用线性部分 operator norm 的保守上界，不能只取最大列长度；第一轮用 Frobenius norm 上界即可。锥剔除初版只用于可验证的正向相似变换且单面材质；镜像、非均匀缩放、shear、双面与奇异变换关闭锥剔除，镜像绘制仍正确切换 winding。

**测试补充：** 小场景 GPU readback 对照 CPU 参考；对不能证明应剔除的 cluster，CPU 参考保留且 GPU 必须保留。相同数学和容差配置时比对集合相等；浮点边界允许 GPU 保守多绘，但不能漏掉 CPU 参考的保守接受项。强制每类队列容量为 1 测 overflow，以 full draw 图像确认覆盖；移动遮挡物暴露静止模型、主/阴影视图不同结果、近裁面、reverse/normal Z 对应归约、shear bounds、空桶和超过单次 dispatch 上限分别测试。

- [ ] 3.1 冻结测试场景：高面数单模型、大量共享实例、遮挡密集、无遮挡、双面/镜像缩放；记录 CPU、GPU 和图像基线。
- [ ] 3.2 编写 CPU 参考结果与 GPU 读回对照；实现 GPU 视锥剔除和保守法线锥剔除，明确非均匀缩放/双面不适用时的保守处理。
- [ ] 3.3 实现 GPU 列表压缩和间接参数生成；验证零可见、全可见、容量边界/溢出。溢出不静默截断：本帧保守回退全量绘制，记录计数并安排安全扩容。
- [ ] 3.4 接入现有 mesh shader/GBuffer；无对应设备能力时选择现有路径。先 GPU 全量，再启用剔除，分离绘制错误与剔除错误。
- [ ] 3.5 为主视图和阴影视图分别生成列表；阴影不能复用主相机结果。验证阴影边缘和镜头环绕画质。
- [ ] 3.6 在上述路径稳定后加入 Hi-Z pass 与遮挡剔除；采用保守规则和时序恢复，处理首帧、相机跳变、resize、移动物体与新暴露区域。无有效历史时关闭历史剔除。
- [ ] 3.7 完成 GPU 同步/图像回归与性能对照；保留关闭 GPU 剔除、关闭 Hi-Z、显示可见 cluster 与溢出计数的诊断选项。

**验收：** 无错误剔除和列表越界，CPU 不逐 cluster 上传可见列表；GBuffer/PBR 与旧路径保持预期一致。报告收益适用场景和额外开销，不预设固定加速倍数。

## 阶段四：层级几何与分页流送

### 4A：全驻留层级 LOD（轻量版首版）

**文件边界：** 新增 `source/engine/geometry/cluster_asset.cppm`、`cluster_builder.cppm`、`cluster_builder.cpp`、`cluster_lod.cppm`；离线构建入口 `source/tools/cluster_builder/main.cpp`；GPU 遍历 shader `source/shaders/geometry/cluster_select.slang`；测试 `source/tests/test_cluster_lod.cpp`。

**格式边界：** 使用版本化 cluster 资产，存储几何、边界、包围体、保守简化误差、层级关系、材质分区。实例引用资产，不复制层级。初版允许全驻留，不依赖流送。

### 4A 候选构建及选择协议（阶段三验收后冻结）

**推荐最小结构：** 用具有唯一 parent 的 replacement group 树起步，先避开 UE 完整 DAG 的共享关系。每个 group 描述同一覆盖区域的粗级 cluster 集合与完整 child group 集合；“一个父级表示”允许由多个 cluster 组成。该 group 的多个粗 cluster 必须共享选择决定，不能逐 cluster 独立做 LOD 阈值测试。初版限制所有 levels 使用同一套 group 分区外边界；锁边导致无法有效简化时停止上推，保留孩子，不强制生成有裂缝的粗级。

**离线流程：** 验证索引/非有限顶点/退化和非流形输入 → 按材质与属性接缝识别拓扑 → 基于邻接并兼顾空间紧凑度划分叶 cluster → 相邻叶组成固定 group → 合并组内几何 → 锁住外边界的位置和连接关系并保留属性接缝 → 简化 → 重新分块为粗级 clusters → 记录 child coverage、group error 与保守 bounds。禁止只锁位置却删除边界上的中间顶点/边形成 T-junction；不能跨 UV/硬法线/材质边界错误焊接。无法可靠处理的非流形/损坏输入回普通网格并给出原因，不静默修成另一形状。

**误差定义：** 误差单位为 asset space 的距离，与包围体分开。父误差必须非减；简化器局部 QEM cost 不能直接被声称为原始表面的严格最大偏差。若 local_error 是保守距离界，采用 `parent_error >= max(child_error) + local_error` 并叠加量化误差；否则先验证/建立保守界再进入自适应 LOD。与 UE 的误差传播实现不同，应记录 deren 的定义和证明/验证方式。格式中记录 error metric/version，避免升级算法后旧 error 值仍按新定义解释。

**CPU reference cut：** `select_lod(cluster_asset const&, lod_view const&, instance_transform const&, lod_policy const&, lod_history const*) -> expected<lod_cut, lod_error>`。从 roots 递归：group 粗级满足 pixel error 且表示有效则整体选粗，否则下降到完整 child 集；不能同时选粗与其孩子，不能只选粗表示的一部分。遍历/输出容量溢出按阶段三协议最高细节回退；选择结果中附带 coverage 验证数据用于 CPU tests。

**投影与历史：** pixel error 从当前视图 projection scale、到保守包围体的最近深度以及保守世界误差计算；透视与正交分别处理，包围体跨近裁面时强制细化。默认 `target_error_px = 1.0` 是质量预算而非严格逐像素上界。迟滞建议 `coarsen <= 0.8 * target`、`refine > 1.0 * target`，中间保留最近成功提交的 group 决定；禁用迟滞时 CPU/GPU cut 应一致。history 按 `(instance_id, asset_version, view_key)` 保存，camera cut、拓扑替换或无连续提交则失效。几何 LOD 更换时传递 surface history invalid，不能只有 previous model 矩阵而声称速度/TAA 已正确。

**格式与设备：** 明确 header magic/version/endian、section offset/count/stride、内容 hash、构建器版本/参数和 asset-space 单位；用宽整数检查加乘、长度、索引与 group cycle 后才上传。首版存无压缩数据，压缩/量化单独验收，避免一次引入两个误差来源。新 local-index mesh shader 前仍遵守 85-triangle 路径；设备 max output vertices/primitives、task group 上限作为 capability/admission，不能将一个设备的数值当通用保证。

**新增 fixtures：** 两相邻 group 一粗一细的平面/曲面、跨 group UV 接缝、硬法线与材质边界、细长三角形、非流形、near-plane、shear transform。逐合法混合 cut 验证边界顶点/边连接与覆盖；父误差非减；相同输入/参数输出 hash 相同；GPU 与无迟滞 CPU cut 对照。RT proxy 首版最高细节普通网格，后续降低 proxy 细节须另列阴影/反射误差预算。

- [ ] 4A.1 设计并测试离线格式：确定性构建、损坏/版本不支持拒绝、整数边界校验；旧资产仍可走普通网格路径。
- [ ] 4A.2 先构建空间上紧凑的叶 cluster，再构建父级简化表示；单独验证材质边界、UV/法线接缝、边界连续性。不能将每个固定索引窗口独立简化后直接拼接。
- [ ] 4A.3 定义层级覆盖规则并编写 CPU 参考选择器：同一区域不同时选父与后代，也不漏覆盖；简化误差随层级保守累计。
- [ ] 4A.4 实现屏幕误差选择与 GPU 层级遍历，接入阶段三可见列表；将容差设为像素单位配置，初始默认目标 1 px，实际画质由回归验证，不视为严格逐像素保证。
- [ ] 4A.5 验证距离变化、近裁面、大缩放、轮廓、接缝与运动历史；加入可配置迟滞，降低阈值附近反复切换。主视图/阴影视图分别设定误差策略。
- [ ] 4A.6 光追使用稳定替代网格并明确与光栅细节的差异；不随每帧 cluster 选择变化重建 BLAS。
- [ ] 4A.7 发布全驻留首版：完整功能/图像/性能验证，统计选择 cluster、三角形、显存、预处理体积和时间。保持普通网格与新路径混合运行。

**验收：** 距离增大时工作量随误差预算下降；轮廓与接缝无可见空洞；LOD 选择覆盖完整；可关闭 LOD 使用最高细节进行对照。实现效果是硬件光栅化的层级几何系统，不承诺 UE Nanite 的完整能力。

### 4B：分页流送（4A 验收后继续）

**文件边界：** 新增 `source/engine/geometry/streaming.cppm`、`streaming.cpp`、页格式和 residency 表；新增 `source/tests/test_geometry_streaming.cpp`，将上传/页表更新作为显式 pass。

**4B 候选状态机：** `absent → requested → loading → upload_pending → resident → retiring → absent`，失败进入带退避的 retry 状态；反馈携带 asset version、page id、请求优先级和 frame serial，过期资产请求拒绝。加载完成不等于 GPU 可用：依赖页、几何上传和映射更新按访问计划完成后，才将完整 replacement group 的新页集合发布到下一可写 frame-slot 页表。依赖闭包不齐则保留完整粗级，不发布半个 group。

root 粗级及其依赖常驻并计入预算；预算小于 root 最低占用时拒绝启用流送，不驱逐 root。首版每页独立解码，不引入 UE 的复杂 fixup/跨页压缩；group/page residency 与 LOD coverage 分开，只有完整可用的 representation 才可选。细节与粗级原子互换，不同时覆盖；驱逐先在新页表退回有效祖先，再等待所有持有旧页表/页 lease 的 GPU 帧完成，最后复用物理页。完成后的诊断/反馈读回可异步，当前 GPU 绘制不等待磁盘或 CPU 读回。

预算分别记录 GPU resident bytes（含 retiring/root）、CPU staging、每帧 upload bytes 和 install count；in-flight 旧页仍计费，不能靠“逻辑已驱逐”超配显存。无可回收页时停止新上传；反馈队列溢出保留粗覆盖、计数并后续重发，不让请求计数回绕。测试发布前 upload refusal、旧资产反馈、依赖缺失、root 预算不足及所有帧仍在途时的驱逐阻塞。

- [ ] 4B.1 将已验证的层级资产划分为页，常驻可覆盖全模型的粗级表示；测试页依赖和离线完整性。
- [ ] 4B.2 实现 GPU 缺页反馈、CPU 异步加载、受预算限制的上传；GPU 不等待磁盘。未就绪细节由有效粗级覆盖。
- [ ] 4B.3 采用帧版本页表和 GPU 完成后的回收；测试加载失败、快速镜头移动、预算耗尽与反复驱逐，确保旧帧地址继续有效。
- [ ] 4B.4 用超出配置驻留预算的数据集验证，记录峰值内存、请求/命中/缺页、上传量和帧时间尖峰；调节预算后覆盖仍完整。

**验收：** 在内存预算内维持完整粗级覆盖，无未就绪地址访问；加载失败可恢复；与全驻留结果比较并说明流送导致的暂时细节差异。

## 通用验证和进度记录

- 构建：`cmake --build build-release-dyn-clang64 -j3`。
- 完整回归：`ctest --test-dir build-release-dyn-clang64 --output-on-failure`；新测试在 CMake 注册后纳入同一入口。
- 每任务先运行能定位行为的测试，再运行受影响回归；每阶段结束运行完整套件。记录测试数量和失败原因，不沿用旧轮次的通过结论。
- 新测试使用现有 `VR_TEST_TARGETS` 注册方式，配置后先 `ctest --test-dir build-release-dyn-clang64 -N` 核对名字与数量。阶段一定位命令：`ctest --test-dir build-release-dyn-clang64 -R "^(test_scene_tree|test_render_snapshot|test_animation|test_runtime_injection|test_acceleration_structures)$" --output-on-failure`；阶段二：`ctest --test-dir build-release-dyn-clang64 -R "^(test_render_graph|test_render_resources|test_pass|test_runtime_injection|test_backend_boundary|test_native_boundary_zero)$" --output-on-failure`。新 GPU tests 同样登记并单独记录设备/驱动/validation 设置，不能用未注册的正则空匹配当通过。
- GPU 阶段使用实际 Vulkan 设备验证，并保存验证层输出；覆盖窗口 resize/最大化恢复与无窗口输出。
- 阶段一、二预期不改变画质，冻结资产、配置、视角、帧数与旧程序做 RGBA/HDR 对照；阶段三先全量绘制对照，再逐项打开剔除。
- 阶段四允许可控简化误差，用最高细节对照、接缝/轮廓特写和运动序列验收，不能只依赖静态截图或全图平均误差。
- 性能固定分辨率、配置、镜头路径、预热、采样长度；报告 CPU/GPU 中位数、P95 和工作量计数。测试高几何压力、多实例、强遮挡与低压力场景。
- 证据保存到 `docs/reviews/YYYY-MM-DD-<stage>/`，文档更新 `docs/render_resource_inventory.md`、`docs/managed_resources.md` 及对应场景/几何说明。
- 每阶段验收后更新本文复选框，并记录完成项、剩余项、兼容路径和已知限制。额度剩余达到 3% 时按用户要求停止并保存续接记录。

## 推荐开始方式

当前资源簇基础已完成，按用户选择优先推进阶段二 A1 纯 CPU 图核心，再做范围/参数、执行与 Bloom 接入；场景身份与快照可独立推进。不要同时改场景存储、自动屏障和几何 cluster 格式。后续阶段根据实测结果细化，保持四阶段目标不变。

阶段一内部优先级：1.0 身份引用清单 → 1.1/1.2 存储及事务化层级 → 1.3 动画与 dirty 更新 → 1.4 资产/材质版本和提交历史 → 1.5 场景各消费者 → 1.6 回归。阶段三先交付 GPU 全量与视锥路径，再评估本帧 Hi-Z；历史两阶段遮挡可独立关闭。阶段四先证明 group cut 与混合 LOD 边界正确，再追求简化率；4B 仍需 4A 验收后决定投入。
