# mergeable_command_buffer 批量提交（ABI 35）

日期：2026-10-18 · 批次：mergeable-command-buffer · 设计：`docs/superpowers/specs/2026-10-18-mergeable-command-buffer.md`

## 决策

新增 tier-1 接口 `mergeable_command_buffer`，**与 `command_buffer` 平行**（两者都直接继承 `object`，
不共享继承关系；录制动词在契约里声明两遍，这是刻意的）。它的唯一差异是提交规则：只能作为
`api_core::submit(std::span<mergeable_command_buffer_ref const>)` 的批次成员，不携带 `execute()`。

批量入口选择 **submit 重载**而不是 execute 重载：`execute` 是录制期合并（Vulkan 形状，D3D12 无对应），
批量只有在提交期才有跨后端语义。

批次语义（契约注释与 spec 同文）：span 顺序执行；片段自包含；无隐式内存屏障；整批原子校验
（非空/同后端/已 end/未提交/未释放/无重复）；后端 retain handle 到提交完成；普通 command_buffer
不能进 span。

## ABI

**34 -> 35**：`api_core` 追加两个 virtual（工厂 + `submit(span)` 重载）= 既有 tier-1 接口的 vtable
append，这是 bump 的唯一理由。新接口类型本身不移动任何已有 slot；既有 `submit(command_buffer&)`、
`execute()`、`begin_commands()`、帧路径语义不变。ABI pin（test_dynamic_link / test_acceleration_structures /
test_runtime_dyn）同步为 35，历史写入 `rhi.contract.cppm`。

## Vulkan 后端

- `owned_mergeable_command_buffer` 组合一个 secondary 级别的 `owned_command_buffer`（`inner`，
  shared_ptr 持有），所有录制 verb 转给它——录制实现零复制。
- `submit(span)`：注册表 provenance 校验 → 状态校验（secondary/已结束/未在飞）→ 创建一个后端拥有的
  owned primary → 按序 `retain_command` + `vkCmdExecuteCommands` → `end_recording` → 调用**既有**
  `submit(owned primary)` 路径（复用 retention/fence/pending/one-shot 规则）→ 释放本函数的 primary 引用。
- 新增具体名 `deren_mergeable_command_buffer_vulkan`（具体名 28 -> 29）。

probe 后端：`probe_mergeable_command_buffer` 组合现有录制 stand-in，`submit(span)` 实现原子校验
（空批/外来/重复/仍在录制/已提交/已释放）。

## 验证（本批最终树实测）

- `ctest`：**49/49 passed**；`test_dynamic_link` 新增批量用例（两段一次提交 ok；重复段、空批整批拒绝）。
- 真设备 `test_texture_group --with-device`：**133 checks, 0 failed PASS**，新增"两个 mergeable 各记录一次
  buffer copy，一次 batch 提交后两处写入都落盘"的断言。
- 像素门禁：完整冻结 OFF gate **15/15 PASS**、`-Visibility -Compare` **PASS**（引擎未迁移，渲染路径不变）。
- doxygen **0 warnings**、`test_docs` 通过。

## 本批不做的 / 已知边界

- 引擎帧路径未迁移（仍 `begin_commands` + primary + execute + 单 submit）；迁移批次单独做，OFF gate 必须保持。
  （已由 `2026-10-18-engine-mergeable-recording` 完成；`command_buffer_kind::secondary` 随后在 ABI 37 退役，
  见 `2026-10-18-retire-secondary-command-buffers.md`。下一行里的 `vulkan_escape::native_command_buffer`
  也已随 ABI 42/43 的 escape 退役删除，见 `2026-10-18-native-escape-retirement.md`。）
- ability 驱动的录制动词仍收 `command_buffer&`（`descriptor_heap::bind/push_data`、`radix_sorter::record`、
  `scoped_synchronization::record`、`mesh_shader::dispatch_mesh`、`vulkan_escape::native_command_buffer`）：
  mergeable 目前能录制 command_buffer 自己的 verb 集合；引擎迁移批次需要为这些入口补 mergeable 目标。
- `submit(span)` 本批是独立提交（同 owned primary，不 acquire/present）；profiler/query 的帧归属与 present
  语义在引擎迁移批次处理。
