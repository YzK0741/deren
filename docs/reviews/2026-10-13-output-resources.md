# G-buffer / HDR passchain 资源迁移 — 2026-10-13

本阶段完成，正常构建和运行；未提交或推送。保留之前的 shadow、cluster、light/head、离屏和阴影修复等未提交改动。

## 最终实现

- 新增 output_targets_pass，描述七种 output_size 图像：HDR、三个 G-buffer target、velocity、物理 TAA scene_input、depth。
- 请求以 context.render_extent 创建，按输出图像编号索引；runtime 继续使用 RHI max_swapchain_images 上界，不改实际帧槽/输出索引规则。
- 三个不同 format 的 G-buffer target 采用独立物理 allocation id；这些 id 追加于现有枚举后。
  GPU 仍使用原 heap slots，pass IO 保留 gbuffer_targets family，scene_color 仍按每帧 TAA 状态路由到 scene_input 或 HDR。
  新物理 id 当前用于 allocation 描述，未替换既有 IO routes。
- 应用链可提供单独的图像类型，兼容链只声明缺失的类型。所有选中描述通过 kind、scope、lifetime、extent、format、mip/layer、instance count 和 required flags 检查后，才改变 borrowed handles 和 heap。
- 图像与 views 归 chain generation 所有；runtime 持有 borrowed handles 和 snapshots。替换沿用 idle wait，成功提交按槽保留应用/兼容 snapshots，成功等待该槽后才退役。
- 尺寸变化创建新 generation，同尺寸可复用；已有 layout/history/Gaussian frame 状态在安全等待后保守重置。现有 render clears 和 barriers 不变。
- 构造时只准备输出图像，shadow/cluster/light/head 保留配置生效后首帧延迟创建。
- 修正旧首帧错误路径：先检查 managed preparation，再发布其他 heap bindings，最后 acquire。
  首帧分配失败返回 acquire_failed，保留旧 heap 和输出图像，后续能重试。
  显式 set_shadow_cascades 增长保留原 wrapper 和原错误策略。
- shader 布局和 RHI ABI32 不变。LDR、history、Bloom、MegaLights、RT visibility 等图像本轮仍走原路径。

## 回归与审查

- 初始 HDR provider 回归：3 checks 中 1 项预期失败（runtime 仍使用自身 allocation），实现后通过。
- 延迟创建回归：100 checks 中 2 项预期失败（构造提前创建 shadow/light），output_only 修正后通过。
- 首帧 allocation failure 回归：修复前进程终止；修复后 7 checks 通过，覆盖未 acquire、heap 未变及成功重试。
- 最终 output_resources：103 checks，通过。覆盖每个输出/每种图像的 heap 绑定、应用/兼容生产者混用、复用、chain handover、按提交槽退役、非法 usage 和 allocation failure 在 acquire 前拒绝、尺寸变化/输出数变化、部分创建失败回滚。
- resource schema：603 checks，通过。
- 完整 Release 构建成功；最终 CTest24/24，27.11 秒。
- 真实 Vulkan test_runtime_dyn --with-device：1164 checks，通过。
- 九组冻结迁移前/最终候选捕获，各40帧、0.5度/帧 sweep：deferred、shadow_single、transparent_blend、sponza、32灯 clustered on/off、laevatain_goo_toon、TAA-on、0.67 render-scale TAA-on。
  全部 RGBA 像素一致，全部同步验证干净。常规1280×720，toon1080×960。
- 最终 Gaussian CPU/GPU 各12帧通过，同步验证干净；保持44差异像素、最大5/255，HDR RMSE2.5238316397326248e-5。
  这是两条路径对比，非两版本捕获对比。
- 最终窗口最大化→恢复两次尺寸变化成功，早期捕获1280×720，与client尺寸一致，同步验证干净。
- 独立审查检查替换、retention、alias、resize 和 output-only 初始化；指出首帧 panic 问题，已修复并复审。
  恢复 cascade setter 所需 wrapper 后完整构建/测试通过。
- git diff --check 通过。

## 证据与续接

- 归档：docs/reviews/2026-10-13-output-resources/。
  包括九组前后 PNG/配置/日志、Gaussian 与 resize 最终配置/日志/PNG/统计、CTest日志、像素比较和SHA256。
  大型 Gaussian HDR EXR 原始文件仍位于工作证据目录，归档保留统计。
- 工作目录：build-release-dyn-clang64/output-resources-check/。
- frozen runtime.before 包含上一完成阶段的运行包、shader 与同一 toon FGD LUT；不要覆盖该包或混用更早的 cluster 基线。
- 文档：managed_resources.md / render_resource_inventory.md 已更新，managed_resources 属于 Doxygen 手册输入。
- 下一阶段：TAA历史和后处理图像（LDR/Bloom），随后 MegaLights/RT visibility；history 生命周期应单独处理，不按帧清零。
- 尚无本阶段必须补完才能正常运行的代码。不要整体重置工作区。
