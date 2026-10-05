# 遗留跨界符号的目标清单

> 数据来源：`python scripts/check_backend_boundary.py --list`（本文件基于 `119ffcc`，即合入 Codex 的 ABI 9–12 堆面之后）。
> 当次读数：**27 个后端符号 / 44 处引用 / 3 个消费者 / 0 个携带 owning STL**；后端定义 1227 个符号。
> 工具分类：18 个 `core::core` 成员、4 个原生 RAII 句柄、3 个 `init_utils`、1 个 `vma_allocator`、1 个模块 initializer data。
> 区域（symbol/member 对）：runtime 27、other 15、pass 2。
>
> **"改进目标"的判据**：翻转门要的是"零依赖"，所以每个符号的目标必须是**它为什么该消失、消失后由什么承担**，不是"把它挪走"。
>
> **每一步的完成条件相同**：边界门降到该步目标值 且 `owning STL = 0` 且 `ctest` 全绿 且 `clang-format-check` 通过 且 **渲染 14 场景哈希逐字节不变**（至少锚点 `deferred = 972A31EC5FF55C87`、`laevatain_old_chain = 190EB09D3E9FDCDA`）。

---

## §0 裁决（已定，2026-10-05）

| # | 问题 | 裁决 |
|---|---|---|
| 1 | GPU 计时（4 个） | **契约新增 timing 面**：一次 mark、一次回读、一个可用位。不再拿裸 `VkCommandBuffer` |
| 2 | 录制面所有权（7 个） | **放进白名单，作为有意例外**——契约**不**新增 secondary/一次性 submit 形状 |
| 3 | `set_window_title` | **删后端成员，标题归应用**；`filters.cpp` 的那处调用一并删 |
| 4 | `vma_allocator::log_statistics()` | **删掉这条调用**，记账归后端自己的生命周期点 |
| 5 | `default_task_pool_threads()` | **宿主/应用决定线程数**，后端不推荐 |
| — | F 的中间态 | 默认**走 (a)**：等 A–E 清完，不为提前消 initializer 专门收窄 import。若你改主意再说 |

### §0.1 裁决 2 的后果：**翻转门到不了 0，门要重新定义**

白名单意味着**可达下限不是 0**。按现在的依赖关系，保留的例外会连带把两条 initializer 一起留在边界上：

| 例外 | 数量 | 为什么它必须留 |
|---|---|---|
| `vk_command_buffer`：移动构造、析构、`operator*()` | 3 | 引擎自己持有并录制二级命令缓冲（契约注释：*"secondary buffers stay the engine's"*） |
| `vk_sampler::operator*()` | 1 | filters 的采样器缓存要拿裸 `VkSampler` |
| `core::make_command_buffer()` / `make_secondary_command_buffer()` | 2 | 上面那些句柄的来源 |
| `init_utils::create_recording_pool(core&)` | 1 | 二级命令池的来源 |
| `initializer for module deren.vulkan.core` | 1 | 只要引擎还 import `deren.vulkan.core:handles`（上面 4 个句柄就在这个分区里），整模块的 initializer 就会跟着来 |
| `initializer for module deren.vulkan.init_utils` | 1 | `create_recording_pool` 留在白名单 ⇒ 这个 import 也留 |
| **可达下限** | **≈ 9** | 其余 18 个应清零 |

⇒ **翻转门的定义必须改**（这是裁决 2 的直接工作项）：`--require-zero` 要变成"**白名单之外为零**"，白名单写进门的配置里、**只减不增**（和棘轮同一条纪律），并且在报告里逐条打印白名单命中项。否则门永远红着，或者有人为了让门变绿去偷偷放宽它。

---

## A. 设备与帧生命周期（6 个）——目标：runtime 改走契约已有的帧动词

| 符号 | 调用方 | 目标 | 手段 |
|---|---|---|---|
| `core::to_next_frame()` | runtime.frames | 帧推进由契约驱动 | tier-1 帧动词（ring 推进的表达需与 `frame_begin` 对齐） |
| `core::acquire_next_image(uint&)` | runtime.frames | 采集归 `frame_begin()`（已回填 `image_index`） | **已有** |
| `core::submit(VkCommandBuffer, uint)` | runtime.frames | 提交归契约，不再把裸 `VkCommandBuffer` 交出去 | 契约的提交动词 |
| `core::present(uint) const` | runtime.frames | 用无参 `present()`（展示自己 `frame_begin()` 采集的那张） | **已有** |
| `core::wait_frame_slot(uint) const` | runtime.frames | 等待归契约，**且保留底层 `VkResult`**（不许统一映射成 `operation_failed`/`INITIALIZATION_FAILED`） | 契约等待动词 + 错误通道 |
| `core::recreate_swap_chain()` | filters.cpp, runtime.cpp, runtime.frames | 重建归 swapchain 面 | `swapchain` **已有**，需重建动词 |

## B. GPU 计时（4 个）——目标：契约新增 timing 面（裁决 1）

| 符号 | 调用方 |
|---|---|
| `core::mark_gpu_timing(VkCommandBuffer, uint, VkPipelineStageFlagBits)` | runtime.cpp |
| `core::begin_gpu_timing(VkCommandBuffer, uint)` | runtime.frames |
| `core::read_gpu_timings(uint)` | runtime.cpp |
| `core::gpu_timing_available() const` | runtime.cpp |

**目标形状**：`mark(command_list&, mark_index, stage)` + `read_results(frame_index)` + `available()`；一次 mark 的配额/容量（今天 `gpu_timing_mark_capacity = 16`）由后端约束并在契约里可查。

## C. 资源工厂与设备查询（7 → 5 个可动）

| 符号 | 调用方 | 目标 | 手段 |
|---|---|---|---|
| `core::make_sampler(VkSamplerAddressMode, float) const` | filters.cpp | 走 `create_sampler` | **已有**（api_core :691） |
| `core::make_image_view(VkImage, VkFormat, VkImageViewType) const` | filters.cpp | 走契约视图面 | **已有**（ABI 7 的 image 面） |
| `core::make_shader_module(span<uchar>) const` | filters.cpp | 走 `create_shader` | **已有**（:692） |
| `core::render_extent() const` | runtime.cpp, runtime.frames | 走 `swapchain::extent()` | **已有**（:298） |
| `core::set_window_title(string_view) const` | filters.cpp | **删后端成员**（裁决 3）：标题归应用；`filters` 的调用一并删 | 删除 + 应用侧设置 |
| ~~`core::make_command_buffer()`~~ | filters.cpp, readback.cpp | → **白名单**（§0.1） | 裁决 2 |
| ~~`core::make_secondary_command_buffer()`~~ | runtime.constructor | → **白名单**（§0.1） | 裁决 2 |

## D. 原生 RAII 句柄（4 个）——**永久例外**（裁决 2）

`vk_command_buffer`（移动构造 / 析构 / `operator*()`，调用方 runtime.constructor、readback.cpp、runtime.frames）与 `vk_sampler::operator*()`（runtime.frames）：**留在边界**，按 §0.1 记入白名单，并在翻转门报告里逐条打印命中。

## E. 构造、分配器、池（4 → 1 个可动）

| 符号 | 调用方 | 目标 | 手段 |
|---|---|---|---|
| **`core::core(rhi::create_info const&)`** | runtime.constructor | **引擎不再构造具体后端类**：构造经 `deren_make_api_core(abi, create_info*, error*)`，`core_owner` 静态类型变成 `shared_ptr<rhi::api_core>`（deleter = `deren_destroy_api_core`） | 入口**已有**；见"顺序"——它必须是最后一步 |
| `vma_allocator::log_statistics() const` | runtime.constructor | **删掉这条调用**（裁决 4），后端在自己的生命周期点记账 | 删除 |
| `init_utils::default_task_pool_threads()` | runtime.constructor | **宿主/应用决定线程数**（裁决 5）：runtime 从自己的配置取，不再问后端 | 删除调用 + 应用侧配置 |
| ~~`init_utils::create_recording_pool(core&)`~~ | runtime.constructor | → **白名单**（§0.1） | 裁决 2 |

## F. 模块 initializer data（2 个）——**没有独立目标**

它们是 import 图的产物：引擎不再 import 具体后端模块时消失。按裁决 2，其中两条会随白名单**长期保留**（§0.1）。走 (a)：不做中间态。

---

## 顺序

1. **① A（6）+ B（4）+ 两处删除**（`log_statistics`、`default_task_pool_threads`）：A 全有现成契约面；B 新契约面；两处删除独立、无风险。**这一步之后边界 27 → 15**（27 − 6 − 4 − 2）。
2. **② C 的 4 个**（`make_sampler` / `make_image_view` / `make_shader_module` / `render_extent`）+ `set_window_title` 成员删除：主战场是 `filters.cpp`（它一个人占 6 个）。**这一步之后 15 → 10**（10 = 白名单 8 + 构造 1 + 一条待定见下）。
3. **③ `core::core(create_info)`**：**最后一步**。只要引擎还命名具体 `core`（上一步之后仍剩白名单里的句柄与池），构造换入口也拿不到解耦；`core_owner` 的静态类型一变，才是"引擎不再知道后端具体类"成立的那一刻。**这是唯一的硬前置**。
4. **④ 翻转门的重新定义**（§0.1）：把 `--require-zero` 改成"白名单外为零"，白名单进配置、只减不增、逐条打印。

> 上面第 ② 步之后账面对不上（10 = 8 白名单 + 构造 1 + 1）：那 1 个是 `init_utils::create_recording_pool` 的 **initializer 与其来源的下游**——以实际 `--list` 为准，不在这里凑数。

## 未定的地方（不凑数）

1. **白名单是否含 `initializer for module deren.vulkan.core`**：取决于 clang 是否会因 `import deren.vulkan.core:handles` 而强制主模块 initializer。**要实测**：把 `runtime` 对 `:handles` 的 import 换成只 import 分区的写法，看 initializer 符号是否仍在清单里。测出来再定，不猜。
2. **`gpu_timing` 的容量与回读时机**（今天 16 个 mark、按帧回读）：新契约面落地时按实际需求定，不预先发明。
