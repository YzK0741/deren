# progress.md — 原生句柄退出（图形 API 走 RHI）进展

> 记录时间：本轮会话结束时。
> **一句话现状：引擎半边已经**一个 Vulkan 符号都不引用**（P-Nm = 0），只剩两件事——① `deren.exe` 仍导入 `vulkan-1.dll`（GUI 插件化做到一半，**当前树不构建**）；② 引擎源码仍有 34 个文件命名 Vulkan 词汇。**
> 计划与验收定义在 [`docs/rhi/NATIVE_HANDLE_EXIT_PLAN.md`](docs/rhi/NATIVE_HANDLE_EXIT_PLAN.md)，逐批的详细记录在 [`docs/rhi/RECORDING_FACE_REFACTOR_STATUS.md`](docs/rhi/RECORDING_FACE_REFACTOR_STATUS.md)。

---

## 0. 目标与判据（为什么做这件事）

目标**不是**"不链接 Vulkan"，而是**后端可替换**：主程序（`deren.exe` + `vulkancorekit`）不得接触任何图形 API 的**类型、入口点与库**，这样第二个后端（D3D12/nul）可以插进来。三条判据按强度排列：

| 判据 | 检查 | 现状 |
|---|---|---|
| **P-Import** | `objdump -p deren.exe` 不出现 `vulkan-1.dll` | ❌ 仍有（X2 差最后一步） |
| **P-Nm** | `llvm-nm` 扫 `vulkancorekit` 的对象无未解析 `vk*` | ✅ **0** |
| **P-Census** | 引擎源码不出现 `Vk*`/`VK_*`/`vk*`/`escape()->native_*`/`#include <vulkan/` | ⚠️ 34 个文件（X5） |

门禁脚本：`python scripts/check_native_boundary.py`（`--require-zero` 变失败门禁）。

---

## 1. 本轮落地的批次（自 `90f0169` 起）

| 提交 | 主题 | 关键效果 |
|---|---|---|
| `90f0169` | 读回变成 **CONTENT**（host image copy）+ launch 移到录制面 | `image::get_content()`（abi 25）；`vulkan/readback/**` 死模块删除；`vkCmd*` 5→3 |
| `a46c961` | **原生边界门禁** + 帧的录制生命周期走契约 | `scripts/check_native_boundary.py`（P-Import/P-Nm/P-Census）；引擎对象 6→5 |
| `f32bc40` | 设备等待与设备地址 | `vkDeviceWaitIdle`→`wait_idle()`；`VkDeviceAddress`→`uint64_t` |
| `479dc0a` | 设备事实成为 **tier-2 ability** `device_capabilities` | 一次清掉整族 `vkGetPhysicalDevice*`/`vkGetDeviceQueue`；引擎引用 13→7；**不 bump abi** |
| `77abbdb` | mesh 派发不再自己解析入口点 | 删两个 `PFN_`、`mesh_indirect_table`、三处 `sizeof(VkDrawMeshTasks*)`；引用 7→5 |
| `41d6b71` | **S1 的 P0 仪器**：加速结构真设备探针 | `tests/test_acceleration_structures.cpp`（隐藏 64×64、验证层开、小场景） |
| `1f73b89` | 加速结构升为 **tier-1 家具**，`ray_tracing` ability 退役（abi 26） | 契约新增对象 + `create/build/refit` 槽位；旧 ability 的三个动词删除 |
| `b6eb1ff` | 后端**真的服务** tier-1 AS 接口（P1b-1） | storage/scratch/尺寸查询/实例缓冲全归后端；探针切到接口上 |
| `e019076` | 引擎 AS 模块搬到接口上（P1b-2） | `acceleration_structure.cpp` 与 `runtime.cpp` 引用**清零**；引用 4→2 |
| `546192c` | micromap 也 tier-1（abi 27） | `VkMicromapEXT` 全族退出引擎；`ray_tracing.cpp` 词汇 33→9；引用 2→1 |
| `d91bd50` | **X4**：`image_use::host_read` + 调用方自有缓冲的 `submit()` | 引擎引用 1→**0**；census 配对数 21→22（census 是数据表，四数联动） |
| `a5e8287` | X2 设计改版：**GUI 独立 DLL（每 API 一个）+ 像后端一样手动导入** | 文档 §4.1/§4.2 |
| `8808b69` | **X2a+X2b**：GUI 插件成形 | `deren_gui_vulkan.dll` **导出** `deren_make_gui`、**导入** `vulkan-1.dll` |
| `a1bc19e` | **WIP 检查点：当前树不构建** | X2c 引擎侧全部编译通过；只剩 `chores.cpp` |

**里程碑**：`d91bd50` 之后 **`engine objects referencing a Vulkan symbol: 0`**。

---

## 2. 当前状态（重要：树不构建）

**唯一失败**：`chores.cpp` 的 **41 处** `deren::vulkan::gui::*`（旧模块的 `debug_panel`/`label_widget`/`checkbox_widget`/`slider_widget`/`combo_widget` 与 `->visible_when`）。GUI 模块已经搬进 `deren_gui_vulkan.dll`，运行时不再 `import` 它，所以 app 侧必须改走**边界接口**。

**接口已经就位**（`promise/gui/gui_entry.hpp`）：

```cpp
deren::gui::panel& panel = runtime.debug_gui().add_panel("deren debug");     // 旧: deren::vulkan::gui::debug_panel&
panel.add_label([]{ return ...; });                                          // 旧: push_back(make_unique<label_widget>)
panel.add_checkbox(L, V).set_visible_when(P);                                // 旧: 建对象 + ->visible_when + push_back
panel.add_slider(L, V, lo, hi).set_visible_when(P);
panel.add_vec3(S, V, speed);  panel.add_combo(S, items, &index, on_change);
```

另需在 `chores.cpp` 加 `#include "../promise/gui/gui_entry.hpp"`（它是普通 TU）。

**这一步试过两次脚本、都失败并回滚**：
- ❌ 第一版按**行尾**判断调用在哪结束 —— 错。lambda 体内一条语句同样以 `);` 结尾，参数被截断、括号失衡（`chores.cpp` 用 `git checkout --` 复原过两次）。
- ⏳ 第二版改成**数括号**（正确算法），但脚本文件没写成功，**尚未运行**。
- ✅ 结论：**一次"数定界符"的脚本转换 + 编译器收尾**，并保留回滚；不要再用行尾启发式。

**X2d（未做）**：做完上面一步后验证 —— `objdump -p deren.exe` **无 `vulkan-1.dll`**、且**不导入** `deren_gui_vulkan.dll`（手动导入），跑 render 14/14 + 一次 `enable_debug_gui()` 会话。

---

## 3. 剩余工作

| 项 | 内容 | 判据 |
|---|---|---|
| **X2d** | 完成 `chores.cpp` → 断掉 `imgui`（已在 CMake 里断掉，等链接成功） | P-Import 归零；render 14/14 |
| **X5** | 源码词汇清扫：34 个文件里的 `VkExtent2D`/`VkFormat`/`VkSampler`/`resolved_binding` raw 车道/`VkBindHeapInfoEXT`/`escape()->native_*` | P-Census 下降 |
| **门禁 scope 补充** | `deren_gui_vulkan` 的源（GUI 模块 + `gui_dll.cpp`）现在**被 P-Census 当成"引擎文件"**计入了（它们不在 `deren_vulkan` 的源列表里）——gate 需要把插件目标的源一并排除 | 数字口径正确 |
| **rt_shadows 冒烟** | 唯一端到端 RT 仪器；本机内存紧张时（~3.5 GB 空闲）**连 HEAD 都崩**，已用基线对照证明与代码无关 | 内存充裕时复跑确认 0 VUID |

---

## 4. 仪器：怎么验证

```powershell
# 构建（-j 3：本机内存紧张时更稳）
cmake --build build-release-dyn-clang64 -j 3

# 边界门禁（三条判据）
python scripts/check_native_boundary.py            # 报告
python scripts/check_native_boundary.py --require-zero   # 失败门禁
objdump -p build-release-dyn-clang64/deren.exe | Select-String "DLL Name"   # P-Import

# 既有 §6 电池
ctest --test-dir build-release-dyn-clang64                       # 19/19
cmake --build build-release-dyn-clang64 --target clang-format-check
python scripts/check_backend_boundary.py --config dynamic --require-zero
& build-spike-clang64\test_backend_boundary_spike.exe --with-device        # 95/0
& build-release-dyn-clang64\test_runtime_dyn.exe --with-device             # 10/0
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare frozen  # 14/14 像素一致

# S1 加速结构探针（真设备、验证层开、小场景；42 checks）
build-release-dyn-clang64\test_acceleration_structures.exe --with-device

# census（配对数是"数据"，改 constant_init 的配方要一起更新它 + 后端两处期望）
python scripts/recording_face_census.py
```

**仪器选择原则**（本轮反复用到）：改动落在哪条路径，就用**覆盖那条路径**的仪器 —— 探针的读回行、mesh 路由日志（`40 dispatches ... 0 through the direct call`）、render 的 14/14 像素哈希。**不要**用"没崩"当证据；**纯移植**（类型/搬运）只跑构建 + 该路径的仪器，不跑全量电池。

---

## 5. 规则与坑（下一轮别重踩）

**架构规则**
1. 图形 API 概念出现在引擎里 = **契约缺口**；用 ability 或 tier-1 家具补，不要 `escape()`。
2. **新增 ability**（新位 + 新接口）**不 bump abi**；**往已有接口追加槽位**才 bump（本 session：25→26→27）。
3. 引擎要写进缓冲的记录（`mesh_task_command`、`acceleration_structure_instance`）→ 布局进契约 + **后端 `static_assert`** 与驱动结构一致。
4. 引擎"能写、后端能读"的资源（AS storage/scratch、micromap 的 256B 地址对齐）→ **归后端**，调用方不该看见。
5. 已知有意例外：`glfw`/`GLFWwindow*`（平台而非图形 API）、`vulkan_constant_init` 的 constexpr 构建器（无调用）、后端与 GUI 插件自身。

**C++ 模块坑（本 session 付费过）**
6. **边界头必须进全局模块片段**：它自声明 `GLFWwindow` 并拉标准库；放进模块内会让模块重声明全局模块已有的名字（clang: `declaration ... follows declaration in the global module`）。
7. **`export module` 不自动导出成员**：`load_gui` 漏 `export` → `declaration of 'load_gui' must be imported from module ... before it is required`。
8. 新增 SHARED 目标里的模块用 `target_sources(... FILE_SET CXX_MODULES FILES ...)`；普通 TU 用 `PRIVATE`。
9. `-Werror` 全家桶：未使用的 helper/变量（`-Wunused-function` 等）会在删掉调用者后立刻变成错误 —— **删调用点时要顺手删 helper**。

**流程坑**
10. `PowerShell` 的这里字符串 + C++ 注释里的反引号会互相打架（`` `ok` `` 被当转义序列）；跨行替换优先用 `edit` 工具或"数定界符"的脚本。
11. 多行替换失败往往不是内容错而是**缩进**或 **CRLF**：先 `Select-String -Context` 看清真实缩进再改。
12. 环境的坑要先归因：本机 15.2 GB 内存、空闲 <4 GB 时最重的配置会环境性失败（`Failed to create image: -2`、`LLVM ERROR: out of memory`、rt 冒烟的 device lost）；**用 HEAD 基线对照**再判断是不是代码问题。

---

## 6. 关键数字一览

| 指标 | 本轮起点 | 现在 |
|---|---|---|
| 引擎对象里的 `vk*` 符号 | 7 个对象 / 23 引用 | **0** |
| `vkCmd*` 调用点 | 5 | 3（都在已文档化的 escape 桶） |
| 含 Vulkan 头的引擎文件 | 37 | 34（含 gate 口径问题，见 §3） |
| `rhi::abi_version` | 24 | **27** |
| GUI 边界版本 | — | `gui_abi_version = 1` |
| `deren_gui_vulkan.dll` | — | 导出 `deren_make_gui`、导入 `vulkan-1.dll` |
| ctest / render / spike / runtime_dyn | 19/19、14/14、95/0、10/0 | 同左（WIP 前一次全绿） |
