# 遗留跨界符号的目标清单

## §现状综述（先读这段；细节在下面各节，**但下面各节是历史**）

**目标已达成**：后端是 `deren_vulkan.dll`，由 exe **按名加载**（`LoadLibraryExW` + 绝对路径 +
`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | …DEFAULT_DIRS`，不搜 `%PATH%`），**唯一导出 `deren_make_api_core`**，
`shared_ptr` 的控制块携带 deleter，**永不卸载**（`dynamic_link` 的 `detach()`）。

**当前读数（`dadea31`，本文件顶部这段话随每次落地更新）**：边界 **0 symbols / 0 站点**；import
**0 站点 / 0 文件**；白名单 **0 条**（`departures` 4 条：`deren_make_api_core`、`deren_destroy_api_core`、
`deren_abi_version`、legacy initializer）；`--require-zero` **exit 0**；`ctest` 18/18；尖刺
**95 checks / 0 failed**；scaffold **7/0**；渲染 **14/14 exit 0，VUID=0**；abi **18**（19 在途）。

**批⑥（A/B）落地后的读数补充**（不替换上一段的 `dadea31` 读数，只把"在途"落成事实）：abi **19**
（`api_core::api_version()` 追加 + 构造路径改签名）；`ctest` **19/19**（新增 `test_runtime_injection`）；
scaffold **10/0**（+3 条"root 被 runtime 接管"的身份断言）；加载与获取移到 **`deren.vulkan.backend_loader`**
（`load_api_core`），`runtime{core, options}` **不再自己加载**；边界 0、import 0、渲染 14/14 均**未移动**。

**门 = 四件仪器，不再是一个数字**：① 符号棘轮；② **import 图**（引擎/应用源码不得 import 任何
`deren_vulkan` 模块）；③ **导出表 / 导入表**（DLL 导出恰好一个 `deren_*`；DLL 导入 `shared_utility.dll`；
exe 与 DLL 都导入 `libc++.dll`）；④ **共享工具单副本**（`libshared_utility.a` 的成员名不得出现在别的归档里）。

**本文件其余部分（§0–§7）是历史**：它们是**当时**的裁决与路径记录（写作时读数是 27 符号 / abi 12）。
**不要再当现状读**；现状以本段与 `DYNAMIC_LINK_PROGRESS.md` 为准。

**后续（已记档，不必现在做）**：`pass/`/`readback/`/`core/filter/` 是否也搬出 `vulkan/`（用户"先做完再考虑"）；
后端去掉自带的 GLFW（只用 Win32 surface）；DLL 失去 sanitizer 覆盖（lld 无法链接 instrumented `-shared`）；
历史里约 95 MB 截图；`demo_lights` 不可复现（任何打开它的场景不能当门）；bloom 缺 config key。
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

白名单意味着**可达下限不是 0**。当时的账（本文写于 b③ 之前）估的是 ≈9；**实测已落到 2**
（③-D/E 的 C 批之后，abi 16，2026-10-06 实测）：

| 例外 | 状态 | 为什么它必须留 / 谁把它带走 |
|---|---|---|
| `core::core(rhi::create_info const&)` | **仍在（1）** | 引擎还在构造具体后端类；动态链接版 runtime 经 `deren_make_api_core()` 构造后离场 |
| `initializer for module deren.vulkan.core` | **仍在（1）** | 引擎侧仍有 **10 个 import 点 / 10 个文件** import deren_vulkan 的模块（见"import 图"，数字以 `--require-zero` 打印为唯一来源）；import 一断即离场 |
| ~~`vk_sampler::operator*()`~~ | **已离场（③-D/E C 批，abi 16）** | 引擎自己经契约 `create_sampler()` 创建并持有那 6 个渲染器采样器（`runtime::init_shared_samplers`），裸句柄走 `vulkan_escape::native_sampler()` |
| ~~`vk_command_buffer` 三件套~~ | 已离场（③-C + abi 15） | 契约 `command_buffer` 拥有型句柄 |
| ~~`make_command_buffer` / `make_secondary_command_buffer`~~ | 已离场（abi 15） | `api_core::create_command_buffer()` |
| ~~`init_utils::create_recording_pool` + 其 initializer~~ | 已离场（③-E） | 每缓冲自持 command pool + 删除 4 个 vestigial import |
| **可达下限（实测）** | **2** | 门就是按这个数跑的（`scripts/backend_boundary_whitelist.json`，`count: 2`） |

⇒ **翻转门的定义已经改完**（③-E）：`--require-zero` = "**白名单之外为零**"；白名单在
`scripts/backend_boundary_whitelist.json`（逐条写符号全名 + 理由 + 谁把它带走）、**只减不增**
（无命中项即失败；棘轮从未记录过的项也失败）、每次报告**逐条打印命中项**。`--update` 永不写白名单。

**§0.1b 符号门看不见的那一层：import 图（③-E 新增的第三件仪器）**

符号数可以归零而引擎仍在 import 后端的**模块**——而 import 模块才是 SHARED 后端真正的障碍
（BMI 必须来自 DLL）。所以 `--require-zero` 同时扫描 引擎/应用 源文件，对任何
`import <一个 deren_vulkan 拥有的模块>` 报 FAIL。模块归属取自 CMake 的
`target_sources(deren_vulkan ...)`，**不看名字前缀**：`vulkan/core/filter/filters.cppm` 声明
`deren.vulkan.core.filters` 却属于**引擎**（vulkancorekit）。

**实测（2026-10-06，③-E 落地时）**：引擎/应用 **38 个 import 点 / 33 个文件**（扫描 129 个源文件，
后端 5 个模块），拆解 `deren.vulkan.constant_init` **26**、`deren.vulkan.core` **9**、
`deren.vulkan.core.pipeline` **3**（**勘误**：③-E 提交正文里的 33/12/3 是报告前缀重复计数造成的；
总数 38 无误，拆解应为 26/9/3）；测试 1 个文件（`test_error_mapping`）。**目标读数：0。**

**逐步收窄的实测**（数字一律以 `--require-zero` 的打印为唯一来源）：
- `constant_init` 移进共享 target（第一步）：**38 → 12 点 / 12 文件**，后端模块 5 → 4；
- `acceleration_structure` / `ray_tracing` 脱离 `core`（1b）：**12 → 10 点 / 10 文件**；
- **当前 = 10 点 / 10 文件**（`deren.vulkan.core` 7 + `deren.vulkan.core.pipeline` 3），测试仍 1 个文件。
**这 10 点才是新 runtime 要清的东西。**

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
    [[nodiscard]] virtual std::uint32_t ring_depth() const noexcept = 0; ///< 替 MAX_FRAMES_IN_FLIGHT（名字待定）
    [[nodiscard]] virtual std::uint32_t position() const noexcept = 0;   ///< 替 29 处字段读（定名，原 slot）
    [[nodiscard]] virtual frame_open_info wait_and_acquire() = 0;        ///< 等该槽 + 采集 + 报身份（定名，原 begin_frame）
    virtual void walk_to_next() noexcept = 0;                            ///< 替 to_next_frame（定名，原 advance）
};

// api_core 上新增一个虚函数：
[[nodiscard]] virtual frame_walker* walk_frames() noexcept = 0;
```

**命名已定（2026-10-05，用户）**：`position()` / `wait_and_acquire()` / `walk_to_next()`；`aquire` 按仓库既有拼写（`acquire_next_image`）归一为 `acquire`。仍待定：`ring_depth()` 的名字、存取器名（`walk_frames()` 与 `walk_to_next` 同族，暂定它）。

**权威**：后端的 `current_frame` 是唯一权威；`position()` 与 `wait_and_acquire()` 报出的 `submit_info.frame_index` 必须一致，由尖刺断言。

### 融合形状强制决定的三件事（`wait_and_acquire()` 把今天的两步合成一步）

今天一帧开头的四步，顺序都是承重的（`runtime.frames.cppm:175-201`）：

```cpp
position = vk.current_frame;          // ① 读游标（不推进）
vk.wait_frame_slot(position);         // ② 主机等该槽的 timeline（0 = 从未提交，直接返回）
this->collect_gpu_timings(position);  // ③ 收集上一帧 GPU 计时 ← 夹在②与④之间
VkResult r = vk.acquire_next_image(image_index);   // ④ 采集；OUT_OF_DATE 由引擎重建并 skip
// 收尾（present 之后，:3297）：vk.to_next_frame();   // ⑤ walk_to_next()
```

| # | 必须决定 | 后果 |
|---|---|---|
| **1** | **③ 计时收集放哪** | (a) `wait_and_acquire()` 顺带收集/上报计时（把 profiling 关注点放进"环"的面）；(b) 引擎改成**采集成功后**再收集（**行为变化**：`OUT_OF_DATE` 那种被跳过的帧不再收集计时——今天是先收集后采集，所以照样收集）；(c) 引擎在调用**之前**收集——**不可能**，那时还没等，时间戳不可读 |
| **2** | **返回与错误通道** | 今天 `frame_begin()` 把 `OUT_OF_DATE` 与其它失败**都压成零值 `submit_info`**，引擎因此分不清"窗口变了"与"设备丢了"。`frame_open_info` 需要带 `error`（至少区分 `out_of_date` / 其它），否则换面只是换个地方丢信息；同理 `wait_frame_slot` 今天 `void` 丢 `vkWaitSemaphores` 结果 |
| **3** | `ring_depth()` 的名字 | 候选：`ring_depth()` / `slot_count()` / `frames_in_flight()`（后者最像 `MAX_FRAMES_IN_FLIGHT`，但带实现味） |

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

---

## §错误机制（定案，2026-10-05）

### 1. 分类：`enum class error` 追加，不新增类型

保留 0–12。**`device_lost` 名字不改**——API 报出的事实是"设备不可用"，而"驱动挂了/TDR/设备被移除"是**诊断**，放 `error_info.message` 与 `native_code`：同一个码也可能来自驱动更新、热拔、超频崩溃、VM 迁移，而调用方的动作完全相同。追加：

| 值 | 含义 | 调用方的动作（值存在的理由） |
|---|---|---|
| `out_of_date` = 13 | 交换链/目标过期 | **重建后重试**，本帧跳过（今天被压进 `operation_failed`） |
| `surface_lost` = 14 | 窗口/表面没了 | 重建表面；窗口已关则退出循环 |
| `timeout` = 15 | 等待/查询超时 | 可重试或降级（今天 `vkWaitSemaphores` 结果被丢） |
| `out_of_device_memory` = 16 | 设备内存耗尽 | **可恢复**：释放资源后重试 |
| `out_of_host_memory` = 17 | 主机内存耗尽 | 通常致命 |
| `initialization_failed` = 18 | 创建失败且不属于上述任何一类 | 报告并退出（工厂 `nullptr` 的"为什么"） |

**刻意不进枚举**：`suboptimal`（是状态不是错误，**按调用点翻**：acquire → `ok`，present → `out_of_date`）；extension/feature/format/layer_not_present → 已有 **`unsupported`**；too_many_objects / memory_map_failed / not_permitted / unknown / validation_failed → **`operation_failed`** + `native_code`；DX12 的 DEVICE_REMOVED / DEVICE_RESET → `device_lost`。

### 2. 区域：`error_zone` 是**函数**，不是字段

```cpp
enum class error_zone : std::uint8_t { api = 0, resource = 1, argument = 2, internal = 3 };
[[nodiscard]] constexpr auto zone_of(error code) noexcept -> error_zone;
```

契约内 zone **由 code 唯一决定**（`unsupported`→resource、`invalid_argument`→argument、`device_lost`/`timeout`/`out_of_*`→api、`abi_mismatch`/`operation_failed`/`initialization_failed`→internal），做成字段就多一个**能和 code 打脸**的真相源。**多来源的 zone 属于应用层**（graphics / asset / config / platform / internal），那里它才是独立信息，契约不背这个字段。

### 3. 诊断值：`error_info`（冻结，不带 `struct_size`）

```cpp
enum class graphics_api : std::uint8_t { unknown = 0, vulkan = 1 };   // 追加式

struct error_info {
    error code = error::ok;                   // 决策层
    graphics_api api = graphics_api::unknown; // 谁产生的
    std::int32_t native_code = 0;             // 原始码；**有符号**（Vulkan 为负，-1000001004 塞无符号就是另一个数）
    std::string_view message = {};            // 后端静态文本（跨边界不能带 std::string）
    std::source_location where = {};          // **后端失败点**捕获（不是引擎调用点）
};
```

三条必须写下的前提/依赖：
1. `error` 保持 `: std::uint32_t`（与契约其余 6 个 enum 一致）；
2. `std::source_location` 进契约的前提是 **两半同一工具链**——这条已被 `vstd`（libc++ std 模块的裁剪分支）与 BMI 隐含强制，现在写成显式条款，并用断言钉住（`static_assert(std::is_trivially_copyable_v<error_info>)` + clang64 上 `sizeof`/`offsetof`）；
3. `file`/`function` 指向后端静态存储，安全性**依赖不变式 4（DLL 从不卸载，`detach()` 就是为它做的）**——写进字段注释。

生产者 helper（后端内部；位置只能在失败点捕获，给虚函数加默认实参会捕到**引擎**的位置）：

```cpp
[[nodiscard]] constexpr error_info failed(error code, std::uint32_t native_code = 0,
                                          std::string_view message = {},
                                          std::source_location where = std::source_location::current()) noexcept;
```

### 4. 翻译在后端，**按调用点**（不是一个全局函数）

三个私有 helper：`acquire_error(VkResult)` / `present_error(VkResult)` / `generic_error(VkResult)`。

- acquire：`OUT_OF_DATE`→out_of_date；**`SUBOPTIMAL`→ok**；`TIMEOUT`→timeout；`SURFACE_LOST`/`NATIVE_WINDOW_IN_USE`→surface_lost；`DEVICE_LOST`→device_lost；`OUT_OF_DEVICE_MEMORY`/`OUT_OF_POOL_MEMORY`/`FRAGMENTED_POOL`→out_of_device_memory；`OUT_OF_HOST_MEMORY`/`MEMORY_MAP_FAILED`→out_of_host_memory；其余→operation_failed
- present：同上，但 **`SUBOPTIMAL`→out_of_date**
- generic（创建/查询/提交）：`INITIALIZATION_FAILED`/`INCOMPATIBLE_DRIVER`→initialization_failed，其余同上

**不裁头文件、不建码表、不生成 .inc**：实测那 41 个 VkResult（23 个在扩展区间）里绝大多数永远到不了我们手上；只列"可能收到"的，其余归 `operation_failed` + `native_code`。

### 5. 从 hopper 学两条 + 改一条规矩

- **`ensure(cond, msg, loc)`** 进 `deren::utility`（`panic` 已在那儿，签名与 hopper 一致），**release 也生效**；
- **`verdict` + `enforce(outcome, classify)` + `propagate`** 放 utility：三态 `pass_success` / `pass_failure{error_info}` / `fatal{error_info}`，**classifier 谎报即 panic**；deren 的 `frame_status`（proceed/skipped/closed/*_failed）**就是现成的 verdict 类型**，帧路径的手写 if 阶梯改成 `classify_acquire` / `classify_present`；
- **改一条规矩**：**`fatal` 由引擎执行**——后端只上报（`device_lost` / `out_of_host_memory` 之类），不自己 `_Exit`；今天启动期后端直接 panic 的几处是**待迁移的例外**。

**§5b 待迁移例外清单：启动期后端直接 panic 的几处**（翻转期随"后端只上报、引擎执行 fatal"一起处理；**每条都记在这里，不单独做 plumbing**）

| # | 位置 | 为什么现在 panic | 迁移后的形状 |
|---|---|---|---|
| 1 | `core::init_swap_chain()`：`"Swap chain not adequately supported"`（formats/present_modes 为空） | 该函数是 `noexcept`，同文件同类创建失败本来就 panic | 经 `initialization_failed` + `message` 上报到 `deren_make_api_core()` 的出参 |
| 2 | `core::init_swap_chain()`：驱动报的 swapchain 图像数 **> `rhi::max_swapchain_images`**（③-D/E A1 的守卫） | 同上；而且这是"后端承诺被驱动打破"的情形，必须大声 | 同 #1（并保留两个数字与两条出路：提高契约上界，或走 abi-17 `swapchain::image_count()`） |
| 3 | `core::core()`：其余启动期 `panic(...)`（命令缓冲/池/清理注册等处的创建失败） | 同上：构造期没有可返回的错误通道 | 同 #1 |

### 6. abi 与顺序

| 变化 | 是否跳 abi |
|---|---|
| 追加 enum 值（13–18）、`error_zone`/`zone_of`、`error_info`/`graphics_api`、`ensure`/`verdict`/`enforce` | **不跳** |
| 入口 `error*` → `error_info*`、`api_core` 追加 `walk_frames()`、新类型 `frame_walker` | **跳（12 → 13，一次做完）** |

**顺序**：① enum 值 + `error_info`/`graphics_api`/`zone_of` + `ensure`/`verdict`/`enforce` + 表驱动测试（新建 `tests/test_error_mapping.cpp`，纯 CPU 无 GPU）→ ② 后端三个翻译 helper + 两处丢失点修复（`wait_frame_slot` 回传结果、`frame_begin` 区分 `out_of_date`）→ ③ 与 `frame_walker` 同批改入口与 `wait_and_acquire()`。

**见证**：表驱动测试全绿 + 布局 `static_assert` + 尖刺里 `error_info.code` 断言 + `ctest` 全绿 + 边界仍 27（本批不动符号）+ 14 场景哈希逐字节不变。

---

## §utility 拆成 `shared_utility` / `static_utility`（定案，步 ④ 的前置）

**动机（实测，不是洁癖）**：`utility` 现在是 **STATIC**（`CMakeLists.txt:220`）且被两半都链；静态配置下链接器只保留一份，所以日志看起来是单实例。**翻转那天** `deren_vulkan` 变 SHARED ⇒ DLL 自带一份 utility ⇒ **两份日志器、两个 `ofstream` 指向同一个 `debug.log`、两次启动轮转**（`utility.cpp:155/179` 的 `rotate_previous_log()` 会**截断**另一份正在写的文件，而第一个句柄仍按自己的偏移继续写 ⇒ 交错/空洞/丢行，`debug.log.old` 收下两个"会话"）。而且**边界门对此完全盲**：它只量 `deren_vulkan ∩ vulkancorekit`，utility 是**第三个库**、不产生后端定义符号。

**拆法**

| | `shared_utility`（**进程一份**，翻转时 SHARED） | `static_utility`（每半一份，恒 STATIC） |
|---|---|---|
| 内容 | **日志 sink + 轮转 + 文件句柄**（`utility.cpp` 的落盘部分）、**panic 汇聚**、分配器钩子（`better_pmr` 的进程级部分）、线程数策略（若两半需一致） | BVH、data_block、frame_clock、frame_stats、thread_pool（每对象状态）、platform_*（无状态）、**`dynamic_link`**、所有模板 |
| 接口形状 | **C 形状/POD 为主**：`void deren_log_text(std::string_view)`、`[[noreturn]] void deren_panic(...)`、钩子注册；**格式化留在调用方**（`log(fmt, args…)` 仍是 inline 模板，转发到导出的非模板核心）⇒ **sink 单实例，模板不必导出** | 同二进制，随便 |
| 依赖方向 | **不依赖** `static_utility` | **可以**依赖 `shared_utility`（用它的 sink）；**反向禁止**；**契约 `rhi` 不依赖两者** |

**为什么 `dynamic_link` 必须留 static 侧**：它是**加载后端的那把钥匙**。放进 `shared_utility` 就变成"exe 必须先加载 shared_utility 才能加载后端"的引导链；而它本身不需要进程级共享。

**增量路径（每步保持树绿）**

1. **现在**：只做**模块/目标拆分**，两个目标**都仍为 STATIC**（行为零变化：一份 sink，与今天相同）——可独立验证（`ctest` + 边界仍 27 + 14 场景哈希逐字节不变）；
2. **翻转时**：`shared_utility` 转 SHARED，与 `deren_vulkan` 翻转**同批**；exe 与后端 DLL **都**静态导入它 ⇒ Windows **按名字只加载一次** ⇒ 单实例（这是机制，不是约定）；
3. **门**：补一条**可检查**的——后端 DLL 的**导入表里出现 `shared_utility.dll`**（而不是自带一份 sink）。尖刺那套导入表检查（窄导出 / Q6 那次）已验证过，工具是现成的。

**未决（留给步 ④）**：目标名拼写（`shared_utility`/`static_utility` vs `utility_shared`/`utility_static`）、模块名（`deren.utility.shared`/`.static` 还是独立顶层模块）、以及 `shared_utility.dll` 的版本/兼容检查是否并入后端包（倾向：并入同一包与搜索路径，**导入表本身就把配对关系固定住**）。

**与"最小修复"的关系**：在拆分落地之前，仍建议先做那条零成本修复——**轮转只由一方做一次**、后端日志走明确路由——因为两次轮转一旦翻转就是必现的日志损坏。

---

## §GPU profiler（定案，与 `frame_walker` 同批）

```cpp
struct gpu_profiler {                       // BORROWED VIEW（无 release()）
    [[nodiscard]] virtual std::uint32_t stage_count() const noexcept = 0;   // 最近一个**已完成**帧的阶段数
    [[nodiscard]] virtual error get_stage_info(std::uint32_t index,
                                               std::string_view* name,
                                               std::uint64_t* duration_ns) const noexcept = 0;
};
[[nodiscard]] virtual gpu_profiler* profiler() noexcept = 0;                // api_core 新增
```

**类型**：`pstring` → `std::string_view*`（视图已在契约里，自带长度，不靠 NUL）；`pduration` → `std::uint64_t` **纳秒**（后端实测 **1 ns/tick、64 valid bits** ⇒ 无损；显示层再转 ms）。

**错误映射**（这就是"接入错误机制"）：`index >= stage_count()` → `invalid_argument`；该帧未完成 / 该阶段无 mark → `not_ready`；设备或配置无 GPU 计时 → `unsupported`（且 `stage_count()` 恒 0）；读回查询失败 → `device_lost`。

**阶段名归谁**：后端只知道"第 N 个 mark"与 `VkPipelineStageFlagBits`（管线阶段，不是语义阶段）；语义名在引擎（`vulkan/profiling` 的 `cpu_phase`）。**定案：名字随 mark 进来**——`mark(command_list&, uint32_t mark_index, std::string_view stage_name, …)`，后端只存**指针**并在 `get_stage_info` 原样返回；规则与 `window_title` 同：**必须是静态文本**（引擎传字面量，比后端活得久）。**不**在契约里定义 `stage_id` 枚举把引擎的阶段划分固化进来。

**它关闭的悬案**：`wait_and_acquire()` 融合"等 + 采集"后，今天夹在中间的 `collect_gpu_timings` 没位置。定案：**收集归后端**（`wait_and_acquire()` 内部、取图之前 latch 该槽上一帧的计时 —— 被 `OUT_OF_DATE` 跳过的帧照样收集），**读归引擎**（返回之后读 `profiler()`）。**行为零变化**，且引擎不再需要在"等"与"采集"之间插步骤。

**落点**：后端视图委托既有 `gpu_timing_mark_capacity=16` / `mark_gpu_timing` / `read_gpu_timings(frame_index)` / `gpu_timing_available()`；名字存进视图的 16 项指针数组；probe 必须实现 `profiler()`；尖刺断言 `profiler() != nullptr`、`stage_count() <= 16`、有 mark 后 `get_stage_info(0,…)` 给出非空名字与正数纳秒、越界返回 `invalid_argument`、无计时设备返回 `unsupported`；引擎侧 `mark_gpu_timing`/`read_gpu_timings` 调用点改走契约。
---

## §frame_open_info（定案，选 A）

```cpp
struct frame_open_info {
    submit_info frame = {};   // 仅当 result.code == error::ok 时有效；否则零值，不得使用
    error_info result = {};   // 决策层 + 诊断（where 由后端在失败点捕获）
};
[[nodiscard]] virtual frame_open_info wait_and_acquire() = 0;   // frame_walker 上
```

**为什么按值返回、而不是 `expected` 或出参**：契约里已有两条失败风格（工厂的裸指针 + `nullptr`；`command_list::use` 返回 `error`），再引入 `std::expected` 就是第三条，且要统一就得整片重写工厂。A 是**消除信息丢失的最小一步**，且不关门（将来统一时 `frame_open_info` → `expected<submit_info, error_info>` 是一次干净替换）。

**三条必须写进注释的语义**

1. `result.code == error::ok` ⇒ `frame` 是已开始的帧；否则 `frame` 为零值——**保留今天"零值 = 没起帧"的可读性，但现在知道为什么**；
2. `result` 可能来自**两步中的任何一步**（等槽 / 采集），`where` + `message` 指出是哪一步——这就是把 `wait_frame_slot` 今天丢弃的 `vkWaitSemaphores` 结果补回来的地方；
3. **按值返回的 POD 是冻结的：加字段 = 跳 `abi_version`。** 这与出参方向的 `error_info*` 不同（那里 `struct_size` 能保护追加）：**按值返回的 ABI 取决于结构大小**，旧引擎 + 新后端会直接错调约定。⇒ 一次设计到位。

**用法**（与已定的 classifier 纪律对接；今天的两个 if 阶梯就是它的内容）：

```cpp
inline constexpr auto classify_acquire = [](frame_open_info const& open) -> verdict {
    if (open.result.code == error::ok)                    return pass_success{};
    if (open.result.code == error::out_of_date)           return pass_failure{open.result};  // → skipped + 重建
    if (open.result.code == error::device_lost ||
        open.result.code == error::out_of_host_memory)    return fatal{open.result};
    return pass_failure{open.result};                                                       // → acquire_failed
};
```

**通用规则（追加进错误机制那节）**：**按值返回的契约 POD（`submit_info`、`frame_open_info`、`error_info`）一律冻结——任何字段变化都跳 abi；只有"由调用方提供存储"的出参结构才靠 `struct_size` 支持追加。**
---

## §命名定案（帧面 + 性能面，全部定案）

```cpp
api_core:
    walk_frames() -> frame_walker*     // 借用视图；与 walk_to_next 同族；避开"类型与存取器同名"
    profiler()    -> gpu_profiler*     // 借用视图

frame_walker:                          // BORROWED VIEW（无 release()）
    slot_count()                        // 环里有多少格（替 MAX_FRAMES_IN_FLIGHT 的 12+ 处）
    position()                          // 本帧所在槽（替 29 处 vk.current_frame 读点）
    wait_and_acquire() -> frame_open_info  // 等该槽 + latch 该槽计时 + 取下一张图
    walk_to_next()                      // 收尾推进（present 之后）

frame_open_info { submit_info frame; error_info result; }   // 按值返回，冻结

gpu_profiler:                          // BORROWED VIEW
    stage_count()
    get_stage_info(index, std::string_view* name, std::uint64_t* duration_ns) -> error
```

**为什么是这几个名字**（一句话各一条）：
- `position()` + `slot_count()` 成对——"第几格 / 共几格"，一眼可读；`frames_in_flight()` 会把**后端的运行期词汇**搬进契约，`capacity()` 会被误读成缓冲容量；
- `wait_and_acquire()` 把"等"与"采"写在名字里（行为见 §附 与 §frame_open_info）；
- `walk_to_next()` 只承诺"环往前走一格"，不承诺它管不了的"帧结束"语义（`end_frame()` 会暗示后者）；
- `walk_frames()` / `profiler()` 与契约既有存取器同风格（"你拿到什么"：`begin_commands()`、`frame_image()`、`frame_readback_buffer()`）；
- `stage_count()` / `get_stage_info()` 由用户定，落为定案。

**至此目标文档内无待裁决项**，四批（① 错误机制 / ② 帧面 / ③ 27 个符号 / ④ utility 拆分）均可直接开工。