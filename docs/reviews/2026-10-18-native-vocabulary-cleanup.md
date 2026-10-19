# native 词汇清理（ABI 34）：删除过渡期原生 pNext 与 vulkan_escape::native_image_format

日期：2026-10-18 · 批次：native-vocabulary-cleanup · 前置批次：`2026-10-18-descriptor-space`（其"非目标/D2"一项）

## 删了什么

1. **`vulkan_heap_image_desc` / `vulkan_heap_image_info`**：heap 图像写入的"原生 VkImageViewCreateInfo"转场参数
   （view_type / format / components / aspect / layout / native_image）。它存在的唯一理由是 heap 视图描述
   一度不可移植；`heap_image_write_info` 的 `image_view_desc` 路径（role/layers/mips + 由 contract image 推导
   format/aspect/view type）早已覆盖全部真实调用方。
2. **`vulkan_command_buffer_info`**：heap recording verb 的"原生 VkCommandBuffer"转场参数
   （`heap_bind_info` / `heap_push_info` 的 `header.next`）。没有任何调用方设置它；录制 verb 现在只接受
   契约 `command_buffer&`。
3. **`vulkan_escape::native_image_format(image const&)`**：escape 上按 image 查询解析后 VkFormat 的 virtual。
   全仓零调用（它的用途——raw heap view 的格式——已由可移植 view 描述取代；session-stable 的
   `native_swapchain_image_format()` 是唯一保留的格式事实）。
4. 以上结构的 `structure_type` 值（`vulkan_heap_image = 0x10000`、`vulkan_command_buffer = 0x10001`）
   **保留为退役值、永不复用**；heap 请求若仍链一个 `next`，`validate_structure(..., allow_next=false)`
   以 `unsupported` 拒绝，而不是遍历链。

## ABI

**33 -> 34**（必须 bump，且只因为第 3 条）：删除 `vulkan_escape` 的一个 virtual 会移动其后所有 slot
（`native_pipeline` / `native_shader_module` / `native_swapchain_image_format` / `get_basis` /
`shader_group_handles`），这正是 `abi_version` 存在的"vtable 形状变化"情形。第 1、2 条是删除类型与
枚举值（值保留、不复用），本身不移动任何 vtable。理由已写入 `rhi.contract.cppm` 的 ABI 34 注释；测试
（`test_dynamic_link`、`test_acceleration_structures`、`test_runtime_dyn`）的 ABI pin 同步到 34。

## 验证（本批最终树上实测）

- `ctest`：**49/49 passed**（含 `test_acceleration_structures` 418 项无设备路径、`test_runtime_dyn`、`test_dynamic_link`）；
- 真设备：`test_texture_group --with-device` **111/111 PASS**（含新增"heap image write 带链 = unsupported"断言）；
  `test_acceleration_structures --with-device` **504/504 PASS**（validation ON）；
- 具体名审计：仍为 28 个（本批没有新增具体实现类）；
- 像素门禁：完整冻结 OFF gate **15/15 PASS**、`-Visibility -Compare` **PASS**（本批只删除无调用方的词汇，
  引擎的 heap 写入本来就走可移植 view 路径，输出不变）。

## 现在的边界（ABI 34 时状态）

- heap 图像写入：**只有** `image_view_desc` 一条路径（role/layers/mips，format/aspect/view type 由 contract
  image 的声明推导；cube 由 `cube_compatible && array_layers == 6` 决定）——raw `VkImageViewCreateInfo`
  不再穿过契约。
- heap recording verb：只接受契约 `command_buffer&`；原生命令缓冲通过 escape 获取，不能再走 heap 请求。
- （后续 ABI 38：`enabled_instance_extensions` / `native_pipeline` / `native_shader_module` 三个零调用槽也已
  删除，见 `2026-10-18-escape-zero-call-cleanup.md`。）
- `vulkan_escape::native_image_format` 已删；需要格式事实时，session 级用 `native_swapchain_image_format()`，
  单 image 的格式由 `image_format` + 后端解析规则表达（契约不暴露 VkFormat）。
- 仅剩的原生参数是 `vulkan_command_buffer_inheritance_info`（secondary recording 的 attachment formats，
  没有可移植拼写），它继续走 tagged pNext 机制。

（**2026-10-18 后续核对（ABI 45）：** 本节是 ABI 34 时状态，已被后续批次取代——`vulkan_escape` 的
`native_buffer`/`native_image`/`native_image_view`/`native_sampler`/`enabled_device_extensions`/
`get_basis`/`shader_group_handles` 在 ABI 42 删除、`vulkan_command_buffer_inheritance_info` 同批删除、整个
escape 接口与 announce 在 ABI 43 删除；ABI 45 起 GUI overlay 自己持有两个 `mergeable_command_buffer`
录制目标（`2026-10-18-gui-owned-recording.md`）。契约层现在没有任何 native 参数，最后一条已不成立。）
