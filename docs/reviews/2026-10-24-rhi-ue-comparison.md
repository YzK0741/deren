# deren RHI 与本地 UE RHI 对照

2026-10-24。比较对象为当前工作区与 `D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/` 的本地源码。本文是架构评估与建议，不是已批准的接口改造计划；未修改 RHI 实现，也未进行性能基准。

## 判断

deren 的问题不是缺少一个“大 RHI”。当前已经有稳定边界的意识、明确所有权、可选能力查询、范围同步、提交保活和渲染图。值得借鉴 UE 的是录制、完成、绑定、验证的职责边界，而不是其完整线程体系或功能规模。

## 证据与差异

| 方面 | UE 本地实现 | deren 当前实现 | 建议 |
|---|---|---|---|
| 录制与提交 | `IRHIComputeContext` / `IRHICommandContext` 录制；`IRHIPlatformCommandList` 表示完成的列表；`FDynamicRHI` 提供 finalize / submit | `command_buffer` 与 `mergeable_command_buffer` 重复大量绘制、拷贝和状态接口；主/次级的提交权限靠独立类型表达 | 保留提交权限区分，共用一个录制操作面；先整理接口，不先引入 UE 式 CPU 命令流 |
| 设备、帧与窗口 | 设备工厂、context、viewport、平台列表分别表达职责 | `api_core` 同时负责工厂、当前帧、swapchain、present、submit、profiler | 先按职责提供借用视图，保持同一 core 所有权；不用为了拆分创建多个 manager |
| GPU 完成 | UE 有 GPU fence、上下文提交以及后端删除队列接口 | 后端已有 fence / timeline 和 `pending_submission`；独立 `submit(command_buffer&)` 只返回 `error`，未向调用者提供此次提交的完成对象 | 暴露轻量 submission token：查询完成、等待、标识设备；先服务现有 graphics queue |
| 描述符 | shader 参数由 context 接收，Vulkan pending state 在 draw/dispatch 前更新与绑定；另有实验性的 descriptor range | 已有逻辑 `descriptor_space` / `descriptor_position`，但 runtime 仍计算 heap 字节偏移，push 常量也通过 `descriptor_heap::push_data` | 逐步扩大逻辑 descriptor space 覆盖面；将参数推送语义与物理堆分离，不另起一套并行绑定系统 |
| 同步 | `FRHITransitionInfo` 以访问语义和子资源表达，transition 创建还区分 pipeline | 已有 `scoped_synchronization`、stage/access、mip/layer/range；还存在较粗的旧 barrier 接口 | 图路径以精确同步为主，保留外部资源的显式起止状态；多队列需求出现时再增加队列间依赖 |
| 生命周期 | `FRHIResource::Release` 进入删除标记路径，后端处理删除队列；descriptor range 文档明确持有所引用资源 | `resource_cluster` / lease 持有 core 与资源；pending submission 保活命令、次级列表与 texture group | 区分 CPU 所有权和 GPU 使用期，补齐保活责任文档与缺口审计；不要将 cluster 销毁直接等同于 GPU 可回收 |
| 验证 | `FValidationRHI` / validation context 包装真实后端，追踪访问与录制状态 | 后端已有来源检查、pending/recording 检查和 scoped barrier validate，测试覆盖不少非法输入；没有发现对应的统一可选验证包装面 | 增量建立轻量开发验证，复用现有检查；不把完整跟踪器塞进 Release 热路径 |
| PSO | `PipelineStateCache` 提供查找、创建与预缓存入口 | 检查的 graphics / compute 创建路径直接创建 native pipeline；graphics 创建传空 native pipeline cache | 先统计重复创建与启动耗时，再做内容键复用和设备/驱动匹配的磁盘缓存；不先建设庞大预缓存系统 |

### 已有能力不能误判为缺失

- `scoped_synchronization` 已有资源描述、范围、验证和记录接口，不需要再发明一套基本 barrier 模型。
- `core.submission_lifetimes.cpp` 已按 fence/timeline 完成回收提交。差距是调用者无法直接使用这一完成信息，而非后端完全没有延迟保活。
- `resource_cluster` 是 engine 层成组所有权，符合此前约定；它与底层提交完成各司其职，不应搬成每个后端重复实现的资源管理器。
- `descriptor_space` 已包含 generation 和逻辑 position。应把现有接口推进完整，而非新增一个同义的 bindless manager。
- 当前 C++ vtable / module 边界针对相同工具链，不应宣称为任意编译器、标准库版本兼容的插件 ABI。UE 的模块边界也不能自动证明这种兼容性。

## 推荐顺序

### 第一批：整理现有机制，减少维护负担

1. **共用录制接口。** 将 draw / dispatch / copy / barrier / pipeline 等共同操作收敛到同一个 recorder 操作面。主列表拥有 execute / submit 资格，mergeable 列表继续只可组合提交；不能为了去重丢掉已有约束。调用点不再同时传两个互斥的 recording 指针。是否使用公共虚基类、独立借用视图或其他实现，需在 ABI 设计阶段决定。
2. **暴露已有提交完成信息。** 先提供 graphics queue 的轻量 completion token。完成、未完成、device lost、无效 token 应能区分；等待不必退回 device-wide `wait_idle()`。同时明确 token 是否持有 core、来自哪个设备、提交失败是否产生 token。
3. **清理职责边界。** 工厂、frame/presentation、command submission 可有独立查询视图，但仍由 core 实例代劳。能力信息尽可能以不可变快照表达，保留现有可选扩展入口。

这批并不直接承诺更高 FPS；直接收益是减少接口重复、明确何时安全回收，降低后续上传、readback 和流式资源实现成本。迁移可以保留旧入口转发，逐项替换调用点。

### 第二批：绑定抽象与开发验证

1. **逻辑描述符覆盖更多资源类型。** 从已有纹理 group/position 出发，覆盖实际使用的 sampled/storage image 与 buffer view；物理 stride、reserved range、native binding 留在后端。引擎仍可以定义 shader 使用的逻辑布局，不要求后端猜测 shader 槽位。
2. **参数推送成为录制语义。** 不再要求业务 pass 为传常量去了解 heap；不同后端在其能力约束下实现同一参数 ABI，不假定 DX12 能照搬 Vulkan 的字节限制。
3. **声明录制边界。** 普通图 pass 明确需要哪些 pipeline/资源状态，原生第三方录制声明自己会改写哪些状态及是否继承。后端可以标脏并重建受影响状态；这一建议不等于已查明此前 GUI workaround 的根因，也不预设新增“重置状态”就能解决驱动问题。
4. **可选验证。** 优先检查同设备来源、begin/end/render scope、pending 重录、descriptor generation 与合法范围；错误附带资源名和 pass/list 名。复用已有 error/error_info，不引入异常或到处 panic。

### 第三批：由实际收益决定的能力

- PSO 内容缓存和可选持久化：先量启动、热重载和重复创建。
- 上传批次与 readback ticket：以 completion token 为基础，先消除不必要的全设备等待。
- transient resource pool：先复用规格一致、GPU 已完成的资源；只有显存压力证明值得时再做真正的 aliasing。cluster 管成组逻辑生命周期，pool 管物理复用，二者不同。
- graphics/compute/copy 队列和跨队列同步：需要实际可重叠工作再做。`async_presentation` 不等于 async compute，不能混为一谈。

## 暂不照搬

- UE 的完整 RHI thread、CPU 命令对象流和翻译任务调度。deren 已有直接并行录制和次级组合，不先证明瓶颈就加一层 CPU 指令解释。
- UE 全量资源类型、历史平台兼容和所有 PSO 状态组合。
- 无需求就做 multi-GPU、稀疏资源、复杂 transient aliasing。
- 把实验性的 `RHIDescriptorRange.h` 当成已经冻结的成熟 API；文件本身明确标为 experimental，适合学习语义，不适合直接照搬。
- 为减少虚调用而立即把所有 RHI 对象改成整数 handle。没有测量证据支持这种整体重写。

## 源码定位

UE（以下路径相对 `D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/`）：

- `RHI/Public/DynamicRHI.h:198`：设备工厂；`:799` 起：context、finalize、submit。
- `RHI/Public/RHIContext.h:231`：完成的平台列表；`:254` 与 `:686`：compute / graphics context。
- `RHI/Public/RHIResources.h:53`：资源引用计数与待删除标记。
- `RHI/Public/RHITransition.h:118`：资源访问切换；`:355`：transition 创建。
- `RHI/Public/RHIValidation.h:43` 与 `RHIValidationContext.h:19`：验证包装与参数/访问检查。
- `RHI/Public/RHIDescriptorRange.h:6`：实验性逻辑 descriptor range 与资源生命周期。
- `VulkanRHI/Private/VulkanPendingState.cpp:373`：绘制前准备并更新/绑定 descriptors。
- `RHI/Public/PipelineStateCache.h:189` 与 `:244`：PSO 查找创建和预缓存。
- `RHI/Public/RHITransientResourceAllocator.h:535`：transient allocator 接口。

deren：

- `source/promise/rhi/rhi.api_core.cppm:1530` 与 `:1743`：两套录制接口；`:2063`：core 入口。
- `source/promise/rhi/rhi.extension.cppm:244`：范围同步；`:332`：物理 heap；`:654`：逻辑 descriptor space。
- `source/backends/vulkan/core/core.api_core.cpp:4710` 附近：独立提交的内部 fence 与保活；调用方仍无 completion token。
- `source/backends/vulkan/core/core.submission_lifetimes.cpp:38`：提交完成轮询。
- `source/backends/vulkan/core/core.declarations.cppm:1275`：pending submission 保活集合。
- `source/engine/resource_cluster/resource_cluster.cppm:74`：engine 层资源所有权。
- `source/engine/runtime/runtime.constructor.cppm:753`、`:1914`：上层 heap 偏移计算。
- `source/backends/vulkan/core/pipeline/pipeline.cpp:218`：native graphics pipeline 创建。
