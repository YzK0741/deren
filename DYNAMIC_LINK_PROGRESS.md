# 动态后端：进度快照（2026-10-06）

> 面向对象：接手继续的下一个 agent（无本会话上下文）。
> 两个前置文档不变：`DYNAMIC_LINK_BOUNDARY_GOALS.md`（目标与裁决）、`DYNAMIC_LINK_IMPLEMENTATION.md`（原始交接）。
> 本文件只回答三件事：**已经落了什么（带哈希与门读数）、现在树上是什么状态（未提交的在途工作）、接下来按什么顺序收尾**。

---

## 0 一句话位置

批①②③（含录制面 A 批）全部落地并过全门；**abi 16**。边界一路收窄：`bec8bd4` 10 → 9，`d97b754`（拥有型 `rhi::command_buffer`）→ **4**，**③-E** → **3**，③-D/E 的第一步（`constant_init` 共享 target）→ 3（import 38 → 12），1b（`acceleration_structure`/`ray_tracing` 脱离 `core`）→ 3（import 12 → 10），**C 批（abi 16，引擎自持采样器）→ 2**，**A1.0 → 2（import 10 → 8）**。当前读数：**2 symbols / 6 站点 / 0 owning-STL**、白名单 **2 条 0 stale**；`ctest` **18/18** + 边界回归 **35/35**；尖刺 **96 checks / 0 failed / 自行退出**；**14 场景哈希逐字节不变**（并有校验层见证 VUID=0）。剩余 2 = `deren.vulkan.core` 的 initializer（**被 8 个文件的 import 强制**）+ `core::core(create_info const&)`，**两条正是动态链接版 runtime 要一起消失的**。

**现在的主仪表是 import 图**（`--require-zero` 每次打印，唯一来源）：**38 → 12 → 10 → 8 点 / 8 文件**（`core` 5 + `core.pipeline` 3，测试 1 文件不计入门）。**目标 0，只有读到 0 且 14 哈希不变才允许删旧 runtime。** 这 8 点里哪些是空 import、哪些是真依赖，已逐点量过并列在 `docs/dynamic_runtime.md` §3。

**路线（用户裁决，取代"扩 escape/最小 ③-D"）：以现在的 runtime 为蓝本写动态链接版，成功后删掉前者。** 新 runtime 从第一天就经 `deren_make_api_core()` 构造、持 `shared_ptr<rhi::api_core>`、永不命名 `core`；渲染链资源改由引擎经契约 `create_image()`/`make_view()` 创建并持有（**A1 已完成**），裸句柄走**已有** escape 访问器。顺序：**③-E（已完成）→ 第 1 步 E/D/1b（已完成）→ 第 4 项 A1/B/C（C 已完成，**A1 已完成：七片**）→ 第 2 步写 `runtime/` 的动态链接版（设计见 `docs/dynamic_runtime.md`）→ 第 3 步原子删除旧 runtime**。

---

## 0.1 A1（渲染链所有权）已完成：七片，每片一笔

| # | 提交 | 内容 | 六项读数 |
|---|---|---|---|
| A1.1 | `9e7eb3b` | `taa_history` 对（引擎建/持/放，写堆） | 全绿 |
| A1.2 | `874675d` | `rt_shadow`（逐帧槽；契约追加 `image_format::r16_sfloat`） | 全绿 |
| A1.3 | `48338fc` | `hdr` + `ldr`（hdr 带 TRANSFER_SRC） | 全绿 |
| A1.4 | `a933d2c` | G-buffer 簇（三张 + velocity + scene_color + depth，走契约 `depth` 角色） | 全绿 |
| A1.5 | `8405a74` | ml 三张 + bloom 四级；`bloom_level_count` 移入共享模块 | 全绿 |
| A1.6 | `8159284` | furnace cube（`cube_compatible`，CUBE 视图由后端从形状推导） | 全绿 |
| A1.7 | `ae66fbd` | 收尾删除（成员/cleanup/`create_render_targets`/三个裸分配器）；派生 extent 的公式合并为一份 | 全绿 |

每片的六项读数相同：构建 0 / `ctest` 18/18 / `clang-format-check` 0 / 边界 **2**（0 stale、0 untracked）/ import **8 点 8 文件（不动，正如裁定：A1 买的是所有权不是数字）**/ 尖刺 96 checks 0 failed 自行退出 / **14 场景哈希逐字节不变** / 校验层 VUID 0。引擎的十四个目标现在全部由 `runtime::create_render_chain_targets()` 经契约创建、持有，并在每一代重建前释放。

**A1.7 的派生 extent 合并（取创建出的图像，见 `docs/dynamic_runtime.md` §1 的同一基准说明）之证据基础，照实写明：**

> bloom 半边与 megalights 链**今天无法被任何 capture 见证**——bloom 只能从 GUI 到达（`runtime.set_bloom` 由 GUI 绑定调用，无 config key），megalights 链则不可复现（实测见 `docs/megalights.md`：`[lighting] demo_lights` 打开时同一二进制三次运行三个帧哈希，`megalights` 开/关都一样）。因此这项改动建立在**结构性论证**（创建与查询同一个基准、同一个来源）+ **14 哈希不变**之上，而不是建立在一次测量之上。

**顺带两条 measured 覆盖缺口（都不属于 A1 的行为改动）**：`bloom` 无 capture 可达；`demo_lights` 的路径不可复现且 validation-dirty（10× `VUID-vkCmdDispatch-None-11376`，`megalights = false` 也一样）。详见 `docs/megalights.md` 的「Known limits, measured 2026-10-06」。


---

## 1 已落地：七笔提交，全部自带 witness

| # | 提交 | 批 | 内容 | 边界变化 |
|---|---|---|---|---|
| 1 | `0584bc2` | ① | 契约：`error` 追加 13–18；`error_zone`+`zone_of()`（纯函数不做字段）；`graphics_api`+`error_info`（冻结 POD，static_assert 钉布局）| 27（不动） |
| 2 | `8ef7370` | ① | utility：`ensure`/`verdict` 三态/`propagate`/`enforce`（说谎 classifier 即 panic；fatal 由引擎执行）；utility 单向依赖契约 | 27（不动） |
| 3 | `8b14dd9` | ① | 后端：`acquire_error`/`present_error`/`generic_error` 三张按调用点翻译表 + `failed()` 生产者；新测试 `test_error_mapping`（90 checks）注册进 VR_TEST_TARGETS 与 CI | 27（不动） |
| 4 | `34fdfed` | ②段1 | **帧面落地（abi 12→13，全计划唯一计划内跳号）**：`frame_walker`/`gpu_profiler`/`frame_open_info`；`api_core` 追加 `walk_frames()`/`profiler()`；**入口 `error*`→`error_info*` 同批**；后端两视图（构造里设 owner）；probe 走两槽环；尖刺结构断言 | 27（mark_gpu_timing 改名，2 行基线手术） |
| 5 | `13e4734` | ②段2 | 引擎切换：29 处 `current_frame` + 12+ 处 `MAX_FRAMES_IN_FLIGHT` + 三裸动词全走帧面；`classify_acquire` 分派（fatal 引擎执行）；collect 读 profiler 面；`present(uint)` 返回改 `rhi::error`（翻译进后端） | **27 → 23** |
| 6 | `0b0c933` | ③A | 三条裁决删除：`set_window_title` 整链（无调用者的死代码）、`vma log_statistics()` 调用（挪进 `vma_allocator::destroy()`，销毁前打）、`default_task_pool_threads`（策略搬至 `runtime::default_task_pool_width()`，测量结论随迁） | **23 → 20** |
| 7 | `7672d45` | ③B | 死工厂转发删除：`user_filter` 的 `make_shader_module`/`make_image_view`/`make_sampler`/`make_command_buffer`/`recreate_swap_chain` 全仓无调用者 → 删而非绕道契约工厂 | **20 → 17** |

每笔提交都过了当时的门：构建 0、ctest（17→18 个，批①加了 test_error_mapping）、clang-format-check 0、边界棘轮只降不升、尖刺 50→61 checks / 0 failed / 自行退出、**渲染 14 场景哈希逐字节不变**（冻结值清单在 `DYNAMIC_LINK_IMPLEMENTATION.md` §1；判据是打印的实际哈希，不是 exit code）。

---

## 2 边界账目（③-D/E C 批之后的实测：2）

| 处置 | 数量 | 符号 | 谁带走它 |
|---|---|---|---|
| **白名单**（`scripts/backend_boundary_whitelist.json`，只减不增） | **2** | `initializer for module deren.vulkan.core` | import 图归零（动态链接版 runtime + `core.pipeline` 的收窄） |
| | | `core::core(create_info const&)` | 动态链接版 runtime：`deren_make_api_core()` + `shared_ptr<rhi::api_core>` |
| ~~`vk_sampler::operator*()`~~ | **已离场** | 见白名单的 `departures` 段 | C 批：引擎自持 6 个采样器（`create_sampler` + `native_sampler`），不再读后端的采样器 |
| **可达下限** | **2**（实测） | 翻转门就是按这个数跑的 |  |

**历史（每一笔的实测下降，供核对）**：27 →（②段2）23 →（③A）20 →（③B）17 →（③-C + abi 14）10
→（`bec8bd4` 命令缓冲自持池）9 →（`d97b754` 拥有型 `rhi::command_buffer`，abi 15）4
→（③-E：白名单门 + 删 4 个 vestigial `import deren.vulkan.init_utils;`）3
→（`06a1080` `constant_init` 共享 target；`4e1634a` 契约环形深度；`c3844c6` 槽格共享模块；`7af1018` AS/ray_tracing 脱离 `core`）3（import 38 → 12 → 10）
→（C 批：abi 16 + 引擎自持采样器）**2**。

**符号之外的另一半账（③-E 新增）**：引擎/应用 **38 个 import 点 / 33 个文件**（`deren.vulkan.constant_init`
33、`deren.vulkan.core` 12、`deren.vulkan.core.pipeline` 3），测试 1 个文件。**动态链接版 runtime 的目标是 0**；
`--require-zero` 会对每一个报 FAIL。

---

## 3 已落地：批③-C（abi 14）——以下为收尾记录（历史）；实测 17 → 10、零 joiners

六个文件的改动已写完并逐一验证过持久化（见 §5 的坑）：

**契约侧（已完成）**
- `swapchain` 追加 `recreate()`（`ok`=重建了 / `not_ready`=0x0 窗口推迟，是状态不是失败）与 `extent()`（返回 `image_extent`，**不预乘 render_scale**——缩放是渲染器的决定）
- `command_list` 追加 `begin_gpu_timing()` / `mark_gpu_timing(mark_index, stage_name)`：**不带管线阶段词汇**（那是后端的测量细节，已内化为"首 mark TOP、其余 BOTTOM"）；mark 顺序错乱返回 `invalid_argument`
- `api_core`：`present()` 返回 `void`→`error`（静默失败正是错误机制要消灭的信息丢失）；追加 `submit(command_list&)` 与 `frame_swapchain()`
- `rhi.contract.cppm`：`abi_version = 14`，12→13→14 两条跳号理由都写全

**后端侧（已完成）**
- `frame_commands` 实现两个计时动词（内部转调既有的 slot 版本）；顺序检查从"引擎读字段"变成"后端拒名"
- `swapchain_view` 借用视图（`release()` 一次具名日志），构造里 `owner = this` 已设
- `core::present()`（契约形）返回 `error`，转发进 `present(uint)` 的翻译
- `core::submit(command_list&)`：校验 list 归属与 `frame_in_flight`，转调**已改名**的 `submit_frame`（避免 `-Woverloaded-virtual` 隐藏虚函数）

**还没做（按此顺序收尾）**
1. **probe**（`tests/probe_backend.cpp`）：实现 4 组新纯虚（swapchain 两动词、command_list 两计时动词、`frame_swapchain`/`submit`、`present` 改返回 `error`）——**不补这个，树编不过**；沿用"echo 描述符"风格做可观察回声
2. **钉值**：`test_dynamic_link` 的 `abi_version == 13u` → `14u`；尖刺同理
3. **尖刺**：swapchain/timing/submit 的结构断言（无 submit 上下文的地方只钉拒绝路径与视图同一性）
4. **引擎切换**（批③-C 的本体）：
   - `submit_and_present`：`vk.submit(...)` → `rhi_face().submit(*begin_commands())` 风格；`vk.present(idx)` → 契约 `present()`；`vk.recreate_swap_chain()` 三处 → `frame_swapchain()->recreate()`
   - 计时写侧：`gpu_mark` 整个塌缩成 `list.mark_gpu_timing(index, name)`（顺序检查已进后端）；`begin_gpu_timing` 走 list；`gpu_timing_available` 用 `profiler()->get_stage_info(0,…)==unsupported` 表达或等价
   - `render_extent`：引擎自维护 —— `frame_swapchain()->extent()` × 自己的 `render_scale`（clamp 到 ≥1x1），`on_swapchain_recreated` 时刷新；`swap_chain_extent` 的两处直读一并处理
5. **过门 + 提交**：预期边界 17 → 9（7 个 C 类符号全部离场，只剩白名单 + 构造）

---

## 4 过程中定下的裁决与实测（文档没写、代码里已体现的）

1. **`error_info` 布局实测**：libc++ 的 `std::source_location` 是**一个指针**（指向静态 impl），8 字节而非 24 ⇒ `sizeof(error_info) == 40`，断言按实测钉住；这反而证实 premise 2（文本在静态存储）。
2. **generic 表不翻译 `OUT_OF_DATE`/`SUBOPTIMAL`**：generic 调用点没有交换链语义，替调用方把未知码翻成"重建交换链"是撒谎——落 `operation_failed` + 原码随行。已写进注释与测试表。
3. **引擎不得调用后端翻译器**：曾在引擎侧调 `present_error(vk.present(...))`，**边界门当场抓获为 1 NEW**（方向反了）。修正：翻译进后端、返回类型改 `rhi::error`（返回类型不进 mangled name）。这条写进了 `core.cpp` 的注释。
4. **`failed()` 的 `native_code` 用 `int32_t`**（交接草案的 `uint32_t` 与字段自身的有符号理由自相矛盾，字段为准）。
5. **死代码先于契约面**：③B 的五个转发全仓无调用者——按实测删除，而不是给空房间修 vtable 通道。`DYNAMIC_LINK_IMPLEMENTATION.md` §6 中 ① 类的三个 `make_*` "改调用点"实为删除。
6. **计时 mark 的管线阶段归后端**：全仓 mark 只有"首 TOP、其余 BOTTOM"一种模式 ⇒ 内化进 `frame_commands::mark_gpu_timing`，契约不携带阶段词汇（避免把 Vulkan 执行模型焊进 ABI）。这回答了目标文档"未定的地方"第 2 条。
7. **尖刺不做 acquire 驱动**（契约无 submit 收尾手段，销毁带 acquired 图像的 swapchain 无效）——帧环行为由引擎路径 + 渲染门见证，尖刺只钉结构。abi 14 落地后 submit/present 都在契约上了，**下次可以驱动真帧环**。

---

## 5 本会话踩的新坑（接手者必读）

| 坑 | 现象 | 处理 |
|---|---|---|
| **Bash heredoc 编辑静默丢失** | 四段 `python <<'PYEOF'` 对源文件的编辑报 ok 但**未落盘**（构建错误暴露） | 改用 Edit 工具逐处重做并即时 `grep` 验证；**每批 heredoc 编辑后先 grep 计数再继续** |
| CRLF/LF 混杂 | 仓库各文件行尾不一（frames.cppm 全 LF，declarations 全 CRLF），python 按字符串匹配易失配 | 脚本先探测 `\r\n` 再替换；失败时 `cat -A` 看真实字节 |
| 边界基线被 json.dump 重排 | `--update` 之外的写法把 500 行 JSON 全重排 | 基线只做**文本级替换**（钉住旧符号全名 + 引号），diff 控制在 2 行 |
| `--update` 拒绝 joiner | 签名改名（mangled name 变）被当作 NEW | 这是设计；手术式 2 行替换 + 提交信息记录理由 |

---

## 6 剩余路线（批③-C 之后的）

1. **③-D 构造入口**（前提：C 类 7 个已清）：`core_owner` 静态类型改 `shared_ptr<rhi::api_core>`，经 `deren_make_api_core()` 构造。引擎侧已有 `rhi_face()`，加载器与 `detach()` 均已就绪。
2. **③-E 门重定义**：`--require-zero` → **白名单之外为零**；白名单进门配置、**只减不增**、逐条打印命中。§6.1 第 1 项**已实测回答**（`deren.vulkan.core` 的 initializer 被强制，9 个引擎成员引用它 ⇒ 下限 **10** 而非 9）；第 2 项以 `--list` 为准（当前 **10 = 白名单 9 + ③-D 1**）。
3. **批④ utility 拆分**：`shared_utility`（sink/轮转/panic 汇聚/分配器钩子，C 形状导出）+ `static_utility`（含 `dynamic_link`）；先全 STATIC 行为零变化，翻转时同批转 SHARED；门补"后端 DLL 导入表出现 `shared_utility.dll`"。**零成本前置修复**（轮转只做一次）建议提前。
4. **步 ④ 翻转本身**。

每步完成条件不变：构建 0 / ctest 全绿 / 格式 0 / 边界棘轮达标 / 尖刺 0 failed 自行退出 / **渲染 14 场景哈希逐字节不变**。

---

## 8 最终收尾：runtime 移出 `vulkan/`（用户要求，2026-10-06）

**状态（2026-10-06，S1 已落地 `61d6055`）**：「runtime 移出 `vulkan/`」这一条**在 S1 就已经满足**——新的契约-only runtime 从第一行代码起就住在根目录 `runtime/`（`runtime.cppm` + `:declarations` + `:constructor`，模块名与分区名沿用 `deren.vulkan.runtime`，所以 `main.cpp`/pass/测试的 import 行零改动）。第 ③ 步（S5）因此只剩「翻默认 + 删旧 `vulkan/runtime/`」，不再有搬迁动作；S5 的提交正文会显式点名用户这条要求已满足。

**时机**：第 ③ 步原子批次（新 runtime 通过、旧 `vulkan/runtime/` 删除）之后，单独一笔。

**为什么**：引擎的运行时住在后端目录 `vulkan/` 里，是这次拆分留下的结构债——目录位置本身在暗示"runtime 属于后端"。

**做法**
- 目标位置：仓库根 **`runtime/`**（与 `vulkan/`、`promise/`、`utility/` 平级）。
- **不搬两次**：`runtime_dyn` **直接建在最终位置 `runtime/`**，第 ③ 步即"新 runtime 在 `runtime/`、删掉旧 `vulkan/runtime/`"。
- **模块名暂不改**（`deren.vulkan.runtime` + 5 个分区保持），所以 `main.cpp`/pass/测试的 import 行**零改动**；改名为 `deren.runtime*` 是独立的机械步骤，**不在本次范围**（用户另行要求才做）。
- 连带改动（各写进提交正文）：CMake `target_sources`/file set 路径；**边界基线 JSON 的路径**（文本级替换，别重排 500 行 JSON）；**import 门与三个按路径解析源码的测试**；三份文档里的路径引用。

**见证**：构建 / `ctest` / 格式 / 边界 **3** / 尖刺 / **14 场景哈希逐字节不变** / 校验层 VUID=0，且 **import 读数不得倒退**（10 → 0 的进度在这一笔下保持）。

---

## 9 候选与开放项（记录在案，故意未做——不许悄悄消失）

1. **`primitive::destroy()` 钩子**（import 预备提交 `9d83371` 的收成）：`vulkan/primitive/primitive.cppm` 的纯虚钩子，**当前没有任何调用者**，三个实现的函数体只是 `reset()` 两个契约句柄（析构函数本来就会做），参数 `vma_allocator&` 已按裁定删除（那是该模块 import `deren.vulkan.core` 的唯一理由）。**裁定：钩子留着**（删钩子是另一件事），本项即为它的在案记录。
2. **前向路径深度**（A1 的下一组）：`depth_images` / `depth_image_views` / `depth_attachment_format` 三项仍归后端所有——它们被 `begin_rendering` 的前向路径读取，A1 有意保留并点名，**不是遗漏**。等前向路径的读取面收窄后再动。
3. **`bloom` 的 config key**（覆盖缺口）：`runtime.set_bloom` 目前只能从 GUI 到达，所以 bloom 半边无法被任何 capture 见证（`docs/megalights.md` 的「Known limits」有实测）。给 bloom 一个 config key 就能补上这半边门——**独立可选提交**，用户要求才做。
4. **引擎侧其他文件是否也搬出 `vulkan/`**（`vulkan/pass/`、`vulkan/readback/`、`vulkan/core/filter/` 等）：**翻转之后再议**（用户原话：「先做完再考虑」）。注意 `vulkan/core/filter/` 与 `vulkan/readback/` 正是 import 门剩下的两个真依赖，它们的最终归属和 `core&` 参数一起在步 2/3 收尾。
5. **动态树的边界门模型**（S1 新发现，**已落地**，见 §10）：动态树里 `deren_make_api_core`/`deren_destroy_api_core` 成为**新**的引擎→后端引用（这正是设计中的边界，写在 `promise/rhi/backend_entry.hpp`），而 `core::core(create_info const&)` 变 **stale**——那是翻转的目标，不是失败。棘轮规则「没被追踪过的符号不能进白名单」⇒ 动态树有**自己的一份 baseline + whitelist**，现在由 `--config legacy|dynamic` 成对选取。

---

## 10 每配置门（S1 之后新增，用户仪器）

**为什么**：两棵树跨边界的**形状不同**——legacy 引擎引用**具体类**（`core::core(create_info const&)`），dynamic 引擎引用 **C 入口**（`deren_make_api_core` / `deren_destroy_api_core`）。共用一套 baseline + whitelist 会让每棵树把对方的符号报成 stale：噪音，不是边界。

**跑法**（每次运行**先**打印它在量哪个配置、哪棵树——这一行**不受 `--quiet` 影响**，防止读错）：

```powershell
python scripts/check_backend_boundary.py --config legacy    # 默认 build-release-clang64 + legacy 那一对
python scripts/check_backend_boundary.py --config dynamic   # 默认 build-release-dyn-clang64 + dynamic 那一对
```

| 配置 | 默认树 | baseline | whitelist |
|---|---|---|---|
| legacy | `build-release-clang64` | `backend_boundary_baseline.mingw.json` | `backend_boundary_whitelist.json` |
| dynamic | `build-release-dyn-clang64` | `backend_boundary_baseline.dynamic.mingw.json` | `backend_boundary_whitelist_dynamic.json` |

**实测（2026-10-06，S1 树状态；数字以门自己的打印为唯一来源）**

| 项 | legacy | dynamic |
|---|---|---|
| 符号 | **2**（baseline 2） | **3**（baseline 3，一次性按动态树实测 `--initialize` 建立） |
| 引用站点 | 4 | 4 |
| owning-STL | 0 | 0 |
| whitelist | 2 hit / 0 stale / 0 untracked | 3 hit / 0 stale / 0 untracked |
| import | **3 站点 / 3 文件** | **3 站点 / 3 文件** |

- legacy 的 2 = `_ZGIW5derenW6vulkanW4core` + `core::core(create_info const&)`，**原样不动**（2 symbols / 2 hit / 0 stale）。
- dynamic 的 3 = 同样那两条 + **`deren_make_api_core` / `deren_destroy_api_core`**；`core::core(create_info const&)` 在动态树里**不出现**——它随旧 runtime 一起消失了，**这正是翻转的目标，不是失败**（它只在 legacy 的那一对里）。
- 两棵树的 **whitelist 都只减不增**，逐条带理由；两个符号加进动态白名单的理由是「**这是设计好的边界**，声明在 `promise/rhi/backend_entry.hpp`」——**没有 `--warn` 通过**。
- 动态 baseline 的建立是一**次性、有记录**的动作：`--config dynamic --initialize`（3 symbols / 4 sites / 0 owning STL / kit 71 members / backend 1235 defined），提交正文写明理由。
- **世界在 S5 之后重新变单数**：S5 删掉旧 `vulkan/runtime/` 时，legacy 那一对（baseline + whitelist）随它一起删，只剩动态一套。

两棵树 `--require-zero` 当前都**红**，符号侧**都**已经没有白名单之外的东西（legacy 2 hit、dynamic 3 hit，均 0 stale / 0 untracked）；红的是另外两条，照实分开写：

| 失败项 | legacy | dynamic |
|---|---|---|
| import 站点 | `3 站点 / 3 文件`（filters、readback、旧 runtime 的 declarations） | `3 站点 / 3 文件`（同一张清单——旧 runtime 的 declarations 在本树不编译，但 filters 与 readback 仍在） |
| 应用证据（main/chores） | 有（`main.cpp.obj` + `libchores.a`） | **没有**：S1-S3 期间 dynamic 树按设计不构建 app/chores（`VR_RUNTIME_SERVES_THE_APP=OFF`），所以翻转门报 "needs main/chores evidence" |

dynamic 那条应用证据的缺口**不是记账问题，是 S4 的验收项**：S4 的 file-list swap 让 dynamic 树重新构建 app/chores（`VR_RUNTIME_SERVES_THE_APP` 塌缩），那一栏随之变成「有」。在此之前，dynamic 树的可用读数就是上面那张表——**符号 3 / import 3**。

---

## 11 S2 的前置：一个在 S1 之前看不见的**硬依赖边**（2026-10-06，实测）

**结论先写**：S2（`:declarations` + `:constructor` 进 `runtime/`）**不能只动 `runtime/`**——它压着一条此前没有被点名的依赖：**共享的 `vulkan/core/filter/filters.cppm`**（`vulkancorekit` 拥有，两棵树都编译）。

**怎么测出来的**：把六个 legacy 分区整份拷进 `runtime/`、只做机械替换后逐点清账：

1. `runtime.declarations.cppm`（4175 行）里**非注释**提到 `core` 的只有 **9 行**，其中真正的类型/接口面只有 **5 处**：`export import deren.vulkan.core;`、`std::shared_ptr<core> core_owner`、`core& vulkan_core`、`runtime(std::shared_ptr<core>)`、以及文件末尾三个自由函数（`create_buffer` / `create_buffers` / `buffer_address`，参数是 `core&`）。这个文件基本是**能整体复用的**。
2. `core::heap_slots` / `heap_slot_offset` / `heap_slot_stride` / `heap_image_capacity` / `heap_sampler_base` **根本不归后端**：`core` 只是把它们从 **`deren.vulkan.render_layout`**（同属 `vulkan_constant_init`，两棵树共享）别名过来的（`core.declarations.cppm` 的 `static_assert` 就是漂移守卫）。动态侧把 `core::heap_slot*` 换成 `render_layout::heap_slot*` 即可——这是一次**纯机械替换**（六个文件共 ~126 处），不需要任何契约新增。
3. `runtime.constructor.cppm` 里 `vulkan_core.X` 的真实调用点总共 ~23 处，绝大多数已有契约等价物（`create_buffer` / `create_buffers` / `create_command_buffer` / `wait_idle`）；`render_scale` 直接取 `create_info::render_scale`（**契约里就有**，`rhi.core_desc.cppm`）；`window` 取 `options.native_window`（`main.cpp:543` 本来就传了）；`heap_grid_offset != VK_WHOLE_SIZE` 这个"堆可用"哨兵由 `descriptor_heap::ready()` + `contract_heap_ready()` 承担。
4. **挡路的那条边**：`runtime.declarations.cppm` 的成员 `user_filter filtered_core;` / `pass_filter pass_resources;` 来自 `deren.vulkan.core.filters`，而 `filters.cppm:85/86/133/134` **今天就持有 `std::shared_ptr<core> owner_share; core* vk_core;`**，`user_filter` 还要 `get_window()` / `get_swap_chain_extent()` / `get_vma()`，`pass_filter` 要 `vma()`。动态 runtime 没有 `core` 可传——**不先把这两个 facade 改成 `std::shared_ptr<rhi::api_core>` + `escape()`，动态 runtime 连类体都放不下**。这正是设计 §3 表格第 7 项（filters 是"真依赖"），此前被排在 S2 之后；实测证明它是 S2 的**同一批次内的先决条件**，不是后续项。
5. **它一旦要改，就不能只改自己**：`filters.cppm` 属于 `vulkancorekit`、两棵树共享，legacy 树也要跟着改（legacy 侧刚好把 `core&` 传进去即可，因为它实现 `api_core`）。用户要"legacy 树全程全绿"，所以这一笔必须**一个提交内同时**改 `filters.cppm` + 两个 runtime 的调用点。这是 S2 从"一个分区的移植"变成"跨共享模块的一笔"的原因。
6. **仍然算不出结论的一处（留档，不猜）**：`graphics_queue_family_index` 在契约里**既不在 `create_info` 也不在 `vulkan_escape`**（`native_queue()` 只给队列句柄）——它只出现在 `runtime.probes.cppm:86/204`（S3 的器械），所以**不挡 S2**；到 S3 时要么加一行 escape 访问器（**会跳 abi，必须先说明理由**），要么从 `native_queue()` 反查（用只读探针借队列做 `vkGetPhysicalDeviceQueueFamilyProperties`，零契约改动但有实现风险）。

**当前状态**：树是**干净的**、两棵树全绿（legacy `ctest` 18/18；dynamic `ctest` 18/18 + `test_runtime_dyn.exe --with-device` 7 checks / 0 failed / 自退出）。S2 的拷贝试验**已全部回滚**，没有半移植状态留在树上。

### 11.1 filter 访问器逐条普查（2026-10-06，写代码之前量的）

方法：全仓 `*.{cpp,cppm,hpp,h}`（含 third_party，命中即为命中）穷举每个访问器名，并覆盖三种到达方式（`runtime->X`、`->X`、`.X`）以及 `pass_resources.` / `filtered_core` 直连；`main.cpp` / `chores.cpp` 单独查（它们分别按指针与引用持 runtime）。

| 访问器 | 活着的调用者 | 裁定 |
|---|---|---|
| `user_filter::wait_idle()` | **1：`main.cpp:2861` `runtime->wait_idle();`**（经 `operator->`） | **活**，唯一一个；改接 `rhi::api_core::wait_idle()`（契约现成虚函数，零新增面） |
| `user_filter::get_device()` | 0（只有 `runtime.declarations.cppm:80/158` 两句注释拿它当例子） | 死代码 → 删 |
| `user_filter::get_window()` | 0 | 死代码 → 删（GLFW 只由 runtime 构造函数用 `create_info.native_window`，`main.cpp:543` 本来就传） |
| `user_filter::get_swap_chain_extent()` | 0 | 死代码 → 删（后继事实是 `frame_swapchain()->extent()`，但没有消费者） |
| `user_filter::get_swap_chain_image_format()` | 0 | 死代码 → 删 |
| `user_filter::get_current_frame()` | 0 | 死代码 → 删 |
| `user_filter::get_vma()` | 0（只有 `runtime.declarations.cppm:2226` 注释点名） | 死代码 → 删 |
| `pass_filter::device()` | 0 | 死代码 → 删 |
| `pass_filter::swap_chain_image_format()` | 0 | 死代码 → 删 |
| `pass_filter::swap_chain_extent()` | 0 | 死代码 → 删 |
| `pass_filter::vma()` | 0 | 死代码 → 删 |
| `pass_filter::register_resource()` | **`runtime.frames.cppm:1883/1886/1889`** | **活**，且完全不碰 `core` |
| `pass_filter::resource()` | **`runtime.frames.cppm:1868`** | **活**，且完全不碰 `core` |

**最关键的一条**：`get_vma()` / `vma()` **零调用者**——`vma_allocator*` 只是被存着回答一个没人问的问题。所以它既不是"契约缺面"也不是"要进 escape"：pass 自己分配资源那件事**已经走 S3 的资源模型**（`register_resource`/`resource`，`runtime.frames.cppm` 就是这么用的）。**两个都删，不动 abi。**

**`graphics_queue_family_index`（量的结果，不是猜的）**：两处（`runtime.probes.cppm:86/204`）用途相同——`VkCommandPoolCreateInfo.queueFamilyIndex`，给两段堆探针创建一次性 `VkCommandPool`，record + `vkQueueSubmit` + 等 fence，跑在帧环之外。**不是诊断打印**，所以"去掉这个数据"的代价是**把两段探针重建到契约命令缓冲上**（改变探针测的东西，且属 S3 风险）。裁定：加进 `vulkan_escape`，**abi 16 → 17**，理由「探针必须按原始设备事实分配/提交；escape 正是为表达不了的原生事实准备的；`native_queue()` 只给句柄，反查队列族是探针不该背的风险」——**随 S3（`:probes`）落地**。

### 11.2 共享 filters 批**已落地**：`0abb40a`（2026-10-06）

`vulkan/core/filter/filters.cppm` + `filters.cpp` + legacy `runtime.constructor.cppm`（少一行初始化）+ `runtime.declarations.cppm`（三处注释与 `operator->` 说明），**一批、两棵树同笔**。

- 十个死访问器**删除**（逐条见 §11.1）；`user_filter::wait_idle()` 改接契约虚函数；`pass_filter` **不再持任何设备根**（两个活成员只碰自己的表），改为 `pass_filter() = default`，legacy 的 `pass_resources{core_owner}` 初始化随之删掉。
- `export import deren.vulkan.core;` → `import deren.promise.rhi;`，**并做了消费者核查**：唯一靠传递性拿到的公开名字是环形深度，它作为 `user_filter::max_frames_in_flight` 继续存在；`controller.cppm` 早已直接用 `deren::promise::rhi::max_frames_in_flight`（其注释写明原因）。**以构建为证**（两棵树），且 import 门下降。
- 读数：构建 legacy 0 / dynamic 0；`ctest` 18/18 两棵树；`clang-format-check` 0；边界 **legacy 2（2 hit/0 stale）/ dynamic 3（3 hit/0 stale）不动**；import **3 站点/3 文件 → 2 站点/2 文件**；尖刺 96 checks 0 failed 自退出；scaffold `test_runtime_dyn` 7 checks 0 failed 自退出；**渲染 14 哈希逐字节不变 + 校验层零 VUID/ERROR/WARNING**（本批有 C++ 改动、`wait_idle()` 是活路径，故按裁定必须跑）。
- 一处已记录的复现坑：两棵树**连着**构建时偶发 `unable to open output file '...pcm'`（`ERROR_USER_MAPPED_FILE`）——**重试即过**，两次都是瞬态，不是代码问题（属于 `DYNAMIC_LINK_IMPLEMENTATION.md` §2 "共享构建树竞争"那一类）。

### 11.3 escape 访问器一次清点（S2/S3 共用，**只跳一次号**）

裁定：S2/S3 期间**所有新增 escape 访问器并进一次 abi 16 → 17**。逐点清账后的结论是——**只剩 3 个，全部有零替代方案的证据**：

| 需要的新 escape 访问器 | 谁要 | 为什么没有替代 |
|---|---|---|
| `graphics_queue_family_index()` | `runtime.probes.cppm:86/204`（两段堆探针建一次性 `VkCommandPool`） | 契约里既没有（`native_queue()` 只给句柄），反查队列族是探针不该背的风险 |
| `native_mesh_dispatch()`（`PFN_vkCmdDrawMeshTasksEXT`） | `runtime.cpp:1375/1382`（`draw_mesh_tasks`，参数是**裸 `VkCommandBuffer`**） | 契约的 `mesh_shader::dispatch_mesh` 收的是 `command_list&`，拿不到裸命令缓冲；另加一层"借帧的 list 去录一段独立工作"比一个函数指针复杂且改变探针/路径语义 |
| `native_mesh_dispatch_indirect()`（`PFN_vkCmdDrawMeshTasksIndirectEXT`） | `runtime.cpp:1394+`（同一原因） | 同上；且契约只声明了**直接**形式（`dispatch_mesh`），间接形式本来就缺 |

**其余全部已被现成契约面覆盖，不需要新增**（逐条已核）：`logical_device`/`instance`/`physical_device`/`graphics_queue_handle` → `escape()` 的 `native_device`/`native_instance`/`native_physical_device`/`native_queue`；`swap_chain_images` → `frame_image()` + `escape().native_image()`；`swap_chain_image_format` / `depth_attachment_format` → `escape().native_image_format(*image)`；`depth_images` / `depth_image_views` / `swap_chain_image_views` → 引擎自建的 `rhi::image` / `image_view` 容器（A1 已把所有权拿过来）；`device_properties` → `escape().native_physical_device()` 上取（或已有查询）；`descriptor_heap_limits.max_push_data` → `descriptor_heap::properties().max_push_data`；`ray_tracing_pipeline_properties` → 契约已有对应查询/扩展；`heap_grid_offset` 的"堆可用"哨兵 → `descriptor_heap::ready()` + `contract_heap_ready()`；`mesh_shader_available` → `abilities()` 的 `mesh_shader` 位；`copy_image_to_memory` → **契约虚函数** `host_image_copy::copy_image_to_memory()`；`frame_readback_buffer()` → **契约虚函数**；`render_extent` → 引擎自维护（`frame_swapchain()->extent()` × `render_scale`）。

### 11.4 `shared_ptr<core>` / `core&` / `core*` **逐类型**普查（2026-10-06，动 S2 之前量）

问题（用户）：**「以前 `vk_image` 等几个类型都需要 core 的 `shared_ptr`，现在怎么办？」** 方法同 filters 那次：全仓 `*.{cppm,cpp,hpp,h}` 穷举三种拼写（`shared_ptr<core>`、`core&`、`core*`，各自含全限定写法），再按 **CMake 的 `target_sources`** 判定文件属于 `deren_vulkan`（后端）还是 `vulkancorekit`（引擎）。

#### A 类：后端内部的持有者——**保持不动**（DLL 自己拥有自己的 core）

| 类型（后端） | 文件:行 | 证据（都在 `deren_vulkan` 的 target_sources 里，且引擎无构造点） |
|---|---|---|
| `core` 自身 + 其 12 个嵌套视图（`command_buffer_view`/`swapchain_view`/`frame_image_slot`/`readback_slot_view`/`commands_view`/`frame_walker_view`/`gpu_profiler_view`/`descriptor_heap` 的 family 视图…） | `vulkan/core/core.declarations.cppm:277,309,333,356,380,393,411,477,570,583,606`（`core* owner`） | 全部在 `vulkan/core/`，属 `deren_vulkan`；引擎侧没有一处构造它们 |
| 后端句柄 `vk_buffer` / `vk_image` / `vk_image_view` / `vk_sampler` / `vk_command_buffer` / `vk_shader_module` / `vk_pipeline` | `vulkan/core/handles/handles.cppm`、`vulkan/core/vma/handles/vma_handles.cppm`、`vulkan/core/vma/vma.cppm`、`vulkan/core/descriptor_heap/descriptor_heap.cppm` | 同上；`vma_allocator::create_*` 的内部注册表 + 引用计数，**引擎从不构造** |
| 拥有型包装 `command_buffer_view::vk_command_buffer buffer` / `vk_buffer readback_slot_buffer` / 六个 `vk_sampler` / `vk_image_view make_image_view(...)` 等 | `vulkan/core/core.declarations.cppm:311,444,478,540,552,658,892-897,1121-1188` | 全在后端；`core.api_core.cpp` 里 `delete this` 时由这些 RAII 成员释放 |
| `init_utils` 的 `core&` free 函数（`create_host_buffer` / `create_host_buffers` / `create_texture_2d`） | `vulkan/core/init_utils/init_utils.cppm:82,109,142`、`vulkan/init_utils/init_utils.cppm`、两份 `.cpp:21,41,64` | **两者都属 `deren_vulkan`**（CMakeLists 435/436/457/458），唯一调用者是 `vulkan/core/core.cpp:18`、`vulkan/core/core.constructor.cppm:44`（`import :init_utils;`）；**引擎侧零调用者**（全仓 `init_utils::` 只有注释命中）⇒ 后端内部，不动 |
| `acceleration_structure` / `ray_tracing` 的 `rhi::api_core* contract` | `acceleration_structure.cppm:210,360`、`ray_tracing.cppm:249` | **已经是契约指针**（这正是"引擎侧持有者"该有的形状），不动 |

#### B 类：引擎侧的持有者——**改成 `shared_ptr<rhi::api_core>` / `rhi::api_core&`**（与 filters 批同形）

| 类型（引擎） | 文件:行 | 谁在调 | 改成什么 |
|---|---|---|---|
| `runtime::core_owner` | `vulkan/runtime/runtime.declarations.cppm:219` | `runtime` 自己的每个成员/方法 | `std::shared_ptr<rhi::api_core>`（S2 落地） |
| `runtime::vulkan_core` | 同文件 `:221` | 四个实现分区的 `vulkan_core.X`（实测 ~40 处真实调用点） | `rhi::api_core&` |
| `runtime::runtime(std::shared_ptr<core>)` | 同文件 `:2293`、`runtime.constructor.cppm:283` | **全仓零调用者**（只有 `main.cpp` 的 `create_info` 构造走 `:278` 那条） | 换成 `explicit runtime(std::shared_ptr<rhi::api_core>)`（顺便保住"共享设备"这个能力），或删——S2 定 |
| free helpers `create_buffer` / `create_buffers` / `buffer_address` | `runtime.declarations.cppm:4164,4172,4180` | `runtime.frames.cppm:72,90,101` 的定义 + 构造/帧路径调用 | 首个参数 `rhi::api_core&`（body 本来就是 `rhi::api_core` 调用，零行为变化） |
| `readback` 的 `core* gpu` + `readback(core&)` | `vulkan/readback/readback.cppm:60,78`、`readback.cpp:23,28,37,48,57,83,117` | **全仓零实例化**（`deren.vulkan.runtime` 在 S2 批次 2 已不再 import 它；`readback x` 无命中） | `std::shared_ptr<rhi::api_core>` + `rhi::api_core&`，取原生句柄走 `escape()`（与设计 §3 第 8 条一致） |

**便利性（为什么 legacy 调用点通常不用改）**：`shared_ptr<core>` **可以隐式转** `shared_ptr<rhi::api_core>`（`core` 实现 `api_core`），`core&` 也可以隐式转 `rhi::api_core&`——filters 批已经靠这一条做到了"只改定义、不改调用者"，`:283` 的 `filtered_core{core_owner}` 一行未动就是实证。

#### C 类：需要"比 core 活得更久"的类型——**本轮零命中**

量到的所有句柄类型都落在 A 类（后端内部注册表 + 引用计数）或契约句柄上，**没有**任何引擎类型需要"活过 core"：`rhi::object_manager<rhi::image|image_view|sampler|command_buffer>` 的 `release()` 就是后端注册表的引用计数，`vma_allocator` 的注册表在 DLL 内。**所以没有需要新契约面的地方，也没有提前停下的理由。**

#### 顺带确认（用户问的"那几个类型现在归谁"）：**引擎已不再持有任何 `vk_*` 包装类型**

- 两棵树的跨界符号集里**一个 `vk_*` 都没有**：
  - legacy 基线 2 条 = `_ZGIW5derenW6vulkanW4core` + `core::core(create_info const&)`；
  - dynamic 基线 3 条 = 同一个 initializer + `deren_make_api_core` / `deren_destroy_api_core`；
  - **`vk_sampler::operator*()`** 记在 legacy 白名单的 `departures` 里（C 批离场），**动态白名单 `departures` 为空**——它在动态树从来没出现过。
- 引擎侧现在只持有**契约句柄**：`rhi::object_manager<rhi::image>` / `<rhi::image_view>` / `<rhi::sampler>` / `<rhi::command_buffer>`（`runtime.declarations.cppm:278-320,1075-1130,1183-1185,1278,1443,1450` 等），原生句柄一律经 `vulkan_escape::native_image` / `native_image_view` / `native_sampler`（连 `tests/spike_backend_boundary.cpp:374/435/444` 也是这么读的）。
- `vk_image` / `vk_image_view` / `vk_sampler` / `vk_command_buffer` 四个类型**仍然存在**，但全部留在 `deren_vulkan` 内部（A 类），**引擎不构造、不命名、不跨界**。
- 由此顺带纠正一条**已过期的白名单理由**（只改文字，不改符号）：动态白名单里 `_ZGIW5derenW6vulkanW4core` 原写"引擎仍 import 3 个文件（含 filters）"——filters 批之后实测是 **2 个文件**（`readback.cppm` + legacy 的 `runtime.declarations.cppm`），已改。

---

## 12 S2 的第一次尝试：**量到第二块承重墙，已整体回滚**（2026-10-06）

**结论先写**：S2（`:declarations` + `:constructor`）**不可能独立落地**。它拖着 `runtime.cpp`，而 `runtime.cpp` 又拖着 **`:frames` + `:probes`**——所以 S2/S3 **本来就是同一笔**，设计与任务切分把它当成两笔是错的。尝试已经**整体回滚**，树回到 `89ef962` 的全绿状态（读数见文末）。

**做完的部分（回滚前实测，可复用）**：
1. `runtime.declarations.cppm`（4200 行）**只剩 9 处非注释 `core`**，全部改完：去掉 `export import deren.vulkan.core;`、`core_owner` → `std::shared_ptr<rhi::api_core>`、`vulkan_core` → `rhi::api_core&`、`runtime(std::shared_ptr<core>)` → `runtime(std::shared_ptr<rhi::api_core>)`、文件尾三个自由函数首参 → `rhi::api_core&`；新增 `create_options` / `window` / `render_scale` / `swap_chain_image_format` / `ray_query_available` / `mesh_shader_available` / `rt` 三个派生成员；`gbuffer_pass_attachment_count` 从 `render_layout::gbuffer_target_count + 2` 本地导出（不命名后端）；`gpu_timing_mark_capacity` 那条 `static_assert` 用本地常量 + 注释（**唯一一处数值重复，已写明**）。
2. `runtime.constructor.cppm`（2400 行）：`std::make_shared<core>` → `make_contract_core()`（`deren_make_api_core` + `deren_destroy_api_core` deleter，与 S1 脚手架同形）；`render_scale` 取 `create_options`（并**照实记录**：契约不暴露后端钳过的值，动态侧自己按文档化的 0.1..1.0 钳，越界时静默 vs legacy 打日志——全在区间内时两者逐值相同）；`window` 取 `create_info.native_window`；53 处 `vulkan_core` 改成契约调用/escape；`core::heap_slot*` → `render_layout::heap_slot*` 共 **88 处**机械改名。
3. `runtime.cpp`（2016 行）：全部改完并可编译（这是**关键发现**：它的未定义符号证明 constructor 依赖它）。

**关键发现（为什么 S2 吞掉 S3）**：`runtime.cpp` 定义的 85 个方法里，构造函数直接调用的有 `refresh_frame_extents()` / `frame_ring()` / `buffer_address()` / `buffer_of()` / `write_heap_buffer()` / `create_buffer(s)`（自由函数）/ `run_heap_probe` / `run_heap_graphics_probe` / `default_task_pool_width()`。构造必须链接它们 ⇒ **`runtime.cpp` 必须进动态树**；而 `runtime.cpp` 里未定义符号的清单又包含 `gbuffer_pass_active` / `scene_target_view` 等 **frames 分区**的方法 ⇒ **frames 也必须进**；frames 又要 probes 的 `draw_mesh_tasks` / `readback` 的 `frame_readback_buffer` 使用者。**一环扣一环，没有"只加两个分区"的中间态。**

**清账读数（供下一笔准备）**：带 `:declarations` + `:constructor` + `runtime.cpp` + `:frames` + `:probes` + `:readback` 一起编译后，剩下的错误是 **24 + 若干**（`-ferror-limit` 截断），全部是同类机械替换：
- frames：`ray_query_available`/`mesh_shader_available`（成员，已加）、`depth_attachment_format`/`ray_tracing_pipeline_properties`（成员，已加）、`gbuffer_target_count`/`gbuffer_formats`/`gbuffer_velocity_format`/`hdr_format` → `render_layout::`（14+ 处）、`device_properties`/`descriptor_heap_limits` → `runtime_detail::physical_properties_of` / `heap_max_push_data`、`vk` 局部绑定缺 3 处、`swap_chain_images`/`swap_chain_image_views` 家族发布 1 处、`present_barrier.image` 1 处。
- probes：`logical_device`/`graphics_queue_handle`/`graphics_queue_family_index`/`heap_grid_offset` 全部 → `runtime_detail::` 共享 helper（18 处）；`host_image_copy` 那处要从裸 `vkCopyImageToMemoryEXT` 改成**契约虚函数** `copy_image_to_memory(image, span, region)`（ABI 形状变了：`image_copy_region` 的字段名是 `extent/mip_level/base_array_layer/...`，不是 native 字段）。
- **一处必须裁定**：`graphics_queue_family_index` 我用**运行时反查**（`vkGetDeviceQueue` 逐 family 借 family 0 的队列，只有正确 family 才返回同一个 handle，Vulkan 自己的规则、只读、零 ABI）——但探针每帧/每次创建 `VkCommandPool` 也要用它。**要么继续用同一个反查 helper（零 abi）**，要么按原裁定在 abi 17 引进 `graphics_queue_family_index()`；我在尝试里用的是反查，未提交，等你再确认。

**树状态（回滚后实测，`89ef962`）**：legacy 构建 0 / dynamic 构建 0；`ctest` 18/18 **两棵树**；脚手架 `test_runtime_dyn.exe --with-device` **7 checks / 0 failed / 自退出**；边界 legacy **2**、dynamic **3**（均 0 stale）。**没有半移植状态留在树上。**

**下一笔的建议切分（若采纳）**：S2+S3 **合并成一笔"runtime 的六个分区一次进树"**，按上面那份错误清单逐条清；中间态不发布。这比"两笔各自可编译"更诚实——因为中间态**在数学上不存在**。

---

## 13 翻转后的批：`shared_utility::shared_object` 收拾设备根的生命周期（用户新决定，2026-10-06，**记档，现在不做**）

**决定**：在 **`shared_utility`**（批④那个每进程一份的库）里补一个 **`shared_object`**，让引擎侧取设备根的那条路（`get_api_core`）**直接拿到 `shared_object`**，取代今天"裸 `shared_ptr<rhi::api_core>` + 自定义 deleter + 永不卸载"三条规矩各自为政的写法。**`shared_object` 会成为引擎侧唯一构造设备根的方式。**

**为什么**：
1. 今天的形状是 `shared_ptr<rhi::api_core>` + `deren_destroy_api_core` 作 deleter：**控制块在一侧分配、在另一侧销毁**——跨 DLL 边界最脆的形状之一。同工具链能工作，但"能工作"正是这一路在消灭的那类依赖。
2. 改成**不透明句柄 + 显式 retain/release**，**引用计数住在后端对象里**（权威在该侧），引擎只拿句柄 ⇒ **没有跨边界控制块**。
3. 而"进程一份的引用计数/注册表"**正是 `shared_utility` 该管的那类状态**——与日志 sink、轮转、panic 汇聚、分配器钩子同性质，不是新概念。
4. 顺带把**不变式 4（DLL 永不卸载）**从"三条散落的约定"变成**一条显式的进程级 pin**：进程持一个引用到底，其余正常计数。**`dynamic_link` 留在 exe 侧不变**（它是加载的钥匙，进 `shared_utility` 会形成"先加载 shared_utility 才能加载后端"的引导链）。

**两条不能破的约束**（写下来，否则这个想法会撞坏已定的裁决）：
1. **契约不依赖 `shared_utility`**（已定：`rhi` 既不依赖 shared 也不依赖 static）。所以**不让 `deren_make_api_core` 返回 `shared_object*`**——那会让契约 ABI 带上一个来自 `shared_utility` 的类型。正确形状：**契约仍只谈 `rhi::api_core*`**；后端新增两个 C 入口 **`deren_retain_api_core` / `deren_release_api_core`**（引用计数在后端对象里）；`shared_utility::shared_object` 是**引擎侧的 RAII 句柄**（进程一份的注册表 + 计数），`get_api_core` 返回它。**契约的返回类型原则不变：按值返回的 POD 冻结、新增函数＝跳号。**
2. **abi 16 → 17 已预定给 `graphics_queue_family_index()`（随 S3）** ⇒ `shared_object` 方案**另计一次跳号（18）**，**与批④ `shared_utility` 拆分同批**，不塞进 S2/S3，也不为省号提前。~~（2026-10-06 更正：那**一次 16 → 17 已被撤销**——`graphics_queue_family_index` 改走运行时只读推导，见 §14。于是 `shared_object` 从 18 变为**下一次可用号**，其余不变。）~~

**批④ 原内容不变**（日志 sink / 轮转 / panic 汇聚 / 分配器钩子 + C 形状导出 + 两目标先都 STATIC、翻转时同批转 SHARED）；**`shared_object` 是加在它里面的第二样东西**。

**终局形状（做 S5/翻转时就知道它要来接谁）**：`deren_make_api_core` 仍是契约的 C 入口（返回 `rhi::api_core*`）；后端对象自己带引用计数，两个 retain/release 入口是它的门；`shared_utility::shared_object` 是引擎侧那个**唯一**的设备根持有者，`get_api_core` 交出的就是它；`dynamic_link` 仍在 exe 侧加载后端并按名字取那三个入口（make + retain + release）。

---

## 14 合并笔（S2+S3）的第二次尝试：**编译面已全通，只剩链接面的定义缺失，仍整体回滚**（2026-10-06）

**结论先写**：六个分区**全部编译通过**（0 编译错误），剩的是**链接期的未定义符号**——也就是说"逐条清错误"这条路的**不确定性已经归零**，剩下的是纯机械的量。两次尝试都在同一轮预算内做不完，故仍**整体回滚**，树回到 `2e8b071` 全绿。

**这一轮做出来的（全部实测，回滚后需要重做但已记录）**：
1. `runtime_detail` 命名空间（在 `:declarations` 里、被所有分区共享）落地：`gbuffer_pass_attachment_count`（= `render_layout::gbuffer_target_count + 2`）、两个镜像常量（`backend_gpu_timing_mark_capacity`=16、`scene_texture_capacity`=128，各带注释）、以及 `native_device_of` / `native_instance_of` / `native_physical_device_of` / `native_queue_of` / `host_copy_of` / `physical_properties_of` / `heap_max_push_data` / `ray_tracing_properties_of` / `graphics_queue_family_of`（声明）。**这些就是"每个后端事实的唯一推导处"**。
2. **判决②落地**：`graphics_queue_family_index` **运行时推导**（`vkGetDeviceQueue(device, family, 0)`，返回 escape 队列句柄的那个 family 就是对的——Vulkan 自己的规则、只读、零 ABI），**构造时算一次、缓存进成员**，失败**panic**（不是默认 0），注释里写明"后端 `find_queue_families` 取的是第一个 `VK_QUEUE_GRAPHICS_BIT` family，其 index 由构造时的 `vkGetDeviceQueue` 产生，所以比较不会错"。
3. **判决③落地（并已在 legacy 树验证编译通过）**：`render_layout::clamp_render_scale(float)` 作为**唯一钳制者**；后端 `core.constructor.cppm` 调它（保留它自己的"越界就打日志"），引擎也调它。**两棵树都能编译**（legacy 树实测 0 错误）。→ **`graphics_queue_family_index` 的 abi 16→17 取消**：见下。
4. 六个分区的机械替换全部跑完：`core::heap_slot*`→`render_layout::`、`hdr_format`/`gbuffer_*`→`render_layout::`、`vk.*`/`this->vulkan_core.*` 的每个后端事实→`runtime_detail::` 或成员、`core&`→`rhi::api_core&`、构造改为 `make_contract_core()`（C 入口 + 后端 deleter）+ 三参委托构造、`rhi_face()` 返回 `*core_owner`。

**取消 abi 17 的结论（必须同步）**：§11.3 里"`graphics_queue_family_index()` 进 escape、abi 16→17"**作废**；合并笔**不需要任何跳号**，abi 停在 **16**。§13 的 `shared_object` 因此从编号 18 变成"下一次可用号"。

**剩下的链接期未定义符号（14 个，就是下一笔的清单）**：`runtime::default_task_pool_width` / `refresh_frame_extents` / `frame_ring` / `buffer_address` / `write_heap_buffer` / `buffer_of` / `run_heap_probe` / `run_heap_graphics_probe`、自由函数 `create_buffer` / `create_buffers` / `buffer_address`、`runtime_detail::graphics_queue_family_of`（**尚未写定义**）。其中前者全部来自 `runtime.cpp`——它**已经进 CMake 的 `VR_RUNTIME_PRIVATE_SOURCES`**（本轮已加，回滚时一并撤掉），定义补上即消。

**下一笔的最短路径（照此执行，少走弯路）**：① 六个分区照上表替换；② 在 `runtime.cpp` 里给 `graphics_queue_family_of` 写定义（遍历 family、`vkGetDeviceQueue` 比对句柄）；③ `VR_RUNTIME_MODULES` 加 `:frames`/`:probes`/`:readback`，`VR_RUNTIME_PRIVATE_SOURCES` 加 `runtime.cpp`；④ 编译——预期只剩 `:frames`/`:probes` 里的**同类机械替换**（本轮已把清单缩到"链接期缺失"这一步，编译错误为 0）；⑤ 全门（**两棵树**构建/ctest/格式/边界/import/尖刺/scaffold/14 哈希程序化比对/VUID=0）。









## 15 合并笔（S2+S3）**已落地**：六个分区一次进树，双树全门通过（2026-10-06）

**结论先写**：第二次尝试留下的"只剩 14 个链接期未定义符号"这条路走完了。六个分区（`:declarations` /
`:constructor` / `:frames` / `:probes` / `:readback` / `runtime.cpp`）现在都在根 `runtime/` 里，动态树
**第一次把整个渲染器跑起来**，而且 **14 个场景哈希与 legacy 控制组逐个相同**。落地在 `wip/step2-merged`
分支的两笔提交上（大切片 + S4 的文件表翻转），**未推送**。

### 15.1 这一笔做了什么

1. `runtime.declarations.cppm`（4200 行）**不再 import `deren.vulkan.core`**：`core_owner` 是
   `std::shared_ptr<rhi::api_core>`（由 `make_contract_core()` 造：C 入口 + `deren_destroy_api_core`
   deleter），`vulkan_core` 是 `rhi::api_core&`，`rhi_face()`/`escape()`/`valid()` 公开（scaffold 用），
   文件尾三个自由函数首参改契约引用。
2. 新增 **`runtime_detail` 命名空间**（声明在 `:declarations`，定义在 `:constructor`，
   `graphics_queue_family_of` 在 `runtime.cpp`）：每个后端事实**只推导一次**。
3. 六个分区的机械替换（§12/§14 的清单）：`core::heap_slots*`/`hdr_format`/`gbuffer_*` →
   `deren.vulkan.render_layout`；`core&` → `rhi::api_core&`；`vk.*`/`vulkan_core.*` 的每个事实 →
   `runtime_detail::` 或成员。
4. `runtime.cpp` 进 `VR_RUNTIME_PRIVATE_SOURCES`，`:frames`/`:probes`/`:readback` 进
   `VR_RUNTIME_MODULES`；**S4 的文件表翻转**：`VR_RUNTIME_SERVES_THE_APP` 在动态树也 ON（应用、
   `chores`、`render_start_demo` 两棵树都建），因为"两棵树都渲染"才是 14 哈希可比的前提。

### 15.2 测量没覆盖、这一笔补上的（逐条实测）

| 缺口 | 处置 |
|---|---|
| `:probes` 的 host image copy | 改成契约虚函数 `host_image_copy::copy_image_to_memory(image, span, image_copy_region)`；**后端第一次服务这个能力**：新增 `core::frame_host_copy`，只在 `host_image_copy_available`（扩展 + feature + GENERAL 在 copy-source 列表里）时广播 |
| 展示图像（`swapchain_image` 家族） | 契约不给"一整串交换链图像"，只借出**本帧那一张**（`frame_image()`）。引擎用 `image::make_view()`（abi 16 item B）自建 OWNED 视图，**按 acquire 到的 image index 发布这一个实例**（pass 正是按 `frame.image_index` 查找的）；视图**每个 frame slot 一个**（该 slot 的下一帧在等过自己的 timeline 之后才来），并在**任何 swapchain 重建之前** `wait_idle()` + 全部释放（否则视图会活过它包着的图像） |
| 表面格式 | `escape().native_image_format(*frame_image())` **在第一次 acquire 之前不存在**（`frame_image()` 按契约是 nullptr），而 `create_passes()` 是首帧之前跑的。**abi 16 → 17**：`vulkan_escape::native_swapchain_image_format()`（§11.3 的"一行 escape 补面"条款正是为这种情形写的；§14 撤下的 17 号由这项使用，理由写在 `abi_version` 与访问器旁） |
| G-buffer 深度格式 | 引擎自己经契约 `depth` ROLE 建了图像 ⇒ 问它自己建的那张（`escape().native_image_format(*gbuffer_depth_images[0])`），构造时一次 |
| mesh dispatch 两个入口 | 用 `vkGetDeviceProcAddr` 在 escape 的原生设备上解析（仓库既定做法），构造时缓存进成员；两处调用点拿的是裸 `VkCommandBuffer`，契约的 `dispatch_mesh(command_list&)` 表达不了 |
| 交换链图像数 | `rhi::max_swapchain_images`（契约承诺的**上界**，也是所有 per-image 数组的尺寸；契约自己的注释说 `swapchain::image_count()` 是 abi-17 的路线而它故意不走） |
| 后端自建窗口（`native_window` 传空） | 这一半拿不到那个窗口（契约只有 caller 填的 `void*`）⇒ GLFW 回调**只在调用方交了窗口时安装**；正式应用永远交（`main.cpp`），所以交互路径零变化。`tests/test_runtime_dyn.cpp` 走的正是"不交窗口"这条路（它此前会在这里空指针崩） |
| `deren.vulkan.core` 的其余事实 | 特性位（ray query / mesh shader：启用扩展 + `vkGetPhysicalDeviceFeatures2`）、`vkGetPhysicalDeviceProperties` 与 RT SBT 属性、`descriptor_heap::properties().max_push_data`、`window`/`render_scale` 取 `create_info` |
| 两个镜像常量 | `gpu_timing_mark_capacity`=16、`scene_texture_capacity`=128 在 `runtime_detail` 里**写明出处**；`gbuffer_pass_attachment_count` = `render_layout::gbuffer_target_count + 2`（同一表达式，不复制数字） |

**顺带修掉的两处**：`runtime::mesh_dispatch` 那个**从未被写、也从未被读**的 vestigial 成员删掉（两个调用点读的是后端 `core` 的指针）；`vulkan/readback/readback.cppm` + `.cpp` 改成
`std::shared_ptr<rhi::api_core>` / 契约面 + escape 取原生句柄 —— import 门里那第二个站点因此离场。

### 15.3 门读数（2026-10-06，本分支实测；数字以门自己的打印为唯一来源）

| 项 | legacy（控制组） | dynamic |
|---|---|---|
| 构建 | exit 0 | exit 0 |
| `ctest` | **18/18** | **18/18** |
| `clang-format-check` | 0 | 0 |
| 边界（符号/白名单） | **2** / 2 hit / **0 stale** / 0 untracked | **2** / 2 hit / **0 stale** / 0 untracked |
| import 门 | **1 站点 / 1 文件**（legacy 自己的 declarations） | **1 站点 / 1 文件**（同上；那张清单是源码扫描，不看本树编不编） |
| 尖刺 `--with-device` | 96 checks / 0 failed / 自退出 | 同（同一棵尖刺树） |
| scaffold `test_runtime_dyn.exe --with-device` | —（legacy 树没有这个目标） | **7 checks / 0 failed / 自退出**；`abi_version() = 17` |
| 渲染 14 哈希 | 14/14（见下） | **14/14 与 legacy 逐个相同**（`matched 14, mismatched 0`，程序化比对） |
| 校验层 | 零 VUID / ERROR / WARNING（渲染门自己按 `VUID-|Validation Error|[ERROR]|[WARNING]|panic` 扫每个场景的日志，14 个场景全部通过这一关） |

**14 个实际哈希（两棵树逐字节相同；也就是 `DYNAMIC_LINK_IMPLEMENTATION.md` §1 的冻结值）**：
`deferred 972A31EC5FF55C87` / `deferred_taa_fxaa 4021B16AFDB2F43E` / `deferred_ssao_off BFE3A472FBAB0B5E` /
`shadow_single A92C5965316679F3` / `unlit F3C2D7FEFDAD864F` / `transparent_blend CC7F77F93487AA5E` /
`sponza 50AF7E46CC1E2A92` / `metal_rough_glossy A1AFBFB61DBFD104` / `glossy_motion 9F31E89BE38B771C` /
`deformation 723569BA0D03640C` / `laevatain_goo_toon CF5A34D8DF6B6FFC` /
`laevatain_goo_toon_body C3365CEEB8AD3723` / `laevatain_no_sidecar E9A2983BEB57D5C5` /
`laevatain_old_chain 190EB09D3E9FDCDA`。**门的 exit code 仍然是 1**（本机参考集是旧的，CHANGED 是预期），
判据是这两组打印出来的实际哈希。

**边界账目跟着降**：动态白名单的第三条（`deren.vulkan.core` 的 initializer）**离场**进 `departures`
（readback 不再 import 后端模块，剩下唯一的 import 者是 legacy 的 declarations，而动态树不编译它），
动态 baseline 由门自己的 `--update` 收紧 **3 → 2**；legacy 白名单那条 initializer 的**理由文字**改成
实测的 1 个文件（符号与条数不动，`count` 仍 2）。

### 15.4 一个门覆盖缺口（实测，记档）

`build-release-dyn-clang64` 里**没有** `chars\` 与 `deren-ab\` 两份**本地资产**（构建目录都被 gitignore，
两棵树各有一份）。`-Full` 第一次跑在动态树上，4 个 `laevatain_*` 场景直接 FAIL（`failed to load model` →
panic → `libc++abi: terminating`，exit `0xC0000409`），补上 `chars\` 后仍有两个场景**哈希不同**——原因是
`deren-ab\gooblender\images\PreIntegratedFGD_GGXDisneyDiffuse.png` 也缺（日志里是 `goo FGD LUT: NOT
uploaded`），补上 `deren-ab\` 之后 14/14 逐字节相同。**结论**：动态树跑渲染门之前必须把这两份资产拷到
它的构建目录旁；这不是代码差异，但"缺资产会被记成 changed"这件事本身值得写下来。

### 15.5 下一笔（S5）的入口

`VR_RUNTIME=legacy|dynamic` 的分支现在只差"删旧 runtime"：legacy 那一对 baseline+whitelist、
`vulkan/runtime/` 六个文件、`main.cpp` 里对旧路径的任何引用都在 S5 里一起走；import 门读到 0 的那一天
就是 `deren.vulkan.core` 的 initializer 离场的日子（现在只剩 legacy declarations 一个消费者，
而它正是 S5 要删的文件）。

## 16 S5（翻转）**已落地**：只剩一份 runtime，import 门读到 0（2026-10-06）

**结论先写**：默认翻成唯一配置、删 `vulkan/runtime/`、删 legacy 那对 baseline/whitelist。判据里最关键的
"**import → 0**"达成（门自己打印 `0 site(s) in 0 file(s)`），边界 **2 symbols / 2 hit / 0 stale**，
**14 个哈希对冻结清单 matched 14, mismatched 0**。

### 16.1 这一笔做了什么

* **删**：`vulkan/runtime/` 七个文件；`scripts/backend_boundary_baseline.mingw.json` +
  `scripts/backend_boundary_whitelist.json`（legacy 那一对，随旧 runtime 一起退役）。
* **CMake**：`-DVR_RUNTIME=legacy|dynamic` 选项与 `VR_RUNTIME_SERVES_THE_APP` **一起退役**（注释写明选项当初
  为什么存在、为什么现在只剩一份文件表），`runtime/` 的六个模块 + `runtime.cpp` 无条件进 `vulkancorekit`；
  `test_runtime_dyn` 不再被条件包着。
* **`scripts/check_backend_boundary.py`**：只剩 `dynamic` 一个配置（`--config legacy` 现在是 argparse 的
  **具名拒绝**，exit 2，而不是悄悄回落到另一对），默认 build-dir 改为 `build-release-dyn-clang64`。
* **三个按路径读源码的测试**改指向 `runtime/`（这一条是 §8 里"import 门与三个按路径解析源码的测试"的兑现）：
  `test_render_resources` 的目录遍历现在走 `vulkan/` + `runtime/` **两个根**（它此前一直读的，是还留在
  `vulkan/` 里的那份副本）；`test_toon_material_sidecar` 两条路径；`test_goo_toon_math` 三条路径 + 一处
  源码文本钉值（`core::heap_slots::goo_fgd_lut` → `deren::vulkan::render_layout::heap_slots::goo_fgd_lut`，
  注释写明钉的是同一常量、只是现在从**两半都编译**的那个模块里取）。
* **`tests/test_backend_boundary.py`**：`--config legacy` 的用例改成"被拒绝"；"每个配置各自一对文件"的用例
  改成"只剩一对，且 legacy 那对被删除"。
* **`docs/dynamic_runtime.md`**：状态改为 **LANDED(S5)**；§1 记录开关退役与"只剩一个配置"；§3 明确标注为
  **移植前的历史测量**（路径已不存在）；§5 记两项开放项（见 16.3）。
* 动态 baseline 由门自己的 `--update` 刷新（应用证据 `complete: true`、import 列表清空、扫描 139 → 132）。

### 16.2 门的读数（唯一存活的树 = `build-release-dyn-clang64`；数字以门自己的打印为唯一来源）

| 项 | 读数 |
|---|---|
| 构建 | exit 0 |
| `ctest` | **18/18** |
| `clang-format-check` | 0 |
| 边界 | **2 symbols / baseline 2 / 2 hit / 0 stale / 0 untracked** |
| import 门 | **0 站点 / 0 文件**（132 源文件扫描；这就是"最后一个站点 `vulkan/runtime/runtime.declarations.cppm:62` 消失"的读数） |
| `--require-zero` | **exit 0 全绿**（"every measured symbol is whitelisted (2 hit), the whitelist has no stale entry, and no engine/application source imports a deren_vulkan module"） |
| scaffold | `test_runtime_dyn.exe --with-device` → **7 checks / 0 failed / 自退出** |
| 尖刺 | `test_backend_boundary_spike.exe --with-device` → **96 checks / 0 failed / 自退出** |
| 渲染 | **14 哈希 vs 冻结清单：matched 14 / mismatched 0**（`scripts/compare_render_hashes.py --frozen`，exit 0；证据 `build-release-dyn-clang64/render-s5-out.txt`） |
| 校验层 | 零 VUID / ERROR / WARNING：门自己按 `VUID-|Validation Error|[ERROR]|[WARNING]|panic` 扫每个场景的日志，14 个场景一次都没命中 |

**14 个实际哈希**（与 `DYNAMIC_LINK_IMPLEMENTATION.md` §1 的冻结值逐个相同）：`deferred 972A31EC5FF55C87` /
`deferred_taa_fxaa 4021B16AFDB2F43E` / `deferred_ssao_off BFE3A472FBAB0B5E` / `shadow_single A92C5965316679F3` /
`unlit F3C2D7FEFDAD864F` / `transparent_blend CC7F77F93487AA5E` / `sponza 50AF7E46CC1E2A92` /
`metal_rough_glossy A1AFBFB61DBFD104` / `glossy_motion 9F31E89BE38B771C` / `deformation 723569BA0D03640C` /
`laevatain_goo_toon CF5A34D8DF6B6FFC` / `laevatain_goo_toon_body C3365CEEB8AD3723` /
`laevatain_no_sidecar E9A2983BEB57D5C5` / `laevatain_old_chain 190EB09D3E9FDCDA`。

**`check_render.ps1` 自己的 exit code 仍然是 1，而这不是失败**：本机参考集是旧的，`CHANGED` 是预期答案；
判据是打印出来的实际哈希（这正是 `compare_render_hashes.py` 存在的理由）。

### 16.3 用户要求的两条，照实记档

1. **"把 runtime 移出 `vulkan/`"：已满足**。新的契约-only runtime 自 S1 起就住在仓库根 `runtime/`（S5 的
   提交正文显式点名这一条）；S5 删掉的，是**还留在 `vulkan/` 里的那份副本**——用户当初看到的目录位置问题，
   到这一笔为止在物理上也不存在了。
2. **"引擎侧其他文件是否也搬出 `vulkan/`"（`vulkan/pass/`、`vulkan/readback/`、`vulkan/core/filter/`）：
   翻转后再议**（用户原话"先做完再考虑"）。这是**开放项**，不是遗漏：搬目录要同时改边界基线的路径、
   import 门与三个按路径读源码的测试，是机械但独立的一笔；记在 `docs/dynamic_runtime.md` §5 与本节。
3. **legacy 构建树 `build-release-clang64` 退役成"同一个配置"**（`VR_RUNTIME` 退役后，重新 configure 它就是
   动态那一套：同源码、无分支），所以"双树比对"这个手段随之不存在，S5 的判据是**冻结清单**。该目录的本地
   资产与场景配置由 lead 的构建目录维护任务收尾（本地数据，不进仓库）。

### 16.4 紧随其后的两笔（门自身的两处缺口）

* **G1 已落地**：`check_render.ps1` 在开跑前检查本次选择真正需要的那几个本地文件（**按场景声明**，不是从 `model`/`extra` 推导——`laevatain_no_sidecar` 的 `.toon.tsv` 是**故意不存在**的，那一份缺失就是它的对照），缺哪个就打印 **缺失文件 + 需要的场景 + 缺的是哪一棵树（`chars\` / `deren-ab\`）** 并 `exit 1`，**一个场景都不渲染**。实测两条路径：藏起 `deren-ab\...\PreIntegratedFGD_GGXDisneyDiffuse.png` → `REFUSING TO RUN ... missing tree(s): deren-ab\` 并 exit 1；放回去 → 正常跑出 `CF5A34D8DF6B6FFC`。为什么不能只在报告里标注：缺资产**不报错**，它渲染出**另一张图**，汇总里就是 14 里的 2 个 CHANGED——与一次渲染回归**逐字相同**，报告写在判据之后，判据已经错了。
* **G2 已落地**：`scripts/compare_render_hashes.py` 成为**仓库里的标准门**（不再是 build 目录里的产物），两种模式：`--against <另一棵树那一轮的输出>`（双树期）与 `--frozen`（只剩一棵时对冻结十四；不带参数即此模式）。`check_render.ps1` 现在**每一轮都把实际哈希写成** `render-check/render_hashes.txt`，并新增 `-Compare frozen|<文件>`：给出 `-Compare` 时，比对脚本的判定**取代**脚本自己那套旧参考集判定，**并成为 exit code**；`--expect` 传本轮**应当**渲染的场景数，少一个就不算过（与 `-Only` 空选择必须报错同一条纪律）。标准门命令与读数：`pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare frozen` → **exit 0**，`against the frozen fourteen: matched 14, mismatched 0`，同时汇总里仍是 `changed: 12`（旧参考集，按设计如此）。

## 17 批④：`utility` 拆成 `shared_utility`（进程级）与 `static_utility`（每半一份）（2026-10-06）

**结论先写**：拆分落地，**无 abi 变化、无行为变化**——`deren.exe` 的 14 个场景哈希对冻结清单仍是
**matched 14 / mismatched 0**，边界 2/0 stale、**import 0**、`--require-zero` **exit 0**。
接手点是你提交的 WIP 快照 `2377abc`（两个目标 + 新 sink 模块已经写好、树编不过）；本笔把它修到全门。

### 17.1 拦住整条链的那个"编译器崩"，以及修法

**症状**：`FAILED: CMakeFiles/static_utility.dir/utility/utility.cpp.obj`，`clang frontend command
failed due to signal`（不是普通 error）。`#include <new>`（你在交接里点的第一次尝试）**已经在文件里**了，
对这一类无效——崩溃点不是对齐 `operator new`。

**实测定位**（clang 22.1.8，日志里的栈 + clang 自己落盘的预处理源码）：

* 帧 `1. utility/utility.cpp:293:97: current parser token ')'`／`2. ...:258 parsing function body
  'deren::utility::write_png'` —— 就是 `write_png` 里那个错误消息 lambda；
* `6. .../__format/format_functions.h:371: instantiating function definition
  'std::basic_format_string<char, const std::string&, std::string>::basic_format_string<char[21]>'`、
  `7. ...:389 instantiating variable definition '...::__handles_'`；
* 崩溃在 `Sema::SubstStmt`（`-fsyntax-only` 一样崩，所以是 Sema 不是 codegen）。

即：**一个字面量 21 元素（20 字符 + NUL）、两个 `std::string` 实参的 `std::format` 调用**，在**这个 TU**
里让 22.1.8 的前端段错误。修法是**把这个形状从该 TU 里拿掉**（`utility.cpp:292-294`）：

```cpp
return std::unexpected("write_png: " + result.error() + " ('" + path.string() + "')");
```

同一函数里的 `std::format("write_png: cannot open '{}'", path.string())`（单实参）**留着**——它不崩，
这正说明这是一次针对**一个形状**的规避而不是全面禁用（注释写在事故点，含 `-fsyntax-only` 这一条证据）。

### 17.2 用户刚定的三条放松：落实位置

1. **接口是 C++、不做 C shim**——本来就是（`deren.utility.shared` 直接导出 C++），现在**写成设计声明**
   （`CMakeLists.txt` 与 `utility/shared/shared_utility.cppm` 的头部：三条放松逐条列出，并说明"后来者别把它
   '修'成 C shim"）。
2. **允许 STL**（`std::string`/`std::vector`/`std::function`/`std::span`/`std::pmr`，`log(fmt,args…)` 可以是
   接口里的模板）——旧的"**NO OWNING OBJECT CROSSES BY VALUE** / 两半不能共用一个 allocator"那段论证**整段删掉**，
   换成实测依据：`deren.exe` 导入 `libc++.dll` 且用 UCRT 堆 ⇒ 一个 C++ 运行时 + 一个堆，跨 DLL 分配安全。
   仍然按 view 传的地方（`log_text(std::string_view)`）**改写成设计选择**而不是禁令（sink 在自己这一侧拷贝、
   格式化留在调用者模板里，所以不需要导出模板符号）。
3. **拆分的真正理由是"模块内状态各一份"，不是堆也不是 STL**——`CMakeLists.txt` / `shared_utility.cppm` /
   `utility.cppm` / `shared_utility.cpp` 四处都改成这条：两份 sink = 两个 `ofstream` 写同一个 `debug.log` +
   两次启动轮转，而 `rotate_previous_log()` **TRUNCATE** 另一份还开着的文件；`log_rotation_claim.*`（按进程 id
   命名的 OS 对象）作为"每个进程只轮转一次"的守卫**保留**。`shared_object`（堆/分配器那件事）明确写成
   **abi 18 的工作**，与本批无关。

### 17.3 本批自己的两条见证（都要机器可查，已进 `scripts/check_backend_boundary.py`）

**(a) 引擎归档与后端归档引用的是同一份共享工具，不是各一份。** 查法：对四个归档跑
`nm --defined-only` / `--undefined-only`（脚本里的 `read_symbols`），两个问题：
①**有没有第二份拷贝**——`shared_utility.cpp.obj` / `shared_utility.cppm.obj` / `better_pmr.cpp.obj` /
`log_rotation_claim.cpp.obj` 这四个成员名，除了 `libshared_utility.a` 之外**任何一个归档里出现就是 FAIL**；
②**另一半是不是真在用这一份**——引擎/后端归档的未定义符号必须与该归档的定义集**有非空交集**（它们调用的
`log`/`panic`/`init_pmr`）。
读数（本笔，`check_backend_boundary.py` 每轮都打印）：

```
shared   libshared_utility.a      209 defined symbols;
         references to them: libstatic_utility.a=3, libvulkancorekit.a=3, libderen_vulkan.a=3;
         own copies elsewhere: 0
```

**(b) `deren.exe` 导入 `libc++.dll`，且没有目标链入静态 libc++。** 查法：`llvm-objdump -p <image>` 读导入表
（静态 libc++ 的特征是导入表里**没有** `libc++.dll`，所以问句是"动态运行时在不在"，而不是"有没有静态符号"）。
镜像清单 `CPP_RUNTIME_IMAGES = (deren.exe, deren_vulkan.dll)`——**后端 DLL（abi 18）一建出来就自动纳入同一条
规则**。读数：

```
cxx      deren.exe                imports libc++.dll
```

两条都在 `--require-zero` 里**会判 FAIL**（不是只打印），脚本的合成树没有 exe/共享归档时按"无可读"跳过，
所以 `tests/test_backend_boundary.py` 的 40 个用例行为不变（18/18 的计数也不变）。

### 17.4 门读数（本笔实交，唯一存活的树 `build-release-dyn-clang64`）

| 项 | 读数 |
|---|---|
| 构建 | exit 0 |
| `ctest` | **18/18** |
| `clang-format-check` | 0 |
| `check_backend_boundary.py --require-zero` | **exit 0**；边界 **2 symbols / 2 hit / 0 stale**；**import 0 站点 / 0 文件**；外加 17.3 的 (a)(b) 两条 |
| 尖刺 `--with-device` | 96 checks / 0 failed / 自退出 |
| scaffold `test_runtime_dyn --with-device` | 7 checks / 0 failed / 自退出 |
| 渲染 `check_render.ps1 -Full -Compare frozen` | **exit 0**，`matched 14 / mismatched 0`（VUID 扫描同前，零命中） |
| abi | **不变**（本批没有 contract 改动） |

### 17.5 本批没做 / 没能验证的

* **abi 18 那一批**（单导出 `deren_make_api_core` 返回 `std::shared_ptr`、删 `deren_destroy_api_core` /
  `deren_abi_version`、白名单 2 → 1）按你的指示**不在本批**。
* `shared_utility` 仍是 **STATIC**：本批只落地拆分并证明"零行为变化"，它与 `deren_vulkan` 一起变 SHARED 是
  下一批（两个一起翻会让拆分本身不可验证）。
* **(b) 只有门在跑，没有单元测试**：造一个"不导入 `libc++.dll` 的 PE"超出这个测试夹具的范围，所以它的守卫
  是**每轮门运行**而不是 `tests/` 里的用例——这一点照实写在这里。
* `deren_vulkan.dll` 目前**不存在**（后端还是 STATIC），所以 (b) 的镜像清单里现在只有 `deren.exe` 一项被读到。

## 18 SHARED 翻转：入口收敛到 ONE EXPORT、后端变 DLL、exe 按名加载（2026-10-06）

**一句话**：这一批之后"deren 用上动态后端"才算**名副其实**——`deren_vulkan` 是 **DLL**、`shared_utility`
也是 **DLL**、`deren.exe` **不链接后端**而是按名解析**唯一一个**导出，**边界读数从 1 走到 0**，
14 个场景哈希仍对冻结清单 **matched 14 / mismatched 0**。

### 18.1 入口收敛（abi 17 → 18）：三个符号 → 一个

`promise/rhi/backend_entry.hpp` 现在只有：

```cpp
extern "C" [[nodiscard]] std::shared_ptr<rhi::api_core>
deren_make_api_core(std::uint32_t abi_version, create_info const* desc, error_info* out_error_info);
```

* **删 `deren_destroy_api_core`**：所有权变成**返回的 `shared_ptr` 的控制块**（后端在自己镜像里
  `make_shared`，deleter 随控制块走），宿主不再解析 deleter、也没有裸指针可 delete。旧写法是为了把
  "同一个 C++ 运行时"排除在边界前提之外（m02244）；**这条前提现在已实测且机器可查** ——
  `scripts/check_backend_boundary.py` 的 `cxx` 检查（`deren.exe` 与 `deren_vulkan.dll` 都导入
  `libc++.dll`、UCRT 堆 ⇒ 一个运行时 + 一个堆）。
* **删 `deren_abi_version()`**：版本成为**入参**，失配即拒绝，后端自己的号放进 `error_info.native_code`
  （loader 测试与尖刺都断言了这个字段）。
* 空 `shared_ptr` = 拒绝；`create_info` 仍是唯一的创建结构、仍只在调用期间借用；空 descriptor 仍是
  `invalid_argument`。`-Wreturn-type-c-linkage` 在声明与定义两处显式 silencing（C linkage 是为了**名字**）。
* 白名单 **2 → 1**；`deren_destroy_api_core` 进 `departures`。

### 18.2 链接形态翻转 + 真正按名加载（exe 侧）

* `deren_vulkan` = **SHARED**（`-DDEREN_BACKEND_SPIKE` **退役**：它当年就是在一次性树里预演这个形态，
  现在形态是常态，选项与它的 `if()` 一起删掉；它带来的 dllexport 宏与 sanitizer 抑制变成无条件）。
* `shared_utility` = **SHARED**。**这一刻"两个 sink"的隐患才真正成立**，而 `shared_utility` 是 DLL
  正是解法：exe 与 deren_vulkan.dll **导入同一个镜像** ⇒ 一个进程一个 sink。`log_rotation_claim`
  仍是第二道防线。见证见 18.5。
* **`target_link_libraries(vulkancorekit PUBLIC deren_vulkan)` 删除**：导入库会让后端在 main 之前被
  加载（正是 dynamic_link 要替代的 load-time binding），DLL 缺失也会变成系统弹窗而不是可诊断的拒绝。
* **加载器**（`runtime/runtime.constructor.cppm` 的 `backend_entry_address()`）：用
  `deren::utility::executable_directory() / "deren_vulkan.dll"` 组**绝对路径**（Q5 教训：
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`，**不搜 %PATH%**），
  `symbol("deren_make_api_core")` 解析**唯一一个**导出，然后 **`detach()` 永不卸载**（§13 实测过的
  `FreeLibrary` 死锁）。函数内 `static` ⇒ 每进程解析一次；失败/缺符号是**具名 panic**。
* DLL 文件名是设计的一部分：`PREFIX ""` 让产物就叫 `deren_vulkan.dll` / `shared_utility.dll`
  （MinGW 默认的 `lib` 前缀会变成 `libderen_vulkan.dll`，而 loader 找的是前者）。
* exe 与 scaffold 测试加的是 **`add_dependencies`（构建顺序）而不是链接**：DLL 必须在可执行文件运行前存在。

### 18.3 翻转暴露的两个"两份进程级状态"，都修在根上

**(1) 后端模块的 BMI 不能被 DLL 服务。** `test_error_mapping` 原来 `import deren.vulkan.core` 拿三个
VkResult 翻译表；后端变 DLL 后那是"让 DLL 导出整个模块面"，与"只有一个导出"冲突。**第三个中立目标**
（前两个是 `vulkan_constant_init` 与两个 utility 半）落地：新模块 **`deren.vulkan.error_tables`**
（`vulkan/core/error_tables.cppm`，含三个翻译器 + `failed`），由 `vulkan_error_tables` 这个 STATIC 目标
承载，**后端与测试都链接它**；`deren.vulkan.core` 改成 `export import` 它，所以后端所有调用点一字未改。
顺带把 import 表的 "tests (informational)" 也从 1 文件变成 **0 文件**。

**(2) 后端自带一份 GLFW。** 这是**渲染门全红的原因**，也是本批最值得记的一条：
14 个场景全部 `exit code -1073740791 (0xC0000409)`，`debug.log` 的最后一行是
`[ERROR] program panic! / error info: can not init surface / at core::init_surface() line 547`。
根因不是 Vulkan：**GLFW 的状态是按镜像算的**。exe 用它自己的 GLFW 建窗口，把这个 `GLFWwindow*` 通过
`create_info.native_window` 交给 DLL，而 DLL 有自己的 GLFW 副本 —— `glfwCreateWindowSurface` 对"别的
GLFW 建的窗口"必然失败（同理 DLL 那份 GLFW 从未 `glfwInit`，`glfwGetRequiredInstanceExtensions` 也
答不出东西）。**修法**：契约的 `native_window` 在本平台改为**原生句柄 HWND**（契约本来就写
"whatever the backend understands"），`runtime::make_contract_core` 用
`glfwGetWin32Window()` 把 engine 自己的窗口转成 HWND 再交给后端（runtime 自己的 `create_options` 仍留
`GLFWwindow*` —— 回调在这个镜像里跑，需要的正是它）；后端 `init_surface` 对 caller window 走
`vkCreateWin32SurfaceKHR`（`VK_USE_PLATFORM_WIN32_KHR` + `GLFW_EXPOSE_NATIVE_WIN32`），实例扩展也用
显式 `VK_KHR_surface` + `VK_KHR_win32_surface`（不向未初始化的 GLFW 提问）。**后端只在"自己建窗口"那条
路径上继续用 GLFW**（尖刺走的就是这条）。

### 18.4 门与仪器

* 边界读数 **1 → 0**：引擎最后一个后端符号引用随按名解析消失（`OK: 0 symbols, baseline 0`），
  白名单**按测量清空**（不是删掉），`departures` 记录 `deren_make_api_core` 的离开。**保留读数为 1
  需要重新链接后端导入库**，那会把 load-time binding 请回来并让显式按名加载变成摆设——所以 0 才是这一批
  的正确读数（比 1 更强）。
* `check_backend_boundary.py` 学会 DLL 形态：后端"定义集"= **导出表**（`llvm-readobj --coff-exports`），
  并新增三条 artifact 检查（`--require-zero` 会 FAIL）：**导出表恰好 `deren_make_api_core` 一个名字**、
  **DLL 导入 `shared_utility.dll`**、`cxx` 覆盖 **DLL 本身**也导入 `libc++.dll`；
  `check_shared_utility` 在 DLL 形态下以"**exe 与 DLL 都导入同一个 `shared_utility.dll`** + 没有归档私藏
  这些 TU"为证据。
* 那句"DLL import/export gates remain separate"不再只是声明：导出/导入面现在是**真的**在量。

### 18.5 读数与见证（唯一存活树 `build-release-dyn-clang64`，master `HEAD`）

| 项 | 读数 |
|---|---|
| 构建 | exit 0 |
| `ctest` | **18/18** |
| `clang-format-check` | 0 |
| `check_backend_boundary.py --require-zero` | **exit 0**：边界 **0 symbols / baseline 0 / 0 site**；**import 0 站点 / 0 文件**；导出表 1 个名字；DLL 导入 shared_utility.dll；两镜像都导入 libc++.dll |
| 尖刺 `--with-device` | **95 checks / 0 failed / 自退出**（比上批少一条：版本符号查询与它的两条断言随符号一起消失，握手改读 `native_code`） |
| scaffold `--with-device` | 7 checks / 0 failed / 自退出（**这条现在真的走了 DLL**：`constructed through deren_make_api_core(); native_device() = ...`） |
| 渲染 `check_render.ps1 -Full -Compare frozen` | **exit 0，matched 14 / mismatched 0**，validation 扫描零命中 |
| abi | **18** |

**见证（本批自己的四条）**

1. **导出表恰好一个 `deren_*`**：`llvm-readobj --coff-exports deren_vulkan.dll` 只有一个 `Name: deren_make_api_core`
   （门里机器检查）。
2. **DLL 导入 `shared_utility.dll`**：`deren_vulkan.dll` 的导入表里有它（门里机器检查）——这就是"一个进程一个 sink"。
3. **日志只有一次轮转**：新建目录写入带标记的 `debug.log`，跑一次 `test_runtime_dyn.exe --with-device`；
   结果 `debug.log.old` 里
   `===== session ended at ... =====` **恰好 1 条**、标记 **恰好 1 份**，`debug.log` 里标记 **0** ——
   一次进程 = 一次轮转（`debug.log.old` 见 `build-release-dyn-clang64/rotation-witness/`）。
4. **进程干净退出**：渲染门 14 个场景各自退出码 0（每个场景还要跑两次做确定性比较），尖刺/scaffold
   自退出 —— "按名加载 + 永不卸载"这条链走通了。

### 18.6 本批没做 / 没能验证的

* **(b) 的单元测试仍缺**：造一个"不导入 `libc++.dll` 的 PE"超出测试夹具范围；守卫是**每轮门运行**。
* **POSIX 分支仍未构建**（CI 只有 windows）：`dynamic_link` 的 `dlopen` 路径与新的
  `native_window = HWND` 语义都是 Windows 事实。
* **交互路径（可见窗口）没有人工跑过**：脚本化捕获走了 HWND + `vkCreateWin32SurfaceKHR` 这条新路，
  交互路径用的是同一个后端分支、同一份代码，但"有人看着窗口动"这件事本批没有验证。
* **`plan_rhi_v4.md` §9 的四条 known-unknowns 里，sanitizer 那一条的代价兑现了**：DLL 无 sanitizer 覆盖
  （lld 无法链接 instrumented `-shared`，见 CMake 的注释），这是翻转的成本而不是本批的疏漏。

### 18.7 一条偶发构建失败的记录（附证据，非本批引入）

Lead 两次在同一棵树上看到 `cmake --build` 偶发 exit 1、重跑即过。本批期间**又复现 7 次**，每次都长同一个
样子，因此记在这里而不是让它被 CI 的重试掩盖：

```
FAILED: [code=1] CMakeFiles/<target>.dir/<file>.cppm.obj CMakeFiles/<target>.dir/<module>.pcm
error: unable to open output file 'CMakeFiles/<target>.dir/<module>.pcm':
        '请求的操作无法在使用用户映射区域打开的文件上执行。'        <- Windows 1224, ERROR_USER_MAPPED_FILE
```

随后是它的**级联**（不是第二个故障）：
`llvm-ar: error: CMakeFiles/<target>.dir/<file>.cppm.obj: No such file or directory` +
`FAILED: lib<target>.a`，因为 `.pcm` 没写出来、对应的 `.obj` 也就不存在。

* **判据**：同一份源码、同一棵树，**紧接着重跑一次就干净通过**（本批的对策是"重试最多 8 次、只在出现
  **非**该错误时才停"）；96 份保留的构建日志里**没有一份**是该错误之外的"真"失败而重试仍过的。
* **性质**：编译器写 `.pcm` 时目标文件正被另一个进程（防病毒/索引器/上一次构建遗留的映射）持有而
  拒绝写入，**不是**本批引入、也不是本仓库代码的错。CMake 自己的 job server 不受影响（错误来自 clang 前端）。
* **含义**：CI 若对该错误直接判红，会把一次重跑就能过的偶发事件当成真断；若**无脑**重试全部失败，又会
  掩盖真崩。建议的机器判据就是本批用的这一条：**只把这一条错误文本视为可重试，其余任何 error 立即停**。
