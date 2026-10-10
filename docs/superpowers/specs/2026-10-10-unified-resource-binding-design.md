# 统一资源绑定设计草案

日期：2026-10-10。状态：供讨论的设计方案，尚未实施。

后续讨论已改为“固定 16 槽、PBR 五槽前缀、管线自由后缀、assign/load 接口”。
最新接口与 Vulkan 实现设计见 [Texture group 设计](2026-10-10-texture-group-rhi-vulkan-design.md)。
本文保留作为路线比较参考；固定九槽、material_id 优先绑定等建议不再是当前选定方案。

## 1. 目标与推荐方案

让 engine 表达“这一组资源按什么顺序供 shader 使用”，由 RHI/backend 负责描述符模型、物理槽位、写入与生命周期。
Vulkan descriptor heap 与无界描述符数组使用相同的材质、绘制参数和 shader 业务代码。

推荐：**有类型的资源句柄 + 按材质组织的资源组 + 后端专用 shader 访问层**。
第一版资源组使用连续的资源引用列表；实际描述符可以分散并共享。
物理连续描述符范围作为后续可选优化，不要求每个 mesh 永久占用固定大小的物理范围。

这里“连续”有两种不同含义：

- 逻辑连续：材质的底色、法线等引用按固定顺序放在同一列表。所有实现必须支持。
- 物理连续：对应描述符真的占据 heap/数组中的相邻位置。可选，不能成为 engine 的隐含要求。

成功标准：相同模型在两种绑定模式下使用相同材质槽位约定并产生相同输出；engine 不计算 heap 字节偏移，
不读取 heap 的 GPU 地址，不为某个后端构造描述符；资源在 GPU 使用期间不会被覆盖或回收。

## 2. UE 参考与适用范围

参考本机 `D:/UnrealEngine-5.8.2-release`，Build.version 为 5.8.2。

| UE 实现 | 借鉴内容 | 本项目的选择 |
| --- | --- | --- |
| FRHIDescriptorHandle | CPU 句柄包含描述符类型与索引 | 使用有类型的逻辑句柄，隐藏物理槽位 |
| FRHIDescriptorRange | 连续范围；shader 数据为 StartIndex、Count | 保留可选的直接范围实现 |
| FRHIResourceCollection | 数量与资源索引列表；集合持有资源 | 第一版采用相似的引用列表语义 |
| BindlessResources.ush | 统一资源加载入口，平台层决定实现 | Slang 公共入口与后端访问实现分离 |
| FVulkanBindlessDescriptorManager | 封装 heap/buffer 分配、更新与映射 | 把物理布局和兼容策略放进 backend |

UE 的 DescriptorRange 明确标为实验功能；本次查到的具体 RHI 范围实现位于 D3D12，不能视为各后端均已完成。
Vulkan 的 ResourceCollection 使用通用索引列表实现。这里借鉴边界与语义，不复制 UE 整套参数、编译或分配系统。

主要源码：

- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h:1397`
- `Engine/Source/Runtime/RHI/Public/RHIDescriptorRange.h:8`
- `Engine/Source/Runtime/RHICore/Internal/RHICoreResourceCollection.h:57`
- `Engine/Shaders/Public/BindlessResources.ush:104`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.h:19`
- `Engine/Source/Runtime/D3D12RHI/Private/D3D12DescriptorRange.cpp:50`

## 3. 为什么先用引用列表

| 路线 | 优点 | 代价 |
| --- | --- | --- |
| 每 mesh 固定物理范围 | base + slot 直接访问 | 共享材质重复分配；大小上限；空洞；多材质 mesh 仍需拆分 |
| 每材质连续物理范围 | 保留直接访问，材质可共享 | 同一纹理可能复制描述符；范围扩展/碎片/流式更新较复杂 |
| 每材质连续引用列表，推荐 | 纹理与材质均可共享；布局固定；跨后端易统一 | 加一次资源索引读取；需要管理小型引用表 |

当前 Material 已经保存五种 PBR 与四种 toon 纹理索引。这与第三条路线接近，适合渐进迁移。
额外读取的实际成本需要测量，不能预先宣称与直接范围性能相同；确认热点后再增加范围实现。

## 4. 所有权与模块边界

```text
mesh / primitive → material_id → material.resource_group
                                  ↓
                         RHI 逻辑资源组与句柄
                                  ↓
                     backend 物理描述符与 GPU 引用表

shader 材质逻辑 → 公共资源访问函数 → heap 或数组实现
```

**engine**：定义材质/渲染 pass 的资源用途、顺序、默认资源和功能标记；创建资源组并引用它。
mesh 的每个 primitive 选择自己的材质，同一材质可供多个 mesh 使用。pass 也可以拥有独立资源组。

**promise.rhi**：定义资源类型、句柄、组布局、创建/更新/释放、能力查询和命令绑定语义。
约定所有权、错误行为与 GPU 可见版本，不携带 Vulkan 类型或 heap 地址。

**backend**：分配物理描述符，把逻辑句柄翻译为 shader 所需索引，管理 GPU 引用表、同步与延迟回收，
提供相应 shader 访问实现。后端不可将不支持的调用实现为空操作。

**shader 公共层**：提供加载资源和采样函数，PBR、toon 等业务代码只依赖这些函数。
底色、法线等语义属于材质层；RHI 只知道 sampled_image_2d 等资源类别。

## 5. 接口语义草图

下列为拟议语义，不是最终 C++ ABI 声明：

```text
resource_handle = 有类型的、不透明的 CPU 资源引用
resource_group_layout = 槽位类型、容量、是否允许默认资源
resource_group = 按布局顺序保存资源引用的对象

create_group(layout, resources) → group 或明确错误
replace_group_resources(group, first_slot, resources) → 更新一个后续 GPU 可见版本
release_group(group) → 释放 CPU 所有权；GPU 使用结束后再回收
```

创建与更新时校验类型、范围、后端归属和布局兼容性；失败不得造成半写入。
资源组持有资源/视图引用，不能依靠调用者临时数组延长资源寿命。
纹理引用指向 image view 而不只是底层 image：视图格式、维度、子资源范围影响描述符。

第一版组内使用一种资源类别：材质纹理组为 sampled_image_2d。
采样器使用独立的有类型句柄/表，第一轮保持现有共享 sampler 行为。
cube、storage image、buffer 等通过独立类型的资源组逐步迁移，不要求混合类型连续范围。

GPU 访问结构建议为：

```text
group_table[group_id] = { reference_base, count }
resource_references[reference_base + local_slot] = descriptor_index
```

`group_id` 是后端提供的 GPU token，由 RHI 写入 GPU 数据；CPU 资源组对象不是裸 token。
token 的有效期覆盖已记录/提交的 GPU 工作，释放后必须等这些工作结束才可复用。
V1 不在 shader 中读取 CPU generation，也不假设 CPU 逻辑句柄的整数值等于物理描述符索引。

未来直接范围实现可增加内部表示种类与 descriptor_base，通过公共 resolver 消化；
该优化需单独评估 shader 分支成本、混合类型布局与后端能力，不作为第一版必做功能。

## 6. 材质槽位与“激活数量”

建议材质 texture group 固定九个逻辑槽位：

| 槽位 | 语义 |
| --- | --- |
| 0 | base color |
| 1 | metallic-roughness |
| 2 | normal |
| 3 | occlusion |
| 4 | emissive |
| 5 | diffuse ramp |
| 6 | shadow LUT |
| 7 | specular ramp |
| 8 | matcap |

缺失贴图填写对应默认资源，保留现有 flags 与 toon 的启用语义；默认资源的取值按现有材质路径确定。
例如 normal 缺失不能简单用一张白色纹理替代其功能判断。

**capacity/count 与 active_count 分开**：

- capacity：分配的槽位上限。
- count：本版本资源组可索引的有效逻辑范围。
- enabled_mask：哪些材质功能启用。

固定九槽材质的 count 可以恒为 9，不必添加 active_count。
只有明确规定“有效资源总是位于前 N 个槽位”的可变列表，才能用 active_count 表达激活数量；
允许中间缺失槽位时，单个数量无法说明哪些槽位有效。

## 7. 绘制与 shader 的统一使用

绘制继续传递现有 `material_id`，材质表新增资源组 token。不要要求每次绘制上传九个句柄。
这样延迟渲染根据 material_id 读取材质、间接绘制和光追命中阶段也能访问同一资源组。
临时 pass 可以通过其 push block 直接携带一个 group token；并不强制所有绑定都经过 push constant。

公共 Slang 接口的拟议用法：

```text
texture = load_texture_2d(material.texture_group, MaterialSlot.BaseColor)
color = sample_texture_2d(texture, material.sampler, uv)
```

这层接口应保留显式 LOD、梯度、Load 和尺寸查询等所需操作；不能只统一 Sample 而漏掉 compute 等阶段。
非一致索引的处理由 shader 实现按实际调用语义正确生成，不能假定每个 subgroup 总访问同一材质。

heap 实现：resolver 取得描述符索引，后端 shader 层转换为 heap handle 并采样。
数组实现：resolver 取得同样语义的索引，访问相应类型的描述符数组。
按资源类别拆分的数组具有独立索引空间；typed handle 和类型化 resolver 防止将这些索引混用。

两条路径都要处理材质表/引用表等 buffer 访问。仅替换纹理采样函数并不意味着整个 renderer 已支持数组模式。
shader 构建增加明确的绑定模式变体；heap 的编译 capability、stride 与数组的 set/binding 布局分开配置。
pipeline 必须与 shader 变体和资源布局匹配，不在每次采样时猜测后端模式。

## 8. 生命周期与更新

资源流式更新不能覆盖仍被 GPU 使用的槽位或引用表条目。统一接口定义如下：

1. 创建/更新生成一个可供后续命令使用的资源组版本。
2. 已记录的命令继续引用旧版本；后端保留该版本的描述符、引用表和资源。
3. 后续命令引用新版本，后端在所需队列上安排上传与屏障。
4. 最后一个 GPU 使用点完成后，才可回收旧版本与槽位。

第一版优先使用已有帧完成机制延迟回收，采用版本化引用表/必要时新分配槽位。
不要把 update-after-bind 当作任意修改正在使用的描述符的许可。
跨队列迁移前先明确每个版本的完成点；不以仅等待 graphics queue 代替实际 compute/transfer 使用结束。

## 9. 能力与不支持的情况

能力查询至少区分：资源类别、容量、非一致索引支持、资源组支持、绑定模式支持。
初期 descriptor heap 与数组属于 backend 的显式选择；相同 shader 业务代码不等于各设备自动支持同等能力。
不支持时返回明确结果或在 pipeline 建立前选择有效变体，不允许渲染中途静默丢资源。

普通 descriptor set 的小规模逐材质绑定 fallback 暂不属于首轮；可在保留组语义的前提下后续增加。
AS 不纳入第一轮通用纹理资源组。现有 mapped AS 与 BDA 诊断兼容路径保持独立，
未来迁移 AS 时必须单独验证，不通过资源组抽象掩盖 ResourceHeapEXT 的既有兼容问题。

## 10. 渐进迁移顺序

这是阶段划分，尚不是逐文件执行计划：

1. **建立契约与最小双模式探针**：定义资源组语义，验证两种模式下创建、采样、更新、回收和错误处理。
   探针必须读取实际 GPU 结果，不能只依赖 CPU 日志。先证明数组路径可用，再扩大迁移。
2. **接入现有 heap 路径**：先包装现有纹理注册及索引分配，保留渲染输出和现有 Material 布局，减少同时变化。
3. **材质逻辑槽位迁移**：引入材质组 token；PBR、toon 统一按语义槽位访问，最后删除重复的旧纹理索引字段。
4. **数组材质路径**：迁移材质/引用表 buffer 与 sampler，形成一条完整且可验证的非 heap 材质访问路径。
5. **pass 资源迁移**：依次处理 IBL、shadow、G-buffer、post、compute 的 typed 资源组。
   完成这些阶段后，才能宣称 renderer 的资源绑定模型整体统一。
6. **清理与优化**：将 engine::render_layout 中的物理 heap 网格移入 backend；保留通用格式、材质语义和 push 布局。
   profile 后决定是否增加连续物理 descriptor range、组缓存或更精简的 GPU token。

shader 拆分建议：保留统一入口文件；分别提供 heap 和 array 实现；材质槽位定义单独维护。
具体文件命名在执行计划中确定，避免第一步大范围改名。
RHI 采用新增可查询接口渐进接入；是否变更 ABI 必须依据实际 vtable、创建结构和 shader 数据布局变化判断，
不预先承诺无需 ABI 更新，也不在本设计阶段修改 abi_version。

## 11. 验证与验收

- CPU：槽位类型/维度错误、越界、跨后端对象、更新失败的原子性、默认资源、重复材质共享。
- 生命周期：提交后更新/释放，延迟完成，复用槽位时确认旧命令仍使用旧资源；覆盖实际使用的队列。
- GPU 双模式：底色/法线/默认资源、显式 LOD、多个材质的非一致索引、资源组更新，读取已知结果。
- 场景：同纹理被多个材质引用，同材质被多个 mesh 引用，一个 mesh 多材质，PBR 与 toon 缺贴图。
- 图像：现有 14 个冻结场景；必要时增加实际覆盖新路径的场景，不因绑定迁移更新基准。
- 性能：比较 CPU 更新开销、GPU 额外索引读取、描述符与引用表占用；分别报告 heap 与数组结果。
- 边界：engine 与 shader 业务代码不再硬编码物理 heap stride/地址；后端日志与能力选择可追踪。

## 12. 需要确认的设计选择

推荐接受以下约定：

1. 材质/渲染 pass 持有资源组，mesh 引用材质。
2. 第一版保证逻辑槽位连续，实际描述符优先共享；物理连续范围留作后续优化。
3. 材质固定九槽，缺失资源使用默认值和已有功能标记；不新增含义模糊的激活数量。
4. 绘制优先保留 material_id，资源组信息放在材质表；pass 可直接传组 token。
5. 先实现并验证 texture_2d 与 sampler 两种绑定模式，再迁移其他资源；AS 独立处理。

若必须要求“每材质固定物理范围 + base/count 直接访问”，可以选择第 3 节第二条路线；
上层组语义保持一致，但需要额外承担描述符复制、连续分配与更新版本管理成本。
