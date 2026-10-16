# deren Render Graph 设计

状态：资源簇、A1 纯 CPU 图核心和 A2 的子资源范围、类型化参数、用途/stage 与单队列抽象屏障基线已经实现；文本/JSON dump、RHI 映射、Vulkan GENERAL 映射和资源绑定录制入口已实现。Bloom 与完整后处理链均有默认关闭的图执行路径，包含合成、FXAA/upscale、acquire 帧资源快照、截图后的最终交接及按 generation 的提交状态发布。场景主链尚未迁移，旧路径继续保留。实际接口见 [CPU 图核心](../../render_graph.md)，本文其余后续接口仍属设计。

用户目标：逐步取得接近 UE RDG 的声明驱动、依赖分析、无用 pass 裁剪和自动同步收益。资源创建位于图外，方便场景、视图和多张图共享；保留现有机制，直到对应功能完成替换并验收。CPU 图核心和后处理接入不要求先完成场景快照或 GPU 几何簇。

## 1. 已实现的基础与本次选择

`source/engine/resource_cluster/resource_cluster.cppm` 是独立 engine 子模块，通过公共 RHI 创建 buffer、image、sampler 和 image view。`resource_lease<T>` 共享一个 RHI 工厂引用，view 保留父 image，最终释放资源后再释放 core。

簇是 append-only、单写者对象；支持同设备 `share(lease)`、持有名称和空簇创建。runtime 提供 `create_resource_cluster(name)`，并可在成功 acquire 后、submit 前调用 `retain_resource_cluster_for_frame(cluster)`，保留至对应帧槽再次完成 acquire。普通簇析构不等待 GPU。详见 [资源簇使用约定](../../resource_clusters.md) 和 [首版实施记录](../../reviews/2026-10-15-resource-cluster.md)。

| 路线 | 结果 |
| --- | --- |
| 给固定 passchain 补一份屏障列表 | 可做迁移验证工具，不能作为最终渲染图 |
| 图引用簇提供的资源，编译访问、依赖、裁剪和同步 | **采用**，复用已实现的所有权基础 |
| 图创建临时资源，再由图内池和 extraction 管理所有权 | 与用户选择重复，首版不采用 |

因此不提供 `graph.create_texture/create_buffer`，也不以 extraction 取得资源所有权。图输出资源在构图前已有 owning lease；图交付的是执行计划和提交后的状态。资源池、延迟创建、图内复用和显存别名均为后续独立优化，不列入首轮可用门槛。

参考本地 UE 5.8.2 的 `RenderGraphBuilder.h` 中 RegisterExternalTexture、AddPass、QueueTextureExtraction，以及参数驱动访问、依赖、裁剪和屏障机制。deren 借鉴收益，使用自己的 RHI 和模块；不复制 UE 的创建、提取接口、参数宏或对象体系。

**图库选择：** 使用 vendored Boost.Graph 1.92.0 作为通用图存储、反向/过滤视图和遍历基础，不再自建通用图库。最小接入已通过实际 utility/vessel、无异常和 C++ module 探针；依赖固定在 `source/third_party/boost`，公共图接口不暴露 Boost 类型。内容版本、资源范围、依赖生成和屏障仍由 deren 实现。稳定调度保留项目的 Kahn 策略以保证登记顺序优先，环返回正常错误，不调用会抛 not_a_dag 的 topological_sort。自定义 selector、图属性分配及显式遍历工作区的适配边界见该目录 README；不能默认所有 BGL 算法均使用 mimalloc。

## 2. 各层职责

| 部件 | 负责 | 不负责 |
| --- | --- | --- |
| `resource_cluster` | 资源创建、共享与批次所有权 | pass 顺序、内容有效性、layout、GPU 完成判断 |
| 场景/视图/后处理资源对象 | 持有簇及具名 lease，按配置与 extent 创建或整体替换 | 直接决定图内屏障 |
| `render_graph::builder` | 本帧资源引用、参数快照、pass 和输出根 | 创建 GPU 对象 |
| `render_graph::compiler` | 规范身份、内容版本、裁剪、依赖、排程、使用区间和抽象同步 | 调用 core、录制或提交 |
| 图执行器 | 解析已提供的 view/绑定，降低屏障，录制并收集提交保留项 | 再建一套资源工厂或私自提交 |
| runtime | acquire、帧槽保留、提交、提交结果和 present | 推断 shader 中未声明的资源使用 |
| 旧 registry / generation / snapshot | 继续服务未迁移路径及其在途所有权 | 强制所有图资源追加全局 resource_id |

sampler 不参与内存读写 hazard，但其 lease 及 descriptor/管线依赖仍需保留。资源簇也不是 allocation 或同步范围：两个簇共享的同一 image 必须归一到同一资源身份，不能按 cluster 地址隔离 hazard。

## 3. 多场景、多视图和多图

一个场景、一个视图或一个簇均不强制对应一张图。推荐以一次 runtime 帧提交为图边界：多个场景/视图通过 `add_scene/add_view/add_post` 一类构图函数往同一个 builder 添加 pass，各自返回局部 ref，最后由合成 pass 消费。同一几何、材质或共享阴影可被多个簇/pass 持有；依赖按实际对象及范围计算。

离屏任务、独立更新频率或明确提交边界可以使用多张图。图 A 输出与图 B 输入共享同一 owning lease，显式交接最终状态、内容有效范围和就绪条件。B 在构图/录制前取得与实际提交顺序一致的交接记录；首版不跨图自动调度。同队列有序提交仍需正确内存依赖；不同队列不能只传 lease。

首版执行单 graphics queue。graphics/compute/copy 表示工作类型，不承诺独立硬件队列；跨队列同步后续按真实调用点扩展。

## 4. 图内引用与资源绑定

拟定命名空间 `deren::engine::render_graph`，核心类型为 `texture_ref`、`buffer_ref`、`pass_ref`、`builder`、`compiled_graph`、`prepared_graph`、`recorded_graph`。ref 包含图 incarnation 和局部索引，默认无效；禁止跨图/跨帧复用。名称只用于诊断，不参与唯一性。

图资源分两层：

- **编译记录**：值类型描述、规范物理身份、初始状态、内容有效范围、view/range。编译器只读这些数据，不调用 RHI；CPU 测试可直接构造。
- **执行绑定表**：拥有实际 lease、view、descriptor 及其他依赖的固定表。索引与编译记录对应，prepare 验证设备、描述和范围。编译产物离开绑定表不能独立执行。

`import_texture/import_buffer` 将图外资源登记为图内 ref；import 指参与本图分析。新建但未初始化的 image 同样 import，明确 `contents=undefined`，不能默认视为有效内容。

正常绑定接受 owning lease。当前簇没有枚举/反查成员能力，构图函数接收明确 lease/资源包，不要求图从簇中猜测资源用途。image view 是 parent image 上的范围和绑定，不作为独立物理资源。所需 view 在图外通过簇创建，prepare 只校验和解析。

规范身份为设备域内的实际 image/buffer 对象；lease 保持对象存活，避免指针回收复用。重复 import、不同簇及同 image 的不同 view 关联到同一对象；不同对象即使名字或旧 resource_id 相同也不能合并。重叠范围的初始状态/有效性矛盾报错。不支持不同 RHI 对象的显存别名。

交换链和旧 registry 暂不能制造 `resource_lease`。专用适配绑定携带实际对象、父 image/范围及真实 owner token：旧路径保留 generation/snapshot，交换链使用 acquire 身份和 runtime 重建/帧完成协议。不得用空 deleter shared_ptr 冒充所有权；无法证明保留期的绑定拒绝 prepare。不为此新增簇的 raw-pointer adoption API。

## 5. 图外资源组织与 resize

例如 `post_resources` 拥有一个簇及四级 Bloom image/view lease，记录 extent 和格式。它在图外确保资源就绪，向构图函数传递具名字段；关闭 feature 可在创建资源包前判断，无需请求其资源。

resize 或描述变化时创建候选资源包，全部成功才替换当前包；失败保留旧包，但 extent 不兼容时应跳过/报错，不能盲用。旧帧 lease 和提交保留项保持旧资源存活。簇不提供 clear、强制 seal 或通用 snapshot；builder 固定本次绑定和参数，并不冻结整个簇。

**首版代价：图外已创建的资源不会因 pass 裁剪撤销创建。** first/last use 用于计划、诊断和后续优化，不授权提前释放在途资源，也不代表已经池化。后续确需延迟创建时，在图外设计资源请求/准备层，再消费编译使用集合，不恢复 graph.create。

## 6. Pass 参数与访问声明

`add_pass(name, parameters, workload, record_callback)` 拥有参数和录制状态。资源字段使用 sampled、storage read/write/read-write、颜色/深度附件、transfer、indirect/vertex/index 等用途包装，携带 ref、view/range、stage 和内容操作。

C++ traits/visitor 从参数生成唯一的规范访问列表，执行时同一参数解析为本 pass 的资源视图；不另公开必须同步维护的读写数组。CPU 核心已支持纹理/缓冲区的内容操作包装、const noexcept 成员 visit_resources、嵌套/数组/optional/span 遍历，以及按值保留的只读参数快照。访问字段已携带用途/stage；执行解析仍待补齐，不建设通用反射或 UE 参数宏。

CPU 标量、feature 和实际资源集合在构图时固定。GPU 地址、bindless、间接参数和附件同样声明实际目标；持有 heap 不能替代声明其指向的可写资源。管线、sampler、descriptor、上传存储也必须进入执行保留项。

示意流程（除资源簇接口外均待实现）：

```cpp
// 图外：post_resources 已创建 image/view，并处理创建错误。
auto hdr = graph.import_texture(scene_resources.hdr_binding(), hdr_input_state);
auto levels = import_bloom(graph, post_resources, bloom_input_states);
auto output = graph.import_texture(output_resources.target_binding(), output_input_state);

add_bloom(graph, hdr, levels, settings); // 仅添加 pass，不创建资源
add_composite(graph, hdr, levels, output, settings);
graph.export_state(output, final_sampled_state); // 输出根 + 最终状态要求
```

Bloom 关闭时构建不读取 levels 的分支或明确有效的 fallback，不能仍声明必读却填空 ref。多次 dispatch/copy 之间存在依赖时优先拆 pass；必要内部 barrier 单独声明人工边界。pass 前 barrier 不能解决 pass 内非法 feedback。回调隐藏访问必须审计，不宣称 C++ 类型系统能阻止任意外部访问。

## 7. 内容版本、依赖与裁剪

公开 ref 不要求手写 SSA。pass 登记顺序确定每次读取看到的先前写入；编译器内部建立范围化内容版本，再按依赖确定性排程。

- read：消费当前内容，活跃路径缺少有效内容时报错。
- discard write：产生新内容，不消费旧值；必须确实 clear 或完整覆盖声明范围。
- preserve write、attachment LOAD：消费旧内容，再产生新版本。
- read-write：同时消费旧版本并产生新版本。
- attachment DONT_CARE/store discard：对应内容变无效，后续不能视为可读。

A1 先支持完整资源范围；A2 覆盖 image mip/layer/aspect 和 buffer 字节区间，完成后才接入生产。不按单像素追踪；只写部分像素不能声称整个 mip discard 初始化，应先 clear 或 preserve 有效内容。范围归一化检查越界、零长度和加法溢出。

**值依赖与物理 hazard 分开。** 从输出根沿值依赖裁剪，再在存活 pass 上建立 RAW/WAR/WAW 和状态约束。discard 覆盖前的死写不能因 WAW 错误保留；仍存活的读者必须在后续覆盖前完成。

根包括 present/export_state、readback、显式外部写入和必须执行的副作用。导入/持有 lease 本身不构成根，写入共享对象也不能隐式推断外部需要；普通 GPU pass 默认可裁剪。`order_after(pass, predecessor)` 只表达顺序并检查环，不自动保留前驱副作用。

所有节点检查结构合法性；内容有效性在活跃路径检查。稳定拓扑排序在无依赖时优先登记顺序，便于图像回归。

## 8. 编译、准备和录制

```text
图外准备资源包，固定本帧场景/配置和输入状态
  -> 构图：登记 lease 绑定、参数、pass 和输出根
  -> 身份/范围归一化，生成内容版本和值依赖
  -> 裁剪，建立存活访问物理依赖，稳定拓扑排序
  -> 使用区间、抽象屏障和最终状态要求
  -> prepare：校验已有绑定/视图，准备 descriptor 与保留项
  -> record：屏障和 pass 命令
  -> runtime 保留、提交，按真实提交结果发布状态
```

`compile() -> expected<compiled_graph, graph_error>` 是纯数据操作。prepare 不创建 buffer/image/view，只解析已有资源并准备绑定；必要 descriptor 分配沿用现有缓存/所有权。record 返回可检查结果。现有 `frame_pass::record` 返回 void，适配必须补足错误传播，不能把提前 return 当成功。

builder 编译后冻结；计划、绑定和参数在 prepare/record 期间不变。回调不借用随后可变的 UI/scene 数据。错误包含 pass、资源、范围和来源，覆盖错图 ref、无效范围、输入矛盾、未定义读取、非法 feedback、环和不支持的访问组合。

沿用 vessel、expected 和无异常风格。CPU OOM 继续 fast_panic；图错误与 RHI 失败正常返回，不将 OOM 设计为可恢复 expected。

## 9. 同步与状态交接

图 use 表达 access、stage、抽象用途/布局和范围，engine 不出现 Vulkan mask。前端布局与后端原生布局允许不同；Vulkan 将普通状态映射为 GENERAL，保留 UNDEFINED/PRESENT 例外，保留顺序、可见性、内容保留/丢弃及范围语义。图初始/最终状态仍是前端交接状态，不当成 VkImageLayout 数值；后端/提交协调层负责真实对象的原生状态。GENERAL -> GENERAL 仍可能需要写可见性；read -> read 仅在用途/状态兼容时合并。采样、storage、attachment、read-only depth、transfer、indirect/vertex/index 和 AS 访问逐项审计。

`source/promise/rhi/rhi.api_core.cppm` 的 image_use、buffer_use、image_barrier、buffer_barrier、barrier_group 不预设覆盖所有组合。执行接入前产出用途/范围到 RHI 的映射表，按首段真实需求扩展；不能表达时明确报错或保留人工区域，禁止省略屏障。跨 DLL 变更按 contract 升 ABI，内部图类型不影响 ABI。

簇/lease 不存 layout、有效内容或完成条件。外部状态记录由提交协调层持有，按实际对象/acquire 代及范围索引，包含状态、内容有效性、提交序号和就绪条件。输入可来自同队列已提交尚未完成的结果，但必须保证队列顺序；不要求 CPU 每帧 wait_idle。

当前基线仅接受覆盖整个导入对象的统一初始状态；混合输入状态需外部归一或后续范围状态入口。最终状态按范围输出，保留写者与读者的保守同步前沿；只读布局转换无逻辑写者时，目的 scope 覆盖全部已支持图像 stage。输出目标状态/epilogue 尚未提供；同步编译对存活 invalidate 返回不支持。

host flush/invalidate、上传就绪、WSI 和跨提交完成条件仍为外部协议，图内 barrier 不能代替。resize 新对象和新的交换链 image 不继承旧对象状态；A 到 B 的最终状态通过图外记录连接，不存进簇。

人工/编译同步所有权划分到资源范围的边界，人工区域明确入口/出口状态和 owner。同一边界只有一个屏障负责人，不能依赖双方重复 barrier。

## 10. 提交保留、历史和失败

binding 拥有 lease 保证构图/录制期间生命周期；GPU 保留须另外登记。执行适配器从存活访问收集 lease 和辅助依赖，形成提交保留包。

资源部分可复用现有机制：图外创建空提交簇，逐个 `share()` 实际使用的 lease，录制前调用 runtime 的 `retain_resource_cluster_for_frame()`。这不创建 GPU 对象或修改源簇。descriptor、管线、上传存储和旧 snapshot 由提交包对应 owner 保留；接入前提供其明确的帧槽保留入口，不能假定 share 支持任意对象。

交换链使用已有 acquire/重建协议，旧 registry 保留真实 snapshot。首版可保守保留整个源簇/辅助资源包，不能声称裁剪后所有未使用资源都会释放。runtime 成功重新 acquire 对应槽后释放保留簇，失败帧保守持有，shutdown 先 wait_idle。直接 RHI 提交/异步上传不由此覆盖。保留登记失败则不录制/不提交；局部录制失败不能提交半图。

所有权与历史有效性分开。TAA 资源已在图外持有，图输出候选最终状态和历史提交记录，不需要 extraction ticket 取得 lease。历史指最近一次成功提交的渲染结果，不指 simulation tick 或 frame slot。

1. 编译/准备/录制失败：不提交该图，不发布状态或新历史。
2. 提交失败：不推进历史，处理 RHI 失败语义；device lost 使相关记录失效。
3. 提交成功：登记真实序号/完成条件，发布后续同队列任务可用状态及历史。
4. 提交后 present 失败：不能撤销状态和资源保留，WSI 重建单独处理。

接入前核对/补充 runtime 结果表达，区分 submit 失败和 submit 成功但 present 失败；不能只按整帧 status 是否 proceed 发布。回退旧路径仅允许在尚无提交副作用且可安全重录时，禁止部分提交后重复整帧。

## 11. 分阶段交付与旧机制退出

| 阶段 | 交付 | 验收 |
| --- | --- | --- |
| 0：资源簇 | 已实现 engine 模块、lease、共享、帧槽保留 | 29/29 CTest 和资源簇 ASan/UBSan；生产 pass 尚未迁移 |
| A1：CPU 图核心 | 已实现局部 ref、完整资源访问、内容版本、根/裁剪、物理依赖和稳定排程 | CPU 测试覆盖死写、preserve/read-write、错图 ref/环、2 万 pass 深链；无 RHI 副作用 |
| A2：范围与参数 | 范围、参数、用途/stage 和抽象同步基线与文本/JSON dump 已实现 | 已验证范围裁剪/有效性及相交 hazard；已覆盖 GENERAL 写可见性、读范围/stage 合并及只读布局转换 |
| B：执行与提交 | lease/旧资源适配、已有 view 解析、RHI lowering、提交包和状态发布 | 设备校验、失败不发布、在途保留、present 失败语义、零 native 越界 |
| C：Bloom 子图 | 图外 post_resources、四级 Bloom、composite 人工交接，新旧可切换 | 图像一致，开关/resize/不同槽正确，同步验证无新增错误 |
| D：扩大覆盖 | composite/Output、TAA、Lighting/GBuffer、阴影、scene/Gaussian/变形/AS 分批 | 绑定、地址/heap 隐式访问、历史和同步分别验收 |
| E：测量优化 | 图外复用/延迟准备、图内复用、多队列、并行录制、显存别名单独设计 | CPU/GPU/显存数据；不作为 A-C 可用的前置条件 |

首段针对 `source/engine/pass/post.cppm` / `post.cpp` 四级 Bloom：HDR -> prefilter -> 三次 downsample -> 四级 sampled 输出，composite 显式消费。曝光目前是 push constant，不虚构曝光 pass。composite、FXAA/upscale、UI、截图和 present 初期人工管理；UI 在 rendering instance 内，不能任意插入图 transition。

每段按“声明验证 -> 可切换执行 -> 回归验收 -> 成为默认 -> 删除对应旧分支”推进。验证模式保留旧屏障，不发图屏障；新执行模式只对接管边界发图屏障。部分声明覆盖不代表整帧验证正确。

删除须同时满足：生产资源已迁移、声明覆盖实际访问、图像/同步回归通过、resize/失败/在途生命周期通过、无剩余调用者。先删具体旧分支和字段；最后一个消费者退出才删 registry/passchain/runner。新增图目录时不顺手 remove 旧机制。

## 12. 文件边界、测试和下一步

新模块位于 `source/engine/render_graph/`，按实际依赖拆分 types/access、builder、compile、parameters、execute；不新增 resource_pool。纯编译模块不 import runtime 或 Vulkan。全局 resource_id 继续服务旧路径，不承载新图局部身份。

新增 `source/tests/test_render_graph.cpp` 并接入 CMake/CI。B 扩展 probe/runtime 注入测试，C 复用 `scripts/windows/check_render.ps1` 等截图和验证工具。

- 内容/裁剪：未定义读、discard 死写、preserve/read-write、LOAD/CLEAR/DONT_CARE、禁用 producer、显式输出和副作用。
- 身份/范围：错图 ref、重复 import、跨簇同对象、多 view、不同对象同名、重叠/越界/溢出。
- 调度/同步：稳定顺序、环、活跃读者后的覆盖、RAW/WAR/WAW、GENERAL 写可见性、已支持工作类型交接。
- 生命周期：簇替换、两帧在途、旧 registry/WSI owner、descriptor/上传保留、未完成对象不复用。
- 状态/失败：编译/录制/提交失败、submit 成功但 present 失败、两图顺序交接、历史与槽区别。
- 生产：Bloom 开关、resize、frame slot 与 swapchain image 不同编号；扩大覆盖时运行固定场景和 Gaussian CPU/GPU 回归。

首版文本/JSON dump 已展示身份、参数访问、粗粒度裁剪原因、依赖、范围、活跃执行使用区间、屏障和计划最终状态；错误也可独立 dump。用户参数值不做反射，64 位整数用十进制字符串保真。RHI 审计见 [映射审计](../../render_graph_rhi_mapping.md)：前端布局是抽象语义，无需与后端原生布局逐值相同；Vulkan 可映射为 GENERAL 并保留全部依赖。独立 scope 转换已实现，旧角色对及单一 stage_hint 仍不能承载全部图前沿；新数据已通过可选 scoped_synchronization 接入 lease 绑定和录制入口；旧 pass 尚未迁移。记录构图/编译/录制时间及活跃 pass 数，未测量前不缓存跨帧图结构。

**A1、A2 范围/参数与单队列抽象同步基线已落地，资源绑定录制、Bloom 及完整后处理链均已接入可选路径。** 范围按访问边界稀疏切分，已支持 mip/layer/aspect、字节区间、局部内容版本与输出根；compile({.synchronization = true}) 显式生成范围屏障与最终状态；RHI scope 转换与 Vulkan 未绑定模板、真实资源录制已接通。规范身份使用稳定的 VMA allocation record 地址，覆盖去重别名；帧输出使用 acquire 快照身份。后处理已完成固定场景图像对照与窗口缩放同步验证，后续迁移场景主链，继续保留旧机制。

