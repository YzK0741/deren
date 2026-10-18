# Gaussian GPU Frame Preparation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** 将高斯逐帧粒子遍历、投影检查和排序迁移到 GPU，提高帧率并通过现有画质与生命周期验收。

**Architecture:** Vulkan 后端私有封装 vendored VRDX，通过可选 RHI 稳定 uint32 key/value 排序能力提供服务。GPU 每帧生成固定位置的键和值，将不可见项排序到末尾，展开可见前缀并写间接 draw 参数；保留现有六顶点光栅和同步 CPU 回退。动态 buffer 和 sorter 按完成帧槽保活，不依赖 swapchain image index。

**Tech Stack:** C++23 modules、Slang、Vulkan、VRDX 0.6.0、现有 RHI、CMake/CTest、Windows GPU 探针。

**Spec:** [2026-10-11-gaussian-gpu-frame-design.md](../specs/2026-10-11-gaussian-gpu-frame-design.md)

## Global Constraints

- VRDX 固定 `b0db22b0c65e17a9b803bc4daf20354898b820eb`，使用 `source/third_party/vrdx/include/vk_radix_sort.h`；不改第三方源码，不把其 CMake 子树接入正常构建，不联网生成 shader。
- 引擎不引入 Vulkan 类型或 native escape；新 recording virtual 提升 RHI ABI，枚举追加不重编号。
- 保持 3σ、0.3 pixel² 正则项、Cholesky 基、alpha <= 0.99、1/255 cutoff、SH 0..3、非预乘 blend 和近裁面相交省略。
- 排序 float32 沿 forward 深度，有限负深度编码为 uint32，正负零规范化，不可见键 UINT32_MAX。相同键按 stable instance ID、local index 确定排序。
- 正常 GPU 路径 CPU 不遍历粒子，不额外等待 GPU，不调用 wait_idle，不使用旧相机排序；能力或初始化失败回退原 CPU 路径。
- 保留当前未提交修复和已有 `gaussian-fixture.ply`。只对本轮新增变更组织提交；用户未要求提交前不执行提交操作。

## Review Focus

1. 乱序实例和重复深度不能随 GPU 调度闪烁：Task 2/4 验证稳定键和值、实例引用还原。
2. 全不可见帧及可见转不可见帧不能沿用旧 indirect count：Task 3/4 验证 reset/finalize 与零绘制。
3. Gaussian 卸载、替换或 resize 时两帧仍在飞行不能释放资源：Task 4/5 验证完成 slot 的拥有关系。
4. VRDX 覆盖 pipeline/layout/push 后不能使 finalize 或光栅读取错参数：Task 2/4 验证录制顺序和重新绑定。
5. 数值溢出、预算拒绝和缺能力必须有确定结果：Task 1/3/4 验证拒绝、整帧 skip 或初始化 CPU 回退。

## 文件与职责

- `source/promise/rhi/rhi.api_core.cppm`、`rhi.extension.cppm`、`rhi.contract.cppm`：indirect 命令、同步角色、可选排序接口与 ABI。
- `source/backends/vulkan/core/radix_sort.cpp`（新建）：唯一 VRDX_IMPLEMENTATION、sorter 所有权、scratch 查询、录制与校验。
- `source/backends/vulkan/core/core.declarations.cppm`、`core.api_core.cpp`、`core.constructor.cppm`：能力查询、句柄/命令接入、启用特性及计时容量；VRDX 实现不堆入现有大文件。
- `source/engine/gaussian_splatting/gpu_frame.cppm/.cpp`（新建）：GPU frame 数据、预算、slot 资源更新和 compute 录制。
- `source/shaders/gaussian/gaussian_projection.slang`（新建）：准备与 vertex 共享的纯数学 helper。
- `source/shaders/gaussian/gaussian_prepare.slang`（新建）：reset、prepare、finalize 入口。
- 现有 `source/shaders/gaussian/gaussian_raster.slang`、Gaussian pass、runtime：GPU 帧消费、间接绘制及 CPU 回退。
- `source/tests/test_gaussian_gpu_frame.cpp`（新建）：契约/fake 测试；`source/engine/gaussian_splatting/probe.cppm` + `gpu_probe.cpp`（新建）：真实 GPU 排序与输出探针。
（2026-10-18 核对：四个 shader 现位于 `source/shaders/gaussian/`，不是 `source/shaders/` 根目录；排序契约测试并入 `test_dynamic_link`（本文件“实施记录”已记），故没有 `test_gpu_sort.cpp`；探针的模块接口文件是 `probe.cppm`（`gpu_probe.cpp` 是其实现单元），没有 `gpu_probe.cppm`。）
- `CMakeLists.txt`、现有 shader 编译脚本、shader source 测试、CI：统一登记新模块、shader、测试与检查。

## Task 1: 通用间接绘制与排序契约

**Files:** 修改上述 RHI 三个分区、Vulkan recording adapter、`source/tests/test_dynamic_link.cpp`、`probe_backend.cpp`、`test_runtime_injection.cpp`、Gaussian/pass fake；新增 `source/tests/test_gpu_sort.cpp`，修改 CMake 测试登记。

**Interfaces:**
- 定义 `draw_indirect_command { uint32_t vertex_count, instance_count, first_vertex, first_instance; }`，size=16、offsets=0/4/8/12。
- `command_buffer::draw_indirect(buffer const&, uint64_t offset, uint32_t count, uint32_t stride) -> error`。
- `buffer_use::indirect_read`、`buffer_use::shader_read_write`，追加到末尾。
- 新 `radix_sorter : object` 拥有 `release() noexcept`、`requirements(uint32_t count) const noexcept -> radix_sort_requirements`、`record(command_buffer&, radix_sort_info const&) -> error`。
- `radix_sort_requirements { uint64_t scratch_bytes, buffer_alignment; buffer_flags scratch_flags; }`；flags 使用项目现有 buffer_flags 位掩码类型，不新增同义类型。
- `radix_sort_info` 包含 `element_count`、keys/values/scratch 三个 borrowed `buffer*` 与各自 uint64 offsets。
- 新 `gpu_sorting : extension`，`create_radix_sorter() -> radix_sorter*`；返回一个拥有引用，object_manager 管理。extension_kind 使用下一空闲 bit，不改既有 bit。

- [x] 写失败测试：16 字节布局；未录制/外来 buffer/错误 stride/越界拒绝；能力 bit 与 query_extension 一致；旧 ABI 在入口被拒绝。
- [x] 运行新测试和 dynamic-link/injection 测试，记录缺接口的 RED。
- [x] 增加接口、ABI 说明和所有 fake 最小实现；Vulkan indirect 映射 vkCmdDrawIndirect，同步分别映射 DRAW_INDIRECT/INDIRECT_COMMAND_READ 和 COMPUTE_SHADER/SHADER_READ|SHADER_WRITE。
- [x] 运行 `ctest --test-dir build-release-dyn-clang64 -R 'test_gpu_sort|test_dynamic_link|test_runtime_injection|test_pass|test_backend_boundary' --output-on-failure`，要求全通过；尚无 sorter 后端时不宣布能力。
- [x] 审核同轮 API/所有实现和 ABI 的一致性，记录本任务变更范围。

## Task 2: VRDX 后端与独立 GPU 排序探针

**Files:** 新建 Vulkan `radix_sort.cpp` 与 engine `gpu_probe.cppm/.cpp`；修改 backend core declarations/api/constructor、CMake、Gaussian probe 启动入口和 `test_gpu_sort.cpp`。

**Interfaces:** 使用 Task 1 sorter 接口；`run_gpu_sort_probe(api_core&) -> expected<void, foundation_error>`，探针可以等待自己的提交，正常 renderer 不得调用此等待。

- [x] 新增契约测试：keys/values/scratch 范围、对齐、storage usage、重叠范围拒绝、count=0 的无操作、一个 sorter 引用只释放一次；scratch overflow 不分配。
- [x] 写实际 GPU 排序探针，输入固定 uint32 keys 和原始位置作为 values，CPU stable_sort 为 oracle；没有 sorter 时必须 FAIL，不能跳过算 PASS。
- [x] 后端私有实现 VRDX，只在已启用 push descriptors、synchronization2、支持的 subgroup/共享内存满足时宣布 gpu_sorting。用 requirements 的真实 buffer usage 转译通用 flags，所有 VRDX operands 从合法后端句柄取得。
- [x] direct sort 使用 CPU 候选总数，elementCountBuffer=null；包装器承担库内部各阶段同步，记录接口明确前置读写发布和后置消费职责，声明调用会覆盖绑定状态。
- [x] 输入数覆盖 0、1、255/256/257、4095/4096/4097、随机 65,537；加入所有键相同、跨分区重复键、UINT32_MAX、递增/递减模式。键和值必须与 stable_sort 完全一致并可重复；失败禁止进入 Task 4 默认接入。
- [x] 用 synchronization validation 跑 GPU 探针，要求零错误；复核排序源码 rank/scan 稳定性与探针结论一致。
- [x] 运行 Task 1 测试及 backend boundary，确保 engine 无 Vulkan/VRDX 依赖；记录探针设备、版本、数量和 hash。

## Task 3: GPU 帧资源、准备与输出

**Files:** 新建 `gpu_frame.cppm/.cpp`、`gaussian_projection.slang`、`gaussian_prepare.slang`、`test_gaussian_gpu_frame.cpp`；修改 `gaussian_raster.slang`、现有 raster probe、shader source 清单、CMake/编译脚本/CI。

**Interfaces:**
- `gpu_frame` 为不可复制拥有对象，保留资产、sorter 与各 slot buffer；暴露 `candidate_count()`、`draw_buffer()`、`params()`，不提供虚假的 CPU visible references。
- `prepare_gpu_frame(api_core&, span<raster_instance const>, raster_camera const&, foundation_limits const&, gpu_frame* reuse) -> expected<shared_ptr<gpu_frame>, foundation_error>`：只做 CPU 实例校验/排序、预算与 slot 更新。
- `gpu_frame::record_preparation(command_buffer&, gpu_prepare_pipelines const&) const -> rhi::error`：reset → prepare → sorter → finalize；`gpu_prepare_pipelines` 拥有对应的三个 compute pipeline。录制仅写 GPU 命令，不修改快照的 CPU 元数据，因此 pass 可以持有 const frame。
- `gpu_frame::completed_stats(uint64_t frame_id) const -> gpu_frame_stats`：只能在 slot 完成后调用，包含 visible/outside/near_omitted/error 与对应 frame ID。

- [x] 写失败的 fake 测试：模型/相机/ID 校验，候选总数溢出，scratch 纳入预算，所有 slot 总量报告，零候选无分配，复用容量不新建 buffer，失败释放半成品，旧 frame 保留 asset。
- [x] 实现实例范围表按 stable ID 排列、映射回原实例索引；geometry/SH 静态布局保持不变。全量 keys/values 位置固定，无 atomic append。
- [x] 提取现有 shader 投影/SH/可见性数学为共享 helper；先运行原 GPU 五场景探针证明重构未改变图像，再新增 compute 使用。
- [x] reset 独立清零状态和 16 字节 draw；prepare 一粒子一次生成浮点深度键、固定 global index 值及统计，非有限检查记错误；finalize 展开排序前缀，任何错误使 draw.instance_count=0。
- [x] 状态使用小型 storage_coherent buffer，GPU 写后已有 slot 完成点读取；不在正常帧插入 readback 等待。若该 backend 无合法 host 映射，初始化失败回退 CPU，不偷用 GPU-only mapped()。
- [x] params 保持原 112 字节 raster_push 与附加三条 heap lane：GPU 模式 visible_count 字段只用作 candidate 上界，实际绘制数量来自 indirect；文档和 API 不将这个字段作为 GPU 可见统计。GPU shader 正常 draw 的 instance ID 总小于间接数量。
- [x] 扩展 GPU probe：先读回排序输入 keys/values 检查深度生成，再与这些实际 uint32 keys 的 CPU stable_sort 精确比对最终 references；单独统计 double 排序差异。验证近/远平面、四边足迹、SH 0..3、多实例和动态溢出整帧零绘制。
- [x] 运行新测试、原 Gaussian 数学测试、shader source 测试、shader 编译与 spirv-val；GPU 图像五场景维持最大 HDR RGB 误差 <=0.005。

## Task 4: Viewer 接入、回退与生命周期

**Files:** 修改 `source/engine/pass/gaussian_splatting.cppm/.cpp`、`runtime.declarations.cppm`、`runtime.frames.cppm`、`source/app/main.cpp`、GPU frame/pass 测试与 probe。

**Interfaces:** Gaussian pass 增加 `set_gpu_frame(shared_ptr<gpu_frame const>)`；CPU `set_frame` 保留，两个模式互斥。runtime 保留独立 `gaussian_gpu_frames` 完成 slot 数组；准备管线和 sorter 由 runtime 持有。

- [x] fake 录制测试 RED：GPU preparation 在 begin_rendering 前；sort 后重新绑定 finalize 和 raster 的 pipeline/push；compute→reference read 与 compute→indirect read 均出现；候选零不绘制，候选非零但 GPU 可见零也不按旧 count 直接 draw。
- [x] 接入 GPU slot 更新和 pass 间接绘制，attachment LOAD/depth restore、clear-only scene 和共享资源初始化保持现有修复；准备失败清空本帧的 GPU draw 引用。
- [x] 增加 `DEREN_GAUSSIAN_PATH=cpu|gpu|auto` 诊断入口；auto 优先 GPU，初始化缺能力/失败同步回退 CPU。明确 gpu 强制模式遇到 unsupported 时返回诊断失败，便于验收不意外测到回退路径。
- [x] GPU 数值错误由 finalize 归零并在 slot 完成后报告；录制错误跳过本帧 Gaussian；初始化后的录制失败不改用旧 CPU/GPU 快照。
- [x] 双 slot fake 测试覆盖清除/替换资产后 pending frame 仍保活、独立容量复用和增长回滚；运行时完成点控制 slot 复用，实际 resize 验证相机/extent/attachments。
- [x] 强制 CPU 和 GPU 分别跑真实花束固定姿态与连续旋转，检查 GPU 同输入重复确定性；运行空 mesh/空粒子、全不可见、三次 resize。asset 卸载/重载所有权用双 slot fake 测试覆盖，未做交互式 viewer 卸载/重载。
- [x] 正确性与回归运行开启 synchronization validation，零 VUID/SYNC-HAZARD；性能采样按 Task 5 关闭 validation。GPU 启动日志记录实际模式和候选数，并要求有效截图。

## Task 5: 独立计时、画质与性能验收

**Files:** 修改 runtime 计时 enum/labels/report、backend timestamp 容量、`profiling.cppm`、GPU frame CPU 计时、旧逐帧 Gaussian 日志；新增 `scripts/windows/check_gaussian_gpu.ps1`（2026-10-18 核对：脚本在仓库根的 `scripts/windows/`，原文写作 `source/scripts/windows/`，而 `source/scripts/` 不存在），更新 Gaussian 文档和本轮 review report。

**Interfaces:** 扩展现有 mark，不另开 VRDX 内部 15 mark 查询；Gaussian intervals 为 prepare、sort、finalize、raster。runtime 与 backend 的 mark capacity 同时由 16 扩到 32，所有数组和 query pool 容量使用该值。

- [x] 计时测试 RED：标记单调、容量足够、无 Gaussian 时保持标签顺序与零区间；lighting end 移到 Gaussian 前，Gaussian 后另隔出 transparent 区间，不将其算作 raster。
- [x] 每个 compute 阶段后记录 mark，sort 前后用已有外围 mark；CPU 实例准备单独计时。GPU 统计在 slot 完成后绑定正确 frame ID；timing 关闭不生成逐帧 cost 日志。
- [x] `check_gaussian_gpu.ps1` 生成同一 asset/camera/extent 的 CPU/GPU configs，分别确认实际模式，保留截图、HDR readback、比较数据与计时原始记录；失败不自动修改 reference。
- [x] 固定图像验收：小样本最大 HDR RGB <=0.005，花束 HDR RMSE <=0.005、最终每通道 RGB RMSE <=1/255。另检查边缘/极近深度高对比差异，不用平均误差掩盖局部缺口。超阈值禁止默认 GPU。
- [x] 性能运行 Release、1280×720，同硬件/asset/camera，关闭 validation、GUI、VSync 和逐帧日志；预热 60 帧后采样 300 帧，静止/旋转各重复三次。报告 median/p95、CPU 准备、四个 GPU 阶段、总 slot 内存。没有可重复收益则 auto 保留 CPU，报告瓶颈。
- [x] 最终运行全 CTest、format、必要 tidy、SPIR-V、backend boundary、原五场景/GPU 排序探针、moving/still 与 resize 回归；普通模型 `check_render.ps1 -Full -Compare frozen` 14/14 必须匹配。hardlink 边界检查若需 sandbox 外执行，只提升该验证命令。
- [x] 完成独立代码审查并处理具体发现；将设备/版本、图像差异、性能结果及剩余限制写入 `docs/reviews/2026-10-12-gaussian-gpu-frame.md`。报告明确 mesh shader 尚未实施，性能以测量为准。

## 执行顺序与检查命令

按 Task 1→2→3→4→5 顺序执行；Task 2 的真实稳定排序验证是接入默认 GPU 路径的前置条件。使用当前带未提交修复的工作区，不从 HEAD 新建遗漏这些修复的 checkout。每任务记录 RED/GREEN 和可审查 diff，无需用户逐项批准常规代码和测试。

构建沿用 `build-release-dyn-clang64`：`cmake --build build-release-dyn-clang64`。定向测试用 `ctest --test-dir build-release-dyn-clang64 -R 'test_gpu_sort|test_gaussian_gpu_frame|test_gaussian_raster|test_gaussian_splatting|test_shader_sources' --output-on-failure`；最终 `ctest --test-dir build-release-dyn-clang64 --output-on-failure`。项目 GPU Windows helper 采用隐藏进程，设置 VK_LAYER_VALIDATE_SYNC=1，并检查新日志和新截图。

建议由当前会话直接实施，接口和帧寿命高度相关，减少分任务交接；末尾独立审查。方案若因 VRDX 实测不稳定或 GPU 数值约定无法达标而需改变，先带失败证据修订设计，不静默改变画质规则。

## 实施记录

已完成并验收，详见 [验收报告](../../reviews/2026-10-12-gaussian-gpu-frame.md) 和 [执行记录](../../reviews/2026-10-12-gaussian-gpu-progress.md)。排序契约测试合并到现有 dynamic-link 测试及真实 GPU probe；GPU probe 实现复用现有 probe 模块，新增 headless test_gaussian_gpu_frame。全部决定和接口调整均记入执行记录。
