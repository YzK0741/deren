# Passchain 资源管理进度记录 — 2026-10-13

状态：2026-10-13 按用户要求继续后，第一阶段实现、验证和证据整理均已完成。此前暂停点已收尾。
工作区：`C:/Users/23530/Desktop/yzk/deren`；当前改动未提交、未推送。
此前离屏渲染、配置和阴影修复的未提交改动仍在同一工作区，不能当作本轮改动清理。

## 用户接受的方案

pass 描述动态资源类型、生命周期与需求；passchain 收集、创建、按生命周期分组，
再分发共享资源快照。pass 使用资源，passchain 管理资源的创建、复用和更新。
在途帧保留旧快照，只有完成对应 GPU 等待后才能释放或重置该槽。

第一轮迁移 raster shadow 的图像及视图，保持 RHI ABI 32、shader 槽位和现有屏障规则。
其他 pass、AS 和外部图像继续使用原有路径。

## 已实现

- 新模块 `source/engine/render_resource/instances.cppm`，已加入 CMake 模块列表。
  支持 image/buffer 描述、生产者/消费者依赖、生命周期分组、共享 generation/snapshot。
- 相容的同名生产者合并；buffer 字节数、image 层数按最大需求创建。
  容量足够时复用，扩容或其他描述变化时创建新版本；缩小保留容量。
- 整批创建成功才切换 active set；冲突、缺少生产者或部分创建失败时保留旧版本，
  不重新分发到 pass。上传用的非空 initial_bytes 暂不支持，直接拒绝。
- generation 持有设备根、分配和视图；视图先于 image 释放，设备最后释放。
  生命周期索引存 resource_id，而不是裸 object*。
- `frame_pass` 增加 describe_resources/bind_resources 默认钩子；
  `pass_chain` 增加 prepare_resources/begin_frame/resources。
  chain 析构前等待设备完成，之后释放自己的 pass 和资源。
- begin_frame 只清零显式请求 zero_on_frame 的 mapped buffer 的已完成帧槽。
  history 保留内容，不能请求这种清零策略；图像仍使用已有 pass clear。
- shadow_pass 声明每帧槽的分层深度图，持有分发快照并从它选择录制用 image/view。
  runtime 原来的 shadow owning vectors 改为借用视图。
- runtime::prepare_managed_resources 在 acquire 之前收集应用链资源。
  没有 shadow 生产者时使用仅管理资源的兼容链；应用链仍独立收集、重置和保留快照。
  每次重新选择资源归属，没有保留旧链的 resource-owner 指针。
- 阴影 generation 改变时保留原有 wait_idle 规则，刷新借用视图和采样 heap 绑定，
  使 shadow cache 失效。级联扩容不再重复创建 light/head UBO。
- 成功 submit 后分别保留应用链快照和 shadow 快照；只有成功完成对应 slot wait 后释放。
  初始化错误不再从已 acquire 的帧返回可重试错误，避免遗留 acquire semaphore。

## 审查

独立审查首次发现四项集成问题：兼容阴影链遮蔽应用链资源更新、切换链遗留旧指针、
创建失败发生在 acquire 之后，以及阴影新版本未同步 runtime 借用视图/heap。
这些问题均已修正；复审未发现剩余重要缺陷。

新增真实 shadow_pass 的动态级联测试验证扩容后两帧槽采样 heap 都指向新 image，
缩小级联复用容量。该 portable 测试未创建真实 shadow pipeline，不能作为完整 GPU
attachment / hand-back barrier / sampled descriptor 三者同时切换的运行证据。

## 已执行验证

- Release 全量构建：成功；目录 `build-release-dyn-clang64`，并发 3。
- 最终 CTest：24/24 通过，20.59 秒。
- `test_runtime_injection --child-managed-resources`：46 检查通过。
- `test_runtime_injection --child-managed-frames`：46 检查通过。
  覆盖在途 generation 保留、完成槽回收、创建失败不 acquire、销毁旧链后切换、
  真实 shadow_pass 的级联扩容/缩小及 heap identity。
- `test_runtime_dyn --with-device`：真实 Vulkan 设备 1164 检查通过。
- `test_docs`：273 检查通过；`git diff --check` 通过。
- 四个 PBR 场景：deferred、shadow_single、transparent_blend、sponza，
  1280×720、40 帧、每帧 sweep 0.5 度，baseline 与 candidate 都无同步验证错误。
  首轮 candidate 与 baseline 已逐像素比较，四场景全部一致。
- 最终 candidate-final 四场景也全部运行成功、同步验证干净。
  恢复工作后已直接比较 candidate-final 与 baseline：四场景的 RGBA 像素全部一致。
- Gaussian CPU/GPU 各 12 帧运行成功并通过验证。
  CPU/GPU 的现有路径差异：44 pixels，最大通道差 5/255；HDR RMSE 2.5238e-5。
  这是两种渲染路径的比较，不能直接当作本轮迁移前后的比较。

## 文档与证据位置

- 使用说明：`docs/managed_resources.md`；已加入 Doxyfile，并从 mainpage 链接。
- 资源清单：`docs/render_resource_inventory.md`，已更新 shadow ownership 并加入 Doxyfile。
- 计划：`docs/superpowers/plans/2026-10-13-passchain-managed-resources.md`。
- 接口设计：`docs/superpowers/specs/2026-10-13-unified-resource-access-design.md`。
- 证据根：`build-release-dyn-clang64/managed-resources-check/`。
- 冻结旧运行包：`runtime.before/`，迁移前 exe SHA256：
  `EE7A0AF98E693A3A350E0FAC72DCB6155C04761CF5B025773AE9E4BB2F03FA7E`。
- 捕获：`baseline-{scenario}`、`candidate-{scenario}`、`candidate-final-{scenario}`。
  各目录包含 PNG、配置、debug.log 和输出日志。
- 首轮像素比较结果：`pixels.json`；最终比较结果：`pixels-final.json`。
- 持久证据：`docs/reviews/2026-10-13-managed-resources/`，含四组前后截图、配置、日志、
  捕获脚本和最终 SHA256 清单。二进制 hash 在归档时记录，未冒充捕获当时记录的 hash。
- 捕获脚本：`run-captures.ps1`，可传 Variant；baseline 使用冻结包，其他 variant 使用当前包。
- Gaussian 证据：`gaussian/cpu/`、`gaussian/gpu/`。

## 本阶段完成状态与后续范围

计划五项均已完成。继续工作时可从其他 pass 的迁移开始，不需要修补本阶段才能正常运行。
本次恢复只完成最终比较与归档，没有修改渲染源代码，也没有重复此前已经通过的 GPU 测试。

其他 pass、AS/imported 类型、自动 descriptor 发布、状态/屏障生成，以及更完整的
render/output extent context 仍留在后续阶段。四场景像素一致证明当前捕获条件下画面保持；
不等同于证明所有场景或所有同步问题已经消失。

长期约定：不使用 ensure_ 前缀；额度剩余 3% 时停下并记录；未经用户要求不消耗 reset credit。
