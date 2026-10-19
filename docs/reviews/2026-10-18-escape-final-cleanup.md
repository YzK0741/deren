# 完成 escape 退役：只剩 native_command_buffer（ABI 42）

日期：2026-10-18 · 批次：escape-final-cleanup · 前置：`2026-10-18-unique-ptr-factories`（ABI 41）

## 决策

`vulkan_escape` 从"把后端的一切借给别人"缩到**一个 slot**：

| 删除的 slot / 类型 | 原因 / 迁移 |
| --- | --- |
| `native_buffer` / `native_image` / `native_image_view` / `native_sampler` | 只剩 spike 与 `test_texture_group` 的 raw 校验调用；spike 里这些 escape 验证段删除，`test_texture_group` 的 `vkCmdFillBuffer` 改成契约 `copy_buffer`（host-initialized source buffer） |
| `enabled_device_extensions` | 只剩 AS probe 的扩展名检查；改为"工厂答不出 acceleration structure 就 skip"，不再读扩展名单 |
| `get_basis` / `shader_group_handles` | 只剩 `rt_traversal_probe` 的 legacy SBT 对比；engine 早已用可移植的 `shader_group_access`，该对比段删除 |
| `api_basis` 类型 + `structure_type::vulkan_device_basis` | `get_basis` 的载体；tag 值保留退役 |
| `vulkan_command_buffer_inheritance_info` + `structure_type::vulkan_command_buffer_inheritance` | native `VkFormat` 拼写的附件继承；engine 已用可移植 `command_buffer_inheritance_info`；spike 的 native-tagged begin 测试删除（portable 测试保留）；tag 值保留退役 |

**保留的最后一个 slot 与理由**：`native_command_buffer` 是 Dear ImGui Vulkan backend 每帧录制自己 draw data
所必需的——第三方渲染 API 要的就是 `VkCommandBuffer`，契约没有也不该为它发明一个 verb。GUI 的设备根已由
`rhi::main_context`（ABI 39）提供；engine 和测试不再使用 escape。

## 伴随的测试简化

- `test_acceleration_structures`：不再查 escape；owned primary 用契约 `api_core::submit` + `wait_idle`
  提交（原来是 raw `vkQueueSubmit` + fence），rt traversal 同样；
- `rt_traversal_probe.hpp`：去掉 escape/main_context 参数、legacy SBT 对比、raw submit；
- `test_texture_group`：fill 走契约 copy（源 buffer 携带 initial bytes）；
- spike：删除"native_* 在真对象上返回非空"的验证段与 native-tagged inheritance begin（portable 链保留）。

## ABI

**41 -> 42**：7 个 tier-2 virtual 删除（其后槽位移位）+ 两个契约类型/两个 tag 退役（值保留）。
历史写入 `rhi.contract.cppm`，contract test 的 ABI 历史块补一段，三个 pin 同步 42。`vulkan_escape`
现在只剩 `native_command_buffer`。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_texture_group` **89**、`test_acceleration_structures` **495**（删掉的 escape 检查）、
  `test_runtime_dyn` **1167** checks PASS；`test_gui_plugin` **35**；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility -Compare` **PASS**；
- doxygen **0 warnings**。

## 契约层还剩什么 native 词汇

只剩：`vulkan_escape::native_command_buffer`（GUI overlay 的第三方录制入口）、`rhi::main_context`
（不透明，无 native 类型）、`vulkan_escape` 这个 ability bit 本身，以及两个已退役 tag 的保留值。
`api_basis`、`vulkan_command_buffer_inheritance_info` 等类型全部删除。
