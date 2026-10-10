# Texture group：RHI 接口与 Vulkan 实现设计

日期：2026-10-10。状态：设计草案，尚未修改产品接口或实现。
本方案采用本轮讨论确定的使用方式，替代前稿的固定九槽与 material_id 优先绑定建议。

## 1. 对外用法

```cpp
texture_group_info info{};               // 16 个槽位，默认空
info.textures[0] = base_color;
info.textures[2] = normal;
info.textures[5] = diffuse_ramp;          // 槽位 5 以后由当前管线定义

auto group = device.assign_texture_group(info);
if (!group) { /* 处理创建失败 */ }

commands.bind_pipeline(pipeline);
// 写入该 draw 的其他参数。
commands.load_texture_group(group);
commands.draw_indexed(...);
```

一次加载影响后续 draw，直到加载其他组或切换 pipeline；同一组可以复用于多个 draw。
这里只定义一个当前材质纹理组，不与 IBL、shadow、G-buffer 等 pass 全局资源混在一起。
后端可以缓存重复加载，但不能因为缓存而漏写新 pipeline 所需的参数。

## 2. RHI 接口草图

下列名称与签名为拟议接口，不是已经编译验证的声明。项目 RHI 目前使用 image 表示纹理，
所以接口写 image*；engine 的 texture 包装对象应先取得对应的 RHI image。

```cpp
inline constexpr uint32_t texture_group_capacity = 16;

struct texture_group_info {
    uint32_t struct_size = sizeof(texture_group_info);
    std::array<std::optional<image*>, texture_group_capacity> textures{};
};

struct texture_group : object {
    // 不暴露 heap 地址、描述符索引、VkImageView 或内部 GPU token。
    virtual ~texture_group() = default;
    virtual void release() noexcept = 0;
};

using texture_group_ref = std::shared_ptr<texture_group>;

// 拟追加至 api_core；成功返回共享拥有对象，失败返回 nullptr。
[[nodiscard]] virtual texture_group_ref assign_texture_group(
    texture_group_info const& info,
    error* result = nullptr) = 0;

// 拟追加至 command_buffer 的真正录制接口，仅声明一次。
[[nodiscard]] virtual error load_texture_group(
    texture_group_ref const& group) = 0;
```

保留用户提出的 optional 指针形状。nullopt 和包含 nullptr 的 optional 都规范化为空槽，
不能把两者当作不同材质状态。裸指针只在 assign 调用期间借用，创建结束后不保存这些指针。
调用期间资源必须有效，也不能与其 release 并发；接口不负责恢复已悬空的输入。

factory 的 error 输出用于精确区分 invalid_argument、unsupported、资源耗尽等；
它是可选输出，不引入异常。错误必须在开始前置为确定值。日志只作诊断，不能替代返回值。
默认参数使用户仍可写 assign_texture_group(info)。

共享指针的 deleter 由 backend 创建并调用 release()，外部不 delete 后端对象。
当前工程已经有 make_command_buffer 返回 shared_ptr 的先例；本轮不另建一套跨 DLL 对象协议。

## 3. 资源组契约

- 固定容量 16，不携带 PBR、toon 等语义标签。
- V1 只接受本 backend 创建的、具备 sampled 用途的普通 2D image；默认使用全 mip 的 sampled 2D view。
- 不接受 swapchain borrowed image、cube、2D array、storage-only image 或来自另一 device 的资源。
- 对其他 view/mip/sampler 需求，后续扩展 slot 信息，不把不兼容资源强行转换成 2D。
- 组创建后不可变。换纹理时创建新组；更新调用的原子性和 GPU 可见版本不成为首轮负担。
- 组复制 shared_ptr 只增加 CPU 共享拥有关系，不复制 16 份描述符或纹理。
- 创建必须全成功或全部回滚，不返回半初始化组。
- 全空组是有效组。load_texture_group(nullptr) 选择后端的全空组，明确清除上一次的绑定。

后端记录一个 present_mask：bit i 表示输入槽位 i 是否提供了纹理。
present_mask 不是 enabled_mask；绑定了扩展纹理并不意味着管线必须启用该功能。
实际激活规则由 pipeline 的 shader 与材质参数定义，不在 group_info 中添加 active_count。

前五槽约定为底色、金属粗糙度、法线、AO、自发光；后十一槽由管线规定。
这些是 engine/pipeline 的约定，Vulkan 只执行“第 i 槽”的资源绑定。

## 4. 保活与设备生命周期

当前 owned_image 包含可复制的 vk_image 拥有引用；复制会通过分配器 retain callback 增加引用。
owned_image_view 本身不保活 image，因此不能只缓存 VkImageView 或原 image*。

Vulkan assign 的步骤是：校验 image 来源与属性，然后复制 owned_image::owned，
再复制构造 view 所需的格式、mip、layer 等值。创建完成后，原 RHI image 包装对象可释放，
组内的 vk_image 仍然保留底层 VkImage/VMA allocation。

其他后端也必须具有等价的资源拥有引用。无法从输入 image 获得有效保活引用时拒绝创建，
不能以“调用者大概还持有纹理”为接口实现。

GPU 与 CPU 的拥有关系分开：

1. 资源组持有底层 image、view/descriptor lease 与 GPU record allocation。
2. command buffer 录制成功的 load 将 shared_ptr 加入 recording_refs，去重存储。
3. submit 建立提交拥有对象，保留命令分配与该次提交的组引用。
4. 提交完成点到达后释放提交引用；command buffer 的引用在 reset/release 且符合 Vulkan 状态规则时释放。
5. 最后引用消失后才回收组 record、描述符租约、view 和 image。

只把 shared_ptr 保存到 command buffer 不够：提交后调用者可能释放 command buffer，
submit 必须持有自己的一份引用。secondary 使用的组与命令分配也必须纳入 primary 的提交拥有关系。
每次提交独立跟踪完成点，不能用“最近一次提交结束”推断所有使用都完成。

现有 owned-command submit 使用无 fence 的 vkQueueSubmit；落实本设计时必须为这条路径补后端完成跟踪，
例如内部 fence 或合适的 timeline semaphore。frame 路径可复用实际 frame 完成机制。
不能依赖普通 draw 后调用 wait_idle 来兑现保活承诺。

设备根生命周期沿用现有资源规则：api_core/device 必须在其资源与 GPU 工作全部结束后再销毁。
shared_ptr<texture_group> 不自动延长 api_core。关闭 renderer 时先排空提交、释放命令/材质组，再卸载 backend。
若以后允许资源组跨 renderer 存活，需引入独立 device lifetime lease，不能只保留原 core*。

## 5. Vulkan 内部对象

```text
owned_texture_group
  owner / device identity
  owning_images[16]             // vk_image 拥有引用，空槽为空
  sampled_view_leases[16]       // 实际 sampled view 与对应描述符的缓存租约
  gpu_record_lease              // 内部 record id 与表中范围
  present_mask

GpuTextureGroup，拟议 80 字节
  uint texture_indices[16]      // 全部为可访问的有效 sampled2D descriptor index
  uint present_mask
  uint padding[3]
```

内部 record id 不出现在公开 texture_group 接口上。引擎把 group 对象交给 backend，
由 backend 提取其内部 id 并写入 shader 参数。
纹理索引在 Vulkan shader 实现中解释；engine 不知道其类型数组、heap 前缀或 descriptor stride。

record 结构必须由 CPU/Slang 布局检查保证大小及偏移一致；80 字节仅为此草案选定的明确布局。
固定 16 槽不要求分配 16 个新的物理纹理描述符：相同 image/view 可共享描述符 lease，
每个组只保存 16 个引用索引。缓存 key 必须包含 view 的格式、维度与子资源范围，不能只比较 image 地址。

空槽统一引用一个后端持有的有效 dummy sampled2D 描述符，不依赖 nullDescriptor feature。
shader 根据 present_mask 做材质的缺失资源处理；dummy 不意味着白色纹理能替代所有槽位的语义默认值。

## 6. assign_texture_group 的 Vulkan 流程

1. 校验 struct_size、device 状态、16 槽输入、sampled 用途与维度；输入仍有效时取得 owning image 引用。
2. 对非空槽构造默认 view；从 sampled view/descriptor cache 获得可共享租约。
3. 空槽使用 dummy descriptor，计算 present_mask，形成完整 80 字节 record。
4. 分配 record id 和 GPU 表空间，准备后端管理的上传/可见性工作。
5. 发布 shared_ptr<owned_texture_group>。任何失败倒序回滚租约与分配，设置 error 并返回 nullptr。

成功表示组可供后续录制使用，不要求每次 factory 都阻塞 GPU 上传。
第一次使用时 backend 必须保证 record 和描述符已可见；上传、flush、barrier/队列依赖由 backend 处理。
若 GPU 表扩容，旧表必须保留到所有引用它的命令完成，不能把扩容实现为覆盖正在使用的表。

容量仍受设备与后端配置限制，不承诺无限组数。描述符、record id 或上传资源耗尽时明确失败。
早期可以使用有上限的表与 free list，再按测量决定是否实现扩容。

## 7. load_texture_group 与 pipeline 约定

load 需要正在录制的 command buffer，以及已绑定且声明支持 texture group 的 pipeline。
backend 校验组属于同一 device；成功后保存组引用，写入该 pipeline 指定的组 token 参数。
加载失败不改变当前组和已录制命令。空 shared_ptr 加载有效的全空组。

pipeline 创建信息增加 struct_size 保护的 texture_group_binding 子描述：

```text
enabled
push_byte_offset               // 一个 uint32 group id，必须四字节对齐
shader_stages                  // pipeline 实际读取该参数的阶段
profile = sampled_2d_16
```

创建时核对 shader 的参数布局、profile、push 范围与设备限制。组参数不能与现有 draw 参数重叠。
heap 模式通过 vkCmdPushDataEXT 写这个 uint；数组模式通过 vkCmdPushConstants 写相同语义的 uint。
后端拥有 pipeline layout 或 heap 创建标志，引擎不选择这两个命令。

现有 scene shader 的 push block 已有模型、frame、geometry 等数据；不能直接占用 spare_lane，
也不能忽视 mesh shader 的块大小。为组 id 选择明确的新偏移，并同步修改所有共享该块的 shader 和 CPU 布局，
验收时核对有效 push size。设备不足时返回 unsupported，不能假设所有设备都提供 256 字节。
普通 draw 参数写入不得覆盖组 id；若参数写入 API 允许触碰保留范围，backend 必须校验或在 draw 前重新写组参数。

bind_pipeline 时失效绑定缓存，为支持组的 pipeline 写入全空组 token；load 后的同 pipeline draw 可复用它。
因此 pipeline 切换不会意外继承上一材质，也不会留下 Vulkan 要求写入但未初始化的 push 字节。
更复杂的“切换 pipeline 自动保留当前组”不是首轮契约。

secondary command buffer 不能假定继承 primary 的组状态；它自己绑定 pipeline 并 load。
draw_indirect 的一次命令使用当前同一组。若一批 indirect 内各 draw 使用不同材质，
需要 draw record/material table 携带 group token，这是后续显式能力，不能靠一次 load 实现。
延迟 lighting 与 RT hit 阶段同样不能依赖光栅 draw 的当前状态；在对应路径接入时保留材质表中的组引用/token。

## 8. shader 公共接口

```text
TextureGroup GetLoadedTextureGroup()
bool HasTexture(TextureGroup group, uint slot)
Texture2D LoadTexture2D(TextureGroup group, uint slot)
```

group 由当前 pipeline 的 token 解析到 GpuTextureGroup；slot 必须在 0..15。
HasTexture 检查 present_mask；LoadTexture2D 解析该槽的描述符索引。
Sample、SampleLevel、SampleGrad、Load、GetDimensions 等业务操作继续作用在返回的纹理对象上。
sampler 第一版沿用现有共享 sampler，不在组里添加隐式的第 17 槽。

PBR 与扩展槽位的默认值、激活条件写在 pipeline shader 中。RHI 不知道 diffuse_ramp 等材质语义。
实际是否发生非一致索引必须正确传递到资源索引路径，不能为了更短的指令假定索引总是 uniform。

## 9. Vulkan 两种存储模式

**Descriptor heap**：复用现有 resource/sampler heap 与 descriptor 写入服务。
view cache 返回 sampled2D 的物理槽索引，GpuTextureGroup 存该索引；backend Slang 层换算为 heap handle。
GPU group table 自身也通过后端的 buffer 访问方式读取。heap 一次按命令所需绑定，不每 draw 新建 heap。

**无界数组**：分配 sampled image 描述符数组、共享 sampler 与 GPU group table 的 buffer binding。
shader 使用相同 token/record/slot 语义，访问 texture_array[index]。
descriptor set/layout 创建、绑定、pool 与更新完全由 Vulkan backend 管理。
这不是 VK_EXT_descriptor_buffer 路径；UE 的 descriptor buffer 仅是参考，不是本轮目标。

数组路径查询并启用实际使用的 descriptor indexing、runtime array 与相应非一致索引能力。
不能把 variable descriptor count 和 update-after-bind 当作一律必需的条件：
初版可分配固定配置容量的完整数组，并以 dummy 填充未使用项；使用可变数量时其 binding 必须符合布局限制。
若在已绑定/待执行 set 上更新描述符，必须使用有效的 feature/flag 组合或版本化 set；
不存在“描述符数组永远可以直接改”的默认承诺。未使用槽位也不能在 GPU 正在读取时复用。

两条路径分别编译 shader 访问实现并建立匹配 pipeline。引擎代码使用同一个 factory/load，
但实现需要同时迁移 group table buffer 和 sampler，不能仅替换 texture Sample 就宣称双模式完成。

## 10. ABI 与实现位置

- promise.rhi：新增 texture_group 类型、info、interface id、能力信息；api_core 新增 factory，command_buffer 新增 load。
- Vulkan core：owned_texture_group、group/view/descriptor registry、GPU group table、命令与提交引用跟踪。
- Vulkan shader 实现：heap 与 array 的组解析/纹理加载实现；公共 Slang 入口保持一致。
- engine：创建/保存 shared_ptr，绘制调用 load；材质参数和管线扩展规则继续由 engine 定义。

当前 ABI 为 28。追加 factory/load 到现有 vtable 属于 ABI 变化，实施时需要升级版本并补拒绝旧 DLL 的测试。
pipeline_desc 新字段须按 struct_size 读取，旧描述默认不使用 texture group。
具体源码拆分在实施计划中确定；不为了做这个接口顺带修改全项目模块命名。

## 11. 首轮范围与测试

首轮只做 sampled2D16、共享 sampler、不可变组、普通/实例化 draw，及 Vulkan heap/数组双模式探针。
不加入可变组 update API、混合资源类型、每 indirect draw 独立组、AS 或任意设备根生命周期扩展。

必须验证：

1. 16 槽顺序、空 optional/空指针、全空组、slot15、同纹理重复出现、来源/维度/用途错误。
2. factory 返回后释放原 image 包装对象，仍可正确采样；不能仅测试 shared_ptr 的 use_count。
3. load 后释放调用者组引用，甚至提交后释放命令的 CPU 引用，GPU 仍读取正确结果。
4. 同组复用、A/B 组交替、pipeline 切换、空组清除和 secondary 录制。
5. 提交尚未完成时不能复用 record/描述符槽；每次提交与所有实际队列的完成跟踪正确。
6. heap/数组两种 shader 给出相同的已知采样结果，验证日志干净；随后扩大到现有冻结图像场景。
7. 创建失败完整回滚；不支持设备/旧 ABI/不兼容 pipeline 有明确错误。

该草案尚未通过编译或 GPU 验证；本轮只设计接口与实现步骤。
