# 删除三个零调用的 escape 槽（ABI 38）

日期：2026-10-18 · 批次：escape-zero-call · 前置：`2026-10-18-retire-secondary-command-buffers`（ABI 37）

## 决策

在"还剩哪些 escape"的盘点里，`vulkan_escape` 的 16 个 virtual 中有 3 个**全仓零调用**：声明、Vulkan
实现、probe 实现之外，没有任何调用点（engine、app、GUI 插件、测试、spike 都没有）。本批只删这三个，
不碰仍有真实调用者的槽位：

| 删除的 slot | 为什么没有调用者 |
| --- | --- |
| `enabled_instance_extensions()` | instance 级扩展列表没有消费者；device 级列表（`enabled_device_extensions`）仍被 `test_acceleration_structures` 的 raw 校验使用，保留 |
| `native_pipeline(pipeline const&)` | 最后一个潜在调用者（`pass::shader_group_handles` 与 pipeline/RT 构建）已走可移植的 `shader_group_access` / contract factory；engine 的 `begin_pipeline` 也不再读 native handle |
| `native_shader_module(shader const&)` | 计算/RT pipeline 的构建已是 contract factory（`pipeline_desc` 带 shader 句柄），没有调用者 |

**保留**：`native_physical_device()`（GUI 插件 `gui_dll.cpp` 构造 ImGui Vulkan init 时真实调用）、
`native_instance` / `native_device` / `native_queue` / `native_swapchain_image_format`（GUI 插件与测试）、
资源句柄与 `native_command_buffer`（测试/spike 的 raw 校验）、`get_basis` / `shader_group_handles`
（`rt_traversal_probe`），以及 `enabled_device_extensions`。

## 实现

- 契约：删三个 virtual 及其 doc（`enabled_*` 的 doc 合并为 device-only；`native_pipeline` /
  `native_shader_module` 的 doc 换成一句"已删"说明）。
- Vulkan：`frame_escape` 删三个 override；`shader_group_handles` 内部改为**就地**从
  `owned_pipeline::native_handle` 读 `VkPipeline`（公共槽删了，SBT query 本身还在）。
- probe：删三个 override，probe 的 escape 仍覆盖契约剩余槽位。

## ABI

**37 -> 38**：删除既有 tier-2 接口的 virtual 会移动其后所有槽位，必须 bump。历史写入
`rhi.contract.cppm`；`test_dynamic_link` 的 ABI 历史块补一段；三个 ABI pin 同步 38。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_runtime_dyn --with-device`、`test_acceleration_structures --with-device`、
  `test_texture_group --with-device`、`test_vulkan_capabilities --with-device` 全 PASS；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility -Compare` **PASS**；
- doxygen **0 warnings**。

## 后续

`vulkan_escape` 还剩 13 个槽位，按调用者分类：

- GUI 插件依赖（`native_physical_device` 等）——要删得先给 GUI 插件一条不经过 escape 的路径；
- 测试/spike 的 raw 校验依赖（`native_command_buffer` / 资源句柄 / `get_basis` 等）——需要把校验改成
  可移植断言，或先补契约 verb（如 buffer fill/clear）；</br>
  **ABI 40 已把设备/队列/实例/格式五个句柄方法退役**（测试与 spike 改 cast `main_context`），见
  `2026-10-18-escape-device-handles.md`。
- 其余（`enabled_device_extensions` 等）可随测试改造一起删。

整体退役顺序仍按上轮盘点：legacy SBT → 原生继承结构 → 资源句柄 → 设备/队列/帧句柄 → 整个 extension。
（该顺序已走完：ABI 40 删设备/实例/队列/格式五个句柄，ABI 42 删其余 slot 与原生继承结构，ABI 43 删除
escape 接口本身，见 `2026-10-18-native-escape-retirement.md`。）
