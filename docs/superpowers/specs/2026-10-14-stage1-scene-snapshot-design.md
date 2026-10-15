# 阶段一实际设计：稳定场景身份、共享几何与冻结渲染快照

状态：设计提案，等待用户审阅；本轮尚未修改生产代码。依据：[四阶段总计划](../plans/2026-10-14-render-extensibility-cluster-geometry.md)、[UE 参考与取舍](../plans/2026-10-14-render-extensibility-ue-source-notes.md)。本设计只细化阶段一，批准后再据此写逐任务实施计划。

## 目标和约束

用户要求开始第一阶段并考虑实际方案。交付目标：节点增删、重挂接、动画更新安全；不同实例共享静态几何；本帧绘制、阴影、CPU 剔除与 RT 使用同一冻结数据；旧快照可在节点删除后继续有效；成功提交才推进运动历史。

保持现有 mesh shader/GBuffer/PBR、透明与角色材质路径、frame slot、heap 和 GPU 同步策略。动态 skin/morph 不改为 GPU cluster 路径，但其节点引用和帧数据也必须迁移。Gaussian 的独立数据路径保持当前入口。阶段一不引入自动屏障、LOD、通用 ECS、任意线程场景修改、全局内容去重或新的 RHI 命令。RHI ABI 预期保持 32；C++ module/API 的破坏性变化按各模块 banner 升版。

已有所有权迁移改动未提交。本轮只写文档；实施前保存工作区基线和现有程序行为证据，不把现存代码改动归到阶段一，也不自动提交或推送。

## 实际代码暴露的边界

| 当前事实 | 源码入口 | 必须处理的结果 |
| --- | --- | --- |
| 节点是 vector 中的值，迭代器保存地址 | [scene_tree.cppm](../../../source/engine/scene_tree/scene_tree.cppm:133) | 结构变化后裸引用不能继续使用；换容器不能解决删除后的身份复用。 |
| 实例借用源 primitive，draw/is_valid 解引用 source | [primitive.cppm](../../../source/engine/primitive/primitive.cppm:2142)、[primitive.cpp](../../../source/engine/primitive/primitive.cpp:264) | 共享资产必须脱离源 leaf 生命周期；删源实例不能破坏其他实例。 |
| 动画保存 node、mesh_node、morph prim 指针及按节点指针索引的 cache | [controller.cppm](../../../source/engine/animation/controller.cppm:783) | 三类引用全部改成句柄/实例身份；仅改 node_target 不够。 |
| 并行 sample_source 写 local 后等待任务 | [controller.cpp](../../../source/engine/animation/controller.cpp:325) | 改为并行计算、单线程提交 pose，避免并行写 dirty 集合。 |
| 录制前更新 motion_previous | [runtime.frames.cppm](../../../source/engine/runtime/runtime.frames.cppm:410) | 分离 previous 上传与 history commit；录制/提交失败不推进历史。 |
| BVH/可见集/各绘制列表借用 primitive | [runtime.declarations.cppm](../../../source/engine/runtime/runtime.declarations.cppm:1709) | payload 改为实例身份或当前快照索引，不以 bvh_dirty 作为唯一存活保护。 |
| RT caster list 借用 primitive，并从它读取 model、材质和地址 | [ray_tracing.cppm](../../../source/engine/ray_tracing/ray_tracing.cppm:88)、[ray_tracing.cpp](../../../source/engine/ray_tracing/ray_tracing.cpp:639) | caster/build 输入拥有冻结记录和 lease，不能留下跨帧借用指针。 |
| 材质/实例/motion/meshlet 使用固定表与 cursor | [runtime.cpp](../../../source/engine/runtime/runtime.cpp:1988)、[primitive.cppm](../../../source/engine/primitive/primitive.cppm:1755) | 动态删除不能只释放 CPU 节点；需要延迟回收表范围，并有容量不足的原子拒绝行为。 |

## 方案选择

采用 **scene 私有句柄槽位表＋共享 geometry_asset＋值语义 draw_packet＋render_snapshot**。

其他两种方式：将节点全部放到 stable heap 可降低迁移工作量，但删除、历史身份和 GPU 数据冻结仍需额外协议；为每帧保留一棵共享节点树能保活对象，却会让可变 primitive 状态进入旧帧并增大树复制/同步成本。两者均不作为主方案。初版仍允许 O(N) 快照打包，优先消除悬空引用和无意义矩阵重算，再测量是否需要增量快照页。

数据流：调用者编辑/动画 pose 提交 → 更新 dirty world → 冻结 draw/material/deformation 描述 → 生成本帧 snapshot → 派生剔除/阴影/RT 输入 → 录制 → submit 成功后 commit history → 对应 GPU slot 完成后释放快照和退休范围。

## 1. 节点身份和 CPU 存储

`scene_tree` 保持纯 CPU，不导入 engine primitive、RHI 或 pass。

- `scene` 拥有私有 `vector<node_slot>`、`vector<node_handle> roots`、free list、dirty worklist 和单调 scene incarnation。
- `node_slot` 保存 generation/alive 与 node payload；payload 的 parent/children 是句柄，叶仍由 `unique_ptr<scene_tree::primitive>` 拥有。节点地址不承诺稳定。
- `node_handle { uint32_t index; uint32_t generation; }` 仅在所属 scene 内解释；跨 owner 使用 `scene_node_ref { scene_id owner; node_handle node; }`。public 跨 scene 边界必须传带 owner 的引用，不能声称裸 index/generation 可以识别其他 scene。
- invalid index 为 `UINT32_MAX`，generation 从 1 开始；复用先递增 generation，达到最大值的槽位删除后永久退役。clear 保留/失效身份记账，不将所有 generation 重置。scene move 转移 incarnation 和身份，moved-from 不可解析旧引用；scene copy 删除。
- `render_instance_id { scene_id owner; node_handle node; uint64_t attachment_serial; uint32_t subinstance; }`。替换叶增加 attachment_serial；instanced batch 的每个子实例有 subinstance 身份；禁止序号回绕继续接纳实例。

核心接口使用 `std::expected`/`std::optional`：`create_node(parent)`、`erase_subtree(node)`、`reparent(node,parent,keep_local/keep_world)`、`set_local(node,matrix)`、`set_metadata(node,metadata)`、`attach(node,unique_ptr<primitive>)`、`detach(node)`、`reset_history(node)`、`clear()`；只读 `resolve(node)`、`roots()`、`children(node)`、`world(node)`。接口参数中的 node/parent 为同 owner 的 node_handle；跨 owner facade 先校验 scene_node_ref。

`resolve` 返回的只读视图只用于本次 CPU 操作，不跨 mutation 留存。structural DFS iterator 保存句柄和 scene structural revision，遍历期间结构修改后明确失效；不会把 stale 地址解引用。dirty/world 更新使用显式栈，避免深树递归耗尽栈。

reparent 先验证存活、祖先关系和新父可逆性、预留所有存储，最后一次提交树边/local/dirty。keep_world 使用实时 CPU world cache，`new_local = inverse(new_parent_world) * old_world`；全局 runtime scene_transform 不混入 CPU local。失败保持树和 mutation revision 不变。矩阵限 finite affine，保留原有 T*R*S/full affine 能力；局部奇异矩阵可存在，作为 keep_world 新父需要求逆时拒绝。单元测试 world 比较采用绝对/相对容差各 `1e-5`，含大尺度与 shear 样例。

## 2. 变化记录、导入与动画

dirty 分结构、transform、geometry、material；结构/transform 变化记录 dirty roots，更新时过滤被其他 dirty root 覆盖的子树。父变化只传播后代；material 不触发世界矩阵重算。runtime `scene_transform` 的变化触发渲染 world/bounds 更新，CPU scene-local world 不需要重算。

保留 DFS 导入顺序、名字、`(import_instance,source_index)` 映射以及多 primitive 的 `/prim` 辅助叶行为。import 祖先栈改为 handles。首轮不新加 import anchor 节点，不改变 offset 的原有矩阵组合方式；动画 binding 的“原始 import root 应用 offset”标记由导入元数据保存，不缓存当前树 depth。重挂接改变关系后，动画继续按绑定局部 pose 更新：keep_world 保证操作瞬间，不能阻止下一次动画更新改 local。

并行动画任务输出到预分配的 source-pose 槽，每个 source 一个唯一 writer；任务不调用 scene setter，不访问可变 leaf。pool join 后，调用者按稳定顺序验证句柄并 `set_local`；之后更新 world，再算 skin/morph upload。root/children 查询和 rig/primitive 数据修改都在这个单写者窗口。

node_target 和 skin mesh target 改 handles；morph target 使用 instance identity/node attachment identity 校验，mesh cache 按 handle 索引。结构变化后增量重建 binding 索引；移除目标跳过并清 cache，必要 joint 丢失则该 rig 进入 disabled 状态。恢复需要显式重新绑定，不继续上传旧骨骼矩阵。非有限/不可逆 skin mesh world 同样拒绝该 rig 更新并诊断。

兼容：保留 runtime 的 create/make/import/set_scene 外层用途，内部调用新 scene API。`get_scene()` 仍返回 scene facade；移除生产直接写 `.local/.roots/.children`。`scene_changed()` 保留为保守全量失效入口，但不替代新 mutation API。name lookup 返回 handle，clone 显式复制 CPU 层级并产生新身份；旧 clone 丢弃 primitive leaf 的行为先保留并写明，共享带叶克隆使用另一个显式入口。

## 3. geometry_asset 和既有绘制策略

新 `deren.engine.scene.geometry_asset` 只负责 immutable GPU geometry view/lease、vertex/index layout、draw ranges、local bounds 和 CPU meshlet 元数据。移入现有 geometry_buffer 的定义并在 primitive 保持既有导出名称，避免 geometry_asset 反向导入 primitive 形成 module cycle。资源由现有 generation/geometry subchain 拥有；asset 不创建第二套 registry，不复制已上传 bytes。

`primitive` 变为 mutable 场景实例适配器：持有 `shared_ptr<geometry_asset const>`、material version、push/flags 与 deformation binding。normal/instanced/static 的 CPU API 保留策略差异，几何字段归资产，实例变化不修改资产。`destroy()` 只放弃当前实例引用，不能破坏共享 geometry；instanced primitive 不再保留 `primitive const* source`，从 source 提取 shared asset 后独立存活。

提供 `runtime::create_geometry_asset(geometry_create_info const&) -> expected<shared_ptr<geometry_asset const>, creation_error>` 与 `create_primitive(asset, instance_create_info)`；旧 create_primitive(info) 是一次创建 asset 再创建实例的 wrapper。只靠 raw spans 不做全局 hash 去重；显式重复使用 asset 可保证只上传一次。glTF import 适配器增加 optional geometry source key，key 的来源身份和 lifetime 要由 loader 明确保证；没有该能力的自定义 DI 使用独立 asset。首轮要求显式共享与 `make_instanced_primitive` 安全，跨不同加载批次的自动去重不列入验收。

immutable `draw_packet` 定义在 primitive module，包含 owned pipeline key、shared asset、冻结 material_push_constants、flags、bounds、draw strategy、draw ranges、instance transforms/deformation 的 active-slot bindings。多材质 chunk freeze 为多个 range record；material 与 model 不回头读取原 primitive。skin/morph 数据缓冲仍按现有完成槽上传，packet 保留该槽实际 generation/范围，不能只留一个可变 CPU rig。

新增 `freeze_draw(freeze_context const&) const -> expected<draw_packet, freeze_error>` 和 `record_draw(draw_packet const&,render_environment&)`。抽出当前 push_geometry/mesh_dispatch 的纯录制逻辑供新旧入口共用。旧 `draw(env)` 可临时冻结并调用同一 recorder，但生产帧统一走 snapshot；无生产自定义派生类的假设需在实施前再次 inventory。测试派生类提供显式 freeze；未知 custom primitive 默认返回 unsupported，不能悄悄退回 mutable pointer 借用路径。此为 C++ API 变更，不新增 RHI ABI。

静态 batch 当前 meshlet/chunk 存在已记录的缺陷，首轮保留未启用 helper 的兼容测试，不将 unrelated static batching 修复并入阶段一。

## 4. 快照、历史和 slot 保留

`render_snapshot` module 导入 scene_tree、primitive 和 geometry_asset；primitive 不导入 snapshot。freeze 在调用者的单写者窗口完成，生产 leaf 类型沿用当前 runtime 创建的 engine::primitive 不变量；scene_tree 的纯 CPU mock 不自动视为 GPU leaf。

快照包含 render_serial、scene revisions、实例 records、draw packets、材质与 table/range leases。GPU dense slot 是 packet 内的本帧地址，不作为 instance_id。公开只读，不能保存指向节点、mutable primitive、controller 或 RT caster 的借用指针。

在 `pace_and_acquire` 完成槽等待之后，维持现有应用 animation/update 写窗口；`begin_recording` 开头完成 dirty update 和 snapshot freeze，再生成 frame draw/shadow/cull/RT 数据。各消费者引用同一快照；不要为 main 与 shadow 各抽一次可变 scene。imported camera/find_world 读 CPU world cache，仍按目前是否应用 runtime scene_transform 的接口语义处理，不顺便改变相机坐标体系。

history map 只保留 `instance_id → last submitted world/deformation version` 等 CPU 数据，不持有旧 geometry asset 导致删除后无法回收。上传本帧 previous 时读取它，不修改它。新增、attachment 替换、teleport、拓扑/变形历史不兼容时 previous=current 且 history invalid；普通材质修改只使 surface history 失效。

`submit()` 返回 ok 后，无分配地发布预先准备好的 history candidate 和 `submitted_scene_snapshots[slot]`；提交失败则丢弃 candidate，不推进 last submitted。提交成功但 present out_of_date 仍是已提交帧，history 按提交推进；resize/camera cut 另按消费者规则失效。对应 slot completion 后清快照，释放 range leases。skin/morph previous 的现有 `advance_motion_deformations` 和 controller `last_weights` 也要区分 sampled/pending/submitted 状态，不能只修刚体世界矩阵。

只允许 CPU mutation window 到 freeze 之前编辑。freeze 之后的请求进入下一帧，录制 workers 只读 snapshot。旧快照在删除/替换后仍可用于检视其冻结 records，但已消费 slot 的可变 deformation GPU 内容不承诺可重新提交；重新渲染旧动态快照需要新 slot 上传其历史数据，属于后续能力。旧在途快照直到 GPU completion 必须始终有效。

## 5. 材质与固定 GPU 表的安全更新

保留当前 capacities：material 16384，instance/motion 8192，meshlet 65536；本阶段不承诺无界增长。新增 range allocator 包装现有 table provider 的 generation，不替代 RHI allocation owner。range 处于 reserved/live/retiring/free 状态；候选资源/表容量均通过后才 attach 到 scene。失败释放 reservation，不能留下可绘制半实例或错误计数。

共享 asset 的 meshlet run 分配一次；实例 transforms/motion 独立分配，instanced batch 保持现有 workgroup-Y 与 push instance base 的表示。range reuse 等待 pending recording 与全部 submitted snapshots 的引用释放。GPU 完成不是立即复用的唯一条件，asset/material 仍被活实例引用时不能回收。

新内容写入未使用范围，材质修改创建新不可变版本，旧 slot 不原地覆写。材质 version 引用完整现有 material record、toon lanes 和 texture/sampler owner；相同纹理 heap slot 要改含义时先执行既有 frame-boundary drain/rollback 协议。table generation 或 heap publish 失败保持旧实例有效并诊断，不能只冻结 CPU bytes 后忽略 GPU 表内容。

容量不足返回 typed capacity error，保留原 scene；不静默 clamp 新实例数量。正常帧不全量 copy 材质表；创建/替换/退休路径可有局部写与等待，统计实际开销。长期增删 fixture 要证明已完成且无引用的 ranges 可回收，不能一直只递增旧 cursor。

## 6. CPU 剔除、阴影与 RT

本帧 lists 保存 snapshot index；跨帧缓存使用 instance_id 与 geometry/material/bounds revision。若继续利用 utility BVH 模板，payload 改成具有 bounds 的 snapshot instance record/ID，BVH cache 拥有其 CPU bounds records。静态场景复用 BVH；结构/geometry 改变先 rebuild，transform 变化也可先 rebuild，后续测量再加 refit。材质路由变化更新 opaque/transparent/overlay/outline 列表，不能只依赖 transform dirty。

normal/instanced、角色与透明绘制均消费 frozen packets；排序保留现有策略和稳定 tie-break。阴影独立选择 caster 集，不复用主相机可见结果。动态 deformed bounds 当前存在已记录限制；缺少可证明的保守 authored/deformation bounds 时该动态实例绕过 CPU frustum cull，避免新删除安全体系仍接受错误漏绘。该保守开销独立报告。

RT 初版保持现有结构构建策略，几何共享不承诺自动共享所有 BLAS。`caster_level` 保存 instance/asset identity、冻结几何/material/deformation 地址及 lease；TLAS transform 和 hit record 来自同一 snapshot。material mask/opaque policy 是 RT topology key 的一部分，不能只按 vertex buffer 判断复用。

增删、asset replacement、mask policy 改变时构建候选 `structure_set`，成功后发布，旧 set 由各 submitted slot 保留直到完成。候选失败且旧 set 与新 scene membership 不匹配时本帧禁用对应 RT 消费并走现有光栅 fallback，不用含已删除 caster 的旧 TLAS。只有 transform 变化则用当前 snapshot 更新 TLAS；skin/morph 沿用已有 refit/bake 次序和同步。空 scene 显式清 RT readiness/table publication，不保留上一场景输出。

## 7. 分段交付和验收

| 子交付 | 文件与独立结果 | 必须验证的断言 |
| --- | --- | --- |
| A：节点身份与修改协议 | scene_tree、CMake、test_scene_tree；runtime/animation 导入与查询调用点编译适配 | erase/clear/复用/扩容/generation 退役；循环拒绝；keep_world 与失败原子性；深树遍历。 |
| B：变化记录与动画提交 | controller、runtime scene facade、test_animation | 多次导入同源互不串联；删除 node/skin/morph target 后无悬空访问；并行与串行 pose 一致；静态第二帧矩阵计算数 0。 |
| C：共享资产与安全表范围 | geometry_asset、primitive、table provider、test_render_resources/runtime injection | 两实例几何 owner/address 相同；删 source 后 instanced packet 仍有效；反复增删安全回收；每个 admission/publish 失败不留下半状态。 |
| D：绘制包与快照历史 | render_snapshot、primitive recorder、runtime frames、test_render_snapshot | 删除/材质替换不改旧 snapshot；提交 A、丢弃 B、提交 C，C previous=A；失败提交与 present out_of_date 区分；deformation history 同步提交。 |
| E：快照消费者和 RT | frame lists/BVH、ray_tracing、AS、test_acceleration_structures | main/shadow/RT 同一 world；删除最后 caster、clear、replace/refusal；old GPU slot 保留几何/table/AS；动态 bounds 保守不漏绘。 |
| F：整阶段回归 | app/import adapters、config fixtures（只有确需新增设置时）、docs/reviews | 原测试全通过；固定资产/配置的 RGBA/HDR 对照；运动序列、窗口 resize/maxrestore、无窗口和 Vulkan validation。 |

子交付是评审边界，不是允许半迁移后把 dangling pointer 路径交给用户。A/B 期间若旧渲染仍依赖节点/primitive 引用，临时在 mutation window 使用 drain-and-recollect 保守桥接，并禁止把未完成的 snapshot lifetime 能力声明为已支持；E 完成后撤去该桥接或记录必要等待。

每个子交付从行为测试失败开始，再实现并运行定位测试。新 CPU tests 在现有 `VR_TEST_TARGETS` 登记且链接 scene_tree 或包含 snapshot 所需 modules；模块文件进入现有 engine FILE_SET，不另造仅测试可见的存储实现。基线/新测量保存 `docs/reviews/2026-10-14-stage1/`；真正执行日不同则用执行日。

验收同时记录节点访问/矩阵计算数、冻结 packet CPU 时间、上传字节、range retired/live 数、AS 重建次数、GPU 中位/P95。保持现有路径图像基线，同时以动态增删/重挂接为新增行为基线；不能拿旧 25/25 CTest 或旧 GPU 回归结论直接填新验收表。

## 审阅后首先实施的范围

优先 A＋B：稳定身份、场景 mutation 协议及所有动画引用迁移。新快照方案从 C/D 接入，E 才完成阶段一完整生命周期验收。先保存构建/行为基线，再为槽位句柄和 keep_world 写测试；不要在首次代码任务中同时换 shader、自动屏障或 cluster 数据格式。

尚需执行前盘点的实现事实：所有外部/测试 custom primitive、application chore 所保留的 primitive 指针、loader source key 的稳定性，以及动态变形表各实际 generation/slot。它们是指定的迁移审计任务，不能被默认为安全。若发现新增跨线程结构 mutation 或不受 runtime 创建约束的生产 leaf，应先调整此设计并说明接口影响。
