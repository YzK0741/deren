# 录制期批量合并与 heap mergeable 目标（ABI 36）

日期：2026-10-18 · 批次：recording-batch-merge · 前置：`2026-10-18-mergeable-command-buffer`

## 决策

这是"把引擎多线程录制 buffer 换成 mergeable"的**前置批次**：引擎还没动，先把 mergeable 段能走的两条
通路补齐。

1. **`command_buffer::execute(std::span<mergeable_command_buffer_ref const>)`**：普通命令缓冲在录制期
   把一批 mergeable 段合入自己（Vulkan `vkCmdExecuteCommands`；D3D12 等提交期合并的后端保留默认
   `unsupported` 体，命名拒绝）。批次规则与 `submit(span)` 同文：span 顺序、自包含、无隐式屏障、
   整批原子校验、段被 retain 到提交完成。
2. **heap 录制目标接 mergeable**：`heap_bind_info` / `heap_push_info` 追加
   `mergeable_command_buffer* mergeable_commands`（与 `commands` 恰好一个），使 mergeable 段可以像
   command buffer 一样发布 heap 绑定/推送数据——这是 scene/shadow 段录制必需的能力。
3. **Vulkan 实现**：`frame_commands` 与 `owned_command_buffer` 各自实现 `execute(span)`（owned 版本额外
   要求 `kind == primary`），共享一个 `execute_mergeable_batch` helper（provenance → 状态 → 去重 →
   `vkCmdExecuteCommands` + `secondary_refs` retain）；`heap_commands` 接受两种目标并各自校验归属/录制态。
4. **probe**：`probe_command_buffer::execute(span)` 实现契约的校验规则（非空、均为本 probe 的
   mergeable、未在录制/未释放），fake 记录一次 execute。

## ABI

**35 -> 36**：给既有 `command_buffer` 接口 append 一个 virtual（带默认 `unsupported` 体的
`execute(span)`）+ 给 `heap_bind_info`/`heap_push_info` 追加字段（by-value POD 追加）——一次
`abi_version` 批次，历史写入 `rhi.contract.cppm`；ABI pin 同步 36。

## 验证（本批当前树）

- `ctest`：**49/49 passed**；
- `test_dynamic_link`：普通 command buffer 在录制期 `execute(span)` 两段返回 ok；空批 `invalid_argument`；
  （probe 的其余批量规则覆盖在前一批的 `submit(span)` 用例里）
- 真设备 `test_texture_group --with-device`：**161 checks, 0 failed PASS**，其中新增"普通 primary 在
  录制期 execute(span) 两个 mergeable 段 → 单缓冲 submit → 两处 copy 都落盘"；
- 像素门禁：完整冻结 OFF gate **15/15**、`-Visibility` **PASS**（引擎未动，渲染路径不变）。

## 下一步（本批不做，新批次）

引擎多线程录制 buffer 的正式迁移：
- `runtime::init_recording_resources`：transparent / shadow cascades / main segments 改
  `make_mergeable_command_buffer`；
- `render_environment` 的录制目标从 `shared_ptr<command_buffer>` 泛化为 command/mergeable 二选一，
  `push_block`/`push_at`/`draw_mesh_tasks*`、`bind`/动态状态注入等端点两路实现（heap 端点直接使用本批
  新增的 mergeable 目标）；
- pass 结构（`segment_buffer::buffer`、`shadow_frame::cascades`、`transparent_frame::secondary`）改
  mergeable ref；合并点改 `io.list->execute(span)`；
- 验收：OFF gate 15/15 字节一致、ON 参考不变、真设备/ctest 全绿。
