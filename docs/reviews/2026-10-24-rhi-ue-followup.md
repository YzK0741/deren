# RHI 标准复审、UE 对照与扩展方向

2026-10-24。对象为当前未提交工作区，以及本地 `D:/UnrealEngine-5.8.2-release/Engine/Source/Runtime/`。
这是审查和候选路线，不是已经批准的接口设计；没有修改 RHI 实现，没有新增性能测量。

## 结论

优先完成资源传输与完成凭据的使用链、收敛逻辑绑定、补开发诊断，而不是继续扩大 api_core。
上传批次、读回请求、延迟释放与简单资源池多数应由 engine 组合已有能力；帧完成、后端诊断、
内存统计等缺少底层事实的能力才考虑独立 RHI 扩展。多队列、完整 typed view、瞬态 aliasing 留待真实需求。

## 文档审查发现

1. **P1：排序操作数生命周期仍有直接冲突。** `docs/rhi_api_standard.md:1192` 的短说明说 operands
   “borrowed only during record”，紧接着的完整段落却要求到 GPU completion，且后者才与 `radix_sorter`
   声明一致。照短说明释放 buffer 会导致 GPU 使用已释放资源。应删除重复摘要，保留唯一的完整条款。
2. **P2：可实现后端的构建兼容边界没有写清。** 标准介绍 DLL/ABI，却未限定相同工具链、目标 ABI、
   标准库与相关构建配置。公开接口含 C++ vtable、span、expected、shared_object 等类型；C 名称的入口
   不会让整个接口获得任意编译器间的 C ABI 兼容性。UE 的引擎模块接口也不能作为这种保证的依据。
   应增加 supported build configuration 条款，区分项目内部插件 ABI 和跨工具链公共 SDK。
3. **P2：预留接口与 ABI 变更判据混在一起。** 第 9.3 节把补 query/swapchain 的新值类型也笼统归为必须
   ABI 54；第 5 节却按既有布局/vtable/签名变化判定。当前禁止调用预留接口的政策可以继续保持，
   但应说明哪些是版本政策、哪些是二进制布局事实。仅完成一个按引用传递的前置声明不等同于改变既有 vtable。
4. **P2：创建失败的叙述比实现的失败路径更整齐。** factory 条款通常写 empty，底层 VMA buffer 上传
   失败仍会 panic（vma.cppm:1150），command pool/buffer 创建也有 panic 路径。这不是本次引入的回归，
   但应列出哪些失败按 empty/error 返回、哪些被项目视为致命错误。不要把无异常写成“所有失败均可恢复”，
   也不要未经决定就承诺 recoverable OOM。若未来需要精确创建错误，新增扩展也必须打通底层错误传播。

第 8 节多处条款同时存在断句的短摘要和完整说明，造成同一项出现两套 Returns/Ownership/Threading。
下一次清理宜先处理这种结构性重复，再扩大条目数量。现有 test_docs 检查 ASCII、链接、输入与代码块等，
并不能发现语义矛盾；测试通过不代表标准已被证明一致。

## 与上一轮对照相比，已经完成的部分

旧的 `2026-10-24-rhi-ue-comparison.md` 是较早工作区的评估，以下内容不能继续标成缺失：

- `command_recorder` 已统一录制操作面，主列表与 mergeable 列表仍保留不同提交权限。
- `api_core.resources/commands/frames` 已有子视图。
- `command_submission` 已有设备标识、sequence、poll/wait 的值类型 token。
- `resource_group` 已覆盖 texture group、sampled/storage image、uniform/storage buffer、sampler、AS。
  Buffer 条目已经有 offset/size；不能把“支持 buffer range”再包装成新能力。
- push 已成为录制语义；已有延迟录制、状态简化、原生调用边界和状态恢复。
- 精确范围同步、cluster、GPU pending 保活和相关设备探针都已存在。

## UE 中实际值得借鉴的边界

以下路径均相对本地 UE 的 `Engine/Source/Runtime/`，是源码定位，不是性能保证：

| UE 机制与位置 | 可以借鉴的内容 | deren 的适配方向 |
| --- | --- | --- |
| RHI/Public/RHIGPUReadback.h:18、:132、:151 | fence 与 staging storage 组合成可查询的读回请求 | engine readback_request，区分数据就绪与呈现完成 |
| RHI/Public/RHIResources.h:1370、:2713、:3304 | buffer 描述、SRV/UAV view 描述与持有所属资源的 view | 先明确当前 raw/range view 的限制，必要时添加 typed/structured view |
| RHI/Public/RHIContext.h:341、:749 | context 接收 shader 参数与逻辑资源 | 保留 resource_group/push，让物理 heap 布局留在后端 |
| RHI/Public/RHIValidation.h:43；RHIValidationContext.h:21 | 可选验证包装与资源/访问追踪 | 从现有 recorder 的验证入口渐进增强，不重写整套代理对象系统 |
| RHI/Public/PipelineStateCache.h:177 起 | PSO 查找、创建与预缓存分工 | 先测重复创建，再做内容缓存；磁盘缓存必须校验设备/驱动 |
| RHI/Public/RHITransition.h:355 起 | transition 同时描述访问与执行 pipeline | 未来多队列时再扩展 queue dependencies，不能只增加一个 queue 枚举 |
| RHI/Public/RHITransientResourceAllocator.h:535 | 资源回收以执行完成区间为条件 | 先做 GPU 完成后复用的普通池，再考虑物理 aliasing |
| RHI/Public/DynamicRHI.h:463、:536、:575 | 内存统计与资源调试标签 | 新的可选统计/诊断扩展 |

`RHIDescriptorRange.h` 明确标注 experimental，并要求额外支持检查；可以学习类型分组与持有资源的语义，
不应照搬成稳定 API。UE 对 shader 切换后的资源状态要求也不能直接替换 deren 已经承诺的逻辑状态保留规则。

## 建议路线与扩展可能性

### 1. 上传批次、异步读回和完成后回收：先做 engine 服务

**实际入口：** `runtime.readback.cppm:82` 的截图读回仍调用 wait_idle；VMA staging upload 为创建期上传
单独提交并等待 fence。现在有 token，但没有把这些流程变成完整的资源传输服务。

- `upload_batch`：收集 staging ranges 和 copy operations，一批录制/提交，完成后统一回收 staging。
- `readback_request`：持有目标 readback allocation、范围、格式和完成信息；提供 ready/read，未完成不暴露可读结果。
- `retirement_queue`：在明确完成的凭据之后释放 engine 持有的资源，不改变底层 borrowed operand 约定。
- 创建时必须马上可用的同步路径仍可保留；运行期流式上传使用显式批次，不偷偷改变 factory 返回语义。

**关键限制：** `command_submission` 明确只接受独立 graphics 工作，不能提交或代表 acquired frame。
所以 buffer 独立读回可以先复用它，帧内截图不能假装已有同样凭据。帧读回需要以下二选一的后续设计：

- 增加可选 `frame_completion` 能力，把指定 acquire/submit epoch 与完成凭据关联；不能只给一个会被下一帧覆盖的 latest token。
- 或由后端提供具有内部完成跟踪的异步 frame-readback 服务，把 frame fence 留在后端。

先服务现有 graphics queue，不必同时建立专用 copy queue。回收、丢弃请求或调用者不再持有 ticket，
都不能让仍被 GPU 使用的 staging/source 提前销毁。这部分需要具体生命周期设计后再实施。

### 2. 扩展逻辑绑定覆盖，继续隐藏物理描述符

`resource_group_entry` 已有大多数资源种类，真正剩余的是：runtime 仍计算 heap_slot_stride/字节偏移，
而 `descriptor_space` 的公开写入面仍只面向 sampled texture group。先梳理这些实际调用点，区分
shader 的逻辑槽位与物理 descriptor storage，再迁移到统一的逻辑布局/位置解析。

不要新增与 resource_group 平行的“通用 descriptor 容器”。可以增加独立的 binding-layout/descriptor-access
扩展或新版本的描述结构，但其对象必须仍是逻辑资源与位置。兼容性、容量、generation、更新可见时点和
旧描述符的 GPU 使用期需要一起规定；只把 offset 换成整数 handle 不会解决这些问题。

Typed/structured buffer view 可作为后续能力：format、stride、range、读写用途、alignment。
当前 raw storage-buffer 路径已有 range，尚不能因此宣称所有 UE 式 typed SRV/UAV 都已支持，也无需为
当前不使用的格式先建设完整 view 类层次。

### 3. 可选开发验证与 diagnostics

比补一大批渲染功能更有价值的起点是：资源/录制/pass 名、首次失败操作序号、期望与实际状态、
设备来源、合法范围、pending 使用。复用 apply_operations 和 scoped validate 的现有入口。
先做 backend 内部的开发期开关；将来真的需要多个后端共用时再提取验证层，避免一开始创建所有对象的代理包装。

可以用独立 `diagnostics` 扩展提供资源标签、命令区间 marker、错误上下文。Debug marker 与实际操作顺序
关联，必须规定它们在延迟队列中的位置，不可当作可以随意删除的普通 setter。
`error_info` 已有固定 ABI 布局，不应直接往里面塞 owning string 或追加字段。

### 4. PSO 缓存与 memory_statistics

Graphics `pipeline.cpp:221`、compute `core.api_core.cpp:1806` 当前把 native pipeline cache 传为空。
这说明这些创建点没有使用 VkPipelineCache，不证明所有场景存在明显启动瓶颈，也不代表驱动内部完全没缓存。
先统计 descriptor/shader key 的重复、创建次数、耗时；再做 backend 内部进程缓存。磁盘缓存需设备/驱动标识、
格式版本和失败回退，shader binding 布局也必须进入 key。

PSO 内存缓存通常不需要公共 RHI 扩展；离线预热/缓存持久化确实成为上层需求时，再考虑 `pipeline_cache_control`。
`memory_statistics` 则适合独立扩展：以稳定快照给出预算、分配量、使用量、峰值和统计有效性，
不暴露 VMA/native allocator。它也为资源池、Subtilis 流式驻留和显存压力决策提供证据。

### 5. 后续能力：资源池、多队列和查询

- 简单 transient pool 放在 engine：规格匹配且 GPU 已完成才能复用。跨帧持有与 cluster 生命周期保持分离。
- 真正 aliasing 需要后端 memory requirements、对齐、兼容性和 alias barrier；届时再考虑 `transient_memory` 扩展。
- 多队列需要队列能力、提交依赖、跨队列 ownership/visibility 和完成语义；`async_presentation` 不等于 async compute。
- 当前 query/mesh_shader 扩展为预留面；已有 draw_mesh_tasks 和 GPU timing，不能混称为完全没有 mesh/timing。
  只有新的 occlusion/statistics 等实际需求出现，才定义完整查询扩展。

## 扩展 ABI 的落地规则

新增独立 extension 加新 bit，一般比给 device_capabilities 或 recorder 追加 virtual 更适合增量演进。
但“新接口不改旧 vtable”不是完整 ABI 审计：

- `all_extension_kinds()` 当前按值返回 array<extension_kind, 12>，增加条目会改变公开 helper 的返回类型。
  需要明确重新编译约定或设计稳定的枚举方式，不能只检查 vtable 就断言 ABI 完全不变。
- `resource_group_entry` 作为 span 元素没有自己的 struct_size；直接追加 stride/format 会改变元素步长。
  需要新版本描述结构/能力，或在一次明确的 ABI 修订中处理。
- properties 按值返回的结构、error_info、token 等也不能为了扩展随意追加字段。
- 新扩展要定义 null/ability bit 一致性、失败码、结构覆盖、同 core 来源、借用/持有关系、线程要求和测试。
  对已有可选能力不支持时明确拒绝；不要让扩展对象存在却悄悄不执行。

## 推荐实施顺序

1. 清理标准中的重复与冲突，并明确构建 ABI 和 fatal/recoverable 边界。
2. 先用已有能力实现 engine upload_batch/readback_request；同时为帧读回确定 completion 入口。
3. 迁移仍暴露物理 heap 布局的调用点；需要新的逻辑绑定能力时再制定扩展。
4. 逐步增加验证/diagnostics 与内存统计，测量 PSO 创建后决定缓存。
5. 有流式驻留压力和可重叠工作证据时，再做池化、typed view、多队列或 aliasing。

这些建议不承诺 FPS 增益。传输服务首先减少粗粒度等待并明确生命周期；缓存是否有收益、图录制是否值得并行，
都需要在对应场景下测量。当前审查只核对源码和文档，未运行新的设备性能实验。
