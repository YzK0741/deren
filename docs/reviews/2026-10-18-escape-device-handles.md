# 退役 escape 的设备句柄（ABI 40）

日期：2026-10-18 · 批次：escape-device-handles · 前置：`2026-10-18-main-context-gui`（ABI 39）

## 决策

GUI 插件改用 `rhi::main_context` 之后，`vulkan_escape` 上这五个 slot 只剩测试/spike 的 raw 校验调用者：

| 删除的 slot | 原调用者 / 迁移方式 |
| --- | --- |
| `native_instance()` | 仅 spike；改 cast `main_context` 的 `instance` |
| `native_physical_device()` | 仅 probe 实现；`main_context` 不直接需要它时不再暴露 |
| `native_device()` | `test_texture_group` / `test_acceleration_structures` / spike；改 cast `main_context.device` |
| `native_queue()` | 同上；改 cast `main_context.graphics_queue` |
| `native_swapchain_image_format()` | GUI 已不用、测试无调用（零调用）；直接删除 |

**保留**（还有真实调用者）：`native_command_buffer`（GUI record + 测试 raw 录制）、
`enabled_device_extensions`（AS 测试的 guard rail）、`native_buffer` / `native_image` /
`native_image_view` / `native_sampler`（测试/spike 的句柄回读）、`get_basis` /
`shader_group_handles`（`rt_traversal_probe` 的 legacy SBT 路径）。

## 迁移方式

测试/spike 不再经过 escape 拿原生设备/队列，而是走 ABI 39 的同一路径：

```cpp
rhi::main_context* const context = core.main_context();
auto const* const main = rhi::do_concrete<deren::vulkan::main_context>(*context, deren::vulkan::main_context_name);
VkDevice device = main->device;
VkQueue queue = main->graphics_queue;
```

- `test_texture_group`：`queue_gate` 改收 `VkDevice`/`VkQueue`（来自 context），`test_heap_sampling`
  增加 context 参数；
- `test_acceleration_structures`：提交和 `rt_traversal_probe.hpp` 的 raw submit 用
  `main_context.device` / `graphics_queue`，文件 import 共享模块；
- `spike_backend_boundary`：实例/物理设备/设备/队列的非空检查改在 context 上做，文件加 Vulkan 头 +
  共享模块 import；
- `probe_backend`：删五个 override 与不再使用的 `probe_image_format`。

CMake：`vulkan_main_context`（ABI 39 的共享布局静态库）链接到 `test_texture_group` /
`test_acceleration_structures` / `test_backend_boundary_spike`。

## ABI

**39 -> 40**：删除既有 tier-2 接口的 5 个 virtual，vtable 后续槽位移动，必须 bump。历史写入
`rhi.contract.cppm`，contract test 的 ABI 历史块补一段，三个 pin 同步 40。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_texture_group --with-device` **87 checks PASS**、
  `test_acceleration_structures --with-device` **506 checks PASS**（含 rt traversal 的 raw submit）、
  `test_runtime_dyn --with-device` **1167 checks PASS**；
- `test_gui_plugin`（加载真实 GUI DLL）：**35 checks PASS**；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility -Compare` **PASS**；
- doxygen **0 warnings**。

## 已知非本批问题

`test_backend_boundary_spike --with-device` 在本机仍有一项**既有**失败：无 `native_window` 时
`present()` 返回 `unsupported`、spike 期望 `not_ready`（本批未改该断言，其余 105 checks 通过）。

## 后续

`vulkan_escape` 还剩 9 个槽：`native_command_buffer`、`enabled_device_extensions`、四个资源句柄
（buffer/image/image_view/sampler）、`get_basis` + `shader_group_handles`。下一步仍是先迁
`rt_traversal_probe` 的 legacy SBT 路径（engine 已用可移植 `shader_group_access`），再处理资源句柄
回读与 raw command buffer（后者要等测试的 raw 录制有契约替代）。
