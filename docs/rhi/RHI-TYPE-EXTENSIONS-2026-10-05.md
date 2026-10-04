# ABI12：接口身份与通用 heap 扩展

本轮接续 ABI11 `e205122`，实现 RHI 类型标识和第一批通用扩展迁移。它是正式动态后端的契约准备工作，产品目前仍使用静态 Vulkan 后端，不能按完整动态后端验收。

## 设计取舍

对象身份采用 RHI 定义的 `enum class interface_type : uint32_t`，通过共同根 `object` 的只读 `type()` 查询。各已有接口构造时自动设置固定 `interface_id`，派生后端不手写编号。编号只能追加，不能复用。与后端自行解释的 `uint64_t` 相比，这能让引擎和后端对接口身份有相同理解。

类型身份与输入参数标签分开：`interface_type` 标识对象实现的接口，`structure_type` 标识请求结构。请求以 `structure_header { s_type, struct_size, next }` 开头，默认初始化即可产生当前版本的标签和大小。

`type()` 用于调试和常规接口错配检查，不验证对象存活、具体实现布局或设备归属。有效对象和匹配 ABI 是调用前提；不能用它证明任意指针安全。共同根不能用于 `delete`，既有 `release()` 与虚析构路径保留。

## 能力查询

每个已定义扩展接口固定实现 `kind()`，并标记为 `final`。调用方可以使用类型化查询：

```cpp
namespace rhi = deren::promise::rhi;
auto* heap = rhi::query_extension<rhi::descriptor_heap>(core);
if (heap == nullptr || !heap->ready()) {
    // 此后端当前不能提供heap服务，按调用方的能力要求处理失败。
}
```

查询先检查 `kind()` 和 `type()` 再转换；不使用 RTTI。后端只在真实 heap 已就绪时广播 `descriptor_heap`。probe 后端只广播它实际实现的 `device_address`，移除了原有无法提供录制服务的空 heap 实现。

## 从 Vulkan escape 移出的服务

`descriptor_heap` 提供七个服务：`ready`、`properties`、`bindings`、`write_image`、`write_buffer`、`bind`、`push_data`。属性和 binding snapshot 返回整数值结构，不暴露后端容器或拥有型对象。四个操作采用带标签的参数结构并返回 `rhi::error`。

`vulkan_escape` 删除对应的七项 heap 服务，继续保留确实需要 Vulkan 的原生句柄和扩展名称查询。其他未实现的能力未在本轮迁移。

普通图像写入示例：

```cpp
rhi::image_view_desc view{};
view.role = rhi::view_role::sampled;
view.layer_count = 0; // 从base_layer开始的全部剩余层。
view.mip_count = 0;   // 从base_mip开始的全部剩余mip。
rhi::heap_image_write_info request{};
request.offset = destination_offset;
request.resource = &image;
request.view = &view;
request.type = rhi::descriptor_type::sampled_image;
rhi::error const result = heap->write_image(request);
// result必须由调用方检查；调用和GPU使用期间保持image有效。
```

普通路径验证图像来自同一个 core 的活跃资源集合，再读取后端实现。它检查用途及子资源范围，并从真实图像描述生成 2D、2D array 或 cube 视图。IBL 的 environment cube、irradiance cube 和 BRDF LUT 已使用这条路径。

buffer 请求暂时仍传设备地址与大小，地址的有效性和设备归属由调用方保证；本轮并未把全部 buffer 资源操作迁移成通用对象接口。

## 明确保留的原生过渡参数

仍依赖 Vulkan 的调用以 `next` 携带 `vulkan_heap_image_info` 或 `vulkan_command_buffer_info`。它们名字和字段明确是 Vulkan 参数，不把原生枚举和裸句柄包装成假通用类型。

- native image 记录保留 flags、swizzle、格式、视图类型、aspect、mip/layer 范围和 layout，并携带所属 `api_core`。
- native command 记录携带所属 `api_core` 与借用的原生命令缓冲。主、次命令缓冲保持原录制点和继承信息。
- 普通请求与 native 参数不能同时提供；错误 context、空句柄和短结构会被拒绝。
- 未知 `next` 或第二层链返回 `unsupported`；不会静默丢弃扩展链。
- 请求、范围、数据和 `next` 只借用到同步调用返回，不发生跨边界释放。

`context` 只能检查调用方填写的上下文是否匹配，不能认证裸 Vulkan 句柄。原生资源同设备、有效且处于正确录制/使用状态，仍是调用方前提。

## 审查时修正的参数边界

对照本次使用的 Vulkan-Headers 1.4.357 `registry/validusage.json`：

- `VUID-VkResourceDescriptorInfoEXT-type-11210` 不允许直接写入 combined image sampler 和两个 dynamic buffer 类型。通用枚举保留其语义，Vulkan 实现统一返回 `unsupported`。
- `VUID-VkPushDataInfoEXT-offset-11418` 与 `data-11419` 要求偏移和长度都是 4 的倍数。`validate_heap_push_range` 同时验证非空、对齐和设备上限，RHI 入口与底层录制都调用它。
- 地址、长度和 heap 范围使用先比较后相减的检查，避免加法溢出。
- 底层 bool 失败统一返回 `operation_failed`；没有精确错误来源时，不虚构 `device_lost`。

## ABI 与验证边界

共同根新增成员改变对象布局，heap 服务迁移改变既有虚表，因此 ABI11 → ABI12。引擎、后端、probe 和其他插件必须一起重编译，旧 DLL 应在创建对象前被 ABI 握手拒绝。C 导出入口数量未增加。

本轮只做编译、链接与归档边界检查；依照用户要求，没有运行测试程序、CTest、窗口或 GPU。编译时断言覆盖标签、短结构、未知链、较长尾部、推送对齐/范围以及非法描述符映射。动态加载和真实 GPU 验收留给异机。

本地构建使用 clang64 22.1.7、CMake 4.3.3、Vulkan-Headers 1.4.357，Release、`--parallel 2`。配置 `VR_NATIVE_ARCH=OFF` 仍使用 `-march=x86-64-v3`，包含 LTO；异机 CPU 和工具链需要满足对应要求。

## 后续工作

1. 异机先运行 probe 的静态/动态加载与 ABI 拒绝检查，再验证实际 Vulkan heap 写入、绑定、push 和 IBL 图像。
2. 补齐 runtime 的错误传播：旧 void bind caller 及部分 push caller 仍忽略返回结果。
3. 继续消除剩余后端符号依赖，达到应用消费者边界为零后再切换正式动态加载。
4. 另行完成共享 GLFW、窗口所有权和正式 DLL 导入/导出验收；本轮未改变现有静态 GLFW 配置。

资源必须在调用和 GPU 使用期间保持存活，core 必须覆盖其资源寿命。活跃集合不是并发 `write_image/release` 的寿命锁；本轮没有解决资源外逃后 core 已销毁的问题。
