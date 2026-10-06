# 动态后端：进度快照（2026-10-06）

> 面向对象：接手继续的下一个 agent（无本会话上下文）。
> 两个前置文档不变：`DYNAMIC_LINK_BOUNDARY_GOALS.md`（目标与裁决）、`DYNAMIC_LINK_IMPLEMENTATION.md`（原始交接）。
> 本文件只回答三件事：**已经落了什么（带哈希与门读数）、现在树上是什么状态（未提交的在途工作）、接下来按什么顺序收尾**。

---

## 0 一句话位置

四批里**批①②已全部落地并过全门**，批③完成 6/13 个符号的清理（27 → 17），**批③的"新契约面"半成品正在树上（abi 14，未提交、未过门）**。最后一个全绿的提交是 `7672d45`。

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

## 2 边界账目：17 = 白名单 9 + 待清 7 + 构造 1

| 处置 | 数量 | 符号 |
|---|---|---|
| **白名单**（裁决 2，有意例外） | **9** | 两条 module initializer（`deren.vulkan.core` 的那条待 §6.1 实测是否随 `:handles` 强制）、`init_utils::create_recording_pool`、`vk_command_buffer` 移动构造/析构/`operator*`、`make_command_buffer`、`make_secondary_command_buffer`、`vk_sampler::operator*` |
| **批③-C 在途**（契约面已在树上） | **7** | `mark_gpu_timing`（原始形）、`begin_gpu_timing`（原始形）、`recreate_swap_chain`、`submit_frame`（已由 `submit` 改名，引擎调用点未切）、`present(uint)`、`render_extent`、`gpu_timing_available` |
| **批③-D**（最后一步） | **1** | `core::core(create_info const&)` —— 经 `deren_make_api_core()` + `shared_ptr`（deleter = `deren_destroy_api_core`） |

---

## 3 在途：树上未提交的批③-C（abi 14）——**半成品，树当前不可编译**

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
2. **③-E 门重定义**：`--require-zero` → **白名单之外为零**；白名单进门配置、**只减不增**、逐条打印命中。§6.1 的两个实测（`:handles` import 是否强制 initializer；白名单精确条数以 `--list` 为准）**仍未做**。
3. **批④ utility 拆分**：`shared_utility`（sink/轮转/panic 汇聚/分配器钩子，C 形状导出）+ `static_utility`（含 `dynamic_link`）；先全 STATIC 行为零变化，翻转时同批转 SHARED；门补"后端 DLL 导入表出现 `shared_utility.dll`"。**零成本前置修复**（轮转只做一次）建议提前。
4. **步 ④ 翻转本身**。

每步完成条件不变：构建 0 / ctest 全绿 / 格式 0 / 边界棘轮达标 / 尖刺 0 failed 自行退出 / **渲染 14 场景哈希逐字节不变**。
