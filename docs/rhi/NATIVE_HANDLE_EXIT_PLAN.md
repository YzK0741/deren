# 后端可移植性计划：所有图形 API 走 RHI

> **为什么要做（这才是目标）**：`deren_vulkan.dll` 是**一个**后端。要能接第二后端（D3D12、Metal、null…），
> 引擎（`vulkancorekit`：`runtime/**`、`vulkan/pass/**`、资源层、GUI 桥、加速结构/RT 模块）**不能接触任何图形 API 的
> 类型、函数或概念**——它只许调契约（`deren::promise::rhi`）。
>
> **导入表不是目的**：`deren.exe` 不出现 `vulkan-1.dll` 只是"引擎确实没有绕过 RHI"的一条机械证据；真正的判据是
> **引擎能否对着一个非 Vulkan 后端编译并通过**（§1 的 P-Gate）。为了"不链接"而链接是上一版计划的错误。
>
> 基线：`objdump -p` / `llvm-nm` 实测于 2026-10-07；既有进度见 `docs/rhi/RECORDING_FACE_REFACTOR_STATUS.md`。

---

## 0. 一句话

**引擎只认识 RHI。** 任何"图形 API 才知道的事"（设备能力、交换链、资源/视图、屏障语义、加速结构、GUI 的渲染面）
都必须由**后端**实现并在契约里表达；引擎里出现 `Vk*`/`vk*`/`vulkan_escape` 就是**契约缺了一个概念**，那一处就是待办事项。

---

## 1. 验收（按强度排序）

| # | 判据 | 怎么测 | 说明 |
|---|---|---|---|
| **P-Gate** | **引擎对着"无 Vulkan"的包含环境编译通过**：引擎的每个 TU 在**看不到 `vulkan/vulkan.h`**、**不链 `Vulkan::Vulkan`** 的条件下编译 | 新 target/option（如 `DEREN_ENGINE_NO_VULKAN=ON`）把 `vulkancorekit` 的源在无 Vulkan 头、无 loader 链接的配置下编译 | **可移植性的硬证据**：任何 `Vk*` 用法都编译失败，链接期任何 `vk*` 调用都成为未定义符号 |
| **P-Run** | 引擎能对着**非 Vulkan 后端**跑起来（已有 `tests/probe_backend.cpp` 的无 GPU 假后端 + `test_runtime_injection` 的注入路径） | 用 probe 侧 `make_core` 创建 `api_core` 并驱动运行时的启动/帧边界 | 证明"只靠契约也能跑"，而不只是能编译 |
| **P-Census** | 引擎源码零图形 API 词汇：`Vk*`、`VK_*`、`vk*`、`escape()->native_*`、`native_*_of`、`#include <vulkan/` | census 扫 `runtime/**` + `vulkan/**`（除 DLL 源与白名单） | 防回潮（新代码一写就红） |
| **P-Cover** | 契约**自足性**：§3 能力矩阵里每一项"引擎需要的能力"都有契约动词 | 矩阵逐项打勾（人工评审 + 已实现动词引用） | 这是"能接第二后端"的实质判据 |
| **P-Import**（机械证据） | `deren.exe` 导入表无 `vulkan-1.dll`；进它的对象零 `vk*` | `objdump -p`；`llvm-nm --undefined-only` | 由 P-Gate 自动带出，单独列出来是为了零成本回归 |

**允许，且不算"接触图形 API"**：
- **窗口**：`GLFWwindow*`（平台概念，不是图形 API）。统一：`create_info::native_window` 由 `void*` 改为 `GLFWwindow*`（契约前向声明 `struct GLFWwindow;`），
  删掉 `backend_loader.cppm` 里 `glfwGetWin32Window` 那层转换。
- **`vulkan_constant_init` 的 constexpr builder**：纯头文件/constexpr，不含 API 调用；但它的 `PUBLIC Vulkan::Vulkan` 要改成"只给头文件的 INTERFACE"。
- **`deren_vulkan.dll` 自己**：它当然可以说 Vulkan——它就是那个后端的实现。

---

## 2. 实测基线（"引擎在哪些地方绕过 RHI"）

**总量**：引擎侧 64 种 `Vk*` 类型、27 个 Vulkan 入口点、12 个 escape 方法（+21 个 `native_*` 包装）、`vkCmd*` 5 处。

### 2.1 链接器视角：进 `deren.exe` 的闭包里引用了 `vk*` 的对象

| 对象 | 引用符号 |
|---|---|
| `imgui_impl_vulkan.cpp`（third_party，经 `imgui` → 引擎 → 主程序） | **72 个**（`vkCreateSwapchainKHR`/`vkCreateRenderPass`/`vkCreateGraphicsPipelines`/`vkCmdDrawIndexed`/`vkAllocate*`/`vkMapMemory`…） |
| `readback.cpp` | `vkCmdCopyBuffer` `vkCmdPipelineBarrier2` `vkCreateFence` `vkDestroyFence` `vkQueueSubmit` `vkResetFences` `vkWaitForFences` —— **已无调用者**（S2 第二批判给后端后运行时不再 import 它）→ **整模块删除**，7 个引用直接消失 |
| `runtime.constructor.cppm` | `vkGetDeviceProcAddr` `vkGetPhysicalDeviceFeatures2` `vkGetPhysicalDeviceProperties` `vkGetPhysicalDeviceProperties2` |
| `runtime.cpp` | `vkDeviceWaitIdle` `vkGetDeviceProcAddr` `vkGetDeviceQueue` `vkGetPhysicalDeviceQueueFamilyProperties` |
| `runtime.frames.cppm` | `vkBeginCommandBuffer` `vkEndCommandBuffer` |
| `runtime.probes.cppm` | `vkCmdPipelineBarrier2` `vkQueueSubmit` |
| `acceleration_structure.cpp` | `vkGetDeviceProcAddr` `vkGetPhysicalDeviceProperties2` |
| `ray_tracing.cpp` | `vkCmdPipelineBarrier2` `vkGetDeviceProcAddr` |

### 2.2 三条把它拉进主程序的链接边

1. `vulkancorekit PUBLIC Vulkan::Vulkan`
2. `vulkan_constant_init PUBLIC Vulkan::Vulkan`（引擎 26 处 import 它只为 constexpr，不需要链接）
3. `imgui PUBLIC Vulkan::Vulkan` + 引擎链接 `imgui`（`imgui_impl_vulkan.cpp` 在里面）

### 2.3 类型与 escape 面（P-Census 要清空的集合，逐项见 §3 矩阵）

- 句柄类：`VkCommandBuffer`(12 文件)、`VkBuffer`(10)、`VkDevice`(9)、`VkImageView`(9)、`VkImage`(8)、`VkAccelerationStructureKHR`(7)、`VkQueue`(6)、`VkPhysicalDevice`(5)、`VkInstance`(4)、`VkMicromapEXT`(2)、`VkSampler`(2)、`VkFence`(1)
- 描述结构类：`VkBindHeapInfoEXT`(5)、`VkImageViewCreateInfo`(3)、`VkDependencyInfo`(3)、`VkMicromapUsageEXT`(3)、AS 形状族（`...BuildGeometryInfoKHR`/`...BuildRangeInfoKHR`/`...GeometryKHR`/`...GeometryTrianglesDataKHR`/`...InstanceKHR`/`...CreateInfoKHR`/`...BuildSizesInfoKHR`/`...DeviceAddressInfoKHR`/`...TrianglesOpacityMicromapEXT`）、`VkSubmitInfo`(2)、`VkRenderingFlags`(2)、`VkPhysicalDeviceProperties[2]`(2)、`VkPhysicalDeviceAccelerationStructurePropertiesKHR`、`VkPhysicalDeviceFeatures2`、`VkPhysicalDeviceMeshShaderFeaturesEXT`、`VkPhysicalDeviceRayQueryFeaturesKHR`、`VkPhysicalDeviceRayTracingPipelinePropertiesKHR`、`VkPipelineRenderingCreateInfo`、`VkTransformMatrixKHR`、`VkQueueFamilyProperties`、`VkMemoryBarrier2`、`VkBufferMemoryBarrier2`、`VkBufferCopy`、`VkFenceCreateInfo`、`VkCommandBufferBeginInfo`、micromap 全族
- 枚举/值类：`VkDeviceAddress`(12)、`VkDeviceSize`(11)、`VkFormat`(10)、`VkExtent2D`(8)、`VkDescriptorType`(4)、`VkImageAspectFlags`(2)、`VkImageLayout`(2)、`VkIndexType`(2)、`VkSampleCountFlagBits`(2)、`VkBool32`(2)、`VkCullModeFlags`(2)、`VkDrawMeshTasksIndirectCommandEXT`(2)、`VkImageViewType`、`VkPipelineLayout`、`VkResult`、`VkShaderStageFlags`、`VkBufferUsageFlags`
- escape 方法（12）：`native_device`(6 文件)、`native_image`(3)、`native_image_view`(12 处)、`native_command_buffer`(4)、`native_buffer`(4)、`native_physical_device`(4)、`native_queue`(3)、`native_instance`(2)、`native_image_format`(2)、`native_swapchain_image_format`(1)、`get_basis`(1)、`shader_group_handles`(1)

---

## 3. 能力矩阵：引擎需要的每一件事，契约在哪、缺什么、哪一批落地

**这张表就是计划的实质**。每一行 = 一个"图形 API 才知道的事"；"引擎现在的做法"是实测到的绕过方式；"缺口"就是要么契约缺概念、要么引擎在做后端该做的事。

| 域 | 引擎现在的做法（实测） | 契约现状 | 缺口 → 落地批 |
|---|---|---|---|
| **窗口** | `GLFWwindow*` 经 `void*` 传后端；`backend_loader` 还转 HWND | `create_info::native_window`（void*） | 类型改 `GLFWwindow*`；删 HWND 转换 → **X0** |
| **实例/设备/队列** | `vkGetPhysicalDeviceProperties[2]`、`vkGetPhysicalDeviceFeatures2`、`vkGetDeviceQueue`、`vkGetPhysicalDeviceQueueFamilyProperties`、`vkDeviceWaitIdle`、`native_device/native_instance/native_physical_device/native_queue` | `wait_idle()` 有；`create_info` 有；无能力/事实查询 | **`device_facts` + `facts()` + `format_is_supported()`**；队列家族/设备队列概念收进后端；**特性启用改由后端自行决定** → **X3** |
| **交换链/呈现** | 运行时已用 `wait_and_acquire()`/`frame_image()`/`present()` ✓；但自己存 `VkFormat swap_chain_image_format`，并有 `native_swapchain_image_format` | `swapchain : object`（abi 14）+ `create_swapchain(desc)`，**但 `swapchain_desc` 是前向声明、desc 被忽略** | **冻结 `swapchain_desc`**（extent/format/present_mode/image_count/flags）+ `format()/extent()/image_count()/needs_recreate()`；运行时不再存 `VkFormat` → **X3** |
| **资源创建** | `create_buffer`/`create_image` 走契约 ✓；`native_image`/`native_image_view`/`native_buffer`/`native_image_format` 仍被使用 | ✓ 有 | 删除这些 native 读取的**调用者**（见下面各行） → **X5** |
| **视图** | 引擎自己建视图：`VkImageViewCreateInfo`、`VkImageViewType`、`VkImageAspectFlags`、`native_image_view` | `image::make_view(desc)` 有 | 视图创建全走契约；`make_view` 的 desc 需覆盖 cubemap/array/aspect → **X5** |
| **采样器** | `render_resource/shared.cppm` 存 `VkSampler`×6、`native_sampler` | `sampler` 类型有 | 存契约 `sampler*` → **X5** |
| **pass 资源表** | `resolved_binding` 的 raw 车道（`VkImageView`/`VkImage`/`VkBuffer`，被 6 个 pass 仅用于判空）；`resource_handles`/`publish_family`/`views_of` | 契约车道 `image_handle`/`view_handle`/`buffer_handle` 已有 | 退役 raw 车道 + 发布面契约化 → **X5** |
| **上传/staging** | `create_upload_buffer` 已契约化 ✓；残留 `VkDeviceSize`/`VkBufferUsageFlags` | ✓ | 类型替换 → **X5** |
| **录制生命周期** | 帧循环自己 `vkBeginCommandBuffer`/`vkEndCommandBuffer` | **`begin_recording`/`end_recording` 已有**（readback 在用） | 帧循环改用它（**白拿的一条**） → **X4** |
| **绘制/派发/网格** | ✓ `draw`/`draw_indexed`/`dispatch`/`draw_mesh_tasks`；残留 `VkDrawMeshTasksIndirectCommandEXT` 手拼 | ✓ | 间接网格走契约动词 → **X5** |
| **RT launch** | ✓ `command_buffer::trace_rays`（刚落地） | ✓ | — |
| **屏障** | 绝大多数 ✓ `barrier`/`barrier_group`；残留：探针的 host 读屏障、micromap、readback | 无 host 访问角色 | **`image_use::host_read`（加值）** → **X4** |
| **渲染 scope** | ✓ `begin_rendering`/`end_rendering` | ✓ | — |
| **动态状态** | ✓ viewport/scissor/cull/depth_write；`render_environment` 仍收 `VkBool32`/`VkCullModeFlags` | `cull_mode` 有 | env 回调签名换契约 → **X5** |
| **描述符/堆** | ✓ `descriptor_heap` 全动词；残留 `VkBindHeapInfoEXT`（hook + scene/transparent pass）、`VkDescriptorType` | `heap_bind_info` 有 | hook 与 pass 签名换契约 → **X5** |
| **管线** | ✓ 契约工厂（graphics/compute/RT/mesh）；`VkPipelineLayout` 在 env | ✓ | env 的 layout 概念 → **X5** |
| **SBT** | ✓ `shader_binding_table_region/properties` + `trace_rays` | ✓ | — |
| **加速结构/RT** | `vkCreateAccelerationStructureKHR`、`vkGetAccelerationStructureBuildSizesKHR`、`vkGetAccelerationStructureDeviceAddressKHR`、`vkDestroyAccelerationStructureKHR`、`vkCmdBuildAccelerationStructuresKHR`、所有几何/实例/refit/micromap 结构、`vkGetDeviceProcAddr` | `ray_tracing` 能力**只有前向声明**（`acceleration_structure_desc` 未冻结） | **S1 形状族**：AS desc/build info/geometry/instances/refit/micromap usage + `address()` → **X6** |
| **GPU 计时** | ✓ `begin_gpu_timing`/`mark_gpu_timing` | ✓ | — |
| **图像读回** | 三段式：`frame_readback_buffer()`（后端 mapped 槽）+ 后端录制 `vkCmdCopyImageToBuffer` + 运行时解包；旧工具 `vulkan/readback`（staging+fence+裸提交，**已无调用者**） | `host_image_copy::copy_image_to_memory(source, span, region)` **已有**；`image_flag::host_transfer` **已有** | **给 `image` 一个 `get_content()`**：`std::expected<image_content, rhi::error> get_content(image_copy_region const& = {})`，`image_content{ extent, bytes_per_pixel, std::vector<std::byte> bytes }`（行紧凑、左上原点）。后端用 **host image copy**（`vkCopyImageToMemoryEXT`）实现 → 引擎侧**没有 staging、没有 copy 命令、没有 mapped 槽**；`frame_readback_buffer()` 与 `copy_image_to_buffer` 三件套退役；`vulkan/readback` **整模块删除** → **X4** |
| **截图源** | 源是 **swapchain image**（`supportedUsageFlags` 可能不含 `HOST_TRANSFER`，这也是 `docs/host_image_copy.md` 里"截图仍用 copy 命令"的原因） | — | **后端自己解决**：截图源改为**引擎/后端自有的目标 image**（带 `image_flag::host_transfer`；运行时已有自己的合成目标），或由后端保留一张 `host_transfer` 捕获图再 `get_content()` —— 引擎侧只写 `image->get_content()`，**不再有"哪种机序"的分支** → **X4** |
| **缓冲读回** | `vkCmdCopyBuffer` + fence + 裸提交（`vulkan/readback`，已无调用者）；探针用的是 host-visible coherent 契约缓冲（✓ 已无原生） | 无 | **`buffer::get_content()`**（后端内部 staging + 提交 + 等待，引擎不碰任何原生）——只有在真需要 GPU 缓冲内容时才加；当前唯一需求（探针）已满足 → **X4（可选）** |
| **探针** | `vkQueueSubmit` + host 读屏障 | 无 host 访问角色 | **`image_use::host_read`（加值）** + owned-buffer 提交/等待 → **X4** |
| **设备地址** | `VkDeviceAddress`（job 接口、AS、SBT） | `device_address::buffer_address` 有 | 类型换 `uint64_t`；AS 侧随 X6 → **X5/X6** |
| **格式映射** | `VkFormat` 表 + `VK_FORMAT_*`（10 文件） | `image_format` 有 | 映射表收进后端；引擎只用契约枚举 → **X3/X5** |
| **GUI** | **ImGui 的 Vulkan 后端编在引擎里**（72 个符号），运行时把 instance/device/queue/format 喂给它 | **无** | **GUI 渲染面由后端服务**：契约加 `gui` 扩展（`init(desc)`/`new_frame()`/`render(command_buffer&)`/`wants_mouse()`），ImGui 后端按后端选择（Vulkan/D3D12/…）编在**各后端**里 → **X2** |

### 3.1 读回的形状（按"内容"抽象，不按"机序"抽象）

```cpp
// 契约：宿主内存里的一段图像内容。行紧凑、左上原点，通道顺序由 `image_format` 决定。
struct image_content {
    image_extent extent = {};            ///< bytes 覆盖的 texel 范围
    std::uint32_t bytes_per_pixel = 0;   ///< 引擎不需要自己算 pitch
    std::vector<std::byte> bytes = {};    ///< row-major，`extent.width * height * depth * bytes_per_pixel`
};

struct image : object {
    /// 把 region 的内容读进宿主内存并返回。后端用 HOST IMAGE COPY 实现
    /// （`vkCopyImageToMemoryEXT`，见 `host_image_copy` 能力与 `image_flag::host_transfer`），
    /// 因此**没有 staging、没有复制命令、没有 mapped 槽**；设备/镜像不支持时是**命名的 error**。
    [[nodiscard]] virtual std::expected<image_content, error> get_content(image_copy_region const& region = {}) const = 0;
};
```

截图路径随之变成三段：`wait_idle()` → `frame_image()->get_content()`（或后端自有的 `host_transfer` 目标）→ 解包成 RGBA 写 PNG。
`frame_readback_buffer()`、`frame_readback_slot`、后端的 `vkCmdCopyImageToBuffer`、以及 `vulkan/readback` 整个模块都退役。


---

## 4. 分批（每批都跑既有 §6 门禁 + P-Gate/P-Run/P-Census/P-Import）

| 批 | 内容 | 触及 | 落地的矩阵行 | abi |
|---|---|---|---|---|
| **X0** | **门禁 + 窗口**：P-Gate 的"无 Vulkan 编译"配置（先允许列出当前违规，作为待办）、P-Census 脚本、P-Import 检查；`native_window` 统一 `GLFWwindow*` | `CMakeLists.txt`、`scripts/`、`promise/rhi/rhi.core_desc.cppm`、`runtime/*`、`main.cpp` | 窗口 | — |
| **X1** | **砍链接边**：`Vulkan::Vulkan` → 只给头文件的 INTERFACE；`vulkan_constant_init` 的 `PUBLIC` → PRIVATE；`imgui` 连同 `imgui_impl_vulkan.cpp` 移出引擎链接图（先让它"编得进、链不进"） | `CMakeLists.txt` | §2.2 三条边 | — |
| **X2** | **GUI 独立成 DLL（每个 API 一个），像后端一样手动导入**（用户的决定，取代"编进 deren_vulkan.dll"的旧写法）：新建 `deren_gui_vulkan.dll`（ImGui + GLFW/Vulkan 后端 + `graphical_user_interface` 整个模块都编在里面，私有链接 `vulkan-1`/`glfw`）；它导出**一个 C 入口** `deren_make_gui(...)`，主程序通过**自己的 loader**（`LoadLibrary`/`GetProcAddress`，与 `backend_loader` 同形）按 `api_type` 解析 `deren_gui_<api>.dll`；边界是**纯头文件抽象接口**（`gui` + `panel`，见 §4.1），`gui_create_info` 带 `rhi::api_core*` + `GLFWwindow*`，DLL 自己用 `vulkan_escape` 取原生句柄（引擎不再喂句柄）；`init(api_type)` 是 DLL 的**自检**（`deren_gui_d3d12` 见到 `vulkan` 就具名拒绝）。`vulkancorekit`/`deren.exe` 不再链接 `imgui` → `deren.exe` 的导入表里 **`vulkan-1.dll` 消失**，也没有 `deren_gui_*.dll`（手动导入）。 | `CMakeLists.txt`、新增 `promise/gui/gui_entry.hpp`、新增 `runtime/gui_loader.cppm`、`vulkan/graphical_user_interface/*`、`runtime/runtime.{cpp,frames.cppm,declarations.cppm}`、`chores.cpp` | GUI | — |
| **X3** | **设备事实 + 交换链 + 格式**：`device_facts`/`facts()`/`format_is_supported()`；冻结 `swapchain_desc` 并实装 `create_swapchain`；`swapchain` 查询；入口点解析全搬进后端；特性启用由后端自决 | `promise/rhi/*`、`vulkan/core/*`、`runtime/runtime.constructor.cppm`、`runtime/runtime.cpp` | 实例/设备/队列、交换链、格式 | 26 |
| **X4** | **读回走 host image copy + 录制生命周期 + 提交/等待**：① 契约给 `image` 加 **`get_content()`**（`image_content` POD），后端用 `vkCopyImageToMemoryEXT` 实现；截图改读**自有的 `host_transfer` 目标**；`frame_readback_buffer()`/mapped 槽/copy 命令退役；**删除 `vulkan/readback` 整模块**（已无调用者，7 个引用随之消失）；② 帧循环改 `begin_recording/end_recording`；③ `api_core` 加 owned-buffer 提交/等待 + `image_use::host_read`（加值） | `promise/rhi/*`、`vulkan/core/*`、`runtime/runtime.frames.cppm`、`runtime/runtime.readback.cppm`、`runtime/runtime.probes.cppm`、删除 `vulkan/readback/*` | 图像读回、截图源、缓冲读回（可选）、录制生命周期、探针、屏障 | 27 |
| **X5** | **类型清扫 + pass/资源层**：`resolved_binding` raw 车道退役、资源发布契约化、`VkExtent2D`→`image_extent`、`VkFormat`→`image_format`、`VkDeviceAddress`→`uint64_t`、`VkDeviceSize`→`uint64_t`、`VkBindHeapInfoEXT`→`heap_bind_info`、`VkBool32`/`VkCullModeFlags`→`bool`/`cull_mode`、`VkSampler`→`sampler*`、间接网格动词 | `vulkan/pass/*`、`vulkan/render_resource/*`、`vulkan/render_environment/*`、`vulkan/primitive/*`、`runtime/*` | 视图、采样器、pass 资源表、动态状态、描述符/堆、管线、间接网格 | 28 |
| **X6** | **S1 形状族（需先设计评审）**：AS desc/build info/geometry/instances/refit/micromap + `address()`；`acceleration_structure.cpp`、`ray_tracing.cpp` 整体走契约；`vkGetDeviceProcAddr` 从引擎消失 | `promise/rhi/rhi.extension.cppm`、`vulkan/core/*`、`vulkan/acceleration_structure/*`、`vulkan/ray_tracing/*`、`runtime/*` | 加速结构/RT | 29 |

**依赖**：X0 先行（否则无法证明）；X1 无条件先做（链接器立刻给出完整待办）；X2 独立（GUI 设计可并行）；
X3 是 X4/X5/X6 的地基（facts/格式）；X5 依赖 X2（资源发布含 GUI 视图）；X6 依赖设计评审。

**每批验证**：`ctest` 19/19、`clang-format-check`、`check_backend_boundary.py --require-zero`、spike 95/0、runtime_dyn 10/0、
**render gate 14/14 逐像素**、**rt_smoke 0 VUID**、**P-Gate**（无 Vulkan 编译）、**P-Census**。
X2/X3/X5 改画面 → render gate 是主判据；X4 改提交/屏障 → render + 冒烟；X6 改 RT → 冒烟是主判据。

---

### 4.1 X2 的形状（用户定：GUI 独立 DLL，像后端一样手动导入）

**为什么这条路**：GUI 是主程序里最后一块 Vulkan（72 个符号全部来自 `third_party/imgui/backends/imgui_impl_vulkan.cpp`）。把它编进 `deren_vulkan.dll`（旧写法）会让**渲染后端**去服务 GUI 的实现细节；做成**自己的 DLL** 则把"这个 API 的 GUI 后端"整块关进插件里，`api_type` 由**被加载的 DLL** 满足——这正是每个 API 一个 DLL 的意义。

**已实测的两个事实（本批）**：
1. **模块编进 SHARED 库、exe 导入它是可行的**（15 分钟实验：`probe_module_dll` SHARED + `probe_module_use` 只链 import lib → `answer() from the DLL == 42`，EXIT=0，exe 导入表只有该 DLL + CRT）。**但我们不走这条**：用户要求像后端一样**手动导入**，而手动导入就必须有 C 入口 + 抽象接口（后端 DLL 的既有形状），所以这个结论只作为"另一条路可行"的记录。
2. **GUI 的运行时面只有 6 个方法**（`init`/`shutdown`/`is_active`/`wants_mouse`/`begin_frame`/`render`）+ panel/widget 层；**调用点 56 处，其中 41 处在 `chores.cpp`**（app 自己搭调试面板的地方）——这就是抽象接口要覆盖的全部。

**边界（纯头文件 `promise/gui/gui_entry.hpp`）**：
```cpp
enum class gui_api_type : std::uint32_t { none = 0, vulkan = 1, d3d12 = 2 };
struct gui_create_info { std::uint32_t struct_size; gui_api_type api;
                         deren::promise::rhi::api_core* core; GLFWwindow* window; };
struct gui { virtual bool init(gui_create_info const&); virtual void shutdown(); virtual bool is_active();
             virtual bool wants_mouse(); virtual void begin_frame(); virtual void render(command_buffer&);
             virtual panel* add_panel(std::string); virtual void remove_panel(panel const&); };
using make_gui_fn = gui* (*)(gui_create_info const&);   // the C entry each deren_gui_<api>.dll exports
```
`gui_create_info` 带的是**契约面 + 窗口**（不是原生句柄）：DLL 自己 `query_extension<vulkan_escape>()` 取 instance/device/queue/format —— 于是 `runtime::enable_debug_gui()` 里那些 `VkInstance`/`VkDevice` 也随之消失。

**分批（每批可单独绿）**：
| 步 | 内容 | 判据 |
|---|---|---|
| X2a | `promise/gui/gui_entry.hpp` + `runtime/gui_loader.cppm`（按 `api_type` 解析 `deren_gui_<api>.dll`，命名失败要像 `backend_loader` 一样**具名 panic**） | 编译 + loader 单测（缺失 DLL 的具名失败） |
| X2b | 新建 `deren_gui_vulkan` SHARED（ImGui 源 + `graphical_user_interface` 模块 + 一个实现 `gui` 的 C 入口）；私有链接 `vulkan-1`/`glfw` | 构建 + `objdump -p` 看它**有** `vulkan-1.dll` |
| X2c | 运行时改走 loader（成员从 `gui::gui_content` 变成`gui*`）；`chores.cpp` 的 panel/widget 调用改接口形（`panel->add_label(...)`） | render 14/14（调试覆盖层默认关，另跑一次 `enable_debug_gui()` 的路径） |
| X2d | `vulkancorekit`/`deren.exe` 去掉 `imgui` 与 `Vulkan::Vulkan` 的传递；`objdump -p deren.exe` **无 `vulkan-1.dll`** | **P-Import 归零**（本计划的最终判据之一） |
### 4.2 X2 的现状（检查点：X2a/X2b 已提交，X2c 差 app 侧一步，X2d 未做）

**已完成并验证**
- **X2a/X2b（提交 `8808b69`）**：`promise/gui/gui_entry.hpp`（一个 `extern "C"` 入口 `deren_make_gui`、`overlay`/`panel`/`widget` 抽象接口、`gui_api_type`/`api_suffix()`、`create_info` 里句柄是 `void*`）；`deren_gui_vulkan` SHARED 目标把整个 `deren.vulkan.graphical_user_interface` 模块 + `gui_dll.cpp` 适配器编进去，私有链接 `imgui`/`glfw`/`vulkan-1`。**实测**：`objdump -p deren_gui_vulkan.dll` 有 `vulkan-1.dll`，且 DLL 导出 `deren_make_gui` ✓。
- **X2c 的后端侧**（未提交，但**编译通过**）：`runtime/gui_loader.cppm`（`LoadLibrary`+`GetProcAddress`，按 `api_type` 组成 `deren_gui_<api>.dll`，缺 DLL/缺符号/abi 不符都是**具名**诊断）；运行时成员改成 `std::shared_ptr<deren::gui::overlay>`，`enable_debug_gui()` 填 `create_info` 并调 `load_gui`，`new_frame`/`record`/`on_swapchain_recreated`/`wants_mouse` 全走接口，析构里显式 `shutdown()`+`reset()`；CMake 把 `graphical_user_interface` 两个源从 `vulkancorekit` 移出、`imgui` 从 `vulkancorekit` 与 `deren` 的链接列表移除（现在只被 `deren_gui_vulkan` PRIVATE 链接）、并加 `add_dependencies(deren deren_gui_vulkan)`（构建顺序，非链接）。

**唯一的失败，也就是剩下的一步**：`chores.cpp` 的 **41 处** `deren::vulkan::gui::*`（`debug_panel`/`label_widget`/`checkbox_widget`/`slider_widget`/`combo_widget` 与 `->visible_when`）仍按**旧模块的类**写。抽象接口的动词已经就位（`panel.add_label/add_checkbox/add_slider/add_vec3/add_combo` 返回 `widget&`，`widget::set_visible_when`），机械替换即可：

```cpp
deren::vulkan::gui::debug_panel& panel = runtime.debug_gui().add_panel("deren debug");   // -> deren::gui::panel& panel
panel.push_back(std::make_unique<deren::vulkan::gui::checkbox_widget>(L, V));             // -> panel.add_checkbox(L, V);
auto w = std::make_unique<...slider_widget>(L, V, lo, hi); w->visible_when = P; panel.push_back(std::move(w));
                                                                                          // -> panel.add_slider(L, V, lo, hi).set_visible_when(P);
```
另需在 `chores.cpp` 加 `#include "../promise/gui/gui_entry.hpp"`（它是普通 TU）。

**这一步用脚本做过两次，都失败并被回滚**：第一版按**行尾**猜调用在哪结束，而 lambda 体内的一条语句同样以 `);` 结尾 → 参数被截断、括号失衡（`chores.cpp` 已 `git checkout` 复原）；第二版改成**数括号**（正确的做法）但脚本文件本身没写成，尚未运行。**结论：这一步用"数括号的脚本一次转换 + 编译器收尾"是对的，但要留出回滚与逐个修残余的时间**；不要再用行尾启发式。

**X2d（未做）**：`objdump -p deren.exe` 目前**仍有** `vulkan-1.dll`（`chores.cpp` 编译不过，链接未发生）；做完上面一步后应变为 **无** `vulkan-1.dll`、且**不导入** `deren_gui_vulkan.dll`（手动导入），然后跑 render 14/14 + 一次 `enable_debug_gui()` 路径。

**这批踩到的两个坑（已修，记下来）**：① 边界头必须在**全局模块片段**里 include（它自己声明 `GLFWwindow` 并拉标准库；放进模块内会让模块重声明全局模块已有的名字，clang 两处报错）；② `export module` **不会**自动导出成员——`load_gui` 写成 `export` 之前，`runtime.cpp` 报 "declaration of 'load_gui' must be imported from module ... before it is required"。
## 5. 为什么不是"把引擎也做成 DLL"

"主程序不链接 vulkan" 若用"引擎变 DLL"来达成，只是把 Vulkan **藏进另一个 DLL**：引擎内部仍然引用 `vk*`（imgui 后端 + 7 个对象），
第二后端依然接不进来。真正要的是**引擎不再需要 Vulkan 才知道任何事**（§3 矩阵），届时：
导入表无 `vulkan-1.dll`、对象零 `vk*`、`deren.exe` 只通过契约入口拿到后端——**都是同一次改动的附带结果**，不需要额外的架构动作。
（`deren_vulkan.dll` 的设计**只导出 ONE name**（`deren_make_api_core`），第二后端就是同样一个 DLL，换掉即可。）

---

## 6. 风险与开放问题

1. **GUI 是最大的单项（72/76 个符号）**：ImGui 的后端本身是 API 特定的（有 `imgui_impl_vulkan.cpp` 也有 `imgui_impl_dx12.cpp`）。
   正解是**把"GUI 渲染面"变成契约的一部分**，ImGui 后端编在各自后端里。这需要一次设计（X2）：`gui` 扩展的形状、
   字体纹理上传、逐帧命令、与动态渲染 scope 的关系。
2. **`vkGetDeviceProcAddr`**：引擎里 3 个文件用它解析 AS/micromap/launch 入口点。launch 已解决（后端自己解析）；
   AS/micromap 要靠 X6 的形状把它们变成契约动词——**X6 必须先设计、后实现**。
3. **读回按"内容"抽象，不按"机序"抽象**（你的指示，已写进 X4）：引擎只要 `image->get_content()`
   （后端用 host image copy 实现），不要 staging/fence/复制命令。两个设计点：
   (a) **swapchain image 可能不支持 `HOST_TRANSFER`**（`docs/host_image_copy.md` 已记这条约束）→ 让**后端**处理：
   截图读自有的 `host_transfer` 目标，或后端保留一张捕获图；引擎侧不出现分支。
   (b) `image_content` 的内存形状：**行紧凑、左上原点、RGBA/BGRA 由 `image_format` 决定**，`bytes_per_pixel` 随内容返回，
   这样引擎不需要知道对齐/间距，也不需要自己算 pitch。
   （`buffer::get_content()` 同理，但当前唯一需求——探针——已经在 host-visible coherent 契约缓冲上满足，所以它是可选项。）
4. **`vulkan_constant_init` 的 constexpr builder**：纯头文件、无 API 调用，允许留在引擎；但要把它对 `Vulkan::Vulkan` 的 `PUBLIC`
   依赖改成"只给头文件的 INTERFACE"（X1）。**它能留，恰恰因为它不含调用**——这条界限要写进 review 规则。
5. **P-Gate 的实现方式**：把引擎源在"无 Vulkan 头/无 loader"配置下编译，需要给 `vulkancorekit` 准备一个不含 Vulkan 包含目录的
   编译配置（同一份源码、两套 defs/include）。X0 要先让这条**能跑起来并列出当前违规**，否则后面无法证明。
6. **abi 连续性**：X2→X6 各一次 bump（25→29），每批同步两个测试的固定值。
7. **窗口**：`GLFWwindow*` 是平台概念（用户已定统一用它），不是第二个图形 API；后端自己决定怎么从它建 surface/swapchain
   （Vulkan 用 `vkCreateWin32SurfaceKHR`，D3D12 用 `CreateSwapChainForHwnd`）。

---

## 7. 完成后的数字

| 指标 | 现在 | 完成 |
|---|---|---|
| 引擎能否对着非 Vulkan 后端编译（P-Gate） | 否 | **是** |
| `deren.exe` 导入 `vulkan-1.dll` | 是（76 个 `vk*` 符号） | 否 |
| 引擎对象引用的 `vk*` | 23 引用 / 7 对象 / 16 符号（其中 `vulkan/readback` 的 7 个引用随模块删除即消失 → **16 / 6**） | 0 |
| 引擎侧 `Vk*` 类型 | 64 种 | 0 |
| 引擎侧 escape 方法调用 | 12 方法 / 21 包装 | 0 |
| 引擎侧 `vkCmd*` | 5 | 0 |
| abi | 24 | 29 |
