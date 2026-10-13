# G-buffer / HDR 迁移暂停记录 — 2026-10-13

用户要求先停下来。尚未修改生产渲染实现；light/head 已完成阶段保持原状。
本轮只完成代码调查、冻结迁移前运行包，以及短暂添加并运行一个 HDR provider 回归。
回归先出现预期失败：runtime 的 post_color heap 图像不等于应用链声明的 HDR generation。
暂停时已移除此尚未实现的测试入口，避免让现有测试集故意失败。

## 续接位置

- runtime.constructor.cppm::create_render_chain_targets 仍创建 HDR、三个 G-buffer target、velocity、scene-color input、depth。
- create_render_chain_targets 在构造及 on_swapchain_recreated 中调用；resize 先 refresh_frame_extents，再重建并 reset_image_generation_state。
- 上述图像按输出图像编号索引，数量仍取 RHI max_swapchain_images。
- scene_color 是每帧别名：TAA 开启时指向 scene_color_images（TAA current input），否则指向 hdr_images；不要把别名变成单一固定 allocation。
- gbuffer_targets 是三个不同 format 的 family；当前 registry 每个 resource_id 只持有一种 descriptor。续接需决定分开命名物理 target，或扩展 family 描述。
- 建议共享 output-target producer 声明实际图像，context 提供 render extent；runtime 选择应用/兼容生产者、完整校验、等待后更新 borrowed handles 和 heap。
- 保留现有 clears、barriers、输出索引及 generation 状态重置；旧 snapshot 延续现有按提交帧槽退役规则。
- history、LDR、Bloom、MegaLights、RT visibility 图像本轮暂不迁移。

## 现有证据

- 冻结包：build-release-dyn-clang64/output-resources-check/runtime.before。
  包含本轮修改前的 deren.exe、DLL、shaders，以及 toon 所需 executable-relative FGD LUT。
- 捕获脚本：同目录 run-captures.ps1；尚未执行该轮 baseline/candidate 捕获。
- 短暂 HDR 子测试：3 checks, 1 failed（预期），失败位置是 post_color heap 未使用 passchain-owned 图像。
- 已移除该子测试和父测试入口；尚无 output-target 生产代码、module 或 schema 改动。
- 上一阶段完整验证和进度：2026-10-13-scene-constants.md。
- 工作区有多轮未提交修改，不要整体重置。未提交或推送。

## 续接状态

用户随后授权继续。已实施本轮迁移；最终结果见 2026-10-13-output-resources.md。上面的记录保留为暂停时的历史状态。
