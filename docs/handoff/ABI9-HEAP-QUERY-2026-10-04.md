# 阶段 1a：描述符堆查询

接续阶段 0 的 `7582414`。本批只迁移 ready/limits/resource_size 查询，未改变描述符写入、push、bind 或二级命令缓冲的录制路径。

- ABI8 → ABI9：vulkan_escape 追加 `heap_ready() const noexcept` 和 `heap_properties() const noexcept`。
- properties 是含整数的 POD 值，逐字段复制真实 heap_limits 和 resource_size；不返回后端类型、引用或分配对象。ready 仍执行原来的真实就绪条件，不用能力位伪造结果。
- runtime 查询经 RHI 接口转发；const 录制步骤借用原有 core 引用查询，不修改帧的 const 接口。仍不宣布尚未接线的 portable descriptor_heap 能力。
- 新接口加入后先编译 producer：exit1，确认 frame_escape 缺两个 pure virtual 实现；`heap-query-red.log`。
- 补齐实现和消费者后，首轮发现两处 const core 局部别名不能用于非 const query_extension；改用 runtime 持有的原有 core 引用。最终全目标构建 exit0；`heap-query-green-final.log`。
- 新鲜边界报告和 `--update` exit0，35 →32，恰好删除 ready、limits、resource_size，0 owning-STL；未放宽边界检查。
- 未运行 CTest、CPU 测试程序、窗口或 GPU。查询结果映射已静态核对，实际运行在异机验收。

日志及 JSON：`D:\deren-workspace\work\abi8-followup`。下一批为原生 image/buffer 描述符写入，使用明确的 Vulkan escape POD，避免在这个批次改动二级命令缓冲的所有权和录制时序。
