# progress.md — 原生句柄退出（图形 API 走 RHI）进展

> 更新日期：2026-10-08（接手后）。
> **最新核验：GUI 插件化 X2 完成；P-Import = 0、P-Nm = 0，P-Census 剩 25 个引擎文件。当前工作区的 TLAS 描述符映射已通过严格 RT 验收：GPU 命中/未命中/实例掩码及两帧槽正确，Sponza 40 帧、验证层干净并生成截图。下文旧的 RT 失败记录属于映射修正前。**
> 计划与验收定义在 [`docs/rhi/NATIVE_HANDLE_EXIT_PLAN.md`](docs/rhi/NATIVE_HANDLE_EXIT_PLAN.md)，逐批的详细记录在 [`docs/rhi/RECORDING_FACE_REFACTOR_STATUS.md`](docs/rhi/RECORDING_FACE_REFACTOR_STATUS.md)。

---

## 0.0 独立复核（Lead，接手方收尾之后）

复核者**没有**参与上述批次，直接在工作树上重跑；全部与接手方的声明一致：

| 仪器 | 结果 |
|---|---|
| 全量构建 | ✅ `BUILD_OK` |
| `ctest` | ✅ **19/19** |
| `clang-format-check` | ✅ 0 |
| 后端 spike（真设备） | ✅ **95/0** |
| `test_runtime_dyn` | ✅ **10/0** |
| S1 加速结构探针 | ✅ **75/0**（接手方从 42 扩到 75） |
| 冻结渲染（像素哈希） | ✅ **14/14** |
| `test_gui_plugin` | ✅ **6/0** |
| `scripts/windows/check_gui.ps1` | ✅ **PASS**（40 帧 on/off、验证层干净、on/off 图像必须不同，并产出截图） |
| `scripts/windows/check_rt.ps1`（严格 RT 验收） | ✅ **PASS** —— 遍历探针验证 GPU 命中/未命中/实例掩码 + 两个 TLAS 槽；Sponza 1080×960、40 帧、验证层干净、产出截图 |
| P-Import / P-Nm / P-Census | ✅ 0 / ✅ 0 / ⚠️ 25 个引擎文件 |

**关于 RT 归因的一点更正（复核时注意到的）**：这次 `check_rt.ps1` 通过时主机**仅 3.3 GB 空闲内存**——比先前崩溃时更低。所以"内存紧张导致 device lost"的旧归因**不能成立**，真正的修正是接手方那批 TLAS 堆描述符映射（以及 `VK_INDEX_TYPE_NONE_KHR`/透明性/FAST_BUILD/scratch 对齐四处遗漏）。接手方的文档已经这样写了，此条只是独立确认。

**结论**：X2（GUI 独立插件 + 手动导入）与"引擎无 Vulkan 符号/主程序无 Vulkan 导入"两条判据**已达成并经独立复核**；RT 验收**已通过**；剩下的是 X5 的源码词汇（25 个引擎文件）与 X5 计划里的第 2–5 步。

---

## 0.1 `deren.exe` 里还有没有 Vulkan？——四层实测（2026-10-08）

| 层 | 问题 | 实测 | 判定 |
|---|---|---|---|
| ① **导入表** | 有没有链接图形库 | 17 个导入，**没有任何** `vulkan-1`/`glfw`/`d3d`/`dxgi`（`shared_utility` + CRT + USER32/SHELL32/GDI32/comdlg32） | ✅ 无 |
| ② **符号** | 有没有 `vk*`/`Vk*` 符号 | `llvm-nm`：**0**（未解析与已定义**都**是 0） | ✅ 无 |
| ③ **二进制字符串** | 镜像里还有没有 Vulkan 名字 | 有，但**来源是 GLFW**：`vkCreateWin32SurfaceKHR`、`vkCreateHeadlessSurfaceEXT`、`vkGetPhysicalDeviceWin32PresentationSupportKHR`、`vkEnumerateInstanceExtensionProperties`、`vkGetInstanceProcAddr`、`VkSurfaceKHR`、`VK_KHR_`/`VK_EXT_`/`VK_MVK_` —— 全部能在静态链接的 `C:\msys64\clang64\lib\libglfw3.a` 里找到，**仓库源码里一处都没有**（命中的是 `third_party/vma` 与 `imgui_impl_vulkan`，它们分别在**后端 DLL** 与 **GUI 插件 DLL** 里）。GLFW 把这些入口点**名字当字符串**交给应用提供的 loader 回调，它自己不 import `vulkan-1` —— 与①一致。引擎自身只留日志文本（`"vulkan runtime initialized"`、`"rhi: vulkan_escape ..."`）与 `.spv` 着色器文件名 | ⚠️ 剩 GLFW 平台层 + 文本 |
| ④ **源码词汇** | 引擎源码还命名什么 | **25 个文件**，全部仍 `#include <vulkan/`；其中 **12 个**还经 `escape()->native_*` 借用原生句柄。最重四个都在 runtime：`runtime.constructor.cppm`(45 tok，16 类型/21 宏)、`runtime.frames.cppm`(34)、`runtime.declarations.cppm`(33)、`runtime.cpp`(15) | ⚠️ X5 清单 |

**工具**：`python scripts/binary_vulkan_scan.py build-release-dyn-clang64/deren.exe`（第③层；`vulkan`/入口点名/`Vk*`/`VK_*`/`.spv` 计数与去重列表）。

**回答"还有没有 Vulkan"**：**依赖与符号层面一点都没有了**（① ② 均 0）；**二进制里还有 Vulkan 相关字符串，但几乎全是 GLFW 平台层的**——按本项目的既定例外（`glfw`/`GLFWwindow*` 是平台而非图形 API）这是允许的；**引擎源码词汇还没清**，即 ④ 的 25 个文件（X5 第 2–5 步）。

---

## 0. 目标与判据（为什么做这件事）

目标**不是**"不链接 Vulkan"，而是**后端可替换**：主程序（`deren.exe` + `vulkancorekit`）不得接触任何图形 API 的**类型、入口点与库**，这样第二个后端（D3D12/nul）可以插进来。三条判据按强度排列：

| 判据 | 检查 | 现状 |
|---|---|---|
| **P-Import** | `objdump -p deren.exe` 不出现 `vulkan-1.dll` | ✅ **0**，也不静态导入 GUI 插件 |
| **P-Nm** | `llvm-nm` 扫 `vulkancorekit` 的对象无未解析 `vk*` | ✅ **0** |
| **P-Census** | 引擎源码不出现 `Vk*`/`VK_*`/`vk*`/`escape()->native_*`/`#include <vulkan/` | ⚠️ **25** 个文件（X5；插件已排除） |

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

## 2. 上轮交接检查点（已解除；最新状态见 §2.1）

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

## 2.1 接手后已完成（2026-10-08，工作区改动尚未提交）

- `chores.cpp` 的 **52 个控件构造点**全部迁到 `panel.add_*` / `set_visible_when`，边界头位于全局模块片段，路径是 `promise/gui/gui_entry.hpp`。转换按配对括号扫描，并在原 `push_back` 位置生成调用，保持控件顺序、回调和显隐条件。
- 修复 DLL 入口遗漏的 `overlay->init(*info)`。新增 `test_gui_plugin`：错误 ABI、空描述、错误 API、缺少窗口；修复前 6 checks / 2 failed，修复后 **6/0**。
- 修复插件私有 GLFW 副本未初始化：插件管理自己的 `glfwInit` / `glfwTerminate`，清理幂等。仅入口初始化成功仍会出现“零缩放、无面板”；现在缩放 **1.50**，已目视确认面板。
- 新增 `scripts/windows/check_gui.ps1`：GUI 开关各 40 帧、验证层干净、图像必须不同，避免“日志成功但没有实际绘制”的假绿。已通过。
- 门禁从 CMake 的 `deren_gui_vulkan` 源列表排除插件源；新增真实临时源码树回归测试，仍检出引擎中的 `VkDevice`。该测试随既有 CTest 边界组一起运行。
- 更新边界探针的 X4 陈旧断言：自有 primary 完成录制后可以独立提交，并等待设备空闲再释放资源；不再用未录制的命令缓冲测试旧的拒绝行为。接口注释同步描述该约定。
- **RT 额外发现与修复**：MASK 烘焙输出的 `index_address == 0` 表示无索引，后端此前仍使用索引格式。现在转换成 `VK_INDEX_TYPE_NONE_KHR`，`VUID-vkCmdBuildAccelerationStructuresKHR-pInfos-03806` 消失。
- **RT 仍未验收**：Sponza / 1080×960 / 40 帧请求在提交时返回 `device_lost`（error 11），无截图。修复前归档日志也有该失败；关闭 MASK/蒙皮烘焙的同场景对照仍失败。此轮测得约 5 GB 空闲内存，不能仅凭上轮的低内存结论归因。新增 `scripts/windows/check_rt.ps1` 严格拒绝提交失败、验证错误和无截图。

验证：完整构建通过；ctest **19/19**（边界组包含 41 个 Python 用例）；冻结 render **14/14**；GUI **6/0 + 开关截图验证通过**；backend spike **95/0**（含自有 primary 提交）；runtime_dyn **10/0**；AS **42/0**；文档 **224/0**；格式检查与动态后端边界门禁通过。RT **未通过**。ABI 保持 RHI 27 / GUI 1。

## 2.2 X5 首批：共享采样器与死依赖（2026-10-08）

分批方案见 [`docs/rhi/X5_NATIVE_VOCABULARY_PLAN.md`](docs/rhi/X5_NATIVE_VOCABULARY_PLAN.md)。按“共享采样器 → 资源发布 → 语义值 → escape 入口 → 无 Vulkan 编译/运行证明”的顺序推进，每批保持资源所有权与画面。

- 共享采样器表的六个字段和 `of()` 改为借用 `rhi::sampler*`。运行时直接发布已有契约对象，删除该路径的 `vulkan_escape::native_sampler()`。资源仍由原来的 `object_manager` 持有。
- 删除没有生产调用者的 `deren.vulkan.bindings` 枚举映射模块、构建条目和 runtime 导入；移除仅验证旧 Vulkan 映射的测试。
- `compute_skin.cppm`、`mask_bake.cppm`、`mask_bake.cpp`、`ray_traced_shadow.cpp` 去掉残留 Vulkan 头。
- 新测试用真实的 fake RHI sampler 对象验证身份、空 hint、未发布 hint、未知 hint，并静态检查返回类型。旧实现编译时因 `VkSampler*` 与 RHI 指针不符而失败；迁移后 `test_pass` **169/0**。
- 完整构建、CTest **19/19**、冻结渲染 **14/14 逐像素一致**、格式检查和动态边界门禁通过；P-Census **31 → 25**、P-Nm / P-Import **0**。独立只读审查未发现首批范围内的缺陷。RHI ABI 保持 **27**。

RT 的 device_lost 仍未解决。本批不代表 X5 整体完成。

### 2.3 RT：与迁移前加速结构对照

历史对照结论已纠正：无 RHI 的 `c245102`（`26a3190^`）、早期静态 RHI 的 `f188b2c`、`7af1018` 和 `9e7eb3b` 虽完成 40 帧，但图像描述符在堆创建前写入而被跳过，raygen 在 `GetDimensions` 后提前返回，不能证明执行了 GPU 射线遍历。给隔离的 `c245102` 仅补齐已有 RT visibility、G-buffer depth/normal 图像的描述符发布后，原生 `vkQueueSubmit` 返回 **VK_ERROR_DEVICE_LOST (-4)**。因此撤回此前的 RHI/DLL/资源迁移回归区间；`874675d` 是实际遍历暴露点，不能视为故障引入点。历史源码与当前的 RT 源码、四个 SPIR-V 一致。

对照发现并修正无索引格式、OPAQUE 标志、TLAS FAST_BUILD 策略、scratch 地址对齐四处行为差异，并让可更新结构同时覆盖 build/update scratch 大小。

同时修正 AS 探针的顶点字节数和 GPU 对象寿命、补上无索引几何检查，真设备 **45/0**、无验证错误。完整 RT 仍失败；临时绕过追踪、屏蔽实例等诊断修改均已撤回。详细证据见 [`RT_AS_COMPARISON.md`](docs/rhi/RT_AS_COMPARISON.md)。

此前完整构建、CTest **19/19**、冻结渲染 **14/14**、格式检查和动态边界门禁通过。当前完整 RT 再验仍为提交返回 11。下一步验证同一描述符堆槽通过普通 AS shader binding 的后端映射访问是否能恢复遍历，并加强验收，防止 GPU 提前返回造成假通过。用户最新要求额度剩 **25%** 时停止并做阶段总结；不自动消耗重置额度。

### 2.4 项目现状核验（2026-10-08）

- 当前分支 `wip/recording-face`，HEAD `808ae15`；33 个改动路径尚未提交（含未跟踪文件）。本轮仅检查并更新进度，未修改实现。
- 当前实现新增 RHI `acceleration_structure_heap_binding` 描述，由 Vulkan 后端映射原有 TLAS heap 槽，RT shader 经普通 AS binding 访问；GPU 探针覆盖真实遍历。
- 本轮复跑 `check_rt.ps1` 成功：两帧 TLAS 槽的 GPU 结果均为 `1,0,0`，40 帧、1080×960、14 个 MASK caster 烘焙、验证层干净、截图生成。该结果取代本文件映射修正前的“完整 RT 未通过”结论；不据此断言驱动故障的普遍根因。
- 本轮复跑原生边界：P-Census 25、P-Nm 0、P-Import 0；动态后端边界门禁通过。
- 本轮 CTest 为 **18/19**：边界测试组中两个 `os.link` 用例因当前执行环境 `PermissionError / WinError 5` 失败，其他 39 个 Python 用例通过，其余 18 个 CTest 组通过。此前 19/19 为历史记录。本轮未重建、未复跑冻结渲染，不能将旧 14/14 当成本轮验证。
- 下一步：同步 RT 专项及重构状态文档，完成当前映射改动的构建/格式/冻结渲染验证，并在允许硬链接的环境复核完整 CTest；之后继续 X5 资源发布和源码词汇迁移。
- 用户要求：**剩余额度达到 25% 时停止并写阶段进度，不自动消耗重置额度**。本次查询短期剩余 33%、周额度剩余 63%；以后持续工作以最新查询为准。

### 2.5 分批提交与 25% 停止报告（2026-10-08）

已按用户要求完成三个本地代码提交：
- `cd697ae9`：GUI 插件激活、私有 GLFW 生命周期、应用控件迁移、GUI 验收和边界测试；包含 X4 自有命令提交断言/注释同步。
- `7d053bb`：X5 共享采样器改用借用 RHI 对象，删除旧 bindings 模块及残留依赖。
- `925e666f`：TLAS heap binding 映射、AS 构建行为修正、探针资源寿命与字节数修正，以及真实 GPU 遍历验收。

本轮最新验证：完整单线程构建通过，格式检查通过，允许创建硬链接的环境中 CTest **19/19**（边界 Python **41/41**），GUI 插件 **6/0** 与 GUI 开关各 40 帧验收通过，严格 RT 验收通过（GPU 探针 **75/0**、两帧槽命中/未命中/掩码结果正确、Sponza 40 帧、干净验证层、截图）。冻结画面哈希独立比较 **14/14** 一致。渲染包装脚本因沙箱重定向的本地参考目录未播种而返回 1；其生成的全部实际哈希已由 `compare_render_hashes.py --frozen --expect 14` 验证通过，未更新基线。

短期额度已用 **75%**、剩余 **25%**；周额度剩余 **62%**。已停止开发和额外验证，未使用重置额度。本报告及专项文档作为第四批收尾提交；只做本地提交，未推送或合并。

下一轮优先推进 X5 的资源发布：将 `resource_handles` / `resolved_binding` 的原生字段及消费者一起迁到 RHI 对象，保持每图像/帧槽索引和生命周期；之后迁移语义值和 escape 入口，补无 Vulkan 构建及非 Vulkan 后端运行证明。当前 P-Census 25、P-Nm/P-Import 0；X5 整体尚未完成。此前 RT 失败记录是映射修正前的历史，最新验收已通过。

## 3. 剩余工作

| 项 | 内容 | 判据 |
|---|---|---|
| **X5** | 源码词汇清扫：25 个文件里的 `VkExtent2D`/`VkFormat`/`resolved_binding` raw 车道/`VkBindHeapInfoEXT`/`escape()->native_*`；GUI 仍借原生句柄传入插件，需要后续契约化 | P-Census 归零 |
| **P-Gate / P-Run** | 按主计划补齐无 Vulkan 包含环境的引擎构建与非 Vulkan 后端运行证明 | 真正的后端可移植性 |
| **RT 映射修正收尾** | 严格 RT 冒烟已通过；同步专项文档并补齐当前改动的完整验证 | GPU 遍历证据 + 40 帧截图已通过；构建/格式/冻结渲染待本轮复核 |

---

## 4. 仪器：怎么验证

```powershell
# 构建（本轮 -j 3 遇到模块映射文件占用，-j 1 通过）
cmake --build build-release-dyn-clang64 -j 1

# 边界门禁（三条判据）
python scripts/check_native_boundary.py            # 报告
python scripts/check_native_boundary.py --require-zero   # 失败门禁
objdump -p build-release-dyn-clang64/deren.exe | Select-String "DLL Name"   # P-Import

# 既有 §6 电池
ctest --test-dir build-release-dyn-clang64                       # 19/19
cmake --build build-release-dyn-clang64 --target clang-format-check
python scripts/check_backend_boundary.py --config dynamic --require-zero
& build-release-dyn-clang64\test_backend_boundary_spike.exe --with-device # 95 checks，含自有 primary 提交
& build-release-dyn-clang64\test_runtime_dyn.exe --with-device             # 10/0
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare frozen  # 14/14 像素一致

# S1 加速结构探针（真设备、验证层开、小场景；45 checks）
build-release-dyn-clang64\test_acceleration_structures.exe --with-device

# GUI 插件的拒绝输入 + 实际绘制（新仪器）
build-release-dyn-clang64\test_gui_plugin.exe
pwsh -NoProfile -File scripts/windows/check_gui.ps1 -BuildDir build-release-dyn-clang64

# RT 全链路严格验收（目前失败：提交返回 device_lost）
pwsh -NoProfile -File scripts/windows/check_rt.ps1 -BuildDir build-release-dyn-clang64

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
| `vkCmd*` 调用点（recording_face_census） | 5 | **0** |
| P-Census 含图形 API 词汇的引擎文件 | 原交接 34 | **25**（修正插件口径后 31，X5 首批再清 6） |
| `rhi::abi_version` | 24 | **27** |
| GUI 边界版本 | — | `gui_abi_version = 1` |
| `deren_gui_vulkan.dll` | — | 导出 `deren_make_gui`、导入 `vulkan-1.dll` |
| ctest / render / runtime_dyn / AS / GUI probe | — | **19/19、14/14、10/0、45/0、6/0** |
| RT 全链路 | 低内存时失败 | **仍未通过**，提交时 device_lost；无索引 VUID 已消失 |

---

# X5 进度与交接（截至 `e029cad6`，工作树干净）

> 交接说明：B2/B3.1/B3.2 已提交且各自**当时全绿**；B3.3（格式族）**试过一次、编译与 ctest 通过但渲染 0/14，已回退**。下面写清失败的确切机制与正确改法，避免下一位重复。

## 1. 已完成并验证（提交号 + 当时的仪器结果）

| 批 | 提交 | 内容 | 验证（当时） |
|---|---|---|---|
| **B2** | `bd3ebf6f` | pass 绑定**只剩契约车道**：`pass::resolved_binding` 删掉 `VkImageView/VkBuffer/VkImage`；`family_entry` 删 raw span；`publish_family(id, element, image_handles, view_handles)`；`own_per_image` 改 `rhi::image_view*` 串；`filters::resource_handles` 改契约三件套；4 处解析校验 + `find()`/`views_of`/`instances_of` + 资源表一致性检查 + 5 个 pass（post/deferred/goo_rim/toon_screen_rim/geometry_buffer_debug）迁移；`tests/test_pass.cpp`（**它就是这条车道的单元测试**）随之迁移 | build ✓ / ctest **19/19** / `test_pass` **169/0** / 冻结渲染 **14/14** / format 0 |
| **B3.1** | `d04454c9` | `VkDeviceAddress` → `std::uintptr_t`（51 处 / 11 文件，按用户裁定）；**并修掉 ctest 非并行安全**（所有测试共用一个 `test-run` 工作目录，`test_runtime_injection` 靠**文件**读子进程 panic 文本 → 并行 18/19、串行 19/19；现在每测试独立目录） | build ✓ / ctest **19/19（并行+串行）** / 渲染 **14/14** / **`check_rt.ps1` PASS**（两帧槽 GPU 遍历 + 40 帧 + 验证层干净 + 截图） |
| **B3.2** | `e029cad6` | `VkExtent2D` → `rhi::image_extent`（31 处 / 12 文件）；恒等 helper `contract_image_extent` 删除；新增**唯一**"面向 API 的转换" `core::render_extent_2d()`（`VkRect2D`/`create_info.imageExtent` 需要 `VkExtent2D`）；一处结构化绑定修正 | build ✓ / ctest **19/19（并行）** / 渲染 **14/14** / format 0 |

**当前边界判据**（`python scripts/check_native_boundary.py`）：
- 引擎对象引用 Vulkan 符号：**0**
- `deren.exe` 图形 API 导入：**0**（也不静态导入 GUI 插件）
- 引擎源码含图形 API 词汇：**25 个文件**（census token 205 → 193）
- RHI abi：**27**（B2/B3.1/B3.2 都只动引擎侧，未动导出契约）

## 2. B3.3（格式族）：试过一次，**渲染 0/14**，已回退 —— 失败机制

**做了什么**（已 `git checkout` 回退）：`VkFormat` → `rhi::image_format`、`VK_FORMAT_*` → 契约枚举值（84 处 / 11 文件；最终把 `runtime/runtime.constructor.cppm` 那 34 处排除，见下）。**编译通过、ctest 19/19 通过**，但冻结渲染 **0/14**。

**失败签名**（验证层，14 个场景全部同一条）：
```
vkBeginCommandBuffer(): pBeginInfo->pInheritanceInfo->pNext<VkCommandBufferInheritanceRenderingInfo>
  .pColorAttachmentFormats[0] (VK_FORMAT_R4G4_UNORM_PACK8) doesn't support VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT
```

**根因（一句话）**：契约枚举 `image_format::rgba8_unorm = 1` 与 Vulkan 的 `VK_FORMAT_R4G4_UNORM_PACK8 = 1` **数值相同、语义不同**。pass 帧的 `color_format`/`depth_format` 换成契约枚举后，后端仍**原样**把它们塞进 Vulkan 结构体（`vulkan/core/core.api_core.cpp:1962-1963` 是 `reinterpret_cast<VkFormat const*>` 和 `static_cast<VkFormat>`）→ 驱动把 1 读成 R4G4。

**即：raw↔契约是双向的，逐字替换只做了 raw→契约 一个方向，在"填 Vulkan 结构体"的那条路上留了洞。** 这不是"转换容易出 bug"，而是**转换点放错了地方**。

**为什么 ctest 全绿而渲染全废**：这条路径只有在真设备 + 验证层下录制时才暴露；`ctest` 抓不到它。**能抓住它的唯一仪器是 `check_render.ps1 -Full -Compare frozen`。**

## 3. B3.3 的正确做法（请照此实现，是一个**原子改动**）

1. **引擎侧**（census 口径内的 11 个文件）：`VkFormat` → 契约 `image_format`；`VK_FORMAT_*` → 契约枚举值。保留 **raw→契约** 的换算点 `contract_image_format(VkFormat)`（它读的是 `vulkan/render_layout` 的 constexpr 表 —— 那是**已允许的例外**），并给它加一个**恒等重载** `contract_image_format(image_format)`（很多站点手里已经是契约值）。
2. **必须同批**在后端加**反向换算**：在 `vulkan/core/core.api_core.cpp` 里加一张 `constexpr VkFormat native_image_format(rhi::image_format)` 表，把 1962/1963 两行改成"先逐项填 `std::array<VkFormat, N>`，再让 `pColorAttachmentFormats` 指向它"，`depthAttachmentFormat` 同样换算。**该文件属于后端（`deren_vulkan` 的源，census 已排除）→ Vulkan 词汇在这里是允许的。**
3. **保持 raw、不要动**的地方：
   - `runtime/runtime.constructor.cppm` 的堆写入路径（34 处，喂 `VkImageViewCreateInfo` / `VkFormat`）；
   - `runtime/runtime.frames.cppm` 的两个堆 lambda `write_sampled_target` / `write_storage_target` 的 `format` 参数；
   - `static_cast<VkFormat>(this->escape().native_*)` 这类**取 raw** 的站点；
   - 成员 `depth_attachment_format`（只喂堆路径）。
4. **验证**：`build` + `check_render.ps1 -Full -Compare frozen`（**14/14** 是唯一判据）；可选 `check_rt.ps1`（RT 也用格式）。

**引擎侧格式族在 HEAD 上的实测残留**（census 口径，共 **84**）：`runtime/runtime.constructor.cppm` 34、`runtime/runtime.declarations.cppm` 28、`vulkan/pass/character_forward.cppm` 4、`vulkan/pass/transparent.cppm` 4、`vulkan/pass/scene.cppm` 3、`vulkan/primitive/primitive.cppm` 3、`runtime/runtime.frames.cppm` 2、`runtime/runtime.probes.cppm` 2、`vulkan/pass/pass.cppm` 2、`vulkan/pass/scene.cpp` 1、`runtime/runtime.cpp` 1。

## 4. 剩余批次

| 批 | 内容 | 体量（census 口径） |
|---|---|---|
| **B3.3** | 格式族（见 §3） | 84 处 / 11 文件 |
| **B3.4** | `VkSampleCountFlagBits`+`VK_SAMPLE_COUNT_1_BIT`；`VkCullModeFlags`+`VK_CULL_MODE_*`；`VkBool32`；`VkBindHeapInfoEXT` → 契约语义值 | 2 / 2 / 2 / 5 文件 |
| **B3.5** | 无 Vulkan 头的编译配置（P-Gate）+ 注入式非 Vulkan `api_core` 跑一帧 + `check_native_boundary.py --require-zero` 成为常设门禁 | — |
| **B4** | 最后的 escape：图像描述符写入、RT shader-group、GUI 创建改"契约面 + 窗口"（会清掉 `runtime/runtime.cpp` 里的 `VkInstance/VkDevice/VkQueue/VkFormat`） | — |
| 其余词汇 | `VkImageLayout`、`VkDescriptorType`、`VkImageViewCreateInfo`、`VkImageAspectFlags` 等 | 25 文件里剩下的部分 |

## 5. 流程教训（这一轮的失误，写给下一位）

- **不要逐处改逐处全量构建**。正确节奏：**一批改完 → 一次构建 → 一次门禁**。我这一轮为了 84 处改动构建了十几次，既慢又引发 `.pcm` 文件占用。
- **中止后台作业会留下仍在运行的 `ninja/c++`**，后续构建报 `unable to open output file ... user-mapped section`（那不是编译错误）。构建前先确认无残留进程；**不要中止正在跑的构建**。
- **`-j 14`**（16 线程机器）：全量构建一次约 1–2 分钟，别再降到 `-j 2`。
- 仪器的选择：**纯类型/词汇迁移用"构建 + 覆盖该路径的那一个门禁"**，不要每次跑全套；但**格式/布局这类"值被另一种语言解释"的改动，`check_render` 是唯一能抓住的仪器**（ctest 抓不到）。
- 每个提交都要留下"当时的仪器数字"；回退时注意**成对数据**（census 的配对数、abi 版本等）必须同批动。
