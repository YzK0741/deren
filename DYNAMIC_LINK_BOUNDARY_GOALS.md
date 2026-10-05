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

## §解决方法总览（27 个符号 → 四种处置）

> 依据：`promise/rhi/*.cppm` 的**全部虚函数面**（`api_core`：`abilities` / `query_extension` / `create_{swapchain,buffer,image,sampler,shader,pipeline,query}` / `begin_commands` / `frame_image` / `frame_readback_buffer` / `frame_begin` / `present` / `wait_idle`；`command_list`：`use` / `copy_image_to_buffer`；`swapchain`：**只有 `release()`**）。

| 处置 | 数量 | 符号 | 解决方式 |
|---|---|---|---|
| **① 契约已有面，改调用点即可** | 5 | `acquire_next_image`（→ `frame_begin()` 已回填 `image_index`）、`present(uint)`（→ 无参 `present()`）、`make_sampler`（→ `create_sampler`）、`make_image_view`（→ `image::make_view`，ABI 7）、`make_shader_module`（→ `create_shader`） | 改 runtime/filters 的调用点；无契约改动 |
| **② 需要新增契约面** | 9 | `submit(VkCommandBuffer,uint)`、`wait_frame_slot(uint)`、`recreate_swap_chain()`、`to_next_frame()`、`render_extent()`、timing 的 4 个 | 见下方"新面形状" |
| **③ 直接删除** | 3 | `core::set_window_title`（连 `user_filter::set_window_title` 的转发一起）、`vma_allocator::log_statistics()` 的调用、`init_utils::default_task_pool_threads()` 的调用 | 删成员/删调用；线程数改由应用经 `create_info` 追加字段或 config 传入 |
| **④ 白名单（不删，记入翻转门）** | 7+2 | `vk_command_buffer`（移动构造/析构/`operator*`）、`vk_sampler::operator*`、`make_command_buffer`、`make_secondary_command_buffer`、`create_recording_pool`、两条 module initializer | 门改成"白名单外为零"，白名单进配置、只减不增、逐条打印 |
| **⑤ 最后一步** | 1 | `core::core(create_info const&)` | 见"顺序"第 ③ 步 |

### 新面形状（② 的 9 个）

| 面 | 建议形状 | 为什么是这个形状 |
|---|---|---|
| 提交 | `api_core::submit(command_list&)`（或 `submit_frame(uint frame_index)`） | 契约里 **没有 submit**：`command_list` 只有 `use`/`copy_image_to_buffer`，`api_core` 只有 `frame_begin`/`present`/`wait_idle`。提交是帧动词缺失的那一块 |
| 帧槽等待 | `api_core::wait_frame(uint frame_index)` | 只有 `wait_idle()`（等全部）；逐槽等待是帧环的语义，且**必须回传 `error`**而不是吞掉 `VkResult` |
| 交换链重建 | `swapchain::recreate(swapchain_desc const&)` | `swapchain` 现在只有 `release()`；重建是它的行为，不该回到 `api_core` 上 |
| 渲染分辨率 | `swapchain::extent()`（POD `image_extent`）+ runtime 自己乘 `render_scale` | 上次说"已有"是错的（`:298` 的 `extent()` 属于 `image`） |
| `to_next_frame` | 不新增：用 `frame_begin().frame_index` 表达环位 | 契约的 `submit_info` 已经带 `frame_index`/`image_index`，环推进是它的推论 |
| timing（4 个） | `mark(command_list&, uint mark_index, image_use) ` / `read_results(uint frame_index)` / `available()` + 容量查询 | 裁决 1：新增 timing 面；`mark` 必须**不**拿裸 `VkCommandBuffer` |

---

## A. 设备与帧生命周期（6 个）——2 个已有面、**4 个要新增契约动词**（此前写成"全有现成面"是错的）



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
| `core::render_extent() const` | runtime.cpp, runtime.frames | 让契约知道渲染分辨率 | ⚠ **`swapchain` 面目前只有 `release()`，没有 `extent()`**；要么给 `swapchain` 加 `extent()`，要么 runtime 自己按 `swapchain_desc` + `create_info::render_scale` 维护并在重建时更新 |
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

---

## §附 新目标：frame 环的面 `frame_walker`（虚基类，由 `api_core` 交出）

**形状（已定）**：做成**契约的虚基类**，由后端 `api_core` 交出，且是**借用视图**——与 `command_list` 同形（契约 :235 的规则：借用视图**不带 `release()`**，`object_manager` 不得包装它）。

**为什么这样比"引擎自己数游标"好**：游标仍然只有一个家——后端自己的 `current_frame`；引擎是**借视图**，不是持有第二个计数器。⇒ 没有两个真相源，也不需要"核对不一致"的机制（引擎侧方案才需要）。

### 它替掉的三样东西（实测）

| 被替掉的 | 处数 | 为什么重要 |
|---|---|---|
| `vk.current_frame`（字段读） | **29**（runtime.frames 19 / runtime.cpp 7 / filters.cpp 2 / filters.cppm 1） | **不产生符号**，但要求引擎知道具体 `core` 的类定义 ⇒ 翻转真障碍 |
| `core::MAX_FRAMES_IN_FLIGHT`（常量） | **12+**（runtime.constructor 377/819/837/848/861/872/988–991/1052–1053…） | 同上，另一种具体类型接触 |
| `to_next_frame` / `acquire_next_image` / `wait_frame_slot`（跨界符号） | 3 个 | 边界 27 → 24 |

### 接口草案

```cpp
/// 帧环的游标，作为 BORROWED VIEW（像 command_list：没有 release()）
struct frame_walker {
    virtual ~frame_walker() noexcept = default;
    [[nodiscard]] virtual std::uint32_t ring_depth() const noexcept = 0; ///< 替 MAX_FRAMES_IN_FLIGHT
    [[nodiscard]] virtual std::uint32_t slot() const noexcept = 0;       ///< 替 29 处字段读
    [[nodiscard]] virtual submit_info begin_frame() = 0;                 ///< 等该槽 + 采集 + 报本帧身份
    virtual void advance() noexcept = 0;                                 ///< 替 to_next_frame
};

// api_core 上新增一个虚函数：
[[nodiscard]] virtual frame_walker* walk_frames() noexcept = 0;
```

**命名**：类型 `frame_walker` + 存取器 `walk_frames()`——刻意避免"类型与存取器同名"（`frame_loop()` 返回 `frame_loop*` 读起来最差）。
**权威**：后端的 `current_frame` 是唯一权威；`begin_frame()` 返回的 `submit_info.frame_index` 与 `slot()` 必须一致，由尖刺断言。

### 落点清单

| 处 | 改动 |
|---|---|
| 契约 | 新类型 + `api_core` 追加一个虚函数 ⇒ **abi 12 → 13**（对已有 tier-1 类型追加虚函数＝形状变化），理由写进 `rhi.contract.cppm` |
| 后端 | `core::frame_walker_view`（与 `frame_image_slot` / `readback_slot_view` 同形的借用视图，委托给既有的 `current_frame` / `wait_frame_slot` / `acquire_next_image` / `to_next_frame`）+ `core::walk_frames()`；**构造函数里务必设 `owner = this`**（`address_view` 忘设 owner 的教训）；既有方法保留，它们从"接口"变成"实现" |
| probe | 必须实现新虚函数（它现在 20 个 `override`），并让测试的位遍历覆盖它 |
| 尖刺 | 断言 `walk_frames() != nullptr`、`ring_depth() > 0`、`slot() == frame_begin().frame_index` |
| 引擎 | ① 一次取面；② 29 处字段读 → `slot()`；③ 12+ 处常量 → `ring_depth()`；④ `wait_frame_slot` + `acquire_next_image` → `begin_frame()`；⑤ `to_next_frame()` → `advance()` |

**见证**：边界 **27 → 24**（三个符号消失），且 29+12 处不再命名后端类型；`ctest` 全绿、格式过、尖刺含新断言、**14 场景渲染哈希逐字节不变**。

**顺序**：契约 + 后端 + probe + 尖刺 为一段（面立起来、可独立验证、引擎不动）→ 引擎的 29/12 处替换为第二段（机械替换，一次一个文件，逐段过门）。
