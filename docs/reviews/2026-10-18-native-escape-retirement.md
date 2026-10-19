# native 词汇清零：删除 escape 接口本身（ABI 43）

日期：2026-10-18 · 批次：native-escape-retirement · 前置：`2026-10-18-escape-final-cleanup`（ABI 42）

## 决策：最后一个 native handle 也不留在契约里

ABI 42 之后 `vulkan_escape` 只剩一个槽：`native_command_buffer`，唯一消费者是 GUI overlay 的
Dear ImGui Vulkan backend（它要一个 `VkCommandBuffer` 录自己的 draw data）。本批次把这个转换搬到
**后端拥有的共享 Vulkan 布局**上：

- `denen.vulkan.main_context`（ABI 39 引入的 `rhi::main_context` 具体布局）新增一个函数指针字段
  `native_command_buffer(api_core&, rhi::command_buffer&) -> VkCommandBuffer`；
- 后端在 `core::main_context()` 里填自己的静态成员 `core::native_command_buffer_of`（原
  `frame_escape::native_command_buffer` 的实现原样保留：frame 的 borrowed list 在无 frame 时答
  nullptr，owned list 走 `contract_command_buffers` 的 provenance 登记表）；
- GUI 插件本来就把 `main_context` cast 成自己的共享布局（ABI 39 的既定机制），现在它用同一个具体
  类型上的字段做转换，不再 query 任何 escape；
- 于是删除整个 `rhi::vulkan_escape` tier-2 接口、`extension_kind::vulkan_escape` 的 announce
  （bit 值保留 retired，永不复用）、backend `frame_escape`/`escape_view`/ability/query 分支、
  probe 的 escape 替身，以及测试与 spike 里所有 escape 用法。

**为什么不把 `VkCommandBuffer` 塞回契约**：契约不认识 `VkCommandBuffer`，也不该认识；这是 API 的
词汇。共享 `main_context` 是 ABI 39 已经定下的"具体 API 粘合层放后端拥有的上下文"——录制目标
转换属于同一类胶水。第三方渲染器的接口要求由"后端提供的函数指针"满足，契约保持干净。

## 删除清单

| 删除 | 替代 |
| --- | --- |
| `rhi::vulkan_escape` 接口（最后一槽 `native_command_buffer`） | `denen.vulkan.main_context::native_command_buffer` |
| `extension_kind::vulkan_escape` 的 announce（bit 值保留 RETIRED） | 无：不再有 escape |
| backend `frame_escape` / `escape_view` / `abilities()` escape bit / `query_extension` escape 分支 / `concrete::vulkan_escape` | 静态成员 `core::native_command_buffer_of` |
| probe `probe_escape` 及其 ability/query | probe 只 announce `device_address` + `descriptor_space` |
| spike 的 escape 能力/`query_extension`/owned handle 检查 | 改为检查 main context 的 device/queue 与 resolver（owned buffer 转换） |
| `test_texture_group` 的 escape 参数 | 直接调用 `main_context.native_command_buffer(*main_context.core, ...)` |
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
- spike：90 checks，1 个**既有的** `present() == not_ready` 失败（无 native_window，与本批无关，已记录）；
- 冻结像素门禁：完整 OFF gate **15/15**、`-Visibility` **PASS**；
- doxygen **0 warnings**，`scripts/check_rhi_validation_audit.py`、`scripts/check_backend_boundary.py` 通过。

## 已知覆盖边界

GUI overlay 的真实录制路径（borrowed frame list -> resolver）只有 app 开启 `[gui] show` 时才会走到；
冻结像素门禁的 capture 以 GUI off 运行，所以自动化覆盖的是 resolver 的 owned-buffer 路径（
`test_texture_group`）与拒绝路径（背板/foreign handle 返回 nullptr）。frame-list 路径与 GUI 插件的
ImGui 调用由手工/交互运行验证——这与本批之前 escape 路径的覆盖方式一致（当时也只有 app 运行覆盖它）。

## 状态

契约层不再有任何 native 类型、native 词汇接口或 native escape：

1. `rhi::main_context`（不透明，无 native 类型；GUI cast 到后端共享布局）；
2. `denen.vulkan.main_context` 的共享 Vulkan 布局（后端拥有，GUI 使用）；
3. `device_address` 的 `buffer_address`（返回地址整数，不是 native 句柄）；
4. 已退役的值：`extension_kind::vulkan_escape` 的 bit、`structure_type::vulkan_command_buffer_inheritance`
   / `vulkan_device_basis` 的 tag 值（永不复用）。
