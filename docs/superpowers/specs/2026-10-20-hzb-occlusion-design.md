# HZB 两遍遮挡剔除（M3）设计：把近景遮挡红利留在 GPU 上

> **状态：设计草案（2026-10-20）。未实现、未验收。**
> 本文是实现设计，不是教程：每一节都对着当前代码核过（文件:行号），每个"现有机制"都注明了它能不能承载本设计；
> 不能承载的地方给出契约增补的形状，而不是假设它存在。
> **行号是写本文时的工作树状态，可能随并行提交漂移**：引用以"文件 + 标识符/函数名/路径"为准，行号只作定位辅助；
> 若行号偏移 ±若干行，按名字重新定位，不要按行号改代码。
> **代码中不出现 "Nanite" 之类的商标**；需要类比时写"Nanite 风格"。标识符/路径用英文，注释与论证用中文（与
> `2026-10-18-subtilis-design.md` 的写法一致）。
> 设计名：**HZB**（hierarchical Z-buffer / Hi-Z），所有新增标识符统一 `hzb_` 前缀，日志行统一 `hzb:` 前缀。

---

## 0. 结论摘要（一页）

### 0.1 要解决的问题（已实测）

Bistro 原始层级场景（2,909 items / 4.2M 三角形）在开启 visibility lane 后，`docs/reviews/2026-10-20-bistro-performance/`
的三个只读探针给出：

| 指标（保守下界） | far（d=140 全景） | near（d=42 街道） |
| --- | ---: | ---: |
| 视锥内可见 meshlet 被**完全遮挡** | 0.41% | **33.28%** |
| 遮挡三角形占可见三角形 | 0.33% | **32.52%** |

host 侧原型（已回退，补丁归档 `occlusion-prototype.patch`）证明这个红利**确实变成 GPU 时间**：

| 指标（130 帧，validation 关） | far OFF | far ON | near OFF | near ON |
| --- | ---: | ---: | ---: | ---: |
| 被剔除的选中记录 | — | 0 / 2.73M（0%） | — | **1.67M / 4.66M（35%）** |
| GPU visibility raster | 23.85 ms | 23.77 ms | 20.79 ms | **14.16 ms（−31.9%）** |
| GPU 帧总时间 | 24.60 ms | 24.90 ms | 21.77 ms | **15.61 ms（−28.3%）** |
| CPU active | 7.68 ms | 19.97 ms（+12.3） | 7.73 ms | 18.95 ms（+11.2） |
| 端到端帧时间 | 23.18 ms | 23.49 ms | 20.61 ms | **18.70 ms（−9.3%）** |
| 画面差异 vs OFF | — | 逐像素相同 | — | **0.314% 像素变化** |

（"被剔除的选中记录"一行是 120 帧累计：far 0/2.73M、near 1.67M/4.66M = 35%；per-frame 见 §1.1 的日志引用。）

原型的两个致命伤：**CPU 每帧 +11.2 ms**（金字塔 + 4.6 万次球测试，near 帧变成 CPU-bound），**不是像素精确**
（近平面深度近似不够保守）。结论：遮挡剔除必须在 GPU 侧做，而且要做出**可证明的保守性**。

### 0.2 本设计的目标

1. **保住近景 GPU 收益**：同机同会话 near 相机下，GPU visibility raster 相对 HZB OFF 下降 ≥ 25%（原型 −31.9%），
   GPU 帧总时间下降 ≥ 20%（原型 −28.3%）。
2. **消掉 host 成本**：`record_visibility_items` 之外的 CPU 增量 ≤ 0.2 ms/帧（新增的是两张表的写入与一次矩阵快照比较，
   不再有逐 meshlet 测试）；不做任何 GPU→CPU 回读。
3. **far 不退化**：far 相机 0 个被剔除记录、帧像素**逐像素相同**、GPU 帧时间不增加（overhead 预算 ≤ 0.2 ms，
   超过则由测量驱动的门控关掉）。
4. **可证明的保守性**：**被剔除 = 上一帧深度里被保守测试完全遮住的预测**。阶段 B 用"视图/模型逐字节冻结"
   门控把这个预测变成"本帧事实"（静止 ⇒ 上一帧深度就是本帧深度）；阶段 C 由第二遍 recovery 用本帧深度
   复测被剔除集来兜底，因此运动场景**不会丢可见几何**（丢的只是"早绘 vs 晚绘"，不是像素）。
5. **默认关闭、可逐阶段回退**：开关关闭时帧字节不变；每个阶段独立提交、独立验收。

### 0.3 三个最重要的设计决策（详见 §3、§4.4、§5）

1. **历史 HZB + 两遍恢复，而不是本帧 occluder prepass**：2 帧在飞 + frame-ring 的 slot wait 已经免费保证
   "本 slot 上一次提交已完成"，所以 2 帧前的 HZB 读取**不需要任何 stall**；本帧 prepass 则要每帧再光栅化一遍
   occluder，成本与它想省的 raster 同量级（计划里 3.6B 的历史预测方案见 subtilis 设计 §11.1；本设计把
   "历史预测 + 本帧复测"做成正式形状）。正确性由第二遍"当前帧 HZB 复测被剔除集"保证，而不是靠"相机不动"。
2. **GPU 直接改写 per-item indirect 命令字的 `groups_x`**，而不是"按上界派发 + shader 里 early-out"：
   RHI 契约没有 `draw_mesh_tasks_indirect_count`（`source/promise/rhi/rhi.api_core.cppm:1635-1637` 只有直接
   `dispatch`、`draw_mesh_tasks`、`draw_mesh_tasks_indirect`），而原型的 −31.9% 恰恰来自"被剔除的 workgroup
   **没有被 launch**"；early-out 会保留 launch 开销。做法是给已有的 `mesh_indirect_buffer` 增加一个 heap
   storage 描述符别名（`heap_slots::hzb_commands`）+ `buffer_flag::device_address`，cull compute 直接写 12 字节
   命令字；这也正是 `runtime.cpp:1595-1597` 注释里预言的"compute culling pass 写 per-(frame, draw) 命令槽"。
3. **压紧后的 survivor 列表写进一张新的 per-frame GPU 表（`hzb_survivors`）**，并把
   `visibility::meshlet_source` 追加一个值 `hzb = 2` 让 resolve 读同一张表：不能在 host-culled 表上原地压紧，
   因为第二遍 recovery 的压紧会覆盖第一遍的 survivor 记录，而 32-bit vis id 已经占满（`visibility.cppm:63`
   的 static_assert），id 里没有"哪一遍"的位。一张表 + resolver 分支是唯一自洽的形状。

附加决策：**HZB 用 storage buffer 表示，不用 mip-mapped storage image**（§4.1、§5）：heap 下 image 描述符是
"一个 view"，写多级 mip 需要每级一个 storage view（每级一个槽位），而 buffer 版本只需要一个描述符 + 一段
"逐级顺序布局"，build/cull 两边都按绝对 word 偏移访问，wiring 只有一行 descriptor write。

---

## 1. 动机与实测基线

### 1.1 证据来源（原文引用）

- 探针表：`docs/reviews/2026-10-20-bistro-performance/README.md:66-74`；
  结构化 JSON：`probe-analysis-near.json:19-26`（`occlusion_off_frustum_visible` = 14,341 / 43,930 meshlet =
  33.28%，1,156,639 / 3,626,626 三角形 = 32.52%），`probe-analysis-far.json`（0.41% / 0.33%）。
- 原型表：`README.md:140-147`；原始数字 `occlusion-prototype-results.json`；补丁 `occlusion-prototype.patch`。
- 探针本身是**保守**的（README.md:63-64）：球偏小、max pyramid、只在其"最近深度仍在最远遮挡之后"才计遮挡，
  所以 33.28% 是下界，真实可剔除只会更多。
- 原型只处理 `subtilis::run_source::visible` 的 item（`occlusion-prototype.patch` 的
  `plan.run.source == subtilis::run_source::visible` 条件），所以它的 35% 是**Subtilis cut 之后**的记录占比
  （日志 `occ-near-on.log`：`subtilis: cut run publication: 43930 -> 38864 records ...`，每帧候选 38,864 条）。
  探针的 33.28% 是**LOD0 全量候选集**（43,930 条）上的占比。两者不是同一个分母，引用时必须写清。

### 1.2 目标与预算

| 项 | 目标 | 依据/说明 |
| --- | --- | --- |
| near GPU visibility raster | ≥ −25%（同会话 OFF/ON 中位数） | 原型 −31.9%；GPU 版多出 HZB build + cull 两个 pass 的成本 |
| near GPU 帧总时间 | ≥ −20% | 原型 −28.3%（host 版没有额外 GPU pass） |
| near 端到端墙钟 | ≥ −15% | 原型被 +11.2 ms CPU 卡住只剩 −9.3%；本设计 CPU 增量 ≤ 0.2 ms |
| far GPU 帧总时间 | 不增加（overhead ≤ 0.2 ms，约 0.8%） | 探针 far 遮挡 0.41%，没有收益可换 |
| far 画面 | 与 OFF **逐像素相同** | 0 条记录被剔除时帧必须字节不变 |
| near 画面（阶段 B，静止相机门控） | 与 OFF **逐像素相同** | 保守测试的强验收（原型 0.314% 是 bug，不是本设计的预期差异） |
| near 画面（阶段 C，运动相机） | 差异集中在精确等深/共面 tie 像素，比例需测量，目标 ≤ 0.05% | 两遍顺序与单遍不同，LESS_OR_EQUAL 的等深 tie 顺序敏感 |
| CPU | 新增 ≤ 0.2 ms/帧 | 原型 +11.2 ms；本设计只多两张表写入 + 每 slot 矩阵/模型快照比较 |

---

## 2. 约束：设计必须绕开的现有事实

（每条都对着代码核过；**加粗**的是本设计直接依赖或直接碰到的）

| # | 事实 | 证据 |
| --- | --- | --- |
| C1 | mesh workgroup 上限：85 三角形 / 255 顶点（8-bit 索引/256 顶点预算）；一个 meshlet 记录 = 48 B（`first_index/index_count/base_vertex/pad/center/radius/axis/cone_cos`） | `source/engine/meshlet/meshlet.cppm:45-47`、`:70-115`；`source/shaders/geometry/mesh_geometry.slang:119-132` |
| C2 | visibility lane 是 **per-item 一次 indirect mesh dispatch**，命令字由宿主写、GPU 读；命令槽 = `frame_slot * mesh_command_capacity + visibility_command_run + item` | `source/engine/runtime/runtime.cpp:1913-1920`；`runtime.declarations.cppm:2002`（`mesh_command_capacity = 3u * meshlet_capacity`）、`:2047`（`visibility_command_run = 2u * meshlet_capacity`） |
| C3 | 间接命令缓冲**不在 heap 上、没有 shader 读它**（注释原文 "IT IS COMMAND DATA, NOT A RESOURCE"），created with `indirect` flag only | `runtime.declarations.cppm:1959-1983`；`source/engine/pass/scene_tables.cppm:43,54-56`（i==6 的 `flags = indirect`） |
| C4 | host-culled meshlet 表：per-frame lane、65536 记录、item 区间 = 导入期 `meshlet_base`，survivor 压紧写在其前缀 | `render_layout.cppm:219`；`mesh_geometry.slang:148-156`；`subtilis.cppm:88-107`；`runtime.cpp:1961-1980` |
| C5 | vis id 32 位占满：valid(1)+item(12)+meshlet(12)+triangle(7)，item 上限 4096、每 item meshlet 上限 4096 | `source/engine/visibility/visibility.cppm:53-69` |
| C6 | resolve 的 meshlet 表分支：`item.meshlet_table == 0 ? 共享表 : host-culled 表`，槽位 = `item.meshlet_base + id_meshlet` | `source/shaders/scene/visibility_resolve.slang:252-258`；`mesh_geometry.slang:148-151` |
| C7 | draw_item 记录 112 B 被 static_assert 与测试钉死（12 个 u32 + model 16 floats），`first_index` 是"仅信息性"字段，**没有候选数 lane** | `visibility.cppm:135-163`（`static_assert(draw_item_bytes == 112u)` 在 `:141`）；`source/tests/test_visibility_buffer.cpp`（shader 镜像布局的解析对照） |
| C8 | G-buffer 深度：per-swapchain-image、`image_format::depth`（后端 `depth_format`，本机 D32_SFLOAT、`[0,1]` 归一化；review 的深度探针就是按原始 D32 读的）、`sampled|depth_attachment|transfer_source`，清屏值 1.0（远平面），`LESS_OR_EQUAL`；heap 采样槽 `heap_slots::gbuffer_depth + image_index` **已存在且已每帧重写** | `source/engine/pass/output_targets.cppm:62,66-101`；`runtime.frames.cppm:2912`；`visibility_raster.cpp:80-93`；`docs/reviews/2026-10-20-bistro-performance/README.md:63` |
| C9 | 2 帧在飞；`wait_and_acquire()` 在录制前**只等本 frame slot 上一次提交**（frame ring 的 slot wait） | `rhi.api_core.cppm:1953`；`runtime.frames.cppm:239-240`；`runtime.post_graph.cpp:344-350`（"CPU writes were published for the completed slot before submission"） |
| C10 | 整帧录制在**一个 primary command buffer**、一个录制线程；render graph 负责 pass 顺序与 barrier，`resolved_io::list` 是唯一录制面 | `runtime.post_graph.cpp:46-47`（21 个 lane 的 `ios`/`passes` 数组）、`:90`（`frame_command_buffer()`）、`:1186`；`pass/pass.cppm:213-351` |
| C11 | graph 的 buffer 声明词表**已支持** `compute_shader`/`mesh_shader`/`task_shader`/`draw_indirect` 阶段与 storage/indirect usage；barrier 由 `scoped_synchronization` 下发，后端 `map_stages` 支持 mesh/task/draw-indirect | `render_graph/types.cppm:84-124`；`render_graph/rhi.cppm:94-160`；`source/backends/vulkan/synchronization/synchronization.cppm:30-72,154-176` |
| C12 | **RHI 的 role-pair 路径没有 mesh shader 侧的 buffer 读**：`buffer_use::shader_read` 的 stage mask 只含 VERTEX/FRAGMENT/COMPUTE（`core.api_core.cpp:2866-2871`），且 `barrier_group::stage_hint` 只作用于 image 与 AS barrier、**不作用于 buffer barrier**（`:3150-3177`，image 在 `:3144-3145`）。=> 手动 role-pair 不能安全地表达 "compute 写 → mesh 读"；**本设计只走 C11 的 graph 声明路径** | `source/backends/vulkan/core/core.api_core.cpp:2866-2935,2997-3015,3150-3177`；`rhi.api_core.cppm:308-343,380-389` |
| C13 | RHI **没有** indirect-count / dispatch-indirect 动词；只有直接 `dispatch` 与 `draw_mesh_tasks_indirect(buffer, offset, count, stride)` | `rhi.api_core.cppm:1635-1637` |
| C14 | 舞台 push 通道 = `vkCmdPushDataEXT`（heap pipeline 无 layout），pass 的 push 端点固定追加 frame_slot/image_index/source 三个 lane；`max_push_data` = 256（本机） | `pass/pass.cppm:188-189,252-264`；`runtime.cpp:1449-1497`；`rhi.extension.cppm:513-526,575-582` |
| C15 | 每帧 CPU 规划 phase 1-3（候选扫描/并行规划/published 裁决）与 phase 4（录制）**在同一个函数 `record_visibility_items` 里**，而它由 raster pass 的 `record_items` 回调在 rendering scope 内调用 | `runtime.cpp:1697-1934`；`runtime.frames.cppm:2134-2142`；`visibility_raster.cpp:132-134` |
| C16 | 命令 run 是手工编号的常量，散在 2 个文件；`scene_tables_pass` 用 `3u * meshlet_capacity` 算命令缓冲大小 | `runtime.declarations.cppm:2002,2047`；`runtime.cpp:1915`；`scene_tables.cppm:43` |
| C17 | 新增 heap 槽位必须同时写 `render_layout.cppm` 与 `heap_slot_constants.slang`，被 `test_render_resources` 逐名/逐值解析比对；槽位只能追加在已用区域（base+1307）之后，`heap_slot_count = 2048` | `render_layout.cppm:162-166,172-375,397-398`；`heap_slot_constants.slang:30-180`；`source/tests/test_render_resources.cpp:595-786` |
| C18 | 新 shader 四处登记：`VR_SLANG_SOURCES`、两个 compile 脚本、`source/app/chores.cpp` 的 `load_shader` + `register_shader`（`test_shader_sources` 比对四处） | `CMakeLists.txt:1413,2036-2060` 的 `VR_SLANG_SOURCES`；`source/app/chores.cpp:193-213`；`source/tests/test_shader_sources.cpp` |
| C19 | 新测试 target 两处登记：`CMakeLists.txt` 的 `VR_TEST_TARGETS` 与 `.github/workflows/ci.yml` 的 `--target` 列表（`test_docs` 第 6 条不变量） | `CMakeLists.txt:1365`；`.github/workflows/ci.yml:351-354`；`source/tests/test_docs.cpp:427-437` |
| C20 | 平台固定关闭：`taa=false`、`shadow=false`、`ssao=false`、`bloom=false` 的验收配置里，相机矩阵逐帧不变；TAA 开时 proj 每帧带 jitter 变化 | `build-release-dyn-clang64/bistro-probe/perf-off-noval.toml`；`runtime.frames.cppm:318-340` |
| C21 | 相机 UBO 的 per-frame lane 在**本帧录制前**被本帧相机覆盖，所以"2 帧前的相机"不能从 UBO 读；历史相机矩阵必须自己存 | `runtime.frames.cppm:318-340`；`primitive.cppm:115-128` |
| C22 | 原型补丁里的遮挡测试有三处近似：`radius_ndc_z = world_radius / clip.w`（z）、`radius_px = world_radius * focal / clip.w`（x/y）、以及"近平面"没有显式保护 | `occlusion-prototype.patch:109-156`（`radius_ndc_z` 在 `:117`） |
| C23 | **不允许每帧 GPU→CPU 回读**：现有的深度回读探针只在截图帧跑（`DEREN_DEPTH_PROBE_CAPTURE`），而原型的"每帧深度拷到 host 可读 buffer"正是它 +11.2 ms CPU 的一部分；本设计的统计只在 shutdown 读一次（`log_meshlet_stats` 模式） | `runtime.post_graph.cpp:1129-1160`；`runtime.constructor.cppm:513-560`；`occlusion-prototype.patch:313-345` |

### 2.1 由约束推出的硬结论

- **不能原地压紧 host-culled 表**：C4 的表既是 cull 的输入又是希望复用的输出，且 C5/C6 的 id 语义要求"一个 item 的所有像素读同一张表"。两遍设计需要两张互不覆盖的 survivor 列表 ⇒ 新表 + `meshlet_source` 新值。
- **不能用 early-out 代替 GPU 写计数**：C13 没有 indirect count；直接 `draw_mesh_tasks_indirect` 仍是唯一入口，所以要让 GPU 决定 `groups_x`，只能写命令字本身（C3 的缓冲必须变成 shader 可写的 heap 资源）。
- **不能手动记 role-pair barrier**：C12 的 buffer role 词表缺 mesh 阶段；走 graph 声明（C11）是唯一不用新增 RHI 词表的路径。
- **HZB 的"上一帧"只能是 2 帧前**：C9 的 slot wait 只保证本 slot 上一次（= 2 帧前）的提交完成；1 帧前的提交可能还在飞。per-frame-slot 也是**最新鲜的无 stall 选择**；per-swapchain-image 反而更旧（3–4 帧）。
- **cull 不能用 dispatch-indirect**：C13 没有该动词；但 cull 的 group 数 = item 数，宿主已知（C5 上限 4096），直接 `dispatch(item_count, 1, 1)` 即可——不需要 GPU 决定 cull 自身的 launch 数。

---

## 3. 方案选择

### 3.1 候选 A：本帧 occluder depth prepass（被拒绝）

先画一遍"大 occluder 子集"的 depth-only prepass，从本帧深度建 HZB，再剔除并画正式 geometry。

- **优点**：同帧、无历史、无 recovery；保守性是构造性的。
- **缺点**：prepass 每帧要重新光栅化 occluder。可剔除上限只有 32.5% 三角形（near 保守下界），而 prepass 自身要画
  "足够覆盖遮挡关系"的几何——对 Bistro 街道视角就是接近整帧的三角形（遮挡物是墙/地面/整条街），成本与收益同量级。
  subtilis 设计 §11.1 已经把路线收敛成 "CPU cut → 全驻留层级资产 → GPU cut → **本帧 Hi-Z**"，但那是**在还没有
  历史方案测量**时写的；本轮原型已经证明历史深度可用且收益真实（见 §1.1），所以用历史+恢复替代 prepass。
- **保留为备用**：如果阶段 C 的 recovery 实测在运动相机下有不可接受的双绘成本（如 recovery 发出 >15% 候选），
  再评估"给最大 N 个 item 做 prepass"的混合方案。

### 3.2 候选 B：历史 HZB + 两遍恢复（采用）

```
帧 N（frame_slot = s = N % 2）:
  cull pass        : 读 HZB[s]（帧 N−2 的深度）与候选表 → 写 survivors / 计数 / 命令字
  raster pass #1   : 按 GPU 写好的组数派发 survivors（清屏 depth + vis id）
  hzb build pass   : 从本帧 raster 后的深度建 HZB[s]（供帧 N+2 用），并快照本帧 view/proj
  cull pass #2     : （阶段 C）用本帧 HZB[s] 复测"第一遍被剔除"的候选 → 写 survivors 第二段 / 计数 / 命令字
  raster pass #2   : （阶段 C）按 GPU 计数派发第二段（LOAD depth + vis id，不重清）
  resolve 等其余 lane 不变
```

- **正确性论证（阶段 C）**：第一遍的剔除结果可以是任意"预测"（历史深度错了、相机跳了都不怕）；
  第二遍把所有被第一遍剔除的候选**用本帧深度重新做保守测试**——本帧深度只含第一遍真实画出的几何，
  所以"被本帧 HZB 保守遮挡"是真实遮挡。被保留的几何一定被画（第一遍或第二遍），最终帧 = 单遍帧
  （差异只在 LESS_OR_EQUAL 的等深 tie 顺序）。
- **收益**：第一遍的剔除只花一次 cull dispatch + 一次 HZB build，没有重复光栅；recovery 只处理"被剔除集"，
  原型里这个集合的可见比例极低（相机移动时新露出的几何通常本来就"上一帧也没挡住"）。
- **阶段 B（只做第一遍）**：正确性靠**冻结门控**——只有"本 slot 的 HZB 快照与本帧视图/模型**逐字节相同**"
  时才启用（§9.2）。这会把 B 的验收限制在静止相机的 capture 上（正是实测矩阵的配置，C20），
  换取"帧逐像素不变"的强 gate；运动相机由阶段 C 打开。
- **为什么不用 stencil/two-pass classification**（"先全画、用 stencil 标出可见性、再对遮挡物做一次分类"的
  经典变体）：它先把**全部候选**画一遍，而这正是本设计要省掉的工作（原型的 −31.9% 来自"被剔除的 workgroup
  不被 launch"）；stencil 分类适合"先画、后着色"的延迟管线，不适合"用遮挡减少绘制"的目标。所以 recovery
  选择"第二遍 cull + 补画"而不是 stencil 分类。

### 3.3 已拒绝的第三种：host 侧（原型）

已被测量否决（README.md:149-154）：+11.2 ms CPU 把 near 帧变成 CPU-bound，端到端只剩 −9.3%；且不保守。
本设计不保留任何 host 侧逐 meshlet 测试代码。

---

## 4. 架构：数据流、三个 GPU 阶段、以及 dispatch 计数怎么回填

### 4.1 HZB 表示：一个 storage buffer 里的 max pyramid（不是 storage image）

**pyramid 语义**：level 0 = 本帧深度的逐像素拷贝；level L = level L−1 的 2×2 **max**；level 尺寸
`w_L = max(1, (w_{L-1} + 1) / 2)`、`h_L` 同理，直到 1×1。深度是 D32_SFLOAT、`[0,1]`（`perspectiveRH_ZO`），
清屏 1.0 = 远平面，所以 "无几何" 像素在 max pyramid 里是 1.0，**天然让遮挡测试失败**（保守），不需要哨兵值。

**为什么不是 mip-mapped storage image**：
- heap 下 image 描述符携带的是一个 view（`heap_image_write_info.view`，`rhi.extension.cppm:554-560`）；
  一个 storage view 覆盖一段 mip 区间，但 shader 的 `RWTexture2D` 不能按 mip 索引，`imageStore` 也没有 mip 参数。
  写 N 级就要 N 个"单级 storage view"描述符，即 N 个 heap 槽位 × 每帧/每镜像镜像，槽位账单与 wiring 都会翻倍。
- buffer 版本只需要 **1 个** storage buffer 描述符；level 偏移是 `width/height` 的纯函数，build/cull 两个 shader
  共享同一段内联函数就能算出来，宿主在 C++ 侧用同一个公式算 buffer 大小（纯模块，可单测，见 §10.1）。
- HZB 不是"被采样的贴图"：cull 只做 `texel` 级 max 读取（整数坐标），没有 sampler、没有 LOD/导数，buffer 语义更贴切。

**布局（一个 slot，内部 per-frame-slot lane）**：

```
hzb_levels（host 一次性算出总长；resize 时重建）
  lane s 的起点 = s * pyramid_words(extent) * 4 B
  lane 内逐级连续：[level 0 ][level 1 ][ ... ][level Lmax ]
  pyramid_words(w,h) = sum_L ceil(max(1,w>>L)) * max(1,h>>L)   // 与 shader 内联函数同式
```

（构建时按 "先 level0 后逐级" 顺序写，读侧按同一公式定位；`ceil` 除法保证与 2×2 max 的边界一致。）

**header（另一个 slot，`hzb_header`，GPU 由 build pass 写）**：build pass 在写 pyramid 的同一个 dispatch 里
由 0 号线程写 header（R/W structured buffer，元素 0）：`magic/width/height/level_count + view(64B) + proj(64B) +
camera_pos(16B)`。**view/proj 必须由 GPU 从相机 UBO 快照**，原因见 C21：宿主在帧 N+2 录制时已经把本帧相机
写进 UBO 的同一 lane，不能再用它表达帧 N 的相机；而 header 与 pyramid 由同一次 build 写出，二者永远一致。

**为什么 per-frame-slot 而不是 per-swapchain-image**：
- C9：frame ring 只等本 slot 的上一次提交，所以 slot s 在帧 N 的读取对象必然是帧 N−2 写的那一份，**无 stall**。
- per-swapchain-image 的 HZB 要等该 image 再次被 acquire（3–4 帧），更旧；且每镜像一份 pyramid 内存 ×3–4。
- HZB 是**派生数据**，不需要挂在某个 image 的生命周期上；resize/重建时只需要"作废 + 重建"（§9.3）。

### 4.2 `hzb_build` compute pass（每帧一次，raster 之后）

- 输入：heap 采样槽 `heap_slots::gbuffer_depth + heap_image_index`（C8，compute 里用 `Texture2D<float>.Load`，
  不用 `Sample`，避免 derivative 能力，见 `heap_access.slang:120-133` 的同类教训）。
- 输出：`hzb_header` lane s + `hzb_levels` lane s。
- push 块（pass 自带，经 `resolved_io::push_block`，尾部自动追加 3 个 lane，C14）：`{ uint level; }` 加 3 lane。
  `width/height` 由 shader 对深度纹理 `GetDimensions` 得到；`frame_slot/image_index` 从追加 lane 读。
- 录制：对 L = 0 .. level_count−1 各一个 `dispatch(ceil(w_L/8), ceil(h_L/8), 1)`；**相邻 dispatch 之间**
  记一个 buffer barrier `{shader_write → shader_read}`（同 buffer、COMPUTE→COMPUTE，RHI role-pair 足够，
  C12 的缺口只在 mesh 阶段）。L=0 的 shader 从深度采样，L>0 从 level L−1 读；L=0 的 0 号线程写 header。
- 保守性：2×2 max；奇数边界的越界邻居**钳到边缘**（replicate 只会把更大的深度带进来，安全）。

### 4.3 `hzb_cull` compute pass（每帧一次，raster 之前；阶段 C 再加一次 recovery）

- 一个 workgroup = 一个 item（`numthreads(64,1,1)`，`dispatch(item_count,1,1)`，item_count ≤ 4096）。
- 每个线程处理候选区间内一条记录（按线程步长循环），记录来源由 `hzb_items[item].source` 决定：
  `0 = 导入期 meshlet 表`（`hzb_items[item].base = leaf.meshlet_base`），`1 = host-culled lane`
  （`base = plan.run.base = leaf.meshlet_base`，见 §2 C4）。
  **两条来源都要支持**：验收配置 `subtilis=false` 时全部 item 走 shared 源（full LOD0），
  `subtilis=true` 时走 visible 源——这与实测矩阵的两种配置一致。
- 对每条候选：`model` 从 `heap_slots_visibility_items` 的当前帧行读（C7 的 `draw_item.model` 是 row-major，
  与 push 块同约定）；球在对象空间，测试用 §6 的数学、以 `hzb_header` lane s 里的 **旧** view/proj。
- 输出（全部写当前帧 lane）：
  - 未被遮挡的记录按**确定性顺序**压紧写入 `hzb_survivors[frame_slot * 65536 + item.meshlet_base + j]`
    （顺序用 wave ballot / prefix count，逐 chunk 做，见 §7.2；确定性让 capture 可复现、表可对拍）；
  - `hzb_results.flags[meshlet_base + i]`：每候选 1 个 u32（0 = survivor，1 = culled），供 recovery 枚举。
    **用 u32 而不是 byte**：Slang 的 `uint8_t` 结构化缓冲元素在部分后端上要走 `RWByteAddressBuffer` 才稳，
    而 65536 × 4 B = 256 KB/帧 lane 的代价可以忽略；每候选被写一次，无需清零、无需原子。
  - `hzb_results.pass1_count[item]`：本 item 的 survivor 数；
  - `hzb_commands` 的该 item 命令字 = `{pass1_count, 1, 1}`（§4.4）；
  - `hzb_results.stats`：candidates / culled / survivors 的原子累加（每 workgroup 一次，不逐记录）。
    注意**每 item 的 count 不是原子量**：该 item 的 workgroup 是它自己的唯一写者（单写者 = 比原子更强的一致性，
    也是"整帧原子性"的来源）；只有跨 item 的累计 stats 需要原子。
- **整帧原子性**：一个 item 的 workgroup 一定把它自己的区间完整算完再写 count/命令字；不存在半个 item 的
  survivor 列表被光栅读到的时刻——光栅在下一个 pass，中间有 graph 的 barrier。

### 4.4 dispatch 计数怎么回填：GPU 写命令字（不是 indirect count）

RHI 事实（C13）：只有 `command_buffer::draw_mesh_tasks_indirect(buffer const& argument_buffer, offset, count,
stride)`；没有 `_indirect_count`，也没有 `dispatch_indirect`。所以：

1. **给 `mesh_indirect_buffer` 增加一个 heap storage 描述符**：新槽 `heap_slots::hzb_commands`，
   在 `publish_scene_table_bindings`（`runtime.constructor.cppm:1734-1748`）里
   `write_heap_buffer(*this->mesh_indirect_buffer, heap_slots::hzb_commands, size, storage_buffer)`。
   它和 host 侧的 `mesh_indirect_mapped` 是**同一块内存的两个视图**，这不算冗余：host 在录制前写的是
   "候选数上界"（= HZB 关闭时的旧行为，也是退化路径），GPU 的 cull pass 在执行时改写为 survivor 数，
   同一次提交里 GPU 写晚于 host 写，GPU 胜出；下一帧复用该 slot 前 frame ring 已经等过上一次提交，无 torn read。
   **shader 侧的命令索引常量（`mesh_command_capacity`、`visibility_command_run`）是第二处 host/shader 镜像**：
   它们可以由既有的 `meshlet_table_capacity = 65536u`（`mesh_geometry.slang:156`）派生
   （`2u * meshlet_table_capacity` / `3u * meshlet_table_capacity`），并在 host 侧的
   `static_assert` / `test_render_resources` 里钉住；阶段 B 的第一个提交必须同时改两侧。
2. **`scene_tables_pass` 给该 buffer 加 `buffer_flag::device_address`**（`scene_tables.cppm:55` 当前是
   `i == 6 ? indirect : device_address`，改成 `i == 6 ? (indirect | device_address)`）：heap 描述符写是
   "地址 + 长度"（`heap_buffer_write_info{address,size}`，`rhi.extension.cppm:561-567`），
   `write_heap_buffer` 走 `buffer_address()`，没有该 flag 时地址为 0（`runtime.frames.cppm:115-122` 的实现与注释）。
3. **命令字写入位置**：cull shader 用与宿主完全相同的公式
   `index = frame_slot * mesh_command_capacity + visibility_command_run + item`（C2/C16），写
   `{ groups_x = survivor_count, 1, 1 }`；宿主在 `record_visibility_items` 里对同一 slot 写同一形状
   （候选数），**渲染窗口外没有任何变化**。
4. **barrier**：graph 声明即可（C11）：cull pass 把 `mesh_indirect_commands` 声明为 `compute_shader` 的
   write，raster pass 已把它声明为 `draw_indirect` 的 read（`render_graph/visibility.cppm:62` 的
   `indirect_buffer` scope；`runtime.post_graph.cpp:453-454` 的 import），graph 会自动插
   "compute write → draw-indirect read"。**不需要** RHI role-pair（也就绕开 C12）。
5. **退化**：如果这个别名因任何原因不可用（descriptor 写失败/设备 flags 拒绝），则本帧冻结
   `hzb_active_frame = false`（§9.1），回到"宿主写候选数 + 不做 HZB 剔除"的旧行为，画面与今天一致。

被拒绝的替代：新增 `draw_mesh_tasks_indirect_count` RHI virtual。它是 `command_buffer` 的新 virtual
（vtable 形状变化 ⇒ `abi_version` 要动，参考 `image::get_content` 在 abi 25 的追加方式），还要两个 backend +
两处静态断言；本设计不需要它就能拿到"不 launch 被剔除 workgroup"的全额收益，所以不做。
另外 `dispatch_indirect` 同样不存在，但 cull 自己的 launch 数宿主已知（C5），不需要。

### 4.5 mesh stage 如何消费压紧列表

`visibility.slang` 的 mesh entry 目前只有"共享 meshlet 表"一条分支（pbr 的 meshlet entry 有 bit0 的
host-culled 分支，visibility 的没有）；本设计需要 bit0（既有语义）和新的 bit1（HZB survivor 表）：

```text
// visibility.slang mesh_main，改这一段（现在只有 meshlet_at）
const bool host_culled = (geometry.base_vertex & 1u) != 0u;
const bool hzb_survivors = (geometry.base_vertex & 2u) != 0u;
const uint table_index = geometry.first_index + group_id.x;
const MeshletRecord meshlet = hzb_survivors  ? hzb_survivor_at(push.frame_slot, table_index)
                            : host_culled    ? culled_meshlet_at(push.frame_slot, table_index)
                                             : meshlet_at(table_index);
```

- 宿主在 `record_visibility_items` 里，用**与 `plan.run` 同一个冻结决定**同时设置三处：
  `plan.block.geometry.base_vertex = hzb_active ? 2 : (visible ? 1 : 0)`、
  `plan.row.meshlet_table = hzb_active ? 2 : (visible ? 1 : 0)`、以及 `plan.row.meshlet_base = run.base`。
  三处不同步就是"raster 读 A 表、resolve 读 B 表"——这是 subtilis M1 已经踩过的坑（subtilis 设计 §5 任务 1-4），
  所以它们必须由同一个值派生，并加一个 CPU 断言（`hzb_active` ⇒ `base_vertex == 2 && meshlet_table == 2`）。
- resolve delta：`visibility_resolve.slang:252-258` 的三元分支扩成三分支，读
  `heap_slots_hzb_survivors` 的 `frame_slot * meshlet_table_capacity + item.meshlet_base + id_meshlet`；
  `draw_item.meshlet_table` 的注释同步（`visibility.cppm:130-132,152`）。
- id 语义不变（C5）：`id_meshlet` = 该 item **survivor 列表里的序号**（= dispatch 的 group id），
  resolve 侧的 `meshlet_base + id_meshlet` 与 raster 侧 `group_id.x` 仍然是同一个索引。

### 4.6 阶段 C：第二遍 recovery（两段 survivor、同一张表）

第一遍被剔除的候选集合由 `hzb_results.flags` 枚举（**每候选一个 u32、而不是位图**：每个候选被写一次、无需清零、
无需原子；65536 × 4 B = 256 KB/帧 lane）。第二遍：

1. **`hzb_cull` 的 recovery 模式**（同一个 shader，push 里 `mode` lane / 或 `pass1_count` 是否非零判定）：
   只处理 `flags == 1` 的候选；HZB 换成**本帧刚 build 的** lane s（build pass 已在本帧 raster #1 之后执行）；
   用本帧 view/proj（同一 header 的 lane s 已被本帧 build 覆盖）。
2. **输出到同一张 survivors 表的第二段**：item 的第二段起点 = `meshlet_base + pass1_count[item]`，
   第二遍 survivor 数写 `hzb_results.pass2_count[item]`，命令字写到**第四个命令 run**
   （`hzb_recovery_command_run = 3u * meshlet_capacity`，`mesh_command_capacity` 从 3 run 扩到 4 run：
   C16 的四个常量同一次改，`scene_tables.cppm:43` 的 `3u` → `4u`）。
3. **mesh stage**：第二遍 raster 用同一个 `VisibilityPush` 的 `pad` lane 当 `pass=1`；
   shader 从 `hzb_results.pass1_count[item]` 取偏移：
   `table_index = meshlet_base + (pass ? pass1_count : 0) + group_id.x`，`meshlet_in_item` 同式写入 id。
   这样两遍的像素都指向同一张表的互不重叠区间，resolve **完全不知道也不关心**是哪一遍画的。
4. 计数不变式（必须单测）：`pass1_count + pass2_count ≤ 候选数 ≤ 4096`（C5 的 id 容量），
   否则本帧冻结 `hzb_active_frame = false` 并在下一帧重新规划。

**为什么不做"两遍 + 每遍一张表"**：id 里没有遍序号（C5），resolve 无法按像素选表；而两遍共用一张
`meshlet_base + 偏移` 的布局可以让 resolve 的索引公式保持不变。
**为什么不在 host-culled 表上原地做**：第一遍的 survivor 记录在第二遍压紧时会被覆盖，而第一遍的像素的 id
仍引用那些记录（resolve 在最后统一执行），见 §2.1。

**阶段 C 的诚实简化（必须写进文档与日志）**：HZB 在 raster #1 之后、raster #2 之前 build，所以它**不含**
第二遍补画的几何。第二遍的几何只可能比第一遍更近，因此这份 HZB 对帧 N+2 是**保守的**（tile 的 max 偏大 ⇒
少剔几块），代价只是下一帧的剔除率略降，不会产生错像素。若阶段 D 测出这个损失显著，再加一个"最终 HZB 重建"
pass（在 raster #2 之后再 build 一次，成本 ≈ 一个 build pass），属于可选优化，不是正确性要求。

阶段 C 的宿主侧记录要能**被调用两次**：`record_visibility_items` 拆出的 record 半段带一个 `pass` 参数
（0 = 第一遍，用 `hzb_command_run`；1 = recovery，用 `hzb_recovery_command_run`，push 的 `pad` lane 写 1）。
第二遍命令字的宿主预写值 = **候选总数**（上界）：若 GPU 的 recovery cull 因任何原因没写，第二遍会重画第一遍
剔除的全部候选 —— 结果退化为单遍帧，正确但慢，符合"降级只能多画"的原则。

---

## 5. 堆槽位计划

### 5.1 新槽位（全部追加在已用区域末尾 base+1307 之后，规则见 C17）

| 槽位名 | 值 | 承载 | 帧 lane | 谁写描述符 | descriptor type |
| --- | ---: | --- | --- | --- | --- |
| `hzb_levels` | base + 1308 | max pyramid（`frame_slots * pyramid_bytes(extent)`） | 内部 lane（`frame_slot * pyramid_bytes`） | constructor / resize 后 `publish_hzb_bindings` | storage_buffer |
| `hzb_header` | base + 1309 | 每 slot 的 `HzbHeader`（`frame_slots * 256 B`） | 内部 lane | 同上 | storage_buffer |
| `hzb_items` | base + 1310 | 每 item `{meshlet_base, candidate_count, source, pad}`（`frame_slots * 4096 * 16 B`） | 内部 lane | 同上 | storage_buffer |
| `hzb_results` | base + 1311 | `flags[65536]（u32/候选）+ pass1_count[4096] + pass2_count[4096] + stats[16]` | 内部 lane | 同上 | storage_buffer |
| `hzb_survivors` | base + 1312 | per-frame lane 的 meshlet 表（`frame_slots * 65536 * 48 B`，与 `meshlet_culled` 同形） | 内部 lane | 同上 | storage_buffer |
| `hzb_commands` | base + 1313 | **别名**：`mesh_indirect_buffer`（`frame_slots * mesh_command_capacity * 12 B`），不新分配内存 | 不适用（shader 自己算绝对 index） | 同 `publish_scene_table_bindings` | storage_buffer |

- 最高使用槽位 base+1313 < base+2047，`heap_slot_count = 2048` 不变；`render_layout.cppm:163-165` 的
  "1308 slots are in use / 740 free" 注释同步改成 "1314 slots / 734 free"（阶段 A 一次改完）。
- 每张表都是**一个描述符 + 内部 `frame_slot` lane**（与 `meshlet_stats`/`meshlet_culled` 的既有模式一致，
  C4/C17 的注释规则："per-frame arrays add heap_frame_slot"），**没有** per-frame descriptor 镜像。
  理由：这些 buffer 的大小与内容布局与 slot 数无关且固定（`hzb_levels` 是唯一的 extent 依赖项，
  但它的 lane 步长 = 同 extent 的 `pyramid_bytes`，shader 能用 header 里的 `width/height` 算出来；而且
  per-frame descriptor 镜像在这里反而**错**：`hzb_levels` 的 lane 起点还依赖 extent，两个独立 slot 缓冲区
  在 resize 时都要重建，没有收益）。
- `hzb_commands` 是既有 buffer 的第二个视图：`mesh_indirect_buffer` 已经有 host 侧映射和 `indirect` flag
  （C3），本设计只追加 `device_address`（§4.4 第 2 点）。**不新建 buffer**，因为它必须和
  `draw_mesh_tasks_indirect` 的 argument buffer 是同一块内存。

### 5.2 buffer 创建/生命周期

- `hzb_items` / `hzb_results` / `hzb_survivors`（+ `hzb_header`）大小与 extent 无关，进 **`scene_tables_pass`**
  （`source/engine/pass/scene_tables.cppm`）：`ids`/`heap_slots`/`descriptions()` 各加条目，
  `life = frame_slot`。**只有 host-visible 的两张**（`hzb_items`、`hzb_results`）设 `zero_on_create = true`
  （`instances.cppm:61` 的规则：`zero_on_create` 只对可映射 buffer 有意义；GPU-only 的三张不需要清零——
  header 用 magic + CPU 的 `hzb_valid` 门控，survivors 只读已写前缀）。`hzb_results` 用 `storage_coherent`
  （host 在 shutdown 读 stats，模式同 `meshlet_stats`），其余用 `storage_gpu_only`；所有 buffer 都带
  `buffer_flag::device_address`，调试探针用的再加 `transfer_source`（§10.2）。
- `hzb_levels` 是唯一 extent 依赖项。推荐同样进 `scene_tables_pass`，`life = output_size`、size 由
  `descriptions(context)` 用 `context.render_extent` 算出（`render_resource::instances::resource_context` 有该字段，
  `instances.cppm:34-40`；`lifetime::output_size` 在 manager 的合法值里，`instances.cppm:373-379`）。
  **实现前必须确认一件事**：manager 对 `output_size` 的 **buffer** 家族在 render extent 变化时确实会重建
  （image 的 output_size 行为是已知的；buffer 没有先例）。若不会，退化方案是：runtime 自己在
  `record_main_drawcalls` 的冻结段里 `ensure_hzb_resources()`（extent 变 → `wait_idle()` → 重建 → 重写两个
  heap 描述符；`runtime.constructor.cppm:1481-1487` 已有 "outputs changed ⇒ wait_idle" 的同类先例），
  并把该 buffer 从 `scene_tables_pass` 里拿出去。
- `hzb_header` 与 `hzb_levels` 的 descriptor 写在 `publish_hzb_bindings()`（constructor 内，或 resize 路径里），
  用 `write_heap_buffer`（`runtime.frames.cppm:121-123`）——目标是"地址 + 长度"，没有第二套写法。
- resize 作废规则：`hzb_valid[0..1] = false`，next 两帧只 build 不 cull（§9.3）。

### 5.3 槽位镜像与测试

- `render_layout.cppm` 的 `struct heap_slots` 加 6 个 `static constexpr uint32_t`；`heap_slot_constants.slang`
  加 6 个同名 `static const uint heap_slots_* = heap_slot_base + N*u;`，再按 C17 的既有约定加
  `#define heap_hzb_levels_slot (heap_slots_hzb_levels + heap_frame_slot * hzb_slot_words)` 之类的组合宏是
  **不需要**的（lane 步长是运行期量，不是槽位），shader 里用 `heap_slots_* + heap_frame_slot` 会错——统一写成
  `heap_at<...>(heap_slots_hzb_items)` 后按 `frame_slot * stride + index` 行内索引（与
  `culled_meshlet_at` 的写法一致，`mesh_geometry.slang:148-151`）。**这条必须写进槽位注释**，否则下一个改的人
  会以为"per-frame 表 = `+ heap_frame_slot`"，而这里是一个描述符 + 内部 lane。
- `test_render_resources`（`source/tests/test_render_resources.cpp:595-786`）自动覆盖名字集合与数值，不需要改测试；
  只在追加槽位的那一个提交里跑它。
- `test_docs` 第 5 条只检查 `docs/*.md` 里的 `heap_slots_<name>`（`test_docs.cpp:411-423`），
  本文在 `docs/superpowers/specs/` 下、不在 Doxyfile INPUT 里，不受该条约束；但阶段 D 若要往
  `docs/visibility_buffer.md`/`docs/mesh_shaders.md` 写 HZB 章节，必须**先**加槽位再做那件事。

---

## 6. 保守测试数学（最易错的部分；原型就是死在这里）

### 6.1 输入与不变量

一次测试的输入：

- 对象空间球：`(c_obj, r_obj)`（`meshlet` 记录，C1）；
- 该 item 的模型矩阵 `M`（`draw_item.model`，C7）；
- **旧** view/proj `(V, P)` 与 `(W, H)`：来自 `hzb_header` lane s（§4.1）——它们就是**生成这份 HZB 的那一帧**
  使用的矩阵（同一 build 写出，不可分）。
- `near/far`：`P` 的透视项直接隐含（见 6.4 的公式），不单独传。

不变量（本设计必须永远成立）：**"被剔除" ⇒ 该 meshlet 的每个三角形在这些像素上与 `[0,1]` 深度里已存在的
几何相比，整片都在更远处（保守意义）**。方向永远是"宁可多画"。

### 6.2 第 1 步：世界空间球

```text
c_w = mul(float4(c_obj, 1), M)                      // 与 visibility.slang:110 同约定（row_major + mul(vector, matrix)）
r_w = r_obj * matrix_max_axis_scale(M)              // 复用 mesh_geometry.slang:213-222 的行和上界
```

`matrix_max_axis_scale` 是行和上界（operator-norm 上界），对含 shear/非均匀缩放的矩阵都保守；这是既有 helper，
不要另写"最大列长"版本（subtilis 设计 §11.5 已经记录过这个错误）。

### 6.3 第 2 步：先做便宜的早退（fail-open = 保留）

```text
w = clip.w = P * V * c_w 的 w
if (w <= 1e-4)                         保留   // 中心在眼后/眼平面附近
d = length(c_w - camera_pos)                   // camera_pos 来自 header（§4.1）
if (d <= r_w)                          保留   // 眼球在球内，任何投影界都失效
d_near = w - r_w                               // 球在眼空间里最近点的深度（w = -z_eye，RH 投影）
if (d_near <= near_plane * 1.001)      保留   // 跨/贴近平面的球必须画（6.4 的界在这里失效）
```

`near_plane` 不需要额外传：从 header 的 `proj` 直接推出 `near = P[3][2] / P[2][2]`
（与 `subtilis_selection.cppm:44-53` 的推导一致；那是同一类 `perspectiveRH_ZO` 矩阵）。

### 6.4 第 3 步：近深度界（原型错误 #1）

NDC z 是眼空间深度 `d_e` 的**单调**函数，所以球上所有点的 NDC z 的下界 = 在 `d_near = w - r_w` 处的值：

```text
z_ndc(d)   = (P[2][2] * (-d) + P[2][3]) / d          // 透视 RH_ZO：d=near → 0，d=far → 1
z_near_ndc = z_ndc(d_near)
```

- 原型用的是 `ndc_z - world_radius / clip.w`（`occlusion-prototype.patch:117`）。把两者相减可见：
  真实偏移 `Δ = A·near·r / (w(w−r))`（`A = far/(far−near)`），原型偏移 `r/w`；**当 `w − r < A·near`
  （球贴近近平面）时原型偏移偏小**，即"近深度界"比真实值更远，可能把可见 meshlet 判成被遮挡。
  默认 near = 0.1 时，这覆盖相机脚下/墙根的几何——这正是 near 街道视角 0.314% 像素变化的来源。
- 因此 6.3 的 `d_near` 保护与这里是**同一个修复的两半**：先拒绝跨近平面的球，再用单调公式算界；两者缺一不可。
- 同样的界还要用于"球整个在 far 之外"的情况：`d_near >= far` ⇒ 在旧像里不可见 ⇒ 保留（保守；不需要专门判断）。

### 6.5 第 4 步：屏幕矩形（原型错误 #2）

原型用 `radius_px = world_radius * focal / clip.w` 画一个"以投影中心为圆心"的圆（`occlusion-prototype.patch:122-129`、
`cluster_occlusion_probe.py:121-129`）。透视下，**离轴**大球在屏幕上的 NDC 足迹会被非线性放大，
`r·f/w` 会低估矩形；矩形太小 → 取 max 的 tile 覆盖不全 → 可能漏掉"更近的遮挡物"→ 非保守。
本设计改用**世界包围盒 8 角的投影包围盒**（球 ⊂ 世界 AABB ⊂ 8 角凸包，透视把凸包映成凸包，取 8 个角的 NDC
min/max 就是保守包围）：

```text
min_ndc = (+inf, +inf); max_ndc = (-inf, -inf)
for each corner k of (c_w ± r_w on each axis):
    clip_k = mul(float4(corner_k, 1), VP)
    if (clip_k.w <= 1e-4)  保留（有角在眼后：不做这次剔除）
    更新 min_ndc/max_ndc = min/max(x/w, y/w)
// Vulkan 的 y 约定：本渲染器 proj[1][1] < 0（y 向下），所以像素映射与 visibility.cppm:320-328 一致
x0 = floor((min_ndc.x * 0.5 + 0.5) * W);  x1 = ceil((max_ndc.x * 0.5 + 0.5) * W)
y0 = floor((min_ndc.y * 0.5 + 0.5) * H);  y1 = ceil((max_ndc.y * 0.5 + 0.5) * H)
x0 = max(0, x0); y0 = max(0, y0); x1 = min(W-1, x1); y1 = min(H-1, y1)
if (x1 < x0 || y1 < y0)                保留   // 旧视锥外：旧像里不可能被遮挡
```

- 八次矩阵乘法是这条测试里最贵的部分，但只在 6.3 早退没命中时才做；4.6 万候选 near 相机下是 GPU 上可忽略的量。
- 也可以换成"切线锥"紧包围，但**不要**换成以中心为圆心的圆：那是这个 bug 的另一种写法。

### 6.6 第 5 步：HZB 取样与判定

```text
span = max(x1 - x0, y1 - y0, 1)
L = clamp(floor(log2(span)) - 1, 0, level_count - 1)     // 目标：矩形在 level L 上 ≤ 4×4 个 texel
scale = 1 << L
tx0 = x0 >> L;  ty0 = y0 >> L
tx1 = x1 >> L;  ty1 = y1 >> L                            // 闭区间，见 4.1 的 ceil 金字塔
far = 0
for ty in ty0..ty1: for tx in tx0..tx1: far = max(far, level[L][ty * w_L + tx])   // ≤ 16 次读取
if (far >= 1.0)                         保留           // 该片没有任何几何（清屏值 = far plane）
if (z_near_ndc > far + 1e-5)            剔除
else                                    保留
```

- `1e-5` 是 NDC 深度 epsilon（D32 在 [0,1] 上的可视差远大于它；原型用的是 `1e-4`，本设计取更小的值只因为
  现在的界是精确公式，而不是"biased small"的近似——**这个 epsilon 只影响保守侧**，宁小勿大，
  但也不能为 0（比较两侧都有浮点误差）。
- 取 level 的公式与 tile 覆盖方向：矩形在 level L 上的闭区间 `[x0>>L, x1>>L]` 覆盖所有可能被球覆盖的像素
  （ceil 金字塔的 tile 含 `[t*2^L, (t+1)*2^L)`）；这是"保守覆盖"，与原型 `cluster_occlusion_probe.py:151-158` 同方向。
- 全部边界情况（`w<=0`、眼在球内、跨近平面、旧视锥外、无几何）都 **保留**：错误方向只能是"多画一个 meshlet"。

### 6.7 这张测试的 CPU reference

`source/engine/visibility/hzb.cppm`（新纯模块，无 RHI 依赖）用 double 重写 6.2–6.6，输入 `(view, proj, W, H,
near/far 由 proj 推出, HZB pyramid, 球, model)`，输出 `occluded/kept`。GPU shader 与它**不是"相似"而是同一组公式**；
单测（§10.1）用随机球 × 随机相机检查两条性质：
1. **保守性**：CPU reference 判"剔除"时，用同一份深度做逐像素的"球投影矩形内每个像素深度都 < 球最近深度"的
   暴力检查也必须成立（在矩形内把球离散成三角形/采样点，或者用 AABB 的逐像素深度下界）；反过来 GPU 版
   （通过 probe 抓表）不允许比 CPU reference 剔除得更多。
2. **边界等价**：近平面/离轴/球内眼三种构造样本上 CPU reference 必须返回"保留"。

---

## 7. Shader 伪代码

### 7.1 `source/shaders/scene/hzb_build.slang`（新，compute）

```text
// push: { uint level; } ++ frame_slot/image_index/extra（pass 端点自动追加，C14）
// 入口 = main；numthreads(8,8,1)
static const uint hzb_max_levels = 16u;
uint pyramid_words(uint w, uint h) { /* 逐级累加 ceil(w/2^L)*ceil(h/2^L) */ }

[shader("compute")]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint slot = push.frame_slot;
    const uint level = push.level;
    uint W, H, levels;
    Texture2D<float> depth = heap_at<Texture2D<float>>(heap_slots_gbuffer_depth + heap_image_index);
    depth.GetDimensions(0u, W, H, levels);                       // 深度纹理的尺寸 = 本帧 extent
    const uint total = pyramid_words(W, H);
    const uint lane_base = slot * total;                          // hzb_levels 的 lane 起点（word）
    RWStructuredBuffer<float> pyramid = heap_at<RWStructuredBuffer<float>>(heap_slots_hzb_levels);
    RWStructuredBuffer<HzbHeader> header = heap_at<RWStructuredBuffer<HzbHeader>>(heap_slots_hzb_header);
    uint w_L, h_L; level_size(W, H, level, w_L, h_L);             // 与 pyramid_words 同式
    if (tid.x >= w_L || tid.y >= h_L) return;
    float value;
    if (level == 0u) {
        value = depth.Load(int3(tid.x, tid.y, 0));   // level 0 = 逐像素拷贝（与 pyramid 同分辨率）
    } else {
        uint pw, ph; level_size(W, H, level - 1, pw, ph);
        value = max over 2x2 of pyramid[lane_base + level_offset(level-1) + clamp(...) ...]
    }
    pyramid[lane_base + level_offset(level) + tid.y * w_L + tid.x] = value;
    if (level == 0u && tid.x == 0u && tid.y == 0u) {             // header 由 build 写出（§4.1、C21）
        CameraUBO cam = camera_at(heap_camera_slot);             // 本帧相机 = 本帧深度的相机
        header[slot] = HzbHeader{ magic, W, H, level_count, cam.view, cam.proj, cam.camera_pos };
    }
}
```

注：level 0 的实现取决于"depth 图像是否与 pyramid 同分辨率"——本设计取同分辨率拷贝，所以 level 0 是
"1 thread per pixel + 直接 `Load`"，不是 2×2。若后续改成半分辨率 HZB（§12 Q2），只改 level 0 的取样。

### 7.2 `source/shaders/scene/hzb_cull.slang`（新，compute）

```text
// push: { uint expected_w; uint expected_h; uint mode; } ++ 3 lanes；numthreads(64,1,1)
[shader("compute")]
void main(uint3 gid : SV_GroupID, uint li : SV_GroupThreadID) {
    const uint slot = push.frame_slot;
    const uint item = gid.x;
    const HzbItem src = heap_at<StructuredBuffer<HzbItem>>(heap_slots_hzb_items)[slot * 4096u + item];
    if (src.candidate_count == 0u) return;
    const HzbHeader hdr = heap_at<StructuredBuffer<HzbHeader>>(heap_slots_hzb_header)[slot];
    // ---- 整帧门控的 GPU 侧兜底：header 不是本 extent（resize 竞态）→ 本 item 全部保留 ----
    // keep_all(item) 必须自己写完：把 candidate_count 条候选逐条拷进 survivors、pass1_count=候选数、
    // 命令字={候选数,1,1}——不能直接 return，因为 raster 已经按 hzb_active_frame 决定读 survivors 表了。
    if (hdr.magic != HZB_MAGIC || hdr.width != push.expected_w || hdr.height != push.expected_h) { keep_all(item); return; }
    const DrawItem row = heap_at<StructuredBuffer<VisibilityDrawItem>>(heap_slots_visibility_items)[slot * 4096u + item];
    // ---- 逐 chunk 处理：read → group barrier → 确定性前缀 → write，顺序压紧 ----
    uint running = 0u;                                    // 本 item 已压紧的 survivor 数
    __shared uint chunk_total;                            // 本 chunk 的 survivor 数
    for (uint base = 0u; base < src.candidate_count; base += 64u) {
        const uint i = base + li;
        MeshletRecord rec;
        bool in_range = i < src.candidate_count;
        if (in_range) rec = src.source == 0 ? meshlet_at(src.base + i)
                                            : culled_meshlet_at(slot, src.base + i);
        bool keep = in_range && !hzb_occluded(rec, row.model, hdr, ...);   // §6 的测试
        // flags：每个候选写一次，无需清零/原子（u32/候选，见 §5.1）
        if (in_range) heap_at<RWStructuredBuffer<uint>>(heap_slots_hzb_results)[flags_offset(slot, src.base + i)] = keep ? 0u : 1u;
        GroupMemoryBarrierWithGroupSync();
        if (li == 0u) chunk_total = 0u;
        GroupMemoryBarrierWithGroupSync();
        // 确定性压紧：wave 内 ballot+prefix；跨 wave 用 shared 计数（16 个 wave 的追加顺序不参与输出顺序）
        const uint wave_keep = WaveActiveCountBits(keep);
        const uint wave_prefix = WavePrefixCountBits(keep);
        uint wave_base = (WaveGetLaneIndex() == 0u) ? AtomicAdd(chunk_total, wave_keep) : 0u;
        wave_base = WaveReadLaneFirst(wave_base);
        if (keep) survivors[slot * 65536u + src.base + running + wave_base + wave_prefix] = rec;
        GroupMemoryBarrierWithGroupSync();
        running += chunk_total;
    }
    if (li == 0u) {
        results.pass1_count[slot * 4096u + item] = running;
        // hzb_commands 是 mesh_indirect_buffer 的 heap 别名，索引公式与宿主一致（§4.4）
        commands[slot * mesh_command_capacity + visibility_command_run + item] = { running, 1u, 1u };
        hzb_stat_add(...);                                   // candidates/culled/survivors，见 §10.3
    }
}
```

`hzb_occluded` 是 §6.2–6.6 的完整实现（新 include `source/shaders/common/hzb.slang`，CPU reference 与它共用
同一组常量：level 公式、epsilon、近平面保护系数）。

**恢复模式（阶段 C）**：`push.mode == 1` 时把候选过滤换成 `flags == 1`，HZB 用本帧 lane s（已是本帧深度），
输出写到 `meshlet_base + pass1_count[item]` 起、计数写 `pass2_count`、命令字写到
`hzb_recovery_command_run + item`。

### 7.3 `visibility.slang` delta（约 6 行）

见 §4.5。**另外**：`base_vertex` 的 flag 语义当前注释只写 bit0（`mesh_geometry.slang` 的 `MeshGeometryLanes`
注释、`visibility.slang:99` 附近），追加 bit1 时必须把两个 bit 的语义写在同一处注释里，且
`apply_meshlet_run`（`visibility.cppm:260-268`）改成接受一个 `meshlet_source` 而不是 bool。

### 7.4 `visibility_resolve.slang` delta（约 4 行）

见 §4.5；`hzb_survivor_at(frame_slot, index)` 与 `culled_meshlet_at` 同形（放在 `mesh_geometry.slang`，
紧挨 `culled_meshlet_at`），这样"表 swap"仍然是一个函数调用，不是第二套索引。

---

## 8. Pass 顺序、barrier、以及 render graph 还是手动录制

### 8.1 结论：全部进 render graph，不手动记跨 pass barrier

- 现有 visibility lane 已经是 graph 的两个 pass（raster lane 19 / resolve lane 20，
  `runtime.post_graph.cpp:46-47,574-694`），HZB 的 3 个 pass 同样声明进 graph：
  `hzb cull`（lane 21）、`hzb build`（lane 22）、阶段 C 的 `hzb recovery`（lane 23）+ 第二个 raster scope。
  `ios`/`passes` 数组从 21 扩到 24/25；lane 号是数组下标（碰撞是静默的，注释在
  `runtime.post_graph.cpp:33-45` 已记录），所以**只允许追加、不允许插入**。
- 为什么不用手动 recording：C12——手动 role-pair barrier 无法把 buffer 写暴露给 mesh 阶段；
  而 graph 的 `scoped_synchronization` 路径（C11）已经支持 `compute_shader→mesh_shader` 与
  `compute_shader→draw_indirect` 的 buffer transition。**这是本设计唯一被允许的跨 pass 同步形状。**
- pass 内部的 barrier（HZB build 的逐级 dispatch 之间）由 build pass 自己在 `record_content` 里
  `io.list->barrier(barrier_group{.buffers = ...})` 记录：同 buffer、COMPUTE→COMPUTE，RHI role-pair 足够。

### 8.2 帧 N 的完整顺序（阶段 B / C）

| # | pass / 动作 | 读 | 写 | barrier 来源 |
| --- | --- | --- | --- | --- |
| 0 | `prepare_visibility_items`（CPU，graph 之前） | — | item 表、host-culled lane、`hzb_items`、候选命令字 | 无（host 写，MAPPED/coherent，提交前可见） |
| 1 | `hzb cull`（compute） | `hzb_levels[s]`、`hzb_header[s]`、`hzb_items[s]`、`visibility_items[s]`、meshlet 表/culled lane、`hzb_results[s]`（stats RMW） | `hzb_survivors[s]`、`hzb_results[s]`、`mesh_indirect_commands[s]`(compute 写) | graph：cull 的写 → raster 的读 |
| 2 | `visibility raster #1`（graphics+mesh，清屏） | survivors（mesh）、命令字（draw-indirect）、相机/材料 | depth、vis id | graph |
| 3 | `hzb build`（compute，逐级） | depth 采样、相机 UBO | `hzb_header[s]`、`hzb_levels[s]` | graph：raster 的 depth 写 → build 的采样读；build 内部逐级 buffer barrier |
| 4 | `hzb recovery`（compute，阶段 C） | `hzb_levels[s]`（刚建好的）、`hzb_header[s]`、`hzb_items[s]`、`hzb_results[s].flags` | survivors 第二段、`hzb_results[s]`、命令字 run 4 | graph |
| 5 | `visibility raster #2`（graphics+mesh，**LOAD** depth/id） | 同上 | depth、vis id | graph：recovery 写 → raster 读；v0.5 的 depth/id 写 → 后续 pass |
| 6 | `visibility resolve` | vis id、depth、五张 G-buffer… | 五张 storage target | 现有声明不变（`render_graph/visibility.cppm:129-170`） |

- **depth 图像的状态来回**：raster #1 写（`depth_stencil_attachment`）→ build 采样读（`sampled` +
  `compute_shader`；graph 的 image 状态机已经在 visibility resolve 上证明过同一对，
  `render_graph/visibility.cppm:148-150`）→ raster #2 再写（attachment）。graph 会插两个 transition；
  下发给 `scoped_synchronization` 的 mask 由 `map_image`（`synchronization.cppm:105-153`）落成
  depth 阶段的 early/late fragment 测试 + compute 读。
- **跨帧（2 帧前写 → 本帧读）不加 barrier**：C9 的 frame-ring slot wait 已经建立提交间的依赖
  （TAA history 的跨帧读是同一先例，`runtime.post_graph.cpp:168-217`、`:1078`）；阶段 A 的 readback 探针
  是这条假设的直接证据。
- **host 写 → GPU 读**（item 表/culled lane/候选命令字）：MAPPED coherent 内存、写在提交前，
  `runtime.post_graph.cpp:348-349` 已经写明这个模式（"CPU writes were published for the completed slot
  before submission"），本设计沿用，不需要 barrier。

### 8.3 与 8.2 相邻的两条纪律

- cull pass 的 dispatch 是 `dispatch(item_count,1,1)`；`item_count = 0` 时 pass 不录制（与 raster 的空帧一致）。
- `hzb build` 只在 visibility lane 实际录制（admitted）的帧执行；geometry-lane 的回退帧**不建也不剔**
  （简单且不改变回退帧的任何字节）。

---

## 9. 失效、降级与门控

### 9.1 整帧冻结（"whole frame or nothing"，沿用 visibility admission 的形状）

`record_main_drawcalls`（`runtime.frames.cppm:870-882`）已经在冻结 opaque lane 的决定；HZB 在**同一个冻结段**
计算 `hzb_active_frame`（一个 bool + 一个原因枚举），本帧之后所有读者只用它，不再各自判断：

```text
hzb_active_frame =
     hzb_occlusion_switch                      // [render] hzb_occlusion / DEREN_HZB_OCCLUSION
  && visibility_lane_frame == visibility       // HZB 只服务 visibility lane
  && main_graph_frame
  && hzb_cull_pass 有 pipeline && hzb_build_pass 有 pipeline
  && hzb_valid[frame_slot]                      // 本 slot 的 HZB 是"这个 extent、这个 scene"下建的
  && hzb_scene_gate(frame_slot)                 // 阶段 B：视图/模型逐字节相同；阶段 C：可放宽为"运动量可接受"
  && hzb_resources_ready()                      // §5.2 的 buffers + 6 个 heap 描述符都写成功
```

任何一个条件失败 ⇒ `hzb_active_frame = false` ⇒ item 表/命令字/`meshlet_table`/`base_vertex` 全部走今天的老路径，
**零像素变化**、一行日志说明原因（每 session 一次，词汇跟 `describe(admission)` 同风格）。

### 9.2 阶段 B 的相机/场景门控（保守性的来源）

阈值 = **逐字节相等**，不是 epsilon：

- 当前帧 `current_ubo.view`、`current_ubo.proj`（unjittered 也存，但比较用实际矩阵）与保存的
  `hzb_source_view[frame_slot]`/`hzb_source_proj[frame_slot]` 完全相等（`std::memcmp`）。TAA 开时 proj 带
  jitter，逐帧必不相等 ⇒ 阶段 B 自动关闭（如实记录；阶段 C 用 footprint 外扩 1–2 px 打开）。
- 本帧可见 item 列表与保存的 `hzb_source_items[frame_slot]`（`{leaf*, model[16]}`）**逐项相等**（数量、顺序、指针、矩阵）。
  这一条覆盖"静态 admission 但节点矩阵在动"的情况（admission 只看 skin/morph/instanced，
  不看节点变换是否动画；`visibility.cppm:499-546`）。
- 保存成本：每帧每 slot 拷一次 item 的 model 列表（4096×64 B = 256 KB 上限，Bistro 2909 item ≈ 186 KB，
  `< 0.05 ms`），然后在阶段 B 的下一帧比较。两个 slot 各一份。
- **阶段 C 的相机运动策略**（不再承担正确性，只决定"第一遍预测值不值得做"）：由宿主计算
  `Δview`（平移长度 / 场景半径、旋转角度）与 `Δproj` 的最大分量差；超过阈值（初值：平移 > 5% 场景半径/帧
  或旋转 > 5°/帧）⇒ 本帧**跳过第一遍 cull**，按单遍全量画（等于今天的帧，无 recovery 开销）；阈值以下是
  待测参数，写在配置里而不是写死。这个门控和"丢几何"无关：recovery 无论门控开不开都保证正确，
  它只是为了不把 GPU 时间花在必然被 recovery 全部撤回的预测上。
- 阶段 C 把门控放宽成"只是预测质量"，不再承担正确性：`hzb_scene_gate` 可以退化成上面这个运动量阈值，
  **但阶段 B 的逐字节门控保留为 capture/调试开关**。

### 9.3 resize / 首帧 / HZB 重建

- 首两帧：`hzb_valid[s] = false` → 只 build 不 cull；从第 3 帧起可用（slot parity 决定）。
- resize（extent 变化）：重建 `hzb_levels`（§5.2），两个 slot 的 `hzb_valid = false`，重新积累两帧。
- 场景内容变化（item 列表变化）由 9.2 的逐项比较覆盖（阶段 B）；阶段 C 允许，recovery 兜底。
- `hzb build` 失败（pipeline 缺 / header 内容不对）：本帧的 cull 已经完成（它读的是上一次成功 build 的历史 HZB，
  与本次 build 无关），所以**本帧像素不受影响**；只需把 `hzb_valid[frame_slot] = false`，此后直到下一次成功 build
  为止该 slot 不再 cull。header 的 magic/extent 兜底（§7.2 的 `keep_all`）保证 GPU 侧也永远不会使用不匹配的 HZB。

### 9.4 容量与溢出

- item 数 > 4096：visibility admission 已经整帧回退（C5、`visibility.cppm:539-544`），HZB 不需要第二套溢出语义。
- 单 item 候选数 > 4096：同样走 admission 的 meshlet overflow（C5）。
- 候选数 > `meshlet_capacity`（65536）：host 侧 `meshlet_culled_write`/`check_reservation` 已经拒绝并整 item 回退
  （C4），`hzb_items` 写入前再断言一次，失败 ⇒ 该帧 `hzb_active_frame = false`。
- `hzb_results` 的 flags 是绝对 `meshlet_base + i` 寻址，`meshlet_base + count ≤ meshlet_capacity` 由 C4 的既有检查保证。

### 9.5 far 视图的测量驱动门控

far 的遮挡率 0.41%：HZB 收益接近 0，但 build + cull 的成本是每帧固定的。策略：

- `hzb_results.stats` 是累计值；宿主在 shutdown/日志窗口读回（一次 wait_idle，模式同 `log_meshlet_stats`，
  `runtime.constructor.cppm:513-560`）。
- 运行期不读回。用**上一帧的 GPU 计数**（不读，改由 cull 的 GPU 侧判断）不可行；因此 far 的门控只能靠
  **相机/场景的变化量**或**最近窗口的剔除率**（后者需要回读，代价一次/60 帧，可在 capture 里允许、发布版不做）。
- 第一版实现：far 也照常跑（overhead 预算 0.2 ms），只有在测量矩阵证明 far 有可见退化时才加
  "每 N 帧一次 stats 回读 + 剔除率 < 2% 时停 M 帧"的策略（§12 Q4）。

---

## 10. 测试与验收

### 10.1 纯 CPU 测试（CI 可跑，无设备）

新增 `source/tests/test_hzb.cpp`，登记进 `VR_TEST_TARGETS` + `.github/workflows/ci.yml`（C19）。
覆盖 `source/engine/visibility/hzb.cppm`：

1. **pyramid 布局**：`pyramid_words/level_size` 在各种 extent（偶数/奇数/1×N/N×1/1280×720/4K）上与暴力
   逐级 `ceil` 一致；level 偏移连续、无重叠、总长等于 size 公式。
2. **保守测试的 CPU reference**：随机球 × 随机相机 × 合成深度（平面、盒、洞、单像素几何）：
   - 判"剔除"时，HZB 覆盖的每个 tile 的 max 深度都严格小于球的最近 NDC 深度（性质检查，不是快照）；
   - 近平面保护、眼在球内、旧视锥外、无几何、离轴大球 五种构造必须"保留"；
   - **反向 RED**：把 6.4 的公式换回原型的 `ndc_z - r/w`、或把 6.5 换回 `radius_px`，测试必须红
     （这是"这个测试真的在测保守性"的证据，同 subtilis 的 RED→GREEN 习惯）。
3. **压紧等价**：输入随机候选 + 随机 keep 掩码，CPU 压紧的输出与 GPU 侧的确定性规则（按 index 升序）逐项相等；
   总数 ≤ 4096 的断言。
4. **门控谓词**：阶段 B 的 `hzb_scene_gate` 在这种输入下的结果（矩阵差 1 ulp、item 顺序变、一个 matrix 变）必须
   逐项验证——这些是纯函数，放在同一个模块里。
5. **pass 录制**（可选，沿用 `test_visibility_raster_pass.cpp` 的 probe backend 形状）：cull pass 的
   `record_content` 发且只发一个 `dispatch(item_count,1,1)`、且**不开 rendering scope**；build pass 的 dispatch
   数 = `Σ ceil(w_L/8)*ceil(h_L/8)`，相邻 dispatch 之间恰有一个 buffer barrier。

### 10.2 GPU 读回探针（阶段 A 的证据）

复用 `DEREN_DEPTH_PROBE_CAPTURE` 的形状（`runtime.post_graph.cpp:1129-1160`：截图帧把 depth 拷到
readback buffer，`wait_idle` 后 host 读）：

- 新环境变量 `DEREN_HZB_PROBE_CAPTURE=<前缀>`：在截图帧把 `hzb_levels[s]`（本帧 lane）与 `hzb_header[s]`
  用 `command_buffer::copy_buffer`（`rhi.api_core.cppm:1694`）拷到 `readback_coherent` buffer 并落盘。
  同一 dump 里附上**本帧 `current_ubo.view/proj` 的副本**，这样脚本可以自洽地验证
  "header 里的矩阵 == 同一帧的相机"，不需要第二个探针。buffer 需要 `buffer_flag::transfer_source`
  （§5.2 已在描述里）。
- 配套脚本 `scripts/measure/hzb_verify.py`：读 `probe-depth-*.bin`（已有 dump 格式，见
  `cluster_occlusion_probe.py:59-63`）+ HZB dump，独立用 numpy 重建 max pyramid，**逐 word 比较**；
  再比较 header 的 view/proj 与 dump 里的当前帧相机矩阵。
- 通过标准：pyramid 0 word 差异；header 的 view/proj 与该帧相机逐字节一致（`std::memcmp` 的日志证据）。

**这是阶段 A 的 RED→GREEN**：先让 dump 与 numpy 参考不一致（正常情况下第一次实现一定会在 ceil/偏移上错），
定位到公式，再对齐；同时这一帧的**截图必须与 HZB OFF 逐像素相同**（build pass 只写自己的 buffer）。

### 10.3 引擎内证据（日志行）

HZB 的计数**写自己的 `hzb_results.stats`，不挤进 `meshlet_stats`**：后者是"mesh entry 每个 workgroup 加一次"的
session 累计语义，而 HZB 是"compute pass 每个 item 累加一次候选/剔除/保留"的语义；两种口径混在一张表里会让
"被剔除了多少 dispatch"这一个问题有两个分母。`hzb_results.stats` 是 host-visible，宿主在 shutdown 读一次
（模式同 `log_meshlet_stats`，`runtime.constructor.cppm:513-560`），日志行：

- `hzb cull (visibility lane): {candidates} 条候选 / {culled} 条被遮 / {survivors} 条保留（{pct}% 被遮），
  第二遍 recovery {r_candidates}/{r_survivors}；本帧 dispatch 从 {old} 降到 {new} 组`
- `hzb build: {levels} 级 / {words} word / {ms:.3f} ms（GPU）`（时间从 GPU mark 读，见 §10.5）
- 首帧路线行（一次/session）：`hzb: 数据路径 = {cull|off}，门控原因 = {reason}`（`describe` 风格）。

### 10.4 capture A/B 验收（同机、同会话、交错 ≥3 次）

基线命令（与原型/探针同一套 harness；配置需 `subtilis = true` 才能复现原型的 38,864 候选集，
`subtilis = false` 则候选集是 LOD0 全量 43,930）：

```text
build-release-dyn-clang64\deren.exe --config build-release-dyn-clang64\bistro-probe\hzb-off.toml ^
    --capture-frames 130 --capture-camera 35,8,42,0,-88,0          # near
build-release-dyn-clang64\deren.exe --config build-release-dyn-clang64\bistro-probe\hzb-on.toml ^
    --capture-frames 130 --capture-camera 35,8,42,0,-88,0          # near
（far 用 --capture-camera 35,18,140,0,-83.071,0；ON/OFF 交错 3 次）
```

（`hzb-off.toml`/`hzb-on.toml` 由 `perf-off-noval.toml` 复制，唯一差异是 `hzb_occlusion` 与 `subtilis`；
GUI 子系统 exe 的启动/收尾方式按既有 harness：`&` 启动、轮询最新 `screenshot_*.png`/进程退出、拷 `debug.log`。）

| 门 | 判据 |
| --- | --- |
| A1 像素不变 | HZB build 开启（cull 关闭）的截图与 OFF **逐像素相同**（`scripts/measure/diff.py` 或 `compare_render_hashes`） |
| A2 pyramid 正确 | §10.2 的 0 word 差异 + header 矩阵一致 |
| B1 near 像素 | 静止相机门控下，HZB ON 与 OFF **逐像素相同**（这是保守测试的强验收，不是"容许 0.3%"） |
| B2 near GPU | 同会话 OFF/ON 中位数：visibility raster ≥ −25%，GPU 帧 ≥ −20% |
| B3 far | 0 条剔除（日志），像素逐像素相同，GPU 帧时间不增加超过 0.2 ms |
| B4 CPU | `record_visibility_items` 的微秒计时 + `scene`/`post` 两相之和相对 OFF 的增量 ≤ 0.2 ms（原型在 `post` 相 +11.2 ms；prepare 拆出去后落在 `scene` 相） |
| C1 recovery | 相机运动 capture（沿街推轨）与 OFF 的差异像素 ≤ 0.05%，且**不得有"丢失几何"的像素**（差异只能出现在等深 tie：用深度探针验证差异像素的深度仍在 ±1 ulp） |
| D1 矩阵 | 3×3 OFF/ON 交错，中位数 + P95；报告新 pass 的两行 GPU 时间与 cull 计数 |

**far 必须逐像素相同、near 静止必须逐像素相同**是本设计相对原型的质量跃迁；如果 B1 出现任何差异，按 §6 的
三处保守性逐条回查（先用 10.1 的 CPU reference 对拍，再去 GPU）。

### 10.5 GPU 计时行

`gpu_mark_id`（`runtime.declarations.cppm:658` 的枚举与紧随其后的 label 表）按**位置**切 interval：在枚举与
label 表里插入 `hzb_cull_end`（在 `visibility_raster_end` 之前）与 `hzb_build_end`（在 `visibility_raster_end`
之后），阶段 C 加 `hzb_recovery_end`。报告必须能读出三行：`hzb cull`、`hzb build`、`hzb recovery`，并且
`visibility raster` 一行保持为"只含 raster"（不把 HZB 成本算进 raster，否则 A/B 无法解释）。

### 10.6 现有测试的回归

- `ctest` 全绿（当前 57/57，以实际数字为准）；`test_render_resources`（槽位镜像）、`test_visibility_buffer`
  （id/记录布局，`meshlet_source` 追加值不改布局）、`test_shader_sources`（新 shader 四处登记）、
  `test_docs`（CI target 列表）、`test_app_config`（新配置键的 read-back）。
- ASan/UBSan 变体与 `.github/workflows/ci.yml` 的 target 列表同步（C19）。

---

## 11. 分阶段实施计划

### 阶段 A：堆槽位 + CPU reference + HZB build pass + 读回验证（只写自己的 buffer，零像素变化）

| 项 | 文件 |
| --- | --- |
| 6 个新槽位 + 镜像 + 注释（1308→1313） | `source/engine/render_layout/render_layout.cppm`、`source/shaders/common/heap_slot_constants.slang` |
| pure CPU 模块（布局公式 + 保守测试 reference + 门控谓词 + 压缩 reference） | `source/engine/visibility/hzb.cppm`（新；新模块要加进 `CMakeLists.txt` 的 source 列表，与 `visibility.cppm` 同段） |
| 单测（先 RED 后 GREEN，§10.1） | `source/tests/test_hzb.cpp`（新）、`CMakeLists.txt:1365`、`.github/workflows/ci.yml:351-354` |
| HZB buffer 创建/描述符（scene_tables_pass 扩展或 runtime 自管，§5.2） | `source/engine/pass/scene_tables.cppm`、`source/engine/runtime/runtime.constructor.cppm`、`runtime.declarations.cppm` |
| build shader + include | `source/shaders/scene/hzb_build.slang`（新）、`source/shaders/common/hzb.slang`（新） |
| pipeline builder | `source/engine/pipelines/pipelines.cppm`（`build_hzb_build`） |
| build pass | `source/engine/pass/hzb_build.cppm/.cpp`（新） |
| pass 接线（graph 数组扩容 + 在 raster 之后追加 build lane） | `source/engine/render_graph/hzb.cppm`（新，或并入 `visibility.cppm`）、`source/engine/runtime/runtime.post_graph.cpp:46-47,574-694` |
| shader 四处登记 | `CMakeLists.txt`（`VR_SLANG_SOURCES`）、`source/shaders/compile_shaders.ps1`/`.sh`、`source/app/chores.cpp` |
| 读回探针 + 脚本 | `source/engine/runtime/runtime.post_graph.cpp`（probe 段）、`scripts/measure/hzb_verify.py`（新） |
| 配置键（默认 false） | `source/application_configuration/application_configuration.cppm:271-284`、`source/application_configuration/application_configuration.cpp:267-272`、`source/app/main.cpp:630-634`、`config.example.toml:394-408`、`scripts/make_config.py:186-187,312-317,455-456`、`source/tests/fixtures/config_full.toml:41`/`config_generated_defaults.toml:40`、`source/tests/test_app_config.cpp:59-60,94-96` |

**验收 gate A**：A1（像素不变）+ A2（0 word 差异）+ CPU 单测全绿。**回滚**：只删/停用 build lane 与 HZB 资源，
不影响任何现有帧（build 只写自己的 buffer）。

### 阶段 B：cull pass + GPU 写计数 + survivor 消费（阶段 B 门控下静态相机）

| 项 | 文件 |
| --- | --- |
| `record_visibility_items` 拆成 prepare（phase 1-3 + 表写入）与 record（phase 4） | `source/engine/runtime/runtime.cpp:1697-1934`、`runtime.declarations.cppm`（plan 成员化）/`runtime.frames.cppm:870-882`（冻结段调用 prepare） |
| `hzb_items` 写入 + `hzb_commands` 别名（`device_address`） | `runtime.cpp`、`pass/scene_tables.cppm:55`、`runtime.constructor.cppm:1734-1748` |
| cull pass + shader | `source/engine/pass/hzb_cull.cppm/.cpp`（新）、`source/shaders/scene/hzb_cull.slang`（新）、`pipelines.cppm`（`build_hzb_cull`） |
| 消费端：mesh stage/`apply_meshlet_run`/`meshlet_source`/resolve 分支 | `source/shaders/scene/visibility.slang:99-101`、`source/shaders/scene/visibility_resolve.slang:252-258`、`source/engine/visibility/visibility.cppm:130-132,152,260-268`、`source/shaders/geometry/mesh_geometry.slang`（`hzb_survivor_at`） |
| graph 声明（cull 的写 → raster 的读；commands 的 compute 写 → draw-indirect 读） | `source/engine/render_graph/visibility.cppm:19-73` 或 `render_graph/hzb.cppm`、`runtime.post_graph.cpp:435-458`（shared imports 增加 hzb 表） |
| GPU mark 行 | `runtime.declarations.cppm:658`（`gpu_mark_id` enum + 紧随的 label 表）、`runtime.post_graph.cpp`（mark 调用） |
| 门控（§9.1/§9.2）+ 日志行 | `runtime.frames.cppm:870-882`、`runtime.cpp`、`runtime.declarations.cppm`（`hzb_active_frame`/`hzb_valid`/快照） |

**验收 gate B**：B1–B4。**回滚**：`hzb_occlusion=false` 时与阶段 A 后完全一致；阶段 B 的提交可以单独 revert，
prepare/record 拆分在关闭时行为不变（RED 证明：拆分后先不接 cull，帧字节不变）。

### 阶段 C：两遍 recovery（运动相机）

| 项 | 文件 |
| --- | --- |
| 命令容量 3→4 run + `hzb_recovery_command_run` | `runtime.declarations.cppm:2002,2047`、`runtime.cpp:1915`、`scene_tables.cppm:43` |
| cull shader recovery 模式 + flags 枚举 + `pass2_count` | `hzb_cull.slang`、`hzb.cppm`、`hzb_results` 布局（§5.1 已预留） |
| 第二个 raster pass（LOAD 而非 CLEAR；`pad` lane 当 pass 标记） | `source/engine/pass/visibility_raster.cppm/.cpp`（新增一个"load 模式"的实例或构造参数）、`runtime.post_graph.cpp` lane 23/24 |
| mesh stage 偏移（`pass1_count`） | `visibility.slang` |
| 门控放宽 + 日志 | `runtime.frames.cppm`、`runtime.cpp` |
| 运动 capture 脚本/矩阵 | `scripts/gates/capture.ps1`（如无现成推轨相机，加一个固定相机序列） |

**验收 gate C**：C1 + 回归 B1–B4。**回滚**：`hzb_recovery=false`（配置）退回阶段 B 的门控语义。

### 阶段 D：性能/质量矩阵与文档

- 3×3 OFF/ON far/near 交错矩阵（README.md 的方法论），GPU/CPU/墙钟三种窗口；
- 更新 `docs/reviews/2026-10-20-bistro-performance/README.md` 的"M3"一节（引用本设计 + 实测）；
- 若数据满足预算，讨论默认开启；否则保持默认关闭并记录原因；
- 文档：`docs/visibility_buffer.md`/`docs/mesh_shaders.md` 增加 HZB 章节（**先**把槽位写进文档的检查范围，
  见 §5.3 的 test_docs 第 5 条）。

### 11.1 风险表

| 风险 | 现状/对策 |
| --- | --- |
| 保守测试仍有非保守角落（近平面/离轴/float 边界） | §6 的三处修复 + §10.1 的 RED 单测 + B1 逐像素验收；任何差异按"先 CPU reference、再 GPU"回查 |
| 两遍顺序改变等深 tie 像素 | 阶段 C 的 C1 门槛；`LESS_OR_EQUAL` 的 tie 只影响共面/重复几何；可用 depth probe 证明差异像素深度一致 |
| 历史 HZB 在快速运动时剔除率低、recovery 发出大量几何 | 阶段 C 测"recovery 发出比例"；若 > 15% 候选，加回退门控（大位移帧直接跳过第一遍 cull） |
| `hzb_levels` 的 resize/manager 行为（§5.2 的未确认项） | 实现前先看 `render_resource::instances` 对 buffer/output_size 的 rebuild 逻辑；不确定性用 runtime 自管 + `wait_idle` 兜底 |
| 命令字被 GPU 写的时序 | 同 slot 上一次提交已被 frame ring 等到；host 写先于提交、GPU 写后于执行；barrier 由 graph 的 compute→draw-indirect 声明保证 |
| 新 pass 增加录制成本 | 3 个 pass、每帧一次 dispatch 级（build 是 ~11 次 dispatch），远低于原型的 11.2 ms；D1 报告实际值 |
| 质量门禁（check_render.ps1）引用帧变化 | HZB 默认关闭，OFF 参考不变；ON 场景按 §10.4 单独验收，不改冻结参考 |

---

## 12. 未决问题（实现前需要 owner/测量回答）

0. **已核实、不是未决（写在这里是为了不再有人重问一遍）**：深度图像在录制点是**可读的**——per-swapchain-image
   的采样描述符 `heap_slots::gbuffer_depth + image_index` 已经存在且每帧重写（C8），`depth_attachment → shader_read`
   的 image 状态对也已经在 visibility resolve 的声明里跑通（§8.2）。HZB build 复用这条路径，不需要新的 image 槽位、
   也不需要额外的 image flag；不确定的只是"同一个 pass 里逐级 dispatch 的 buffer barrier"这一形状，
   而它是 COMPUTE→COMPUTE 的既有 role-pair（§8.1）。
1. **`[render] hzb_occlusion` 的发布策略**：阶段 B 的"逐字节门控"是只作为 capture/调试能力（默认关闭、
   文档如实写"仅静止相机保真"），还是必须与阶段 C 一起才能进入任何默认配置？本文建议前者、后者作为 D 的决策。
2. **HZB 分辨率**：本设计用全分辨率 level 0（与离线探针一致，CPU reference 可直接对拍）。半分辨率 HZB
   （level 0 = 2×2 max）能把 `hzb_levels` 内存与 build 成本降到约 1/3，代价是最小 footprint 变成 2×2 px、
   CPU reference 要多一层语义。是否值得需要阶段 A 的内存/时间实测；先按全分辨率落地。
3. **`hzb_levels` 的 buffer/output_size 生命周期**（§5.2 的唯一未确认机制）：manager 是否对 buffer 家族在
   render extent 变化时重建？若否，runtime 自管（`wait_idle` 一次/ resize）是兜底形状。
4. **far 视图的自适应门控**是否需要（§9.5）：需要阶段 D 的 far 矩阵证明 overhead 真的出现才做；
   回读 stats 会引入一次/60 帧的 GPU→CPU 同步，与"无 stall"原则冲突，默认不做。
5. **cull 的 workgroup 形状**：本设计 = 1 workgroup/item/64 线程。若某 item 候选数很小（<64），workgroup 利用率低；
   若 Bistro 的 item 分布让 cull 的 GPU 时间超出预算（预计 < 0.1 ms），再考虑"按候选总数网格化 + per-item 边界表"的
   形状（那需要 host 侧前缀和，先不做）。
6. **两遍的 dispatch 是同一个 `hzb_cull` shader 还是拆成两个 shader**：本文建议同一个 shader + `mode` 分支
   （规则一致、单测一套）；若分支影响 wave 占用再拆。
7. **是否需要 `hzb_survivors` 的 `transfer_source` 读回**（调试）：加了无害；阶段 A 的探针只读 `hzb_levels`/`hzb_header`，
   阶段 B 想抓 survivor 表对拍时再加。

## 13. 仍需测量的数字（写进报告，不能只写"预期"）

- HZB build pass 的 GPU 时间（1280×720 全分辨率、11 级）与 `hzb_levels` 的显存：1280×720 每 lane
  ≈ W·H·(4/3)·4 B = 4.92 MB，两 lane ≈ 9.8 MB（4K 时 ≈ 88 MB，必须如实报告）。
- cull pass 的 GPU 时间（2909 item / 38,864 候选 near；43,930 候选 subtilis off）。
- **实际剔除率**：near 相机下候选/剔除/保留的百分比，与探针 33.28% 和原型 35% 的对比（分母写清）。
- **dispatch 组数下降**对 visibility raster 的实际影响（GPU written count vs 原型 host 缩小 dispatch）：这是"−31.9% 是否可复现"的核心。
- far 相机的 overhead 绝对值（build + cull）与像素差异（必须 0）。
- 阶段 B 静止门控下的像素差异（必须 0）；阶段 C 运动 capture 的差异比例与"丢失几何"检查。
- CPU 增量：`record_visibility_items` 微秒计时 + `scene`/`post` 两相之和相对 OFF 的增量（prepare 落在 `scene` 相）。
- 两遍 recovery 的候选/发出比例（运动相机），若 > 15% 候选则触发 §11.1 的回退门控讨论。
- 3×3 交错矩阵的 GPU/CPU/墙钟中位数、P95（单会话、未锁频的结论边界沿用 README.md:165）。

## 14. 一句话总结

**GPU 建 2 帧前的 max-HZB，GPU 用 §6 的保守球测试压紧 survivor 列表并直接写 per-item 的 indirect 计数，
第一遍只派发 survivor、第二遍用本帧 HZB 复测被剔除集补画。** 它保住了原型测到的近景 −28% GPU 收益，
把 +11.2 ms 的 host 成本换成 ≤0.2 ms 的表写入，并且用一个可对拍、可 RED 的 CPU reference 把保守性
从"看起来对"变成"测试钉住"。默认关闭，逐阶段验收，逐阶段可回退。

## 15. Lead 验收决策（2026-10-20，验收本规格时定）

以下是 §12 未决问题中需要 owner 决定的五项，以及两条附加验收要求。实现按这些决定执行；要改变决定必须带着测量证据回到这份文档。

1. **阶段 B 的"字节精确静态门控"可以作为默认关闭的采集能力先落地。** 它只在 view/proj 逐字节相同的帧启用，是验证 conservative GPU cull 的最小风险切片；但**默认开启/产品化必须等阶段 C（两遍 recovery）**，否则运动相机下它不是保守的。
2. **接受 full-res HZB 的内存**（720p 约 9.8 MB、4K 约 88 MB，按 §4.1 的 storage-buffer 方案）。阶段 A/B 先按 full-res 做；阶段 D 再测半分辨率金字塔，只有拿到"半分辨率不放宽 recovery 且 GPU 收益不降"的证据才切换——不在正确性之前先做性能取舍。
3. **阶段 B 用 heap alias（`hzb_commands` + `device_address`）让 GPU 写命令字，本轮不给 RHI 加 `draw_mesh_tasks_indirect_count`。** 先验证收益；若 alias 被驱动拒绝、或阶段 D 显示 draw 数本身是瓶颈，再把 indirect-count 作为独立的 contract 增量提案。
4. **resize 时 buffer `output_size` family 是否重建，是阶段 A 必须验证的第一件事。** 若 `render_resource` 不重建 buffer，按 §5.2 的 fallback（runtime-owned buffer + resize 时一次 `wait_idle`）实现，并把验证结果写进 review 证据；不允许未验证就假设。
5. **同意追加 graph lanes（19/20 之后，按现有手编号规则）并在阶段 B 把 `record_visibility_items` 拆成 prepare/record。** 拆分必须保证 HZB 关闭时行为与当前逐字节一致——现有的 OFF/ON 截图逐像素对照就是 gate。

**附加验收要求：**

- **阶段 A 的 HZB 读回验证**必须包含"GPU 构建的 pyramid 与 host 用 `DEREN_DEPTH_PROBE_CAPTURE` 深度离线构建的 max pyramid 逐 texel 相等"（若允许误差，必须在报告里给出误差来源与上界），这是 HZB 数学正确性的唯一机内证据。
- **阶段 B 的 near 画面差异**必须与 host 原型（0.314% 像素、max 10）对比并解释差异来源（近深度界改进了哪些像素）；不能只报"更小"。


## 16. 阶段 B2 实施验收（2026-10-21）

已接入 GPU survivor 与 per-item indirect commands，保持阶段 B 的默认关闭与字节精确静态门控。完整 host-cut run 才进入新来源；无法发布时用 UINT32_MAX sentinel，保留 host command，raster/resolve 同时回退原记录。跨 pass 同步由图声明，探针另行显式发布 GPU→host 可见性。

Bistro 远/近视点 CPU-reference 判断100%一致，同变体 OFF/ON 逐像素相同；强制 overflow 与移动 gate 关闭时也保持完整画面。各三轮交错测量的 GPU frame 中位数：far24.47→10.62ms（−56.60%）、near21.80→5.09ms（−76.65%），包含 cull 成本。Release58/58，相关 ASan/UBSan4/4；独立审查的一项探针同步问题已修复并复验。明细与已知 near OFF-vs-OFF 两变体抖动见 `docs/reviews/2026-10-20-bistro-performance/hzb-b2-verify.md`。

下一阶段为 C 的 current-frame recovery，尚未实现；动态相机/TAA jitter 仍关闭剔除，不能因此默认开启阶段 B。
