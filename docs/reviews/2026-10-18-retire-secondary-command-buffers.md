# 退役二级 command buffer（ABI 37）

日期：2026-10-18 · 批次：retire-secondary · 前置：`2026-10-18-engine-mergeable-recording`（无 ABI bump，仍 36）

## 决策

引擎的多线程录制已全部改用 `mergeable_command_buffer` 段 + 录制期 batch 合并之后，契约里的"二级
command buffer"不再是任何真实路径的形状。本批把它退役，而不是留一个没有使用的第二套录制角色：

1. **`command_buffer_kind::secondary` 保留值、不再可用**。`create_command_buffer()` /
   `make_command_buffer()` 只接受 `primary`；`secondary` 被**命名拒绝**（`nullptr` + log），不会被静默
   提升为 primary。枚举值保留（枚举值是 ABI，永不复用），注释写明 RETIRED。
2. **`command_buffer::execute(command_buffer&)` 删除**。这是最后一个"逐条 secondary"的录制期合并入口
   （ABI 36 的 `execute(span)` 取代了它，引擎已只用 batch）。删一个既有 tier-1 接口的 virtual 会移动
   其后的所有槽位，所以本批是 **ABI 36 -> 37**。
3. **Vulkan 的 secondary 分配退到后端内部**：新增 `core::allocate_owned_command_buffer(VkCommandBufferLevel)`，
   契约工厂只走 primary；`make_mergeable_command_buffer` 直接调用它分配 secondary（mergeable 的 Vulkan
   实现本来就需要一个真正的 secondary 级别缓冲）。`owned_command_buffer::kind` 仍是内部字段，
   `begin_recording` 的 inheritance 逻辑不变。
4. **`mergeable_command_buffer` 的批次语义不变**（span 顺序、自包含、无隐式屏障、整批原子校验、
   retain 到提交完成）；`render_pass_continue` 现在只对 mergeable 段有意义。

## 测试 / probe / spike 更新

- `probe_backend`：删掉 `probe_command_buffer::execute(command_buffer&)`；工厂拒绝 secondary；
  mergeable 的内层 stand-in 不再伪装成 contract secondary。
- `test_dynamic_link`：owned-buffer 用例改为"secondary kind 被工厂拒绝"（`nullptr`）；mergeable 的
  `execute(span)` 两段/空批规则仍覆盖。
- `test_runtime_dyn`：真设备用例改成 mergeable batch 的 retain 语义（原 secondary 的 retain 用例）。
- `test_texture_group`：删除 `use_secondary` 维度；原 secondary 原生录制用例改为直接在 owned primary 上
  录 `vkCmdFillBuffer`（`test_heap_sampling` 的 primary 路径不变）。
- `test_backend_boundary_spike`：owned primary + mergeable batch；原 secondary 的
  inheritance 用例改在 mergeable 上（`command_buffer_begin_info` 的 tagged/portable inheritance 不变），
  `execute(single)` 换成 `execute(span)`（空批 `invalid_argument`、ended 段 ok）。
- `test_pass` / `raster_probe` / `test_texture_upload_layout` 等 fake 的 `execute(command_buffer&)`
  override 删除。

## ABI

**36 -> 37**：删除 `command_buffer::execute(command_buffer&)` virtual（槽位移动）+ enum 值语义 retire。
历史写入 `rhi.contract.cppm`；三个 ABI pin 同步 37。

## 验证

- `ctest`：**49/49 passed**；
- 真设备：`test_texture_group --with-device` **86 checks PASS**（secondary 维度删除后的 primary 用例）、
  `test_runtime_dyn --with-device` **1164 checks PASS**、`test_acceleration_structures --with-device`
  **504 checks PASS**；
- 冻结像素门禁：完整 OFF gate **15/15 PASS**、`-Visibility -Compare` **PASS**；
- doxygen **0 warnings**；
- `test_backend_boundary_spike`（手工 spike，不在 ctest 门禁内）的 `--with-device` 在本机有一项**与本批无关**
  的既有断言失败：无 `native_window` 时 `present()` 返回 `unsupported`，而 spike 期望 `not_ready`（该
  断言未被本批改动）；本批改写的 owned-primary/mergeable-batch 段全部通过。

## 遗留

- `mergeable_command_buffer` 的 Vulkan 实现仍分配 secondary 级别缓冲——这是实现细节，不是契约角色；
  将来如果某个后端能直接用 primary 做 batch 合并，这个内部选择可以各自变化。
- 引擎 `secondary_pass` 枚举名（transparent 槽）是引擎内部命名，本批未改；它不涉及契约 kind。
