# GUI overlay 持有自己的录制目标（GUI ABI 5 / RHI ABI 45）

日期：2026-10-18 · 批次：gui-owned-recording · 前置：`2026-10-18-shared-object-migration`（ABI 44）

## 决策

之前 GUI overlay 在引擎已经打开的 rendering instance 里直接录制，靠的是引擎把 frame 的
`rhi::command_buffer` 借给它。这个批次把录制目标的所有权移到 GUI：

- **宿主创建、GUI 持有**：`gui::create_info` 增加两个
  `rhi::mergeable_command_buffer_ref recording_targets[2]`（引擎在 `enable_debug_gui()` 里用
  `api_core::make_mergeable_command_buffer()` 创建并移交）；`gui_abi_version` 4 -> 5。
- **每帧取 native**：`overlay::record(command_buffer_begin_info const& begin)` 用当前持有的段开始录制，
  把段 cast 到共享 Vulkan 类 `denen::vulkan::mergeable_command_buffer` 后 `get()` 拿 `VkCommandBuffer`
  交给 ImGui；录完 `end_recording()` 并把**段交回宿主**。契约仍不认识任何 native 类型。
- **宿主执行**：引擎的 overlay hook（post / fxaa / upscale 三个最终写者）把当前 rendering instance 的
  附件继承（portable `command_buffer_inheritance_info`）传给 overlay；录制段在 pass 的 instance **结束
  之后**由 runtime 用一个新的 rendering instance 执行（`VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT`，
  颜色附件 LOAD、无 clear）。分成两个 instance 是 Vulkan 的要求：带 secondary-contents 标志的 instance
  不能包含 primary 的 `vkCmdDraw`。
- **双缓冲安全**：两个段按帧奇偶交替；引擎的 frame slot ring 在每帧开始等待对应 slot 的时间线，所以
  同一奇偶段的上一次使用必然已完成。

## 两个 Vulkan 细节（实现时被 validation 抓出）

1. **ImGui 自己绑定 classic descriptor set**：段开始时不能继承引擎的 descriptor-heap bind infos，否则
   ImGui 的每次 bind 都触发 VUID-...-11295/11296。`command_buffer_begin_info` 追加
   `bool inherit_descriptor_heaps = true;`（RHI ABI 45，append），overlay 段的 begin 传 `false`。
2. **secondary-contents instance 不能混 primary draw**：覆盖层不能继续复用 pass 的 instance，改为
   pass 结束后单独开一个 LOAD + secondary-contents 的 instance 执行 GUI 段。

## ABI

- **RHI 44 -> 45**：`command_buffer_begin_info` 追加 `inherit_descriptor_heaps`（默认 true，旧调用者
  语义不变）；三个 pin 同步 45。
- **GUI 4 -> 5**：`create_info` 追加两个录制目标字段，`overlay::record` 改签名（返回录好的段）。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_runtime_dyn` **1167**、`test_texture_group` **88**、`test_acceleration_structures` **495**
  checks PASS；`test_gui_plugin` **35**；
- **GUI 真路径**：`deren.exe --config gui-on.toml --capture-frames 2 --capture-windowed`（`[gui] show = true`，
  validation layers on）退出码 0，日志只有 `gui plugin: deren_gui_vulkan is up` 与
  `gui_content: ImGui overlay initialized`，**无 VUID、无 panic**；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility` **PASS**（GUI off 帧不变）；
- doxygen **0 warnings**。

## 覆盖边界

冻结像素门禁的 capture 以 GUI off 运行（overlay 会让图像不确定），所以 overlay 路径的自动化覆盖是上面
那条 `--capture-windowed` + `[gui] show = true` 的真设备运行（本批次实测）；交互式按键/鼠标路径仍靠手工。
