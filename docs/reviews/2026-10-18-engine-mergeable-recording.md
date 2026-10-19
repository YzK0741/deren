# 引擎多线程录制改用 mergeable_command_buffer

日期：2026-10-18 · 批次：engine-mergeable-recording · 前置：`2026-10-18-mergeable-command-buffer`（ABI 35）、
`2026-10-18-recording-batch-merge`（ABI 36）

## 决策

引擎里"多线程录制"的三处 secondary 全部换成 `mergeable_command_buffer`，合并点从
`command_buffer::execute(command_buffer&)` 换成 ABI 36 的 `execute(std::span<mergeable_command_buffer_ref const>)`：

| 位置 | 之前 | 现在 |
| --- | --- | --- |
| main pass 的每 worker 段（`main_segments`） | `make_command_buffer(secondary)` | `make_mergeable_command_buffer({})`，scene pass 录制后 **一次 batch** 合入 frame primary |
| shadow 每 cascade（`shadow_recording`） | 同上 | 同上，每 cascade 一段 batch 合入 |
| transparent（`secondary_command_buffers[transparent]`） | 同上 | 同上，单段 batch 合入 |

**record 路径仍是同一套 contract verb**：draw 路径通过新的引擎级
`recording_target{command | mergeable}` 选择录制目标；heap push 用 ABI 36 的
`heap_push_info::mergeable_commands`，其它 verb（bind pipeline、viewport/scissor、depth/cull 动态状态、
mesh dispatch）在 `recording_target` 的小 helper 里二选一。`pipeline_handle` 增加 mergeable 的
`begin_pipeline` 重载。

**渲染结构不变**：frame primary 仍然先 `begin_rendering`，段仍以
`command_buffer_usage::render_pass_continue` 录制，`execute(span)` 在 scope 内按顺序
`vkCmdExecuteCommands`——这正是原来 `execute(single)` 的序列，只是入口变成 batch。所以像素结果必须
逐字节一致（OFF gate 的硬门）。

## 实现要点

- `render_environment`：新增 `recording_target`（两个 contract 句柄二选一 + 常用 verb 的 dispatcher）；
  `push_block` / `push_at` / `draw_mesh_tasks` / `draw_mesh_tasks_indirect` / `bind` /
  `set_depth_write_fn` / `set_cull_mode_fn` 全部改为 target 参数；primitive 的 draw 路径只把
  `env.command_buffer` 改成 `env.target`。
- runtime endpoints：`push_stage_block` / `push_geometry_block` / `draw_mesh_tasks*` 双路实现
  （heap 端点写 `commands` 或 `mergeable_commands`；mesh dispatch 走双方同签名的 contract verb）；
  pass 层仍然收 `command_buffer&` 的 `push_pass_stage_block` 作为薄适配。
- `contract_push_heap_data` 改为 `(command_buffer*, mergeable_command_buffer*)` 二选一。
- 老 `execute(command_buffer&)` 保留（transparent/shadow/scene 之外没有其它使用者），不删除。

## 验证

- `ctest`：**49/49 passed**（`test_pass` 的 scene-pass 生命周期用例改用 mergeable 段 fake，begin/end 失败
  路径仍然覆盖）；
- 真设备：`test_texture_group --with-device` **161 checks PASS**；
- 像素门禁：完整冻结 OFF gate **15/15 PASS**、`-Visibility -Compare` **PASS**（本批的硬目标）；
- doxygen **0 warnings**。

## 未做 / 后续

- 单线程 pass 仍直接记到 frame primary 的 command_buffer（`io.list`），这是预期：它们不是多线程录制。
- pass 框架的 `resolved_io::push_block` 等 pass 级 endpoint 仍收 `command_buffer&`；只有 `render_environment`
  这层支持 mergeable。若将来把整个 pass 录制都 batch 化，再迁移这一层。
- `command_buffer_kind::secondary` 的普通 command buffer 仍可创建（测试/其它调用方），不删除。
