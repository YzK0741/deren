# GUI 通过 RHI 的 main_context 取设备根（ABI 39）

日期：2026-10-18 · 批次：main-context · 前置：`2026-10-18-escape-zero-call-cleanup`（ABI 38）

## 决策

GUI 插件是 `vulkan_escape` 剩余句柄方法（`native_instance` / `native_physical_device` /
`native_device` / `native_queue` / `native_swapchain_image_format`）的最后一个非测试调用者。本批不再让
插件逐个查 escape，而是由契约给出**一个不透明的 main-thread context**，插件按自己的 API 布局 cast：

1. **契约新增 `rhi::main_context : object`**：只携带 concrete-identity tag（`object::type`），
   **不携带任何 native 词汇/句柄成员**。所有权是 backend 的，`api_core::main_context()` 返回借用指针；
   没有 `release()`。
2. **`api_core` 追加 `main_context()`**：默认体返回 `nullptr`（和 `execute(span)` 的默认体同一规则），
   所以 probe/测试替身不实现也保持 concrete；真 backend 返回自己的 context。
3. **具体 Vulkan 布局放在共享静态模块 `denen.vulkan.main_context`**
   （`source/backends/vulkan/main_context/main_context.cppm`）：`denen::vulkan::main_context final :
   rhi::main_context`，带 `rhi::api_core* core` + VkInstance/VkPhysicalDevice/VkDevice/VkQueue +
   queue family + 呈现格式 + frames-in-flight；这是 `deren_vulkan.dll` 与 `deren_gui_vulkan.dll`
   **共同编译**的 interface unit（同 `vulkan_constant_init`/`renderer_layout` 的共享布局机制），
   tag 与布局不会在两侧分叉。
4. **GUI 边界**：`gui::create_info.core` 被 `main_context` **取代**（gui ABI 3 -> 4）。插件在
   `init()` 里 `is_concrete` 校验 tag 后 `do_concrete` cast，取句柄初始化 ImGui；不认识的 context
   按名拒绝（未识别 API 的插件的标准行为）。`record()` 的每帧 native command buffer 仍走
   `vulkan_escape::native_command_buffer`（这个 slot 的迁移需要另一条路径，未在本批）。
5. **runtime**：`enable_debug_gui()` 把 `vk.main_context()` 交给插件；不再交 `api_core*`。

## ABI

- **RHI 38 -> 39**：`api_core` append 一个 virtual（默认体，vtable 追加），新类型 `rhi::main_context`
  本身不需要 bump；历史写入 `rhi.contract.cppm`，三个 ABI pin 同步 39。
- **GUI 3 -> 4**：`create_info` 字段语义变化（`core` -> `main_context`），`gui_abi_version` bump；
  `test_gui_plugin` 的 static_assert 与拒绝路径同步。

## 后续（ABI 43）

ABI 43 删除了 escape：GUI 的 per-frame 录制目标不再由上下文回调转换，而是走**同一个规则的另一个
共享单元**——契约传 RHI `command_buffer`，插件检查 concrete tag 后 cast 到共享 Vulkan 类
`denen.vulkan.command_buffer` 并调用 `get()`。设备根仍在这个上下文里。契约里不再有 escape。见
`2026-10-18-native-escape-retirement.md`。

## 验证

- `ctest`：**49/49 passed**（contract test 增加 `main_context() == nullptr` 的默认体检查与 ABI39 历史注释）；
- 真设备：`test_runtime_dyn --with-device` **1167 checks PASS**（新增 main context 非空/标签/同一对象检查）、
  `test_texture_group --with-device` 等既有设备测试不变；
- `test_gui_plugin`（手工，加载真实 `deren_gui_vulkan.dll`）：**35 checks PASS**，含 main_context 缺失/
  截断 descriptor 的拒绝路径；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility -Compare` **PASS**（GUI 在 capture 路径不启用，
  渲染路径不变）；
- doxygen **0 warnings**。

## 后续

- GUI `record()` 仍要 `vulkan_escape::native_command_buffer`；等测试/spike 的 raw 校验换掉之后，可把
  这个 slot 也收进 main_context（例如给 context 加一个"记录到 native command buffer"的转换函数指针，
  或让 GUI 的 record 走 contract verb）。
- （已由 ABI 40 完成。）有了 main_context 之后，`vulkan_escape` 的 GUI 句柄方法（instance/physical device/device/queue/
  swapchain format）**可以在下一批删除**——只剩测试/spike 调用者需要迁移；这是上轮盘点里"设备/队列/帧
  句柄"退役路径的第一步。
