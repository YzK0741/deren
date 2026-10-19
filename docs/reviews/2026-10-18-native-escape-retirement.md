# native 词汇清零：删除 escape 接口本身（ABI 43）

日期：2026-10-18 · 批次：native-escape-retirement · 前置：`2026-10-18-escape-final-cleanup`（ABI 42）

## 决策：最后一个 native handle 也不留在契约里

ABI 42 之后 `vulkan_escape` 只剩一个槽：`native_command_buffer`，唯一消费者是 GUI overlay 的
Dear ImGui Vulkan backend（它要一个 `VkCommandBuffer` 录自己的 draw data）。做法不是给契约加 native
类型，而是**继续用 RHI 类型传递、由使用侧检查 concrete tag 后 cast**——和 `rhi::main_context`
（ABI 39）完全相同的规则：

- 契约照常把 `rhi::command_buffer&` 交给 GUI（`overlay::record` 的签名本来就收 RHI 类型）；
- 新增共享布局模块 `denen.vulkan.command_buffer`（后端与 GUI 插件编译同一份静态单元）：
  - `vulkan::command_buffer : rhi::command_buffer`——**共享的 Vulkan 类**，一个私有
    `VkCommandBuffer native`，`get()` 返回它、`set_target()` 给后端维护；
  - 两个 concrete identity 常量（frame view / owned buffer）；
  - `is_command_buffer()`（tag 检查）；
- 后端两个具体 command buffer（frame view `frame_commands`、recording buffer `owned_command_buffer`）
  都继承这个 Vulkan 类，并在录制目标变化时 `set_target()`：分配时写自己的 handle；frame view 在
  `acquire_next_image()` 时写 frame slot 的 buffer、在 frame 提交/推进时清空；
- GUI 插件 `record()`：**传 RHI 类型 → 检查 concrete tag → 转型成 Vulkan 类 → `get()`**：
  `static_cast<const deren::vulkan::command_buffer&>(commands).get()` 拿 `VkCommandBuffer` 交给 ImGui。
  契约里没有 native 类型、没有转换回调、没有 escape，共享类里也没有所有权/owned-borrowed 语义。

**为什么不把 `VkCommandBuffer` 塞回契约**：契约不认识 `VkCommandBuffer`，也不该认识；这是 API 的
词汇。GUI 插件就是 Vulkan 插件，它在自己这一侧做 tag 检查 + cast 是 ABI 39 已经定下的规则。
**也不再需要"后端提供的函数指针"**：RHI 类型 + tag 检查 + cast 到 Vulkan 类 + `get()` 就够了，
且 cast 是真正的基类转换，不是布局猜测。

## 删除清单

| 删除 | 替代 |
| --- | --- |
| `rhi::vulkan_escape` 接口（最后一槽 `native_command_buffer`） | 传 RHI `command_buffer` → tag 检查 → cast 到 `denen::vulkan::command_buffer` → `get()` |
| `extension_kind::vulkan_escape` 的 announce（bit 值保留 RETIRED） | 无：不再有 escape |
| backend `frame_escape` / `escape_view` / `abilities()` escape bit / `query_extension` escape 分支 / `concrete::vulkan_escape` | `frame_commands` / `owned_command_buffer` 继承 Vulkan 类并通过 `set_target()` 维护 native handle |
| probe `probe_escape` 及其 ability/query | probe 只 announce `device_address` + `descriptor_space` |
| spike 的 escape 能力/`query_extension`/owned handle 检查 | 改为检查 main context 的 device/queue 与 shared command-buffer cast（owned buffer 转换） |
| `test_texture_group` 的 escape 参数 | tag 检查 + cast 到 Vulkan 类 + `get()` |
| `test_dynamic_link` 的 escape ability 期望 | probe 的 bit 集更新，并 walk `all_extension_kinds()` |
| `test_runtime_dyn` 的 escape bit 检查 | 断言 retired bit **不被** announce（bit 永不复用） |

## ABI

**42 -> 43**：删除整个 tier-2 接口 + 不再 announce 一个 bit；`all_abilities()` 从 12 位变 11 位
（0x1FF7 -> 0x1FD7），`all_extension_kinds()` 从 12 项变 11 项。历史写入 `rhi.contract.cppm`，三个
ABI pin 同步 43。`extension_kind` 枚举值 `vulkan_escape = 1u << 5` 保留，标注 RETIRED。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_runtime_dyn` **1167**、`test_acceleration_structures` **495**、
  `test_texture_group` **88**（含 owned command buffer 经 main context 的转换）、
  `test_vulkan_capabilities` **39** checks PASS；`test_gui_plugin` **35**；
- spike：89 checks，1 个**既有的** `present() == not_ready` 失败（无 native_window，与本批无关，已记录）；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility` **PASS**；
- doxygen **0 warnings**，`scripts/check_rhi_validation_audit.py`、`scripts/check_backend_boundary.py` 通过。

## 已知覆盖边界

GUI overlay 的真实录制路径（borrowed frame list -> shared cast）只有 app 开启 `[gui] show` 时才会走到；
冻结像素门禁的 capture 以 GUI off 运行，所以自动化覆盖的是 owned-buffer 的 `get()`（native handle）路径
（`test_texture_group`）与拒绝路径（`is_command_buffer` 对 foreign handle 答 false）。frame-list 的
字段维护点（acquire 写入 / submit 清空）与 GUI 插件的 ImGui 调用需要 app 运行覆盖——这与本批之前
escape 路径的覆盖方式一致。

## 状态

契约层不再有任何 native 类型、native 词汇接口或 native escape：

1. `rhi::main_context` / `rhi::command_buffer`（不透明/契约句柄，无 native 类型）；
2. `denen.vulkan.main_context`（设备根布局）与 `denen.vulkan.command_buffer`（Vulkan 类 +
   `get()`）两个共享单元（后端拥有，GUI 插件 tag 检查后 cast 使用）；
3. `device_address` 的 `buffer_address`（返回地址整数，不是 native 句柄）；
4. 已退役的值：`extension_kind::vulkan_escape` 的 bit、`structure_type::vulkan_command_buffer_inheritance`
   / `vulkan_device_basis` 的 tag 值（永不复用）。
