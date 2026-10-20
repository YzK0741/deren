# Bistro texture acceptance / 4096 capacity — 2026-10-20

**Final checkpoint 2026-10-20 19:40（用户要求继续后完成本轮；改动仍未提交、未 push）。**

状态：两项容量契约都已扩展并拿到真实 GPU 证据——①可见性打包 ID 的 item 4096（原始未合并层级 2909 primitives，2906 项入表，无回退）；②场景纹理数组 128→**1024**（网格 1024→**2048** 槽）；③textured Bistro 资产已按 pin 重建并全项校验（`textured-verified/`），在远/近两个固定相机下 albedo/lit 四张纹理帧全部出图（516 个纹理槽、无白染、validation 0）；④**PBR 路径验收完成**：开阴影的 PBR lit 远/近 + G-buffer 的 normal/roughness/metallic/AO 通道，确认 normal/MR 贴图被 PBR 着色消费；⑤**性能增益已实测**（独立报告 `docs/reviews/2026-10-20-bistro-performance/`）：同一 2909 项场景容量解锁使 GPU 帧时间 178.5→26.7 ms（~6.6×），Subtilis 在 4096 容量上再减 11–12% 三角形、GPU 帧时间 −8.7%、端到端帧时间 −8.2%；⑥**优化空间探针 + CPU 选层并行化完成**：探针测得视锥剔除 0–2%（无空间）、近景遮挡 33%（保守下界，M3 主攻）、远景亚像素 cluster 22%（M4 主攻）；`record_visibility_items` 已并行化（选层 CPU 8.6→~2 ms，cuts/截图与串行版完全一致，CTest 57/57、ASan 6/6）。**剩余：纹理场景 Subtilis OFF/ON 对照**（需临时 builder 补丁）以及是否提交的决定。

## 本轮完成（证据见 README.md / results.json）

1. **原始层级 4096 容量帧**：`bistro-geometry.gltf`（6006 nodes / 2909 primitives / 0 textures）→ `visibility raster: recorded 2906 item(s)`（cap 4096），无溢出/回退，validation 0，GPU 总 29.28 ms。截图 `capacity4096-original-hierarchy.webp`；旧的黑图原因（glTFTransform 覆写 `bistro.bin`）已修复并复核 blob。
2. **textured-verified 资产重建**：`scripts/measure/prepare_bistro_textures.py`（343 DDS→PNG，层级不合并，隔离 bin）→ `convert_bistro_metalrough.py`（234 材质 core-MR 近似）→ 新增 `scripts/measure/verify_bistro_textured.py` **全 PASS**（隔离 bin 与上游逐字节一致、逐 mesh 三角形数不变、597 张引用图齐全、无 spec/gloss 残留）。旧 `D:/deren-assets/bistro/textured/` 不可信，**勿用**。
3. **纹理容量契约**：`scene_texture_capacity = 1024` 在 `source/engine/render_layout/render_layout.cppm` **由网格推导**（`materials - textures`），core/runtime 两处镜像改为 alias；纹理区 0..1023、materials 起点 512→1024、其后所有槽统一 **+512**、`heap_slot_count` 1024→2048（最高用槽 base+1307，余 740）。实测 heap 日志：texture array @1179648、material table @1212416，1280 KiB 资源堆内。契约测试新增三方相等 + `static_assert(capacity>=517)` 编译期 RED 牙；`test_render_resources` 665 项检查通过。
4. **纹理真机帧**：textured Bistro 加载 811 textures / 254 materials / **2909 primitives**；四组运行（albedo/lit × 远/近）均 **516 个纹理描述符**（cap 1024）、无容量降级、validation 0；GPU 总 26.34/26.90/23.74/23.71 ms（单会话，非基准）。截图 `textured-albedo-far/near.webp`、`textured-lit-far/near.webp`。
5. **回归**：槽位 +512 重编号前后同一容量帧**逐像素相同（0 差异）**；最终二进制重跑两张截图也逐像素相同。Release **CTest 57/57**、ASan/UBSan 目标 **6/6**、`test_docs` 383 项检查通过；`clang-tidy --warnings-as-errors` 对 subtilis.cppm / visibility.cppm / render_layout.cppm **干净**。
6. 顺手修掉 3 处既有 `readability-trailing-comma`（visibility.cppm:135/410/555 附近）与 1 处 `readability-redundant-inline-specifier`（render_layout.cppm 的 `heap_slot_offset`）。

7. **PBR 验收（追加）**：引擎场景 shading 本来就是 metallic-roughness PBR（`shading.slang`：GGX + split-sum IBL + 方向光/级联阴影/聚簇点光）。6 组运行：`pbr-lit-far/near`（shadow=true，validation 0、516 纹理槽）+ G-buffer 通道 `pbr-normal-near`（细节清晰）、`pbr-roughness-near`（25 值、std 15）、`pbr-metallic-near`（大部分 0，金属构件高值）、`pbr-ao-near`（**平值 1.0：上游资产 254 材质均无 occlusionTexture**，引擎支持但资产不携带）。开 3 级联阴影后单会话 GPU 总时长约 85/127 ms（关阴影参考 ~27 ms，非基准；shadow 行读数 0.00 ms 且时间落在 visibility raster，逐 pass 拆分不可靠）。metallic 通道存在 Khronos 近似转换的低金属泄漏（0.13–0.25），不能当 ground truth。
8. **优化空间探针 + CPU 选层并行化（追加，见 `docs/reviews/2026-10-20-bistro-performance/`）**：用临时 cluster dump + 既有 `DEREN_DEPTH_PROBE_CAPTURE` 深度回读 + 离线 `scripts/measure/cluster_occlusion_probe.py` 测得——far 视锥剔除 0%、near 1.9%；**遮挡（保守下界）far 0.33% / near 32.52% 可见三角形**；**<1px cluster far 22.05% / near 3.09%**。据此定优先级：HZB 遮挡（M3）主攻近景，软件光栅（M4）主攻远景（互补，按相机分布二选一先做）。`record_visibility_items` 重构为「串行候选扫描 → 并行逐 item 规划（进程级线程池）→ 按 item 序用有序区间表裁决 published 重叠（重叠 item 串行重规划）→ 串行记录」：选层 CPU ~8.6→~2 ms，cuts 与截图和串行版完全一致，CTest 57/57、ASan 6/6。临时探针已全部移除，生产代码只保留并行化改动。
9. **调参实验（负结果，钩子已移除）**：运行时 cut 误差预算 1→4 px 只让选中记录降 ~1%、GPU 时间不动（画面差异已到 1.2-1.7%/0.3-1.6% 像素）；层级构建参数（target_ratio .35 / max_error .03）同样 ≤1% records、GPU 噪声内。两个"改数字拿性能"的假设都证伪——**剩余收益只能来自结构性 GPU 侧**：近景 HZB 两遍遮挡（M3，33% 可见三角形保守可剔）或远景软件光栅（M4，22% cluster <1px），参数调优无法触及。
10. **M3 host 侧原型（已验证潜力后回退，见性能报告）**：逐帧深度回读 + CPU Hi-Z + 并行规划内保守球剔除。near：**35% 选中记录被剔除 → GPU visibility raster 20.79→14.16 ms（−32%）、GPU 帧 −28%**，但 host 成本 +11.2 ms CPU（帧变 CPU-bound），端到端只 −9.3%，且 0.314% 像素变化（近平depth 近似不够保守）→ 不发布、已回退并归档 `occlusion-prototype.patch`。结论：遮挡值得做，但**必须 GPU 侧**（HZB build + compute 剔除 → dispatch），才能拿到 −28% 而消掉 host 成本。这是 M3 的下一步形状。

11. **M3 阶段 A1（已验收提交）**：新增 HZB 的 6 个堆槽位（`base+1308..1313`：levels/header/items/results/survivors/commands；最后一个是 `mesh_indirect_buffer` 的 descriptor 别名，不新分配），host/shader 镜像与网格 static_assert 同步；新增纯 CPU 模块 `deren.engine.visibility.hzb`（pyramid 布局公式、max-pyramid 构建、§6 保守球测试含修正后的近深度界/8 角点矩形、§9.2 字节级 scene gate、§4.3 确定性压紧）与 `test_hzb`（1031 checks，含把原型两个错误公式本地复现的 RED 判别用例）。CTest 58/58、test_render_resources 690、test_docs 385；阶段 A2（build shader/pass/图接线 + GPU 读回探针）待派。

12. **M3 阶段 A2（已验收提交）**：GPU 侧 HZB build lane 落地——`hzb.slang` 布局镜像 + `hzb_build.slang` 每级一个 dispatch（2x2 max + 边缘复制，level0 线程 0 在同 dispatch 写 header，view/proj 来自相机 UBO 的 GPU 快照）、`hzb_build` pass（12 级、相邻级之间一个 COMPUTE→COMPUTE buffer barrier）、graph 追加 lane 22（数组 21→23，lane 21 留给 phase B 的 cull）、runtime-owned `ensure_hzb_resources()`/`publish_hzb_bindings()`（GPU-only 表，每次 extent 变化最多一次 wait_idle，`hzb_valid` 失效）与 `DEREN_HZB_PROBE_CAPTURE` 读回 + `scripts/measure/hzb_verify.py`。门控 OFF 零 hzb 行、像素与改动前逐像素相同；ON（far/near 两相机）pyramid 与 numpy 参考 **0 word 差异**（1,228,844 words）、header 矩阵与该帧相机**逐字节一致**、像素 0 变化、validation clean；CTest 58/58。§5.2 验证：render_resource 的 buffer output_size family 在增大时会重建（instances.cppm:189-201），但因 resource_id schema 与 scene-table 的 host-visible/zero-on-create 准入契约，A2 两表改用 runtime-owned fallback（代码注释记录，phase B 复查）。证据：`docs/reviews/2026-10-20-bistro-performance/hzb-a2-verify.txt`。下一步 phase B（cull pass + GPU 写计数 + survivor 消费）。

13. **M3 阶段 B1（已验收提交）**：GPU cull pass 落地——`hzb.slang` 增加 §6 保守测试的 GPU 镜像（8 角点矩形、单调近深度界、fused Sterbenz margin、完整 fail-open 列表）、`hzb_cull.slang` 每 item 一个 workgroup 遍历候选（flags → 升序确定性压紧 survivor → pass1 count + stats，越界冻结不发布部分结果）、`hzb_cull` pass 在 raster **之前**（图 lane 21）、host 侧填充 `hzb_items` + 字节级静态门控快照、runtime-owned 三张相位 B 表、`DEREN_HZB_CULL_PROBE_CAPTURE` 的 history lane 读回 + 机内 CPU 参考逐候选对照 + `hzb_cull_verify.py`。实测（subtilis=true，130 帧）：**far 71.514% / near 92.831% 候选被保守剔除**（每帧 50,808→14,473 / 43,115→3,091 survivors），CPU 参考 **100% 决策一致**（50808/50808、43115/43115）、survivor 顺序与 per-item count 全匹配、0 overflow；cull GPU 成本 far 0.51-0.63 ms / near 0.28-0.39 ms；像素 0 变化（near 仅有改动前就存在的 2901px/两变体抖动）；CTest 58/58。**重要修正**：原型/离线探针用的 `r_w/d` 界比精确单调界松 1834x（far）/954x（near），所以"far 0%、near 33%"只是极弱下界——B2 消费后的真实收益可能远大于预期。证据：`docs/reviews/2026-10-20-bistro-performance/hzb-b1-verify.txt`。B2 已在2026-10-21完成，见下一条。

14. **M3 阶段 B2（2026-10-21）**：实际消费 GPU survivor、写 per-item indirect3字、raster/resolve 同源与 sentinel 完整回退；命令缓冲复用并补 device_address，图声明 compute→mesh/compute/indirect 同步。真实 far/near 同变体截图像素差异0，CPU reference100%，probe7044/6978项通过；强制 overflow 与移动 gate 关闭均保持完整图像。三轮交错 GPU frame 中位数 far24.47→10.62ms、near21.80→5.09ms，Release58/58、相关ASan4/4。独立审查发现的 GPU→host 探针可见性已修复并复验。保持默认关闭，下一步 phase C recovery。证据：`docs/reviews/2026-10-20-bistro-performance/hzb-b2-verify.md`、`hzb-b2-performance.json`；大文件归档 D:/deren-assets/hzb-b2。

## 变更文件（未提交）

- 容量：`source/engine/render_layout/render_layout.cppm`、`source/shaders/common/heap_slot_constants.slang`、`source/backends/vulkan/core/core.declarations.cppm`、`source/backends/vulkan/core/descriptor_heap/descriptor_heap.cppm`（工作尺寸注释）、`source/engine/runtime/runtime.declarations.cppm`、`source/tests/test_render_resources.cpp`、`source/tests/test_goo_toon_math.cpp`。
- 上一轮沿用：`source/engine/geometry/subtilis.cppm`、`source/engine/visibility/visibility.cppm`(+trailing comma)、`source/shaders/scene/visibility*.slang`、`source/tests/test_subtilis.cpp`、`source/tests/test_visibility_buffer.cpp`、`docs/superpowers/specs/2026-10-18-subtilis-design.md`。
- 文档：`docs/descriptor_heap_migration.md`（新增 2026-10-20 容量增长小节并标注旧快照）、本目录 `README.md`/`results.json`/`asset-manifest.json`/证据。
- 脚本：`scripts/measure/prepare_bistro_textures.py`（隔离 bin 版本）、`convert_bistro_metalrough.py`、`test_bistro_materials.py`、新增 `verify_bistro_textured.py`。
- 未跟踪保留：`.vscode/`、`docs/reviews/2026-10-20-ci-checkpoint.md`、`docs/reviews/2026-10-19-ci-checkpoint.md`。

## 剩余工作（按顺序）

1. **纹理场景 Subtilis OFF/ON**：把上一轮 `docs/reviews/2026-10-20-bistro/acceptance-only.patch` 的 builder 思路适配到当前 `runtime.cpp`（`DEREN_BISTRO_BUILD` 时在 `create_primitive` 里 build 层级），应用到工作区、重编译、用 `textured-albedo/lit` 配置各跑 OFF/ON（远+近），记录 fallback/coarse/像素差；然后反向应用补丁、重编译、重跑测试。**无纹理原始层级的 OFF/ON 性能增益已由 `docs/reviews/2026-10-20-bistro-performance/` 实测**（含临时补丁的 apply/revert 记录与 CPU 选层代价），纹理版主要验证"带纹理时的画面/收益是否一致"。
2. 可选：normal map 朝向与参考图比对；MASK/alpha 轮廓逐材质核对；transmission 材质仍明确在验收外。
3. 提交/推送决策（本轮按要求未做 `.git` 变更）；提交前重跑 CTest + `git diff --check`。

## 边界与提醒

- 资产是外部大文件，只在 `D:/deren-assets`，不入库；复现命令与许可署名见 `README.md`。
- 性能数字是单会话 GPU pass 统计，不构成收益结论。
- **无临时 builder 补丁、无 crash handler、无测试入口残留在生产代码**；生产 runtime diff 只有容量契约与既有 subtilis/visibility 变更。
- 旧 `textured/` 目录是不可信测试产物（bin 覆写事故），只使用 `textured-verified/`；`bistro.bin` 当前 blob 为 `f6c918e6...`（已复核）。
