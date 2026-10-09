# X5 交接：做了什么 / 没做什么

> 最新接手核验（2026-10-09）：`aa42191e` 的未验证迁移已经修复，并通过完整构建、真设备 spike **100/0**、CTest **19/19**、冻结渲染 **14/14**、严格 GUI/RT 与格式检查。新增独立 RHI 继承标签，保留旧 Vulkan 格式整数的语义；深度角色与 GUI 格式边界已修正。详细记录见 `progress.md`。
> 第二批已核验：索引枚举及无用头文件清理后原生词汇文件 **25 → 9**；构建、CTest 19/19、冻结渲染 14/14、严格 RT 与格式检查通过。
> 以下是上一位 agent 的历史交接记录；“未验证”指当时状态，不代表当前验证结果。B4 与可移植性证明仍未完成。
> 判据现状（`python scripts/check_native_boundary.py`）：引擎对象引用 Vulkan 符号 **0**、`deren.exe` 图形 API 导入 **0**、含词汇的引擎文件 **25**、census token 193（B3.2 后）→ **156**。

---

## 一、做了什么

### 已验证（提交且当时全绿）

| 批 | 提交 | 内容 | 当时验证 |
|---|---|---|---|
| B2 | `bd3ebf6f` | pass 绑定只剩契约车道（`resolved_binding`/`family_entry`/`publish_family`/`own_per_image`/`resource_handles`；5 个 pass + 资源表检查 + `test_pass` 迁移） | build、ctest **19/19**、`test_pass` **169/0**、渲染 **14/14**、format 0 |
| B3.1 | `d04454c9` | `VkDeviceAddress` → `std::uintptr_t`（51 处/11 文件）；**ctest 并行安全修复**（每测试独立工作目录） | ctest **19/19（并行+串行）**、渲染 **14/14**、**RT 验收 PASS** |
| B3.2 | `e029cad6` | `VkExtent2D` → `rhi::image_extent`（31 处/12 文件）；删恒等 helper；新增唯一 API 面转换 `core::render_extent_2d()` | build、ctest **19/19**、渲染 **14/14**、format 0 |

### 已写、**未验证**（提交 `27344fe1`、`5fa79a32` + 当前工作树 9 个文件）

1. **B3.3 格式族**（`VkFormat`/`VK_FORMAT_*` → `rhi::image_format`）
   - 引擎侧 11 文件替换；保留**唯一 raw→契约换算点** `contract_image_format(VkFormat)`（读已允许的 `render_layout` constexpr 表）+ **恒等重载**；
   - `depth_attachment_format`、堆写入路径（`runtime.constructor.cppm` 33 处、`runtime.frames.cppm` 两个堆 lambda）**保持 raw**；
   - **后端唯一反向换算点**：`vulkan/core/core.api_core.cpp` 的命令缓冲继承（原本 `reinterpret_cast<VkFormat const*>`）改为逐项走既有的 `native_image_format(...)`，用局部 `std::array<VkFormat, 8>` + 超限拒绝。
2. **B3.4**
   - cull：`render_environment` 的 `set_cull_mode_fn`/`cull_mode_recorded` 用 `rhi::cull_mode`；`runtime.frames.cppm` 两处 lambda 的**手写 VK_CULL_MODE_* 映射删除**，直接 `set_cull_mode(mode)`（后端 `set_cull_mode` 已换算）；
   - `VkBool32` → `bool`（字段、记录值、`std::function` 签名、两处 lambda）；
   - samples：`VkSampleCountFlagBits` → `std::uint32_t`、`VK_SAMPLE_COUNT_1_BIT` → `1u`（7 处；契约的 `samples` 就是普通计数，见 `rhi.extension.cppm:508`）。
3. **B3.5 堆绑定**：删除**死路径**（再无调用点）：`contract_heap_bind_infos`（定义+声明）、`runtime::fill_heap_bind`、`scene_frame`/`transparent_frame` 的两个 `fill_heap_bind` 函数指针、`runtime.frames.cppm` 两处发布行 —— 共 12 处 `VkBindHeapInfoEXT`。**后端本来就从自己的堆推导继承**（`core.api_core.cpp:1989+` 的注释即为此），pass 层 abi 20 起已不再调用。
4. 工作树里还有：后端 `this->owner->depth_attachment_format`（原先误写 `this->`）、若干注释改写（不再点名已删类型）。

## 二、没做什么

1. **全部验证都没做**：自 `e029cad6` 之后**没有一次成功的构建、没有跑过门禁**。最后一次真实编译错误是 `core.api_core.cpp` 的 2 条（工作树里已修）。最后一次成功的构建/渲染是 B3.2。
2. **`VK_NULL_HANDLE` 36 处未做**（清单最后一族）：逐处判断 —— 契约句柄 → `nullptr`；计数/枚举 → `0`。多数落在仍在读 raw 的 escape/堆路径（`runtime.constructor.cppm`、`runtime.frames.cppm`、`ray_tracing.cpp`、`acceleration_structure.cpp`、`render_environment.cppm`）。
3. **B4 未做**：图像描述符写入、RT shader-group、GUI 创建改"契约面 + 窗口"（会清掉 `runtime/runtime.cpp` 的 `VkInstance/VkDevice/VkQueue/VkFormat`）。
4. **可移植性证明未做**（原计划 B3.5 的后半）：无 Vulkan 头/库的 CMake 配置、注入式非 Vulkan `api_core` 跑一帧、`check_native_boundary.py --require-zero` 常设门禁。
5. **`VkFormat`/格式宏残留 46 处 = 有意保留的 raw 集**（堆写入路径 + raw→契约换算点），不是待办。

## 三、下一步（只有两条命令）

```powershell
cmake --build build-release-dyn-clang64 -j 10
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare frozen
```

- 构建若报 `unable to open output file ... user-mapped section`：那是 `.pcm` 被占用的**锁问题、不是编译错误**——删掉报错的那两个 pcm 再构建一次即可。
- **绿** → 把 B3.3+B3.4+B3.5 合成一个正式提交。
- **红** → 一次改完所有错误，再跑同样两条命令。
- 渲染 14/14 是**唯一**能发现"契约枚举值被当成 Vulkan 值"这类错误的门禁（ctest 抓不到，B3.3 第一次就是这样：编译过、ctest 19/19、渲染 0/14）。
