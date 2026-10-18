# mergeable_command_buffer 与批量提交（设计）

日期：2026-10-18 · 状态：已实现（本批）· 关联：`docs/reviews/2026-10-18-mergeable-command-buffer.md`

## 1. 问题与目标

现在的契约里，"合并"只能发生在**录制期**：调用方先有 primary，再把 secondary 用
`command_buffer::execute()` 合进去，最后 `api_core::submit(command_buffer&)` 提交。这个形状是
Vulkan 的，D3D12 没有对应动作（command list 之间只能在队列提交时合批）。

本批引入一个**平行**的录制概念 `mergeable_command_buffer`：它可以被录制，但**只能作为批次的一员**
通过 `api_core::submit(std::span<...>)` 提交。批量如何实现由后端决定：

- **Vulkan**：mergeable = secondary；后端自己拥有一个 merge primary，按 span 顺序
  `vkCmdExecuteCommands`，然后走既有的队列提交（acquire/present semaphore 不变）；
- **D3D12**：mergeable = command list；`submit(span)` = `ExecuteCommandLists(batch.size(), ...)` + present。

## 2. 契约形状（ABI 35）

```cpp
struct mergeable_command_buffer : object {   // 与 command_buffer 平行，不继承它
    // 与 command_buffer 同签名的录制面：release / begin_recording / end_recording /
    // use / copies / timing / rendering / bindings / draws / dispatch / mesh / ray launch /
    // AS+micromap build / dynamic state / barriers / clear / load_texture_group
    // 不含 execute()：mergeable 是叶子段，合并只发生在提交期。
};
using mergeable_command_buffer_ref = std::shared_ptr<mergeable_command_buffer>;

struct api_core {
    // 既有 submit(command_buffer&) / execute() / 帧路径全部不动
    virtual mergeable_command_buffer_ref make_mergeable_command_buffer(command_buffer_desc const&) = 0;
    virtual error submit(std::span<mergeable_command_buffer_ref const> batch) = 0;
};
```

**为什么是 submit 的重载而不是 execute 的重载**：`execute` 是录制期把 secondary 记录进 primary，
Vulkan 能做、D3D12 不能；批量只有在"提交期"才有跨后端语义。所以批量入口挂在提交动词上。

## 3. 批次语义（写进契约注释）

1. 按 span 顺序执行，等价于把片段拼成一个逻辑流；
2. 每个片段自包含：状态与 render scope 不跨片段边界继承；
3. 合并**不插入隐式内存屏障**；hazard 由录制方用 `barrier` 动词写清楚（后端可加 API 必需的执行依赖，
   不改变可见性语义）；
4. 整批原子校验：非空、同一后端、都已 `end_recording()`、未提交、未释放、无重复；任一失败整批拒绝，
   不产生部分提交；
5. 后端 retain span 中的 handle 直到提交完成；调用方可以 submit 返回后立刻丢弃引用；
6. 空白片段（空的 span）是调用方 bug，拒绝；
7. 普通 `command_buffer` 不能放进 span（类型不同：Vulkan primary 不能被 execute 进主缓冲，
   D3D12 语义也不对称）。

## 4. 后端实现

- **Vulkan**：`owned_mergeable_command_buffer` 组合一个以 secondary 级别创建的 `owned_command_buffer`
  （录制实现一份都不复制，所有 verb 转发）。`submit(span)`：
  校验整批 → 创建一个 owned primary → 按序把每个 secondary retain 进 primary 的 `secondary_refs`
  并 `vkCmdExecuteCommands` → `end_recording` → 调用**既有的** `submit(owned primary)` 路径
  （fence、pending、retention、one-shot 规则全部复用）→ 释放本函数持有的 primary 引用。
- **D3D12**（未来后端）：`submit(span)` = 校验 + `ExecuteCommandLists` + present；list 状态在
  `Reset` 后是默认值，与"片段自包含"语义一致。
- **probe**：`probe_mergeable_command_buffer` 组合现有录制 stand-in，`submit(span)` 实现契约的原子
  校验规则并把"批次被接受"计数；没有设备时录制本身记录不出命令（诚实答案）。

## 5. 本批不做的（明确记录）

- **引擎不迁移**：帧/渲染图仍然走 `begin_commands` + primary + `execute()` + `submit(command_buffer&)`；
  本批只在契约、Vulkan/probe 与测试中落地新面。OFF/ON 参考不变。
- **ability 驱动的录制动词仍收 `command_buffer&`**：`descriptor_heap::bind/push_data`、
  `radix_sorter::record`、`scoped_synchronization::record`、`mesh_shader::dispatch_mesh`、
  `vulkan_escape::native_command_buffer`。引擎迁移批次需要为它们增加 mergeable 目标（并做相应
  ABI 批次），否则 mergeable 只能录制 command_buffer 自己的 verb 集合。
- **profiler/query 的帧归属**：当前 timing 落在 frame primary 上；mergeable 化后 marker 会分散在片段里，
  迁移批次需要定 per-mergeable query pool 归并或 `command_buffer_desc` 带 frame slot。
- **present 语义**：`submit(span)` 本批是"独立提交"（和 owned primary 一样，不 acquire/present）；
  帧路径保持原 submit。

## 6. 验收

- 契约/probe：批量顺序接受、空批/重复段拒绝、已提交段拒绝；
- 真设备 `test_texture_group --with-device`：两个 mergeable 各记录一次 buffer copy，一次 batch 提交后
  两次写入都落盘（本批实测 133 checks PASS）；
- `ctest` 49/49、完整冻结 OFF gate 15/15、`-Visibility` PASS、doxygen 0 warnings。
