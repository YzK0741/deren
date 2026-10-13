# MegaLights / RT shadow 图像资源迁移 — 2026-10-13

三个pass的图像资源迁移已完成，可正常构建和运行；未提交或推送。之前阶段的未提交修改保留。

## 实现

- MegaLights trace pass描述半分辨率trace；temporal pass描述resolve和history；RT shadow pass描述全分辨率R16F visibility。
- 三张MegaLights图像按输出图像编号索引，visibility按帧槽索引。scope/count admission与adopt使用各自描述，避免统一按输出数误访问RT数组。
- 共享output_targets_pass扩展至17种图像，为兼容链提供缺失类型；具体pass与共享生产者的相同请求合并，不重复创建。
- trace/resolve/visibility发布既有sampled与storage两种heap描述。resolve含transfer-source usage，history含transfer-destination usage且生命周期history；shader、heap槽位、barriers与拷贝顺序不变。
- runtime对ML和RT图像只持有borrowed handles与snapshots；旧创建循环移除。legacy render-target rebuild只剩furnace cube。
- MLtrace generation更换时重置序列计数；temporal resolve/history更换时作废局部历史/结果状态；runtime在ML三元组更换时清除history-valid数组。
- 等待、bridge切换、应用/兼容生产者选择、提交快照按槽退役沿用已验证路径。普通reconcile不清图像内容。RHI ABI32、resource enum数量不变。

## 验证

- 新heap身份回归在实现前3checks中2项失败，实现后通过。
- ray_resources91checks：具体pass需求、sampled/storage绑定、frame-slot/output数量不同、半分辨率奇数取整与1x1下限、复用、两提交槽保留/退役、非法RT scope在acquire前拒绝、部分创建失败回滚。
- 扩展output_resources181checks、resource schema603checks通过。
- 完整Release构建成功；CTest24/24，24.81秒。真实Vulkan runtime1164checks通过。
- 13组前后捕获，各40帧、0.5度/帧sweep，全部RGBA像素一致且同步验证干净。
  含此前10组、rt_smoke、32灯MegaLights+RT、0.67渲染比例MegaLights+RT+TAA。
- 三个专门光追场景采用baseline-ray包配置作为有效基线，捕获脚本断言startup配置确实加载。
  旧脚本最初生成的专门场景配置不计入有效比较；保留工作目录以供追溯。
  ML/RT日志中有rt_shadow、megalights_trace、megalights_temporal的declaration handle验证，确认相关stage被解析运行。
- ML+RT+缩小渲染比例+TAA同时开启，定时最大化再恢复，两次resize验证干净，捕获1280×720与client尺寸一致。
- Gaussian CPU/GPU各12帧通过且验证干净；保持44差异像素、最大5/255、HDR RMSE2.5238316397326248e-5。此为两路径比较，不是版本比较。
- 独立审查没有发现本轮引入的生命周期、索引或descriptor缺陷；git diff --check通过。

## 已发现的原有时域问题

2026-10-14更新：以下审查发现已在[独立时域修复](2026-10-14-megalights-history-fix.md)中解决，保留原记录供追溯。

源代码审查确认：megalights_history_valid从未提升为true；megalights_temporal.slang无条件读取history。
当前CPU的false只决定UNDEFINED→SHADER_READ layout transition，不抑制shader读取未定义历史。
这在迁移前已存在，本轮保留算法行为；前后像素相同不能证明时域累计正确。
后续应修复有效标记反馈与shader首帧历史分支，再单独验证首次使用、重新启用、replacement与resize。
这不是未完成的资源迁移，但它限制MegaLights时域正确性，不能遗漏记录。

## 证据与续接

- 归档：docs/reviews/2026-10-13-ray-resources/，含13组前后PNG/配置/日志、像素统计、脚本、Gaussian/resize证据、CTest与SHA256。
  大型HDR EXR仍在工作目录，归档保留统计。
- 工作/冻结包：build-release-dyn-clang64/ray-resources-check/及runtime.before；基线包含上阶段TAA/LDR/Bloom迁移，并携带同一toon FGD LUT。
- 管理说明和资源清单已更新，管理说明仍在Doxygen输入中。
- 图像部分剩furnace/IBL等共享资产和导入呈现资源；进一步迁移还有共享场景缓冲、Gaussian缓冲、AS、采样器及自动descriptor/barrier管理。
- 本阶段无影响正常运行的半成品。不要整体重置工作区。
