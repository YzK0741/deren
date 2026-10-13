# Light/head 每帧常量资源迁移 — 2026-10-13

本阶段完成，可以正常构建和运行；未提交或推送。保留工作区此前离屏、阴影、shadow/cluster 资源迁移等改动。

## 实现

- 新增 scene_constants_pass，声明 light/head 两种 frame_slot 常量缓冲；应用链与兼容链可分别提供两者。
- passchain 创建、持有并分发 generation，runtime 只保留 borrowed handles 和共享 snapshot。
- CPU 仍计算 light/head 参数，成功等待当前帧槽后经 write_frame_buffer 写入；移除 runtime 私有映射指针。
- 上传检查 id、类型、scope、槽号、容量和 coherent mapping；不提供隐式 GPU 同步。
- 新 generation 用 light_ubo{} / head_ubo{} 初始化，保留头部默认轴向；常量不按帧清零。
- 自定义生产者的所有描述通过 admission 检查后才更新桥接和 heap；替换遵循 idle wait，提交快照按槽退役。
- 新 head_ubo resource_id 追加于原有 id 后；shader 布局、heap 槽位、RHI ABI 32 不变。

## 验证

- Release 完整构建成功；CTest 24/24，通过时间 20.81 秒。
- scene-constants portable 子测试 60 检查通过：两槽隔离、任意时间 setter、默认轴向、链切换、旧 generation 退役、错误清零 policy 和上传边界。
- 新上传回归在实现前先失败（unsupported），随后通过。
- resource schema 602 检查通过；此前 managed/cluster 子测试继续通过。
- 原生 Vulkan test_runtime_dyn --with-device：1164 检查通过。
- 七个原生场景各 40 帧、0.5 度/帧 sweep，均无 VUID、SYNC-HAZARD 或验证错误。
- deferred、shadow_single、transparent_blend、sponza、32 灯 clustered on/off、laevatain_goo_toon 的迁移前后 RGBA 像素完全一致。
  常规场景 1280×720，toon 1080×960。
- 最初冻结 toon 基线遗漏 executable-relative FGD LUT，产生黑色轮廓；仅补齐同一 LUT 后重新捕获即完全一致。
  shader 字节也一致。错误资源包的初次捕获单独归档，不作为有效基线。
- 独立代码审查检查初始化、wait、frame-slot 上传、provider 切换和退役，未发现具体剩余缺陷。

## 证据与后续

- 持久证据：docs/reviews/2026-10-13-scene-constants/，含配置、日志、PNG、捕获脚本、像素比较和 SHA256 清单。
- 冻结可运行包：build-release-dyn-clang64/scene-constants-check/runtime.before；包含上阶段 cluster push/light-count 修复。
- 已更新 docs/managed_resources.md 和 render_resource_inventory.md；managed_resources 已列入现有 Doxygen 手册输入。
- 下一阶段优先考虑 G-buffer 和 scene-color/HDR 图像；历史图像另外处理，避免按帧清零或错误退役。
- camera/deformation、其他 pass 分配、AS/imported 资源、自动 descriptor 发布和自动屏障仍走原路径。
- 本阶段没有影响正常运行的半成品；当前无须继续补代码即可运行。
