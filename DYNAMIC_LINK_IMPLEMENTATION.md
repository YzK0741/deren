# deren 动态后端：实施交接（详细版）

> 面向对象：接手实施的下一个 agent（无本会话上下文）。
> 本文件只讲**怎么做**；决策理由与全部实测证据在 `DYNAMIC_LINK_BOUNDARY_GOALS.md`（424 行，v4 目标清单）。
> 状态基线：**主干 `master`**；代码层最后一个**已验证**版本是 `119ffcc`（其后所有提交都是 markdown）。

---

## 0 先读什么

| 文档 | 作用 |
|---|---|
| **`DYNAMIC_LINK_BOUNDARY_GOALS.md`** | 目标清单 + 六条已定裁决 + 四个面的定案（错误机制 / `frame_walker`+`gpu_profiler` / 命名 / utility 拆分）。**动手前通读**，尤其 `§现状综述`、`§错误机制`、`§frame_open_info`、`§GPU profiler`、`§命名定案`。 |
| `docs/handoff/ABI{8,9,10,12}-*.md`、`docs/rhi/RHI-TYPE-EXTENSIONS-2026-10-05.md` | 上一批（ABI 9–12 堆面）的实现笔记，风格与陷阱参考 |
| `docs/rhi/VERIFY_buffer_face.md` | 一次**独立验证**的完整报告（集合算术 / 所有权审计 / 门 / 两个诚实负面结论）——本仓库的验收标准长什么样，看它 |

---

## 1 现状与复核（先跑一遍，别信本文）

实测数字（`119ffcc`）：

| 项 | 值 |
|---|---|
| 契约 `abi_version` | **12**（`promise/rhi/rhi.contract.cppm`） |
| 跨边界符号 | **27 symbols / 44 引用 / 3 消费者 / owning-STL 0**；后端定义 1227 符号 |
| `ctest` | **17/17** |
| 尖刺 `--with-device` | **50 checks / 0 failed / 自行退出** |
| 渲染 14 场景 | 全部跑通，**每个实际哈希与合入前逐字节相同** |

复核命令（在仓库根，MSYS2 clang64 的 `bin` 在 PATH 里）：

```powershell
cmake --build build-release-clang64
ctest --test-dir build-release-clang64
cmake --build build-release-clang64 --target clang-format-check
python scripts/check_backend_boundary.py                 # 期望 OK: 27 symbols, baseline 27
python scripts/check_backend_boundary.py --list           # 27 个符号 + 调用方
cmake --build build-spike-clang64 --target test_backend_boundary_spike   # 只建 target！
& .\build-spike-clang64\test_backend_boundary_spike.exe --with-device
pwsh -File scripts\windows\check_render.ps1 -Full -BuildDir build-release-clang64
```

**渲染门的读法（重要）**：本机参考集是 2026-10-01 的旧集，`CHANGED` 是预期，**脚本必然 exit 1**（CHANGED 计入 `$fail`）。判据是**打印出来的实际哈希**，必须与下列冻结值一致：

```
deferred             972A31EC5FF55C87
deferred_taa_fxaa    4021B16AFDB2F43E
deferred_ssao_off    BFE3A472FBAB0B5E
shadow_single        A92C5965316679F3
unlit                F3C2D7FEFDAD864F
transparent_blend    CC7F77F93487AA5E
sponza               50AF7E46CC1E2A92
metal_rough_glossy   A1AFBFB61DBFD104
glossy_motion        9F31E89BE38B771C
deformation          723569BA0D03640C
laevatain_goo_toon   CF5A34D8DF6B6FFC
laevatain_goo_toon_body C3365CEEB8AD3723
laevatain_no_sidecar E9A2983BEB57D5C5
laevatain_old_chain  190EB09D3E9FDCDA
```

---

## 2 环境、构建与门

**工具链**：MSYS2 **clang64**（clang 22.x + libc++ + lld），CMake ≥ 4.3，Ninja；`-fno-rtti -fno-exceptions -Werror`；C++23 modules；**slangc 必需**（无则 configure 失败）；`slangc` 版本 2026.18.2。

**门（每个批次都必须全过，缺一不可）**

1. 全目标构建 exit 0；
2. `ctest` 全绿；
3. `clang-format-check` exit 0 —— 注意**构建过程本身会跑 `clang-format -i`**（`Formatting sources with clang-format`），它会改源码；把格式化带来的差异**单独归因**，别当成修复；
4. 边界门读数达到该批目标值（见批③）；
5. 尖刺 `--with-device` 0 failed 且**自行退出**；
6. **渲染 14 场景哈希逐字节不变**；
7. 翻转门 `--require-zero`：**白名单之外为零**（③-E 改完），并且**引擎/应用不得 import 一个 `deren_vulkan` 拥有的模块**。

**门的具体跑法（③-E 之后）**

```powershell
python scripts/check_backend_boundary.py                  # 棘轮：只降不升；报告里打印白名单命中项与 import 读数
python scripts/check_backend_boundary.py --update         # 把基线收紧到实测值（永不写白名单）
python scripts/check_backend_boundary.py --list           # 符号清单 + 全部残留 import + 后端模块名
python scripts/check_backend_boundary.py --require-zero   # 翻转门：白名单感知 + import 图（见下）
```

- **白名单**：`scripts/backend_boundary_whitelist.json`，逐条写**符号全名 + 理由 + 谁把它带走**，每次运行逐条打印（`HIT` / `STALE`）。三种失败：实测符号在白名单之外、白名单项无命中（只减不增）、白名单项是棘轮从未记录过的（新依赖不能靠改这个文件变成合法）。`--update` 永不触碰它。
- **import 图**：符号数可以归零而引擎仍在 import 后端的**模块**，而 import 模块才是 SHARED 后端的真正障碍（BMI 得来自 DLL）。门读取 CMake `target_sources(deren_vulkan ...)` 里的模块名（**不看名字前缀**：`deren.vulkan.core.filters` 其实是引擎模块），扫描引擎/应用源文件；测试源单独统计、不进门。
- **实测（2026-10-06，③-E 落地时）**：符号 **3** / 11 站点 / 0 owning-STL；import **38 点 / 33 文件**，拆解 `deren.vulkan.constant_init` **26**、`deren.vulkan.core` **9**、`deren.vulkan.core.pipeline` **3**（**勘误**：③-E 提交正文写的 33/12/3 来自报告前缀的重复计数；总数 38 无误），测试 1 文件。
- **第一步（`constant_init` 独立成 `vulkan_constant_init` target）之后**：import **12 点 / 12 文件**（`core` 9 + `core.pipeline` 3），后端模块 5 → 4；测试仍 1 文件。动态链接版 runtime 的目标是 **import = 0**。

### 已踩过的坑（照做，别重踩）

| 坑 | 现象 | 处理 |
|---|---|---|
| **clang 22 codegen 崩溃** | `toon_screen_rim.cpp` / `upscale.cpp` 编译期崩（`__libcpp_allocate` 路径） | 两个文件已加 `#include <new>`（注释写明"aligned `operator new` 必须可见"）。**别再删这行** |
| **`FreeLibrary` 死锁** | 后端 init 过之后卸载 DLL，进程 0 CPU 卡死 80+ 秒 | **永不卸载**（不变式 4）。加载器有 `detach()`：交还句柄、析构不再卸载。测试/尖刺都用它 |
| **尖刺整树构建失败** | `cmake --build build-spike-clang64` 在 3 个无关测试 target 上 `clang frontend command failed due to signal` | **只建 target**：`--target test_backend_boundary_spike` |
| **`git add -A`** | 两次事故：把构建日志、把 24 张根目录截图（≈95 MB）扫进提交 | **显式列路径**；仓库已加 `/screenshot_*.png` 忽略规则 |
| **控制台 exe 弹窗** | 用 `Start-Process` 跑 console 子系统程序会弹窗口 | 用 `&` 直接跑，或后台 job |
| 共享构建树竞争 | 链接报 `Permission denied`（别人正在跑 exe） | 一次只一个人构建；重试即可 |
| 半成品树的度量 | 引擎归档比后端旧时，边界门会**少报** | 门有 300 s 提醒；**先构建完整再量**；`--list` 的读数为准 |
| 已释放句柄 | `native_buffer(*b)` / `device_address::buffer_address(*b,…)` 的前提是**调用方只能问这个后端交出去的句柄**（无 RTTI 无法核对类型） | 严守契约前提；别把已 `release()` 的句柄传回去 |

---

## 3 硬性纪律

**要**：
- 每笔提交**自带 witness**（谁证明它有效：测试名/命令/数字），提交信息写"为什么"而不只是"做了什么"；
- 改动契约（`promise/rhi/**`）时同步更新 `rhi.contract.cppm` 的**版本注释**（每条跳号写清理由）；
- 新增/改动契约面时同步三处：**probe**（`tests/probe_backend.cpp`，目前 20 个 override，编译期强制）、**尖刺**（断言）、**引擎侧调用点**；
- 大量改动前先 `python scripts/check_backend_boundary.py --list` 存档现状（集合，不是计数）。

**不要**：
- 改写已推送的历史 / 强推 `master`（共享分支，另一条 session 也在用）；
- 合并 `codex/dynamic-link-v3`（**无共同祖先**，且它自己标注 compile FAILING；它是**归档**）；
- 用"计数下降"当翻转证明：**棘轮通过 ≠ 翻转完成**，翻转门是 `--require-zero`（批③要把它改成"白名单之外为零"）；
- 把"没跑过的测试/没量过的数字"写成结论（本仓库对此零容忍，`docs/rhi/VERIFY_buffer_face.md` 里连验证者自己的措辞都被纠正过）。

---

## 4 批① 错误机制（**不跳 abi**，不动符号）

### 4.1 契约：追加错误值

`promise/rhi/rhi.contract.cppm` 的 `enum class error : std::uint32_t` 保留 0–12（`device_lost` **不改名**），追加：

| 值 | 含义 | 调用方的动作 |
|---|---|---|
| `out_of_date = 13` | 交换链/目标过期 | 重建后重试，本帧跳过 |
| `surface_lost = 14` | 窗口/表面没了 | 重建表面；窗口已关则退出 |
| `timeout = 15` | 等待/查询超时 | 可重试或降级 |
| `out_of_device_memory = 16` | 设备内存耗尽 | 释放资源后重试（可恢复） |
| `out_of_host_memory = 17` | 主机内存耗尽 | 通常致命 |
| `initialization_failed = 18` | 创建失败且不属于上述任何类 | 报告并退出 |

追加值**不跳 abi**。`suboptimal` **不进枚举**（它是状态，按调用点翻译，见 4.5）。

### 4.2 契约：`error_zone` 是**函数**

```cpp
enum class error_zone : std::uint8_t { api = 0, resource = 1, argument = 2, internal = 3 };
[[nodiscard]] constexpr auto zone_of(error code) noexcept -> error_zone;   // 实现见下
```

映射（契约内 zone 由 code 唯一决定，**不做字段**——字段会与 code 打脸）：

```
unsupported                                        -> resource
invalid_argument                                   -> argument
device_lost / timeout / out_of_*_memory /
surface_lost / out_of_date                         -> api
abi_mismatch / operation_failed /
initialization_failed / ok                         -> internal
```

### 4.3 契约：`error_info`（冻结，**按值返回的 POD 一律冻结**）

```cpp
enum class graphics_api : std::uint8_t { unknown = 0, vulkan = 1 };   // 追加式

struct error_info {
    error code = error::ok;
    graphics_api api = graphics_api::unknown;
    std::int32_t native_code = 0;        // **必须是有符号**：Vulkan 的码是负的（-1000001004）
    std::string_view message = {};       // 后端静态文本（跨边界不能带 std::string）
    std::source_location where = {};     // **后端失败点**捕获，不是引擎调用点
};
```

三条前提写进注释：
1. **两半同一工具链**（`std::source_location` 布局由实现定义；`vstd`/BMI 已隐含强制这条，现在写成显式条款）；
2. `file`/`function` 指向**后端静态存储**，安全性依赖**不变式 4（DLL 从不卸载）**；
3. 断言钉住：`static_assert(std::is_trivially_copyable_v<error_info>)` + clang64 上 `sizeof`/各字段 `offsetof`。

生产者 helper（放后端内部，例如 `vulkan/core/core.error.cppm` 或现有 declarations 的私有区）：

```cpp
[[nodiscard]] constexpr error_info failed(error code, std::uint32_t native_code = 0,
                                          std::string_view message = {},
                                          std::source_location where = std::source_location::current()) noexcept;
```

**只能这样捕获位置**：给契约虚函数加默认实参会捕到**引擎**的位置（默认实参在调用点求值）。

### 4.4 `utility`：学 hopper 的两件（**同二进制内**，随便用 `std::string`）

```cpp
// deren::utility
void ensure(bool condition, std::string_view description = "",
            std::source_location location = std::source_location::current()) noexcept;   // 假 → panic（release 也生效）

struct pass_success {};
struct pass_failure { rhi::error_info failure; };   // 退化为"改写后的错误"
struct fatal        { rhi::error_info reason; };
using verdict = std::variant<pass_success, pass_failure, fatal>;
inline constexpr auto propagate = [](auto const& outcome) -> verdict { ... };
template <typename value_type, typename classify_type>
auto enforce(result<value_type> outcome, classify_type&& classify) -> result<value_type>;
```

- `result<T>` = `std::expected<T, rhi::error_info>`（**引擎侧**；契约不引入 `expected`）；
- `enforce` 内两条守卫**照抄**：classifier 对失败判 `pass_success`、对成功判 `pass_failure` ⇒ **都 panic**；
- **`fatal` 由引擎执行**：后端只**上报**（`device_lost`/`out_of_host_memory` 之类），**不自己 `_Exit`**。今天启动期后端直接 panic 的几处是**待迁移例外**；
- `frame_status`（`proceed/skipped/closed/acquire_failed/begin_recording_failed/end_recording_failed/submit_failed/present_failed`）**就是帧路径的 verdict 类型**，不需要新造。

### 4.5 后端：**按调用点**翻译（三个 helper，不是一个全局函数）

放 `vulkan/core/core.api_core.cpp`（或新 `core.error.cpp`，在**后端**，那里本来就有 `vulkan/vulkan.h`）：

```cpp
error acquire_error(VkResult);   // OUT_OF_DATE→out_of_date；SUBOPTIMAL→ok；TIMEOUT→timeout；
                                 // SURFACE_LOST / NATIVE_WINDOW_IN_USE→surface_lost；DEVICE_LOST→device_lost；
                                 // OUT_OF_DEVICE_MEMORY / OUT_OF_POOL_MEMORY / FRAGMENTED_POOL→out_of_device_memory；
                                 // OUT_OF_HOST_MEMORY / MEMORY_MAP_FAILED→out_of_host_memory；其余→operation_failed
error present_error(VkResult);   // 同上，但 SUBOPTIMAL→out_of_date
error generic_error(VkResult);   // INITIALIZATION_FAILED / INCOMPATIBLE_DRIVER→initialization_failed；其余同上
```

**不裁头文件、不建码表、不生成 `.inc`**：实测该头里 41 个 VkResult（23 个扩展区间），只列"本项目可能收到"的，其余归 `operation_failed` + `native_code`。

### 4.6 批① 的见证

- 新建 **`tests/test_error_mapping.cpp`**（纯 CPU，无 GPU）：表驱动喂每个"可能收到"的 `VkResult` 进三个 helper，断言 unified 值；含 `SUBOPTIMAL` 在两个调用点给出**不同**结果这条；
- `static_assert` 布局断言（4.3）；
- 在 `CMakeLists.txt` 注册该测试（参考既有 `test_*` 的注册方式 + `VR_TEST_TARGETS` 是否包含它）；
- 边界仍 **27**（本批不动符号）、14 场景哈希不变。

### 4.7 顺手修掉两个实测信息丢失点（同批或紧随）

- `core::wait_frame_slot` 现在**丢** `vkWaitSemaphores` 的结果 ⇒ 让它能通过 `error_info` 报出（等批②的面落地后由 `wait_and_acquire()` 承载）；
- `core::frame_begin()` 把 `OUT_OF_DATE` 与其它失败**一起压成零值** `submit_info` ⇒ 批②的 `frame_open_info` 取代它。

---

## 5 批② 帧面（**唯一跳 abi 的批**：12 → 13）

### 5.1 契约（`promise/rhi/rhi.api_core.cppm` + `rhi.contract.cppm`）

```cpp
/// 帧环的游标 —— BORROWED VIEW（像 command_list：**没有 release()**，不得被 object_manager 包装）
struct frame_walker {
    virtual ~frame_walker() noexcept = default;
    [[nodiscard]] virtual std::uint32_t slot_count() const noexcept = 0;              // 环有几格
    [[nodiscard]] virtual std::uint32_t position() const noexcept = 0;                // 本帧所在槽
    [[nodiscard]] virtual frame_open_info wait_and_acquire() = 0;                     // 等 + latch 计时 + 取图
    virtual void walk_to_next() noexcept = 0;                                         // present 之后推进
};

struct frame_open_info {                     // 按值返回 ⇒ **冻结**（加字段＝跳 abi）
    submit_info frame = {};                  // 仅当 result.code == error::ok 时有效
    error_info result = {};                  // 决策 + 诊断
};

struct gpu_profiler {                        // BORROWED VIEW
    virtual ~gpu_profiler() noexcept = default;
    [[nodiscard]] virtual std::uint32_t stage_count() const noexcept = 0;
    [[nodiscard]] virtual error get_stage_info(std::uint32_t index,
                                               std::string_view* name,
                                               std::uint64_t* duration_ns) const noexcept = 0;
};

// api_core 追加（两个虚函数）：
[[nodiscard]] virtual frame_walker* walk_frames() noexcept = 0;
[[nodiscard]] virtual gpu_profiler* profiler() noexcept = 0;
```

`rhi.contract.cppm` 记：**12 → 13：`api_core` 追加 `walk_frames()`/`profiler()`（对已有 tier-1 类型追加虚函数＝形状变化），并新增 `frame_walker`/`gpu_profiler`/`frame_open_info`。**

### 5.2 行为规格（**顺序是承重的**）

`wait_and_acquire()` 内部顺序：

1. 读游标（**不推进**）；
2. **等**该槽的 timeline（value 0 = 从未提交 ⇒ 直接返回，与今天一致）；
3. **latch** 该槽上一帧的 GPU 计时（今天 `collect_gpu_timings(position)` 夹在"等"与"采集"之间——**归后端**，这样被 `OUT_OF_DATE` 跳过的帧**照样被收集**，行为零变化）；
4. **采集**下一张图；
5. 返回 `frame_open_info`：成功 ⇒ `{submit_info{slot, image_index}, error::ok}`；失败 ⇒ `frame` 零值 + `result`（**等或采集任一步的失败都走这里**，`where`/`message` 指出是哪一步）。

`frame_open_info` 的语义：`result.code == ok` 之外 ⇒ **不得使用 `frame`**（保留今天"零值 = 没起帧"的可读性，但现在知道为什么）。

### 5.3 后端（`vulkan/core/core.declarations.cppm` / `core.api_core.cpp` / `core.constructor.cppm`）

- 加两个**嵌套借用视图**（与既有 `frame_image_slot` / `readback_slot_view` / `commands_view` 同形）：

```cpp
struct frame_walker_view final : deren::promise::rhi::frame_walker {
    core* owner = nullptr;
    [[nodiscard]] std::uint32_t slot_count() const noexcept override;
    [[nodiscard]] std::uint32_t position() const noexcept override;
    [[nodiscard]] frame_open_info wait_and_acquire() override;
    void walk_to_next() noexcept override;
};
struct gpu_profiler_view final : deren::promise::rhi::gpu_profiler {
    core* owner = nullptr;
    std::array<char const*, gpu_timing_mark_capacity> stage_names = {};   // 名字**随 mark 进来**
    [[nodiscard]] std::uint32_t stage_count() const noexcept override;
    [[nodiscard]] error get_stage_info(std::uint32_t, std::string_view*, std::uint64_t*) const noexcept override;
};
```

- **构造函数里务必 `owner = this`**（实测教训：`address_view` 忘设 ⇒ `buffer_address()` 全部答 0，尖刺报 2 个 FAIL 才查出）；
- `slot_count()` → `MAX_FRAMES_IN_FLIGHT`；`position()` → `current_frame`；`wait_and_acquire()` → 既有 `wait_frame_slot` + `acquire_next_image` + `read_gpu_timings` 的步序；`walk_to_next()` → `to_next_frame()`；
- `gpu_profiler`：`stage_count()` → 最近一个已完成帧的 `mark_count`（无计时 ⇒ 恒 0）；`get_stage_info` → `index >= stage_count()` ⇒ `invalid_argument`；未完成/无 mark ⇒ `not_ready`；设备或配置无计时 ⇒ `unsupported`；读回失败 ⇒ `device_lost`；时长为**纳秒**（该设备 1 ns/tick、64 valid bits）；
- **阶段名归引擎**：`mark` 入参加 `std::string_view stage_name`（**静态文本**，引擎传字面量；后端只存指针，规则同 `window_title`）。**不要**在契约里定义 `stage_id` 枚举（会把引擎的阶段划分固化进 ABI）。
- `SUBOPTIMAL` 的翻译**按调用点**：acquire → `ok`（照常渲染），present → `out_of_date`（重建）。

### 5.4 probe 与尖刺（编译期强制）

- `tests/probe_backend.cpp`：实现 `walk_frames()` 与 `profiler()`（假数据即可，测试的位遍历与一致性门会走它们）；
- `tests/spike_backend_boundary.cpp` 断言：

```
walk_frames() != nullptr; slot_count() > 0;
position() == frame_begin().frame_index;                    // 权威是后端，交叉核对
profiler() != nullptr; stage_count() <= 16;
get_stage_info(stage_count(), &n, &d) == error::invalid_argument;
有 mark 后 get_stage_info(0, …) 给出非空 name + 正数纳秒;
无计时设备：stage_count()==0 且 get_stage_info(0, …) == error::unsupported
```

### 5.5 引擎切换（机械替换，一次一个文件，逐段过门）

| 旧 | 新 |
|---|---|
| `static_cast<uint32_t>(vk.current_frame)` **29 处**（runtime.frames 19 / runtime.cpp 7 / filters.cpp 2 / filters.cppm 1） | `frames.position()` |
| `deren::vulkan::core::MAX_FRAMES_IN_FLIGHT` **12+ 处**（runtime.constructor） | `frames.slot_count()` |
| `vk.wait_frame_slot(slot)` + `vk.acquire_next_image(idx)` | `frames.wait_and_acquire()` |
| `vk.to_next_frame()` | `frames.walk_to_next()` |
| 帧路径的两个 if 阶梯（acquire/present） | `enforce(open, classify_acquire)` / `classify_present`（`frame_status` 作 verdict） |
| `vk.mark_gpu_timing` / `vk.read_gpu_timings` 调用点 | `profiler()` 面 |

取面方式：`rhi::frame_walker& frames = *this->rhi_face().walk_frames();`（`runtime::rhi_face()` 已存在）。

**见证**：边界 **27 → 24**（`to_next_frame`/`acquire_next_image`/`wait_frame_slot` 三个消失）→ 随引擎替换继续下降；29+12 处不再命名后端类型；14 场景哈希不变。

---

## 6 批③ 27 个符号（三条件路 + 最后一步）

现状 27 个，按**处置**分五类（详见目标文档 §解决方法总览）：

| 处置 | 数量 | 做法 |
|---|---|---|
| ① 契约已有面，只改调用点 | **5** | `acquire_next_image`→`frame_begin()`（批②后并入 `wait_and_acquire`）、`present(uint)`→无参 `present()`、`make_sampler`→`create_sampler`、`make_image_view`→`image::make_view`、`make_shader_module`→`create_shader` |
| ② 需新增契约面 | **9** | `submit(command_list&)`、`wait_frame`（批②已并入）、`swapchain::recreate()`、`swapchain::extent()`、`render_extent`、timing 4 个（**已由 `gpu_profiler` 覆盖**） |
| ③ 直接删除 | **3** | `core::set_window_title`（连 `user_filter::set_window_title` 转发一起删，窗口归应用）、`vma_allocator::log_statistics()` 的调用、`init_utils::default_task_pool_threads()` 的调用（线程数归宿主/应用） |
| ④ **白名单**（有意例外，不删） | **7+2** | 4 个 `vk_command_buffer`/`vk_sampler` 句柄 + `make_command_buffer`/`make_secondary_command_buffer`/`create_recording_pool` + 2 条 module initializer |
| ⑤ 最后一步 | **1** | `core::core(create_info const&)`：改经 `deren_make_api_core()` + `shared_ptr<rhi::api_core>`（deleter = `deren_destroy_api_core`）。**前提是其余 24 个已清**——只要引擎还命名具体 `core`，换构造也拿不到解耦 |

**顺序**：①（A 6 + B 4 + 两处删除）→ 边界 27→15；②（C 4 + 删 `set_window_title`）→ 15→10；③ 构造（随批②的 abi 批）；④ 门重定义。

**门必须改（这是白名单裁决的直接工作项）**：`--require-zero` → **"白名单之外为零"**；白名单进门的**配置**、**只减不增**（与棘轮同纪律）、每次报告**逐条打印命中项**。否则门永远红，或有人为了让门变绿去放宽它。

### 6.1 两件必须实测、不许推断的

1. **`initializer for module deren.vulkan.core` 是否被 `import deren.vulkan.core:handles` 强制**：实测法——把 runtime 对 `:handles` 的 import 收窄到分区写法，看该符号是否仍在 `--list` 里；
2. **白名单的精确条数**：以 `--list` 为准，不要在文档里凑数。

---

## 7 批④ utility 拆分（**不跳 abi**，步 ④ 前置）

**动机（实测）**：`utility` 现为 STATIC 且两半都链；静态配置下链接器只留一份，所以日志看起来是单实例。**翻转那天** `deren_vulkan` 变 SHARED ⇒ DLL 自带一份 ⇒ **两份日志器、两个 `ofstream` 指向同一 `debug.log`、两次启动轮转**（`utility.cpp:155/179` 的 `rotate_previous_log()` 会**截断**另一份正在写的文件）。边界门对此**完全盲**（只量 `deren_vulkan ∩ vulkancorekit`，utility 是第三个库）。

| | `shared_utility`（进程一份；翻转时 SHARED） | `static_utility`（每半一份；恒 STATIC） |
|---|---|---|
| 内容 | 日志 **sink + 轮转 + 文件句柄**、**panic 汇聚**、分配器钩子（`better_pmr` 进程级部分）、线程数策略（若两半需一致） | BVH、data_block、frame_clock、frame_stats、thread_pool、platform_*、**`dynamic_link`**、**所有模板** |
| 接口 | **C 形状/POD 为主**：`void deren_log_text(std::string_view)`、`[[noreturn]] void deren_panic(...)`；**格式化留在调用方**（`log(fmt, args…)` 仍是 inline 模板 → 转发到导出的非模板核心）⇒ sink 单实例、模板不必导出 | 同二进制，随便 |
| 依赖 | **不依赖** `static_utility` | **可**依赖 `shared_utility`；反向禁止；**契约 `rhi` 不依赖两者** |

- **`dynamic_link` 必须留 static 侧**：它是加载后端的钥匙，放进 shared 会形成"exe 先加载 shared_utility 才能加载后端"的引导链；
- **增量路径**：**先只拆模块/目标，两者仍 STATIC**（行为零变化，可独立验证）→ 翻转时 shared 转 SHARED，与 `deren_vulkan` 同批（exe 与后端 DLL **都按名字静态导入** ⇒ Windows 只加载一次 ⇒ 单实例是机制不是约定）；
- **门**：补一条**可检查**的——**后端 DLL 的导入表里出现 `shared_utility.dll`**（而非自带 sink）。尖刺那套导入表检查（窄导出 / Q6）已验证过；
- **零成本前置修复**（拆分落地前就该做）：**轮转只由一方做一次**、后端日志走明确路由——两次轮转一旦翻转就是**必现**的日志损坏。

---

## 8 建议的提交切分与顺序

1. `rhi: the error set grows six general graphics values, and the zone is derived`（批①契约侧）
2. `utility: ensure, verdict, enforce and propagate - and the lying classifier panics`（批①utility）
3. `vulkan: the backend translates VkResult per call site` + `tests: the error mapping table`（批①后端+测试）
4. `promise: the frame face lands - frame_walker and gpu_profiler (ABI 13)`（批②契约）
5. `vulkan: the frame loop's face is real; the runtime stops counting frames itself`（批②后端+probe+尖刺）
6. `vulkan: the engine walks frames through the contract`（批②引擎 29+12 处）
7. 批③ 按三条件路逐笔（每笔：`boundary` 达标 + 14 哈希不变）
8. 批④ 拆分为独立两笔（先全 STATIC）

每笔提交前：构建 + `ctest` + 格式 + 边界 + 尖刺；**只有批②及其后续涉及渲染路径时才必须跑 14 场景**（但建议每批都跑）。

---

## 9 未验与风险（照实写，别粉饰）

- **F1/F2/F3（设备能力/属性链/选卡）在本机无法行为验证**：本机设备该有的都有，缺失路径不发生；证据止于 CPU 回归（37/37，见 `TECHNICAL_BUGFIXES` 相关提交）；
- **F4（压缩纹理上传布局）** 同样是 CPU 回归（78/78）+ 算例锚点；**无证据表明当前材质使用压缩格式**，不得宣称"修好了渲染"；
- `laevatain_no_sidecar` 的输入资产曾缺失（现已补上，`E9A2983BEB57D5C5`），若再缺即为覆盖缺口而非回归；
- **95 MB 截图仍在 git 历史里**（`c2c7577` 引入）：要真正瘦身需 `filter-repo` + `--force-with-lease`，属独立决定；
- 本机参考集是旧集（2026-10-01）：**门的 exit code 不是判据**，只有打印的实际哈希是。
