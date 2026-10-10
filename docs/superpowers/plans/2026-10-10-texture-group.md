# Texture Group Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 实现可实际采样的 16 槽不可变 texture_group；各 API 后端使用同一个 assign/load 接口；Vulkan 仅实现 descriptor heap，提交完成前资源不被回收。

**Architecture:** RHI 只暴露组对象、输入槽位和管线使用约定。Vulkan 管理 sampled view 租约、描述符、80 字节 GPU record 与完成点。先验证资源及提交寿命，再接入 heap 探针，最后接入普通 draw 和一个现有渲染路径。

**Tech Stack:** C++23 modules、现有 Vulkan/VMA/descriptor heap、Slang、SPIRV-Reflect、现有 CMake 与 vk_test。

**Spec:** `docs/superpowers/specs/2026-10-10-texture-group-rhi-vulkan-design.md`

## Global Constraints

- 固定容量 16，默认空 optional；optional(nullptr) 与 nullopt 同义。
- 前五槽为 engine 的 PBR 顺序，后十一槽由 pipeline 定义；present_mask 不等于 enabled_mask。
- 不可变 sampled2D16，只接受本 device 的 owned、sampled、单 layer、非 cube 2D image。
- group 不延长 api_core/device/DLL 寿命；不把普通 draw 的 wait_idle 当作保活实现。
- 空组有效；load(nullptr) 清除上一组。失败不修改已绑定组，不返回半初始化对象。
- 不暴露内部 token、描述符索引或 Vulkan 句柄；成员不使用尾缀下划线；object.type 使用静态具体名称。
- 不顺带迁移 indirect 每 draw 材质、RT hit 材质表、可变组或整个 renderer 的设备创建回退模式。
- 每阶段保存验证结果并分批本地提交；剩余额度到 5% 时停止并写进度报告。

## Review Focus

- 提交后立即释放 group 与 primary/secondary：GPU 仍读到原纹理，分配不可提前复用。由任务 2/3 的延迟提交测试验证。
- 新增组时已有组正在执行：descriptor set 不就地更新，record/descriptor 只在最终 lease 释放后复用。由任务 1/4 验证。
- 绑定 pipeline 或写其他 push 参数：不能继承上一个 pipeline 的组，也不能覆盖 token。由任务 3 验证。
- 异常输入及中途耗尽：不遗漏 error 输出、不泄漏 view、record 或 image 引用。由任务 1 验证。
- 同名 Vulkan 对象来自另一个 device：名称不代替来源校验。由任务 1/3 验证。

## Implementation Decisions

### Public contract

在 `rhi.api_core.cppm` 增加 `texture_group_capacity = 16`、`texture_group_info`、`texture_group`、`texture_group_ref`。
`texture_group` 构造函数接收静态 type 名称，Vulkan 使用 `deren_texture_group_vulkan`。

```cpp
struct texture_group_info {
    uint32_t struct_size = sizeof(texture_group_info);
    std::array<std::optional<image*>, texture_group_capacity> textures{};
};
using texture_group_ref = std::shared_ptr<texture_group>;
virtual texture_group_ref assign_texture_group(texture_group_info const&, error* result = nullptr);
virtual error load_texture_group(texture_group_ref const&);
```

两项虚函数追加到既有接口末尾；不支持的 backend 返回 unsupported。ABI 30 → 31，同步三处版本测试。
`pipeline_desc` 末尾追加 `texture_group_binding`：enabled、push_byte_offset、shader_stage_bits、profile=sampled_2d_16。
stage bits 的公开取值为 vertex=1、fragment=2、compute=4、mesh=8；不接受零值或未知位。
首版不允许 RT pipeline 启用该绑定；返回 unsupported，后续通过 hit 材质表显式接入。
旧 struct_size 的 descriptor 默认不启用组。启用时 token 是独立的 4 字节 uint，偏移四字节对齐。
Vulkan pipeline 仅支持现有 descriptor heap；RHI 不增加 Vulkan 模式开关。其他 API 自行映射其绑定操作。

### Resource budgets and backend mapping

首版固定最多 1024 个非空身份组，最多 2048 个 sampled-view heap lease（含 dummy）；不在线扩容。
组表有 1025 个 80 字节 record：record 0 全空，每个非空组占一个 record。
记录布局是 indices[16]、present_mask、padding[3]；CPU 与 Slang 校验 80 字节、mask 偏移 64。

heap 的 group-table descriptor 位于槽 17664，之后 2048 槽给 texture lease。
该范围独立于旧纹理范围，检查实际 heap 容量及 stride。固定池耗尽返回 out_of_device_memory。
显式 nothrow 分配失败返回 out_of_host_memory；STL 分配沿用项目无异常构建行为。

用户于 2026-10-10 明确：统一各 API 的绑定接口，不为 Vulkan 提供向下兼容。
因此取消 Vulkan runtime-array shader、descriptor set/layout/pool 和双 record 设计。
其他 API 可采用纹理数组等模型，具体映射属于其后端实现，不属于本轮 Vulkan 工作。

dummy 是 backend 持有的有效 sampled 2D image/view；所有空槽填 dummy，不使用 nullDescriptor。
共享 sampler 沿用现有配置。group table 使用 host-coherent 内存，发布前完成写入；
提交提供 host-write → shader-read 可见性。已有 record 不被新增组改写。

### File boundaries

- `source/promise/rhi/rhi.api_core.cppm` / `rhi.contract.cppm`：公开接口、pipeline 元数据与 ABI。
- 新建 `source/backends/vulkan/core/core.texture_groups.cpp`：factory、lease/cache、record 分配、load 与释放。
- 新建 `source/backends/vulkan/core/core.submission_lifetimes.cpp`：完成点轮询与提交拥有对象。
- `core.declarations.cppm`：以上实现的声明及状态；`core.api_core.cpp`：bind/record/execute/submit 接入；
  `core.cpp`：frame submit、slot wait、wait_idle 与 teardown 接入。
- `descriptor_heap.cppm`、`pipeline/pipeline.cppm`、`pipeline/spirv_parser.cppm`：容量验证、匹配 pipeline/layout 与 push 检查。
- 新建 `source/shaders/texture_group.slang` 与 `texture_group_probe.slang`；`CMakeLists.txt` 注册源码、includes、shader 与探针。
- 新建 `source/tests/test_texture_group.cpp`；现有动态边界/runtime 测试同步 ABI 及默认不支持行为。
  CPU 契约断言放入既有 headless 测试；新增 GPU 探针按现有 runtime probe 注册，设备测试需 `--with-device`，不加入 VR_TEST_TARGETS。
- `source/engine/primitive/primitive.cppm` / `.cpp`、`runtime/runtime.frames.cppm` / `runtime.constructor.cppm`、
  `source/shaders/unlit.slang`：最后接入一个既有普通 draw 路径。

## Task 1 — Immutable groups and rollback

**Consumes:** ABI 30 image::share()、owned image 来源注册表、默认 image view 创建。
**Produces:** assign_texture_group、owned_texture_group、sampled_view_lease、texture_group_state、固定 record 池。

- [x] 添加 CPU 契约测试：16 个默认空槽，slot15、optional(nullptr)、截断 struct_size 与旧 ABI 被拒绝。
- [x] 添加设备 factory 测试并观察红灯：全空组成功，合法 owned image 成功，外来 device/borrowed image/
  cube/array/storage-only 拒绝；result 在所有路径有确定值。
- [x] 实现接口、ABI 31 与 factory；先收集 image 拥有引用并释放 image registry 锁，再访问 group/cache 池，避免锁顺序反转。
- [x] 先完整验证输入，后分配 lease/record；失败倒序回滚。cache key 包含拥有 identity、格式、范围与 sampled2D 角色。
- [x] 测试原 image factory 引用释放后仍由组保活、重复纹理共享 view、组销毁后引用消失，以及满池失败后释放可再次创建。
- [x] 运行目标测试与 test_dynamic_link；提交资源模型与验证结果。

## Task 2 — Recording and submission ownership

**Consumes:** texture_group_ref、existing owned_command_buffer、frame_commands、frame timeline。
**Produces:** recording group/secondary leases、owned-command intrusive allocation lease、pending_submission。

- [x] 写失败测试：submit 后释放原 primary/secondary 引用不销毁 native pool；未完成时不能 reset/re-record；完成后回收。
- [x] owned command buffer 的 factory 引用和内部 lease 分开计数；recording 状态保存 group 与被 execute 的 secondary allocation lease。
- [x] owned submit 使用内部 fence；frame submit 在实际 submit_frame 成功路径捕获引用，关联真实 slot timeline value，不能只改 api_core::submit。
- [x] 失败 submit 不创建 pending 状态。每次提交单独保留 command allocation 与该次 group 集合；并发重复提交按 Vulkan 用法拒绝或独立跟踪，不能覆盖完成点。
- [x] assign/load、slot wait、wait_idle 及 teardown 轮询/排空完成记录。组/lease 的实际析构在 pool 锁外执行。
- [x] primary 录制的 secondary 在 primary 丢弃或完成前不能被改写；secondary 自行 bind/load，不能假定继承组。
- [x] 运行生命周期测试；提交完成点跟踪与验证结果。

## Task 3 — Heap pipeline, load and shader reads

**Consumes:** 任务 1/2 的组、lease、完成点，以及 pipeline_desc::texture_group_binding。
**Produces:** heap 模式 assign/load 可实际采样，公共 Slang 组接口、compute/普通 draw 探针。

- [x] 写 GPU 探针红灯：slot0/slot15 为已知颜色，重复 image、空槽、A/B 交替、load(nullptr)、同组重复 load。
- [x] 实现 heap table/texture descriptors；load 校验同 device、正在 recording、已绑定且支持组的 pipeline。
- [x] bind_pipeline 为支持组的 pipeline 写全空 token；成功 load 保留组、绑定 heap 并写 token。失败保持原组。
- [x] 用 SPIRV-Reflect 验证 token 成员是该偏移的 uint32、阶段与 push 范围匹配，超出设备限制或重叠成员时拒绝 pipeline。
- [x] 其他 push 写入与 token 范围重叠时拒绝；相关 draw/dispatch 路径保持 token 已初始化，不能只靠 bind 缓存。
- [x] Slang 提供 GetLoadedTextureGroup、HasTexture、LoadTexture2D 和共享 sampler getter。
  LoadTexture2D 的返回类型用 TextureGroupTexture2D alias：Vulkan 为 DescriptorHandle<Texture2D<float4>>；
  保持 Sample/SampleLevel/SampleGrad/Load/GetDimensions 的使用语义，非一致索引传递到实际资源索引。
- [x] 用 GPU readback 验证提交后释放 image/group/command 的颜色、mask 与空组结果，不能以 use_count 替代采样验证。
- [x] 测试不支持 pipeline、跨 device 组、无 recording、pipeline 切换、secondary 与延迟完成点；提交 heap 可用阶段。

## Task 4 — Ordinary graphics probe

**Consumes:** 已验证的 heap group、公共 Slang 接口和 pipeline 绑定元数据。
**Produces:** 普通 graphics draw 的纹理组采样及 GPU readback 验证。

- [ ] 添加已知颜色的普通 draw 探针，覆盖组交替及空组清除。
- [ ] 核对 fragment token、其他阶段 push 布局以及实际 graphics 状态。
- [ ] 延迟提交后释放调用者引用，验证 GPU 颜色和 validation 日志。
- [ ] 提交 graphics 探针与验证结果。

## Task 5 — Existing ordinary draw integration

**Consumes:** heap probe 已验证的 factory/load，现有材质 image、普通 pipeline 与 draw 参数。
**Produces:** unlit 普通 draw 实际使用 texture_group，其余 pipeline 保持未启用组的兼容行为。

- [ ] 为 unlit 每个材质创建/保存 group：按 PBR 0..4 填充已有 image；既有材质因子继续独立传入。
- [ ] 给 unlit 明确增加 token push 成员与 CPU 布局，创建 pipeline 时声明绑定；不挪用 spare_lane。
- [ ] 在普通/实例化 draw 前 load；shader 的底色槽从组读取，缺失槽使用原材质默认语义。
- [ ] mesh/meshlet/RT 等未接入入口维持原路径；不把 unlit 的验证说成全材质迁移完成。
- [ ] 运行 unlit、缺纹理材质、组交替及完整 frozen 渲染回归；提交首个产品路径与结果。

## Task 6 — Final review and report

- [ ] 完整构建 `cmake --build build-release-dyn-clang64 -j10`；CTest `--output-on-failure -j8`。
- [ ] 运行 texture_group 的 heap 生命周期与 GPU 探针、runtime device 测试、现有两条 RT probe。
- [ ] `check_render.ps1 -BuildDir build-release-dyn-clang64 -Full -Compare frozen`；hash 匹配且 validation/log 检查通过。
- [ ] 独立审核容量耗尽、不同 device、提交失败、并发使用、secondary 保活、shutdown 与 feature 缺失；修复重要问题。
- [ ] 更新设计状态与 `docs/reviews/2026-10-10-texture-group.md`，记录已接入路径和未迁移路径，分批本地提交。

## Execution handoff

推荐本会话由主 agent 顺序实施，阶段末做独立审核。资源、命令和 pipeline 的接口依赖较强，
不适合多人同时修改 core.declarations/core.api_core；独立 shader 测试可在接口稳定后拆分。
计划 review 重点是 heap 槽位与提交寿命，以及首个产品路径限定为 unlit 普通 draw。
